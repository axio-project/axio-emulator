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
`key_value`) on both DPDK and RoCE. The target starts fully colocated at C1=16
and C2=16. The peer remains fixed at C1=8 and C2=8 except for reciprocal route
updates. Smoke sessions stop after at most two tuning rounds; paper sessions
stop on convergence or after twenty rounds.

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
handler as a file-transfer path. Both start from a 16/16 target and an 8/8
peer. The output lists the diagnosis, candidate, configuration, outcome,
throughput, P99.9 latency, elapsed time, and final historical best for every
round.

This is explicitly an Axio adaptation. It evaluates the current script-based
bootstrap workflow and does not claim to reproduce the paper's unavailable
OvS/LineFS probe-event versus direct-measurement comparison.

## Failure boundary

Missing required metrics, packet drops, NIC completion errors, invalid build or
configuration identities, unavailable required counter providers, and failed
endpoint lifecycle operations make a command fail. A numeric trend that differs
from the paper remains valid evidence and is printed without a PASS/WARN label.
