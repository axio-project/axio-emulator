# PipeTune Measurement, Diagnosis, and Tuning Controller

PipeTune `measure` runs one bounded Axio experiment and publishes the evidence
used by the offline P1-P4 diagnosis and cold-start tuner. The controller can
run on a workstation with SSH access to both endpoints, or directly on either
endpoint.
Axio, `perf`, and `pcm-pcie` always execute on the configured testbed hosts.

PipeTune `diagnose` reads only published artifacts. It never starts Axio,
contacts an endpoint, or changes C1-C6. PipeTune `bootstrap` and `resume`
compose measurement and diagnosis into bounded, resumable cold-start tuning.

## Build requirements

PipeTune uses Python 3.10 or newer and the standard library. Build the native
configuration tool on the controller before invoking the Python CLI:

```bash
meson setup build-tools -Ddatapath=false
ninja -C build-tools axio-configure
```

Build the Axio client and server binaries on their respective hosts with the
same source revision and the endpoint's own TOML file:

```bash
meson setup build-client -Daxio_config="$PWD/config/client.toml"
python3 toolchain/axio_build.py build-client --target axio

meson setup build-server -Daxio_config="$PWD/config/server.toml"
python3 toolchain/axio_build.py build-server --target axio
```

The default provider paths are `/usr/bin/perf` and `/usr/sbin/pcm-pcie`.
`pcm-pcie` also requires PCM/MSR access. Provider permission or capability
failures are recorded as unavailable metrics and do not fabricate zero rates.

## Configure controller placement

`deployment.transport` describes how the controller reaches each endpoint. It
does not change the datapath fingerprint.

For a workstation controlling both hosts, set both endpoint files to SSH. For
example, the client file can contain:

```toml
[deployment]
role = "client"
transport = "ssh"
host = "rDesktop_01"
ssh_port = 22
ssh_user = "ubuntu"
workdir = "/home/ubuntu/git_repos/codex/axio-emulator-e61"
use_sudo = true
numa_node = 1
```

Use the corresponding server role, host, work directory, and NUMA node in the
server file. PipeTune invokes the system `ssh` client, so normal SSH config,
proxy, key, and host-alias rules continue to apply.

When the controller runs on an endpoint, use `local` for that endpoint and
`ssh` for its peer:

```toml
[deployment]
role = "client"
transport = "local"
host = ""
ssh_port = 0
ssh_user = ""
workdir = "/home/ubuntu/git_repos/codex/axio-emulator-e61"
use_sudo = true
numa_node = 1
```

All network, handler, topology, and knob fields still describe the endpoint
where Axio runs. Validate the pair before measuring:

```bash
build-tools/axio-configure validate-pair TARGET.toml PEER.toml
```

Both configs must enable JSONL metrics and must use matching values for
`other.iterations`, `other.window_seconds`, `tuning.warmup_windows`, and
`tuning.sample_windows`. Iterations must cover warmup plus sample windows.

## Run a measurement

The target is the endpoint whose host counters and later tuning knobs are in
scope. The peer is frozen experiment context. Either role can be the target:

```bash
python3 -m pipetune measure \
  --target-config TARGET.toml \
  --peer-config PEER.toml \
  --output results/session-001 \
  --axio-configure build-tools/axio-configure
```

Use `--target-binary` or `--peer-binary` only when the binary is not at
`WORKDIR/build-ROLE/axio` on that endpoint.

PipeTune always starts the server role before the client role. The client still
generates traffic even when the server is the target. Only the target receives
perf and PCM collection. The source TOMLs are copied into the trial unchanged;
materialized copies differ only in runner-owned metrics output fields. PipeTune
rejects any target or peer knob leakage.

Startup readiness means the managed workload exists. Sampling begins only
after both endpoint JSONL streams contain the configured number of valid,
monotonically increasing warmup windows. `perf` attaches to the actual Axio PID,
including when `sudo` uses a separate monitor process.

## Artifact layout

`--output` is a new session directory and must not already exist:

```text
session-001/
  session.json
  trials/
    trial-.../
      trial.json
      host-metrics.json
      configs/
        canonical/{target,peer}.json
        source/{target,peer}.toml
        materialized/{target,peer}.toml
      endpoints/
        target/{metrics.jsonl,stdout.log,stderr.log}
        peer/{metrics.jsonl,stdout.log,stderr.log}
      providers/
        perf-version.{stdout,stderr}
        perf-{load,store}.{stdout,stderr}
        pcm-version.{stdout,stderr}
        pcm-pcie.{csv,stdout,stderr}
  diagnoses/
    TRIAL_ID.json
```

