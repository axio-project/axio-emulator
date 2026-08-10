# Axio and PipeTune Artifact Evaluation

This document specifies the command-line experiments shipped for artifact
evaluation. The scripts publish numeric evidence; they do not draw figures or
classify an observed trend as matching the paper.

## Execution model

The reference controller runs on `rDesktop_01`. The client is a local,
eight-core colocated load generator, and the server is a remote tuning target
on `rDesktop_02`. Both endpoints use logical core positions in NUMA node 1.
The target topology is materialized from a 16-core resource pool.

All topology changes use a colocated-first policy. For C1 application cores
and C2 dispatcher queues, `min(C1, C2)` workspaces carry both roles. Remaining
application workspaces are app-only. The supported experiment matrices never
use C2 greater than C1.

C3 denotes one common value applied to application RX/TX batch size,
dispatcher RX/TX batch size, and NIC RX/TX post size.

The scripts accept `--profile smoke|paper`, `--output`, `--resume`, and
`--dry-run`. Smoke runs use two warmup and three sample windows. Paper runs use
ten warmup and twenty sample windows. A result directory is immutable except
when an explicitly resumed session verifies its manifest, source identity,
configuration fingerprints, binaries, and completed trial artifacts.

## Experiment entry points

The public entry points are:

```text
artifact-eval/run_e2e.sh
artifact-eval/run_figure3.sh
artifact-eval/run_figure6.sh
artifact-eval/run_figure7.sh
artifact-eval/run_figure8.sh
artifact-eval/run_figure14.sh
artifact-eval/run_all.sh
```

Each command writes a human-readable `summary.md`, a tabular `summary.csv`, an
experiment `manifest.json`, generated endpoint configurations, build evidence,
and the unmodified trial artifacts produced by Axio, perf, PCM, and PipeTune.

## End-to-end tuning

The end-to-end experiment runs twelve independent PipeTune bootstrap sessions:
six message handlers (`t_app`, `l_app`, `m_app`, `file_write`, `file_read`, and
`key_value`) on both DPDK and RoCE. DPDK starts from a fully colocated C1=16,
C2=16 target. RoCE RC requires one peer QP per target dispatcher, so it starts
from C1=16, C2=8 while the peer remains fixed at C1=8, C2=8. This uses the
target's full 16-core NUMA budget without creating unmatched RC QPs. Smoke
sessions stop after at most two tuning rounds; paper sessions stop on
convergence or after twenty rounds.

The top-level table reports baseline and historical-best throughput, relative
improvement, baseline and best C1/C2/C3, client P99.9 latency, completed rounds,
and the PipeTune stop reason. Per-case reports preserve every diagnosis,
candidate, expected-impact gate, objective gate, acceptance or rollback, and
elapsed time. The last exploratory cursor never replaces the historical best.

## Paper-aligned sweeps

All sweeps use DPDK on the 200 Gbps reference testbed and keep the eight-core
peer fixed.

- Figure 3 uses the 128-byte L-App echo workload. It evaluates C1 values
  4/8/12/16 at C2=4 and C3=32; C2 values 4/8/12/16 at C1=16 and C3=32; and C3
  values 16/32/64/128 at C1=8 and C2=4. Paper mode repeats every point twenty
  times.
- Figure 6 evaluates L-App and M-App at C1 values 1/2/4/8/16, C2=1, and C3=16.
  L-App reports application-TX allocation stall distributions. M-App reports
  application-RX completion distributions and LLC-store miss rate.
- Figure 7 evaluates L-App and T-App at C1=C2 values 1/2/4/8/16 and C3=128.
  Both report application-RX completion distributions. L-App adds LLC-store
  and ItoM/write miss rates; T-App adds LLC-load and ItoM/write miss rates.
- Figure 8 evaluates T-App at C1/C2=4/4 and C3 values 32/64/128/256/512, and
  L-App at C1/C2=8/1 and C3 values 16/32/64/128/256. T-App reports completion,
  LLC-load, and ItoM/write data. L-App reports allocation stalls.

