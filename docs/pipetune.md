# Script-Based PipeTune

PipeTune is a Python controller for bounded Axio experiments. It measures one
target endpoint, diagnoses the dominant datapath cost, and evaluates tuning
candidates through repeated cold starts. The peer endpoint supplies traffic
and remains fixed except when a target queue change requires a reciprocal
route update.

The script-based implementation tunes C1-C3. It reports C4-C6 as manual
follow-up choices because those parameters require an Axio rebuild. The
configuration fields are defined in the
[`Axio Configuration Guide`](configuration.md).

## 1. Roles and Execution Model

Each run has three roles:

| Role | Responsibility |
| --- | --- |
| Controller | Reads both TOML files, creates trials, and stores results |
| Target | Runs Axio and supplies Axio, `perf`, and `pcm-pcie` measurements |
| Peer | Runs the other Axio endpoint and supplies traffic |

The target may be the client or the server. The client always initiates
traffic, and PipeTune always starts the server before the client.

The controller may run on a workstation, the client host, or the server host.
`deployment.transport` in each TOML specifies how it reaches that endpoint:

- `local`: execute directly on the controller host;
- `ssh`: execute through the system SSH client.

For an SSH endpoint, PipeTune executes
`WORKDIR/pipetune/remote_worker.py`. It sends each materialized trial
configuration to `WORKDIR/.pipetune/trials/`, starts the existing Axio binary,
retrieves the measurements, and removes the remote trial directory. It does
not clone the repository or build Axio during tuning.

## 2. Prepare the Controller and Endpoints

PipeTune requires Python 3.10 or newer and uses only the Python standard
library. Build the configuration utility on the controller:

```bash
meson setup build-tools -Ddatapath=false
ninja -C build-tools axio-configure
```

Prepare both endpoints before running PipeTune:

1. Check out the same reviewed Axio revision on both hosts.
2. Set each TOML's `deployment.workdir` to that host's repository path.
3. Build `build-client/axio` on the client and `build-server/axio` on the
   server with their respective PipeTune TOML files.
4. Install `/usr/bin/perf` and `/usr/sbin/pcm-pcie` on the target.
5. Permit non-interactive `sudo` when `deployment.use_sudo = true`.

For an SSH endpoint, verify both SSH and sudo before tuning:

```bash
ssh -p PORT USER@HOST true
ssh -p PORT USER@HOST sudo -n true
```

For a local endpoint that uses sudo, run `sudo -n true` on the controller
host. PipeTune uses SSH batch mode and cannot answer a password prompt.

The repository provides a reference pair:

- [`config/pipetune/server-16c.toml`](../config/pipetune/server-16c.toml):
  16-core colocated server target;
- [`config/pipetune/client-8c.toml`](../config/pipetune/client-8c.toml):
  fixed 8-core colocated client peer.

These files assume that the controller runs on the reference client host. The
client uses `transport = "local"`, and the server uses `transport = "ssh"`.
For another testbed, update `[deployment]` and `[network]` first. When the
controller is a separate workstation, set both endpoints to SSH.

Both TOML files must be present on the controller. Their configured workdirs,
worker scripts, binaries, and Git revisions must also exist on the endpoint
hosts.

Validate the pair on the controller:

```bash
build-tools/axio-configure validate-pair \
  config/pipetune/server-16c.toml \
  config/pipetune/client-8c.toml
```

Bind and build the client configuration on the client host:

```bash
meson configure build-client \
  -Daxio_config="$PWD/config/pipetune/client-8c.toml"
python3 toolchain/axio_build.py build-client --target axio
```

Bind and build the target configuration on the server host:

```bash
meson configure build-server \
  -Daxio_config="$PWD/config/pipetune/server-16c.toml"
python3 toolchain/axio_build.py build-server --target axio
```

PipeTune expects `WORKDIR/build-ROLE/axio` by default. Use
`--target-binary` or `--peer-binary` only when a binary is stored elsewhere.

## 3. Measure One Configuration

Run one bounded trial from the controller:

```bash
python3 -m pipetune measure \
  --target-config config/pipetune/server-16c.toml \
  --peer-config config/pipetune/client-8c.toml \
  --output results/measure-001
```