`session.json` uses `pipetune.session/v1` and indexes typed trial manifests.
`trial.json` records the target, endpoint roles and transports, process results,
Git/binary/config fingerprints, cleanup status, schemas, and artifact hashes.
`host-metrics.json` records four target rates plus provider availability and
the argv/exit status for every provider command. Each Axio `metrics.jsonl`
remains the compact six-stage `axio.metrics/v1` stream.

Human-facing JSON is already pretty printed. It can also be inspected with:

```bash
python3 -m json.tool results/session-001/session.json
python3 -m json.tool \
  results/session-001/trials/TRIAL_ID/host-metrics.json
```

JSONL intentionally stores one compact object per line. Use `jq` when a
pretty view of individual windows is useful:

```bash
jq . results/session-001/trials/TRIAL_ID/endpoints/target/metrics.jsonl
```

## Run offline diagnosis

Diagnose the only trial in a measurement session with:

```bash
python3 -m pipetune diagnose --session results/session-001
```

If a session indexes more than one trial, select one explicitly:

```bash
python3 -m pipetune diagnose \
  --session results/session-001 \
  --trial TRIAL_ID
```

The command prints pretty, sorted `pipetune.diagnosis/v1` and atomically writes
the same bytes to `SESSION/diagnoses/TRIAL_ID.json`. Re-running diagnosis may
replace that derived file but never changes `session.json`, trial manifests,
metrics, provider output, or source configurations. The diagnosis records the
SHA-256 of every input artifact. A completed probe uses
`TRIAL_ID--probe-PROBE_TRIAL_ID.json` so it does not replace the baseline-only
result.

PipeTune removes exactly `tuning.warmup_windows`, summarizes the following
`tuning.sample_windows` independently for Axio, perf, and PCM, and reports
median, median absolute deviation (MAD), and the effective uncertainty. It does
not invent a per-window join among collectors that have no common timestamp.
For ordinary stage, throughput, and latency statistics, uncertainty is
`max(abs(median) * configured_relative_floor, 3 * MAD)`. The leading elapsed
component is dominant only when its gap over the runner-up exceeds both the
leader's and runner-up's uncertainty; otherwise the stage result is ambiguous.

The elapsed-component comparison uses one unit, microseconds per packet:

- Application and dispatcher completion/stall values already use us/packet.
- NIC TX uses `submit_time_per_packet_us`.
- Aggregate NIC RX uses `1 / throughput_mpps`.
- NIC RX completion intervals remain supporting evidence and are not ranked
  against per-packet stages.

The paper-aligned decision order is:

1. A dominant pipeline stall is P1.
2. A dominant NIC elapsed component is P3.
3. A dominant application/dispatcher completion requests a C1 probe.
4. A significantly positive normalized LLC slope after that probe is P2.
5. Otherwise, significant non-conflicting directional DDIO/I/O evidence is P4.

TX uses LLC-store and PCIe-read evidence; RX uses LLC-load and ItoM/write. The
opposite pair is always retained as control evidence: a conflict lowers
confidence and an opposite I/O conflict prevents P4. Missing rates are `null`
with an explicit name in `missing_metrics`; PipeTune never interprets an
unavailable rate as zero.

For the C1 perturbation, PipeTune requests `application_core_count + 1` while
an inactive application workspace exists. At the pool maximum it requests
`-1`, provided the result remains legal for the configured dispatcher count.
The normalized LLC slope is:

```text
(probe_rate_percent - baseline_rate_percent) * probe_direction
```

Its threshold is the maximum of the configured percentage-point floor and
three MADs from either side. This normalization makes both the normal `+1`
probe and the maximum-core `-1` probe answer the same question: does LLC miss
rate rise as C1 rises?

Measure the requested candidate as another immutable session, then bind it to
the baseline diagnosis:

```bash
python3 -m pipetune diagnose \
  --session results/baseline \
  --probe-session results/c1-probe
```

Use `--probe-trial` when the probe session contains multiple trials. A probe is
accepted only when the baseline result is `probe_required`, the candidate is
the exact requested C1 value, both endpoints keep the same binaries and
immutable settings, and only the target C1 plus its derived application mapping
changed. Completed results record `completed_probe`; `required_probe` becomes
`null`, so the automatic tuner cannot execute the same probe twice.

Diagnosis returns `inconclusive` instead of guessing when stages overlap within
uncertainty, no legal C1 perturbation exists, required counter data is missing,
or evidence is too weak/conflicting. A failed peer-health gate is reported as
`peer_unhealthy` and is never used as target evidence.