Figures 6--8 publish true P1, P50, and P99 batch percentiles. The historical
plot sources label minimum, average, and maximum values as percentiles; those
paper-repository files are not modified. Mean, minimum, and maximum are kept in
the artifact output so the distinction remains auditable.

## Figure 14 adaptation

The Figure 14 command runs two Axio bootstrap trajectories: a DPDK
dispatcher-level packet echo as a switch-like path, and a RoCE `file_write`
handler as a file-transfer path. The DPDK path starts from a 16/16 target; the
RoCE RC path starts from a 16/8 target so its dispatcher count matches the 8/8
peer's QP count. The output lists the diagnosis, candidate, configuration,
outcome, throughput, P99.9 latency, elapsed time, and final historical best for
every round.

This is explicitly an Axio adaptation. It evaluates the current script-based
bootstrap workflow and does not claim to reproduce the paper's unavailable
OvS/LineFS probe-event versus direct-measurement comparison.

## Failure boundary

Missing required metrics, packet drops, NIC completion errors, invalid build or
configuration identities, unavailable required counter providers, and failed
endpoint lifecycle operations make a command fail. A numeric trend that differs
from the paper remains valid evidence and is printed without a PASS/WARN label.

## Running the artifact

Run the controller from `rDesktop_01`, where the client transport is local and
the server is reached through the SSH settings in
`config/artifact/reference-200g/`. Passwordless SSH and `sudo -n` must work on
both endpoints. On another testbed, edit only the four reference files'
`[deployment]` and `[network]` sections before starting.

List an experiment without touching the testbed:

```bash
./artifact-eval/run_e2e.sh \
  --profile smoke \
  --output results/ae/e2e \
  --dry-run
```

Execute one experiment or the complete smoke suite:

```bash
./artifact-eval/run_figure3.sh \
  --profile smoke \
  --output results/ae/figure3

./artifact-eval/run_all.sh \
  --profile smoke \
  --output results/ae/all-smoke
```

`smoke` uses two warmup and three sample windows, one repeat per sweep point,
and at most two tuning rounds. `paper` uses ten warmup and twenty sample
windows, 20 repeats for Figure 3, five repeats for Figures 6--8, up to twenty
E2E rounds, and up to five Figure 14 rounds. Command-line overrides include
`--warmup-windows`, `--sample-windows`, `--repeats`, `--tuning-rounds`, and
`--sessions`. Use `--case CASE_ID` to run one matrix entry, for example:

```bash
./artifact-eval/run_e2e.sh \
  --profile paper \
  --case dpdk-t-app \
  --output results/ae/e2e-dpdk-t-app
```

Resume only with the same Git SHA, matrix, profile, and verified artifacts:

```bash
./artifact-eval/run_e2e.sh \
  --profile smoke \
  --resume results/ae/e2e
```

The harness builds `axio-configure` when needed, validates the pair, runs the
testbed preflight, and caches each Axio binary under
`build-ae/<build-fingerprint>`. It never uses a global `pkill`. Each result
contains `manifest.json`, `builds.json`, generated configurations, immutable
PipeTune trials, `summary.csv`, and `summary.md`.

For allocation-stall experiments, the client is the target because it owns the
app-TX stage. For handler-completion experiments, the server is the target
because it owns app-RX. Figure 3 and all E2E sessions tune the server. The
`file_read` E2E baseline uses C3=16. This is large enough to establish the
DPDK pipeline while avoiding the artificial 1,632-packet response burst caused
by C3=32 at each application workspace. It retains the required 16/16 starting
topology on DPDK. RoCE file workloads start at C3=16 to bound packetized buffer
demand. The other RoCE E2E cases start at C3=64, which produces a usable
diagnostic perturbation with the 16/8 RC topology.

Figure 14 is an Axio adaptation. Its output must not be presented as a
reproduction of the unavailable OvS/LineFS probe-event comparison. None of the
commands reads or modifies the paper repository's historical data.