PipeTune performs the following operations:

1. validates the endpoint pair and build fingerprints;
2. creates trial-specific metrics paths;
3. transfers the materialized TOML files to the endpoints;
4. starts the server and then the client;
5. discards the configured warmup windows;
6. collects Axio metrics on both endpoints and host counters on the target;
7. returns the artifacts to `results/measure-001`.

The two configurations must use identical window policies, and
`other.iterations` must cover `warmup_windows + sample_windows`. The output
directory must not already exist.

The target counters are:

- LLC load miss rate;
- LLC store miss rate;
- PCIe read miss rate;
- ItoM/write miss rate.

Unavailable hardware counters are stored as `null` with a reason. They are
never reported as zero. Raw `perf` and `pcm-pcie` output is retained for a
successful trial.

## 4. Diagnose the Measurement

Diagnosis is offline. It reads the completed session and does not contact an
endpoint or start Axio:

```bash
python3 -m pipetune diagnose --session results/measure-001
```

Use `--json` to print the complete diagnosis document. If a session contains
multiple trials, select one with `--trial TRIAL_ID`.

A concise diagnosis contains:

- `Result`: P1-P4, `probe_required`, `inconclusive`, or `peer_unhealthy`;
- direction: `tx`, `rx`, or `n/a`;
- confidence: `high`, `medium`, `low`, or `none`;
- target and peer throughput;
- the longest target stage;
- the four target miss rates;
- `Next`: the next measurement required by the diagnosis.

The decision order is:

1. a dominant pipeline stall indicates P1;
2. a dominant NIC stage indicates P3;
3. a dominant application or dispatcher completion requires a C1 probe;
4. a significant increase in LLC misses as C1 increases indicates P2;
5. otherwise, consistent direction-specific I/O evidence indicates P4.

The C1 probe distinguishes a memory-limited completion stage from a
compute-limited stage. It is evidence, not an accepted tuning action.

`inconclusive (rx, confidence none)` means that the strongest evidence was on
the RX path, but it was insufficient for a P1-P4 decision. For example,
`dominant completion has no legal C1 perturbation` means the longest stage was
an application or dispatcher completion, but the configured topology could
not produce a valid C1 probe. This message does not indicate a missing metric.

`peer_unhealthy` means the peer reported drops, completion errors, or another
health violation. Correct the experiment and run `measure` again before using
its target evidence.

A diagnosis is a hypothesis. A candidate becomes a tuning result only after a
new cold-start trial confirms both its expected local effect and its
end-to-end benefit.

## 5. Run Automatic Tuning

Start a new tuning session from the largest NUMA-local core pool that may be
used by the search:

```bash
python3 -m pipetune bootstrap \
  --target-config config/pipetune/server-16c.toml \
  --peer-config config/pipetune/client-8c.toml \
  --max-iterations 4 \
  --output results/tune-001
```

`bootstrap` starts a new measurement sequence; it does not reuse an earlier
standalone `measure` session. `--max-iterations` counts completed diagnosis
rounds and cannot exceed `tuning.max_iterations` in the TOML files.

For each candidate, PipeTune applies two acceptance gates:

1. **Expected impact.** The diagnosed stall or miss rate must change in the
   expected direction beyond measurement uncertainty.
2. **End-to-end objective.** A latency-infeasible baseline requires a
   significant reduction in client P99.9. A latency-feasible baseline requires
   a significant increase in server throughput without violating the latency
   SLO, except that a count reduction may preserve equivalent throughput when
   it releases physical cores.

If the accepted baseline violates the latency SLO, any candidate must
significantly reduce client P99.9. C3 does not release a core and therefore
still needs a significant throughput gain once the accepted baseline is
latency feasible; before feasibility, it must instead significantly reduce
client P99.9. The equivalent-throughput/fewer-core exception applies only to
count reduction.

The normal memory-efficiency actions are:

| Diagnosis | Ordered candidate actions |
| --- | --- |
| P1 | C1 - 1; then double the direction-linked C3 triple |
| P2 | C1 - 1 |
| P3 | C2 - 1 |
| P4 | C1 + 1; C2 - 1; then halve the direction-linked C3 triple |