## Run automatic cold-start tuning

Start from a reviewed target/peer pair. The target owns the C1-C3 search; the
peer remains frozen except for reciprocal `remote_dispatchers` updates when
target C2 changes. Use the largest application/dispatcher pool that you are
willing to allocate, preferably C1=C2 so the normal search begins without
dispatcher sharing:

```bash
python3 -m pipetune bootstrap \
  --target-config TARGET.toml \
  --peer-config PEER.toml \
  --max-iterations 4 \
  --output results/tune-001 \
  --axio-configure build-tools/axio-configure
```

The same command works from a workstation using SSH+SSH and from an endpoint
using local+SSH. Only `deployment.transport`, `host`, `ssh_user`, `ssh_port`,
and `workdir` differ; Axio still runs on the endpoint described by each TOML.
`--max-iterations` counts completed diagnosis rounds, not candidate trials, and
cannot exceed `tuning.max_iterations` in the input pair.

Bootstrap performs controller/config and endpoint Git/binary/build identity
checks, publishes an immutable initial config pair, and then runs the only
first-round baseline. It does not hide an extra uncounted measurement before
the loop.

### A diagnosis is only a hypothesis

For every round, PipeTune measures the accepted pair, diagnoses P1-P4, performs
the required C1 perturbation when necessary, and materializes each legal action
as a separate cold-start trial. A candidate is accepted only when both gates
pass beyond the uncertainty of both trials:

1. **Expected tuning impact:** P1 requires the diagnosed stall to fall; P2/P4
   require the direction-linked LLC miss rate to fall; P3 requires the
   direction-linked I/O rate to fall.
2. **End-to-end objective:** while client P99.9 violates the latency SLO, it
   must improve significantly. Once feasible, server throughput must improve
   significantly without violating the latency SLO, except that a count
   reduction may preserve equivalent throughput when it releases physical
   cores.

Missing expected-impact evidence is not zero and cannot accept a candidate. A
candidate that fails either gate is rolled back; PipeTune continues with the
remaining actions. If all executed candidates fail both gates,
`all_candidates_invalid` means the current search policy cannot improve the
accepted parameters. When memory candidates fail and no compute evidence
exists, PipeTune instead reports
`memory_candidates_exhausted_without_compute_evidence`. Either result publishes
the historical best pair. Latency feasibility alone never ends a round.

Each trial changes exactly one logical action. Actions are tried in this
deterministic order and invalid configurations are filtered before execution:

| Diagnosis | Ordered candidate actions |
| --- | --- |
| `probe_required` | set C1 to the exact requested perturbation value; this is evidence, not an accepted tuning action |
| P1 | C1 - 1; then double the direction-linked C3 triple |
| P2 | C1 - 1 |
| P3 | C2 - 1 |
| P4 | C2 - 1; then halve the direction-linked C3 triple |

The TX C3 triple is application TX batch, dispatcher TX batch, and NIC TX post
size; the RX triple is the corresponding three RX values. C2 materialization
also updates only the peer's reciprocal routes. The lock-averse filter is
applied before the candidate set is published.

### Topology-aware memory and compute search

The target topology supplies a hard NUMA workspace budget `U`: the number of
configured workspace/core entries available on `deployment.numa_node`.
PipeTune derives the active application count `A`, dispatcher count `D`, role
overlap `O`, and physical-core use `P` from workload groups. Every candidate
must keep `P <= U`; pool length alone does not activate a workspace.

The search begins in the memory phase. It evaluates every legal P1-P4 C1/C2/C3
sibling from one immutable accepted anchor. A count reduction normally needs a
significant end-to-end gain, but may also pass with equivalent throughput while
releasing physical cores. C3 does not release a core and therefore still needs
a significant throughput gain once the accepted baseline is latency feasible;
before feasibility, it must instead significantly reduce client P99.9. Exact
objective ties prefer the candidate with fewer physical cores.

Rejecting one memory candidate is not enough to add CPU capacity. PipeTune
enters the compute phase only after every diagnosis-relevant memory signal is
exhausted and the measured longest stage provides positive application or
dispatcher completion-time evidence. NIC or stall dominance does not trigger
compute expansion.

For an application bottleneck, the legal alternatives are a one-to-one split,
a direct boundary split when a full split exceeds `U`, and one complete
balanced application fanout layer. For example, with `U = 16`, PipeTune may
compare `16A/16D` colocated directly with `8A/8D` split and `16A/8D` balanced
fanout. The direct boundary candidate avoids accepting worse intermediate
counts merely to reach the split.