For P4, the C1 candidate reuses the completed `C1 + 1` diagnostic probe when
the target and peer configurations are identical. If the NUMA workspace or
topology cannot represent the additional application core, PipeTune skips C1
and continues with C2 and C3.

TX C3 contains application TX batch, dispatcher TX batch, and NIC TX post
size. RX C3 contains the corresponding RX fields.

If memory-efficiency candidates fail and completion time indicates insufficient
CPU capacity, PipeTune evaluates topology-aware compute candidates within the
NUMA workspace budget `U`. For an application bottleneck, the alternatives
include a one-to-one split and one complete balanced application fanout layer.
For a dispatcher bottleneck, dispatcher expansion remains one-to-one; the
search does not increase C2 while holding C1 fixed. Failed trials never replace
`best.toml`.

## 6. Interpret Completion and Resume a Session

The final JSON status contains:

- `phase`: `complete` when no command is still running;
- `completed_rounds`: diagnosis rounds executed;
- `stop_reason`: why the loop ended;
- `outputs`: size and SHA-256 of each published result.

Common stop reasons are:

| Stop reason | Meaning |
| --- | --- |
| `max_iterations` | The requested round budget was consumed |
| `all_candidates_invalid` | Legal candidates ran, but none passed both gates |
| `no_legal_candidate` | Topology or policy validation removed every candidate |
| `memory_candidates_exhausted_without_compute_evidence` | Memory actions failed and stage evidence did not justify adding CPU capacity |
| `infrastructure_failure_limit` | Repeated execution or health failures reached the configured limit |
| `invalid_control_evidence` | Baseline or probe evidence violated the measurement contract |
| `no_significant_improvement` | A compatibility stop from an older rolled-back state |

`all_candidates_invalid` does not mean that the session failed. It means that
the current policy found no beneficial candidate from the accepted
configuration. The search may accept equivalent throughput while releasing
physical cores, but only for a count reduction. The historical best pair is
still published.

Inspect a session without contacting either host:

```bash
python3 -m pipetune status --session results/tune-001
```

Continue an interrupted session with:

```bash
python3 -m pipetune resume --session results/tune-001
```

Resume verifies the stored configuration-tool hash, endpoint Git commits,
binaries, configurations, trials, and state history before starting another
trial.

The principal outputs are:

| File | Purpose |
| --- | --- |
| `best.toml` | Historical-best target configuration |
| `peer.toml` | Matching peer configuration and reciprocal routes |
| `report.md` | Human-readable trajectory, gates, and stop reason |
| `iterations.jsonl` | One audit record per diagnosis round |
| `session.json` | Measurement or tuning session index |

Run `best.toml` and `peer.toml` together for final validation. C4 inflight,
C5 MTU, and C6 memory-pool handler remain build-sensitive manual choices in
this version.

## 7. Troubleshooting

**The remote worker cannot be opened.** `deployment.workdir` does not point to
the checked-out Axio repository on that endpoint, or the endpoint uses a
different revision. Confirm that `WORKDIR/pipetune/remote_worker.py` exists.

**SSH waits for input or fails immediately.** Test the exact `host`, `port`,
and `user` from the TOML with batch-compatible key authentication. If
`use_sudo = true`, also verify `sudo -n true`.

**A build fingerprint does not match.** Rebind the endpoint build directory to
the PipeTune TOML and rebuild Axio. Runtime C1-C3 trials reuse that binary;
build-sensitive C4-C6 changes require another build.

**A counter is unavailable.** Read the provider status and raw files in the
trial directory. Common causes are missing `perf` events, missing PCM/MSR
permissions, an absent NUMA socket in PCM output, or incomplete provider data.

**The diagnosis has no legal perturbation.** The active topology is at a
resource boundary or cannot satisfy its count and routing constraints. Inspect
the resource pools and workload groups in the target TOML.

**A tuning session stops after an infrastructure failure.** Read `report.md`
and the relevant endpoint/provider stderr files. Correct the environment, then
use `resume` if the session is resumable; do not reuse its output path with
`bootstrap`.