For a dispatcher bottleneck, dispatcher expansion remains one-to-one: split,
boundary-split, paired C1/C2 growth, and a dispatcher-direction C3 increase are
eligible when they fit. C1-fixed/C2-growth is never generated because it would
introduce dispatcher fan-in to one application.

Compute candidates must significantly reduce the diagnosed completion metric
and improve the end-to-end objective. If the accepted baseline violates the
latency SLO, any candidate must significantly reduce client P99.9; it need not
reach feasibility in one step. Once the baseline is latency feasible, a
compute candidate must remain feasible and significantly improve throughput.
The equivalent-throughput/fewer-core exception applies only to count reduction,
not compute expansion. A fanout may therefore be accepted despite extra
sharing only when it passes the applicable objective and reduces the measured
compute bottleneck. An accepted compute candidate returns the next round to
memory diagnosis. Failed trials never replace `best.toml`; they remain
immutable evidence, and a canonical target/peer pair is never cold-started
twice in one session.

### Convergence and stop reasons

Every terminal result publishes the verified historical-best target/peer pair,
including when the final attempted pair was rejected. `report.md` records one
of these stop reasons:

- `no_legal_candidate`: validation or the lock-averse policy removed every
  action.
- `all_candidates_invalid`: legal actions ran, but none passed both gates.
- `no_significant_improvement`: a legacy rolled-back boundary was restored;
  new dual-gate rounds normally use `all_candidates_invalid`.
- `max_iterations`: the configured diagnosis-round budget was consumed.
- `infrastructure_failure_limit`: consecutive execution or health failures
  exhausted the retry budget.
- `invalid_control_evidence`: baseline/probe evidence violated its structural
  contract, so tuning failed closed.
- `memory_candidates_exhausted_without_compute_evidence`: memory actions did
  not yield an acceptable result and the stage evidence does not justify a
  compute expansion.

### Inspect and resume

Status verifies the immutable state chain and prints a readable JSON summary.
It performs no cleanup, SSH, process control, fingerprint probing, or output
publication:

```bash
python3 -m pipetune status --session results/tune-001
```

Resume verifies the stored controller `axio-configure` SHA-256, both endpoint
Git/binary/build identities, all state/config/trial hashes, and the active
cursor before continuing:

```bash
python3 -m pipetune resume \
  --session results/tune-001 \
  --axio-configure build-tools/axio-configure
```

Interrupted or unhealthy baseline/probe control observations retry within the
configured infrastructure budget without consuming a diagnosis round.
Structurally invalid control evidence fails closed. Repeated resume of a
completed session is byte- and mtime-idempotent.

### Tuning outputs

```text
tune-001/
  session.json
  inputs/{target,peer}.toml
  configs/
    accepted-0000/{target,peer}.toml
    round-.../{probe,candidates}/...
  state/generation-........json
  trials/TRIAL_ID/...
  best.toml
  peer.toml
  iterations.jsonl
  report.md
```

`state/` is an immutable, previous-hash-linked history. `iterations.jsonl`
contains one compact audit record per diagnosis round. `report.md` first
summarizes the memory/compute policy and physical budget, then separates the
accepted trajectory from probes and rejected candidates. Each round links the
baseline, required perturbation, candidate trial manifests, persisted
diagnosis, all four rates, expected-impact comparison, end-to-end comparison,
accept/rollback result, stop reason, and remaining C4-C6 suggestions.

`best.toml` and `peer.toml` are the paired historical best and are the files to
run for the final validation. C4 inflight, C5 MTU, and C6 memory-pool handler
remain build-sensitive manual follow-up choices in this version.

## Failure and cleanup behavior

Every endpoint and provider is launched through a foreground, session-scoped
worker. Cleanup signals only the recorded private process group and removes
only that trial's contained remote scratch directory; PipeTune never uses a
global `pkill`. Cleanup is idempotent.

The local session directory is published atomically only after both endpoint
processes exit successfully, all artifacts validate, remote cleanup completes,
and `session.json` is written. A failed measurement leaves no authoritative
output directory. Raw provider output is retained for successful trials even
when a provider is unavailable. A provider command that succeeds but loses its
declared output artifact is treated as an infrastructure failure.

Do not run two measurements or bootstraps with the same `--output`. Use a fresh
directory for each independent measurement or tuning session. Continue an
existing tuning directory only with `resume`; inspect it with `status`.
