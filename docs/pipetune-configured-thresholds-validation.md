# Configured effect thresholds: RoCE validation

Implementation: `40934a0` on `codex/pipetune-unified-search`.
This replaces dispersion-based acceptance in the earlier unified-search record;
the memory/compute search and RoCE datapath are unchanged.

## Decision rules

- Statistics retain samples, median, unit and a configured effect threshold.
  MAD and derived uncertainty are neither calculated nor serialized.
- Relative improvements must exceed the larger baseline/candidate median
  multiplied by the existing configured fraction.
- Counter changes use `miss_rate_percentage_point_floor`.
- Fewer-core throughput preservation permits at most the baseline throughput
  multiplied by `throughput_relative_floor`; dispersion cannot widen that bound.
- An infeasible baseline still prioritizes client latency. Once feasible, the
  objective maximizes server throughput while preserving the latency SLO.
- Raw windows remain available. Old sessions are not resumed across controller
  source changes. No special-case policy was added for a particular trial.

## Prior-artifact replay

The earlier L-App round-2 baseline has P99.9=103.276 us and throughput=35.3435
Mpps. Its 15/15 candidate has P99.9=7.415 us and throughput=41.2125 Mpps.
Replaying both immutable trial artifacts with the new controller gives:

| Quantity | Result |
| --- | ---: |
| Observed latency reduction | 95.861 us |
| Required reduction, 3% | 3.09828 us |
| Objective accepted | true |
| Candidate latency-feasible | true |

The previous rule required 204.198 us and rejected every possible nonnegative
candidate latency from that baseline.

## Verification scope

Three new regressions failed before the change and passed afterward. Focused
checks cover objective (14), expected impact (11), diagnosis (30), reporting
(4), session (27), and convergence (10). Python syntax smoke covers 32 modules.
No complete Python suite was run. Independent read-only review found no
blocking production issue. Both remote endpoints compiled exact SHA `40934a0`.

## Live experiment

Both hosts use the existing isolated checkout:

```text
/home/ubuntu/git_repos/codex/axio-search-cdf44e6
```

RoCE RC, MTU 4096, 128-byte request frames; fixed client 8/8 colocated and
server starting at 16/16 colocated, NUMA budget 16. T-App response payload is
22 bytes; L-App response payload is 86 bytes. Warmup/sample=10/20, at most 20
diagnosis rounds. Initial RX and TX batch/post triples are 16/16/16.

Artifacts: `results/unified-tuning-40934a0/`. Each case retains native
`session.json`, `iterations.jsonl`, `report.md`, `best.toml`, `peer.toml`, and
raw trial artifacts. Input configurations and prior results are not modified.

## Results

| Handler | Frame | Initial Mpps | Best Mpps | Gain | Best C1/C2/C3 | Placement | Best P99.9 us | Rounds |
| --- | ---: | ---: | ---: | ---: | --- | --- | ---: | ---: |
| T-App | 128 B | 45.595 | 45.595 | 0.00% | 16/16/16 | colocated | 5.873 | 2 |
| L-App | 128 B | 34.627 | 42.733 | 23.41% | 14/14/16 | colocated | 7.696 | 4 |

C3=16 means all six batch/post settings are 16. In the tables below, RX C3x2
sets app-RX batch, dispatcher-RX batch and NIC-RX post to 32; all TX settings
remain 16. P99.9 is the median of the client measurement-window P99.9 values.

### T-App: complete trajectory

Both rounds diagnose P1/RX with `dispatcher_rx.stall` as the longest component.

| Round | Measurement | C1/C2 | Mpps | P99.9 us | Decision |
| ---: | --- | --- | ---: | ---: | --- |
| 1 | Baseline | 16/16 | 45.595 | 5.873 | LLC-load 9.038%; already below 40% |
| 1 | RX C3x2 | 16/16 | 44.564 | 6.691 | Expected impact passes; throughput falls; reject |
| 2 | Baseline remeasurement | 16/16 | 45.328 | 6.131 | LLC-load 8.296% |
| 2 | Smaller colocated pair | 15/15 | 43.150 | 6.310 | Throughput falls by 2.178 Mpps; reject |
| 2 | Reuse round-1 RX C3x2 | 16/16 | 44.564 | 6.691 | Expected impact passes; E2E fails again |

No parameter change is accepted. Best remains the first baseline. The count
candidate loses more than the configured 1% throughput-preservation allowance.

### L-App: complete trajectory

All four rounds diagnose P1/RX with `dispatcher_rx.stall` as the longest
component. LLC-load at each baseline is 23.085%, 25.909%, 11.074% and 21.966%.

| Round | Measurement | C1/C2 | Mpps | P99.9 us | Decision |
| ---: | --- | --- | ---: | ---: | --- |
| 1 | Baseline | 16/16 | 34.627 | 102.978 | Initial historical best; latency exceeds SLO |
| 1 | RX C3x2 | 16/16 | 30.113 | 182.211 | Expected impact and E2E fail |
| 2 | Baseline remeasurement | 16/16 | 29.204 | 155.398 | Compare candidates against this round's baseline |
| 2 | Smaller colocated pair | 15/15 | 41.354 | 10.020 | Accept latency improvement; next count 14 |
| 2 | Reuse round-1 RX C3x2 | 16/16 | 30.113 | 182.211 | Expected impact and E2E fail |
| 3 | Baseline remeasurement | 15/15 | 41.223 | 7.039 | Now latency-feasible; compare throughput |
| 3 | Smaller colocated pair | 14/14 | 42.623 | 7.856 | Accept throughput improvement; next count 13 |
| 3 | RX C3x2 | 15/15 | 39.953 | 7.386 | Stall reduction below 5% threshold; E2E also fails |
| 4 | Baseline remeasurement | 14/14 | 42.733 | 7.696 | Updates best measurement of the accepted configuration |
| 4 | Smaller colocated pair | 13/13 | 42.017 | 8.602 | Throughput loses 0.715 Mpps; reject |
| 4 | RX C3x2 | 14/14 | 41.116 | 36.663 | Expected impact and E2E fail |

The decisive live comparisons are:

- 16/16 -> 15/15: latency reduction 145.378 us exceeds the configured
  4.66194 us requirement. The candidate satisfies the 100 us SLO and is accepted.
- 15/15 -> 14/14: throughput gain 1.3995 Mpps exceeds the 0.426225 Mpps
  requirement. Both samples satisfy the SLO; the candidate is accepted.
- 14/14 -> 13/13: throughput loses 0.715 Mpps, more than the 0.427325 Mpps
  allowed for equivalent-throughput core reduction. It is rejected.

Thus the live run, not just offline replay, accepts 15/15 and continues search.
The 42.623 -> 42.733 Mpps change is remeasurement of the same accepted 14/14
configuration, not another parameter-tuning gain. The initial 16/16 baseline
also varies across rounds; the table retains both observations.

### Termination and audit

Both stop with `memory_candidates_exhausted_without_compute_evidence`.
Directional LLC is already below 40%, so the high-LLC midpoint exploration does
not activate. All visited counts exceed eight: same-count split would require
more than the 16-core budget. The final leader remains a stall, not a completion
component, so compute expansion is not entered. This is the existing search
policy, not a claim that either result is globally optimal.

Audit: T-App has four trials/280 endpoint windows; L-App has ten trials/700
endpoint windows. All 980 windows report zero NIC RX completion errors. All
28 endpoint RC connection artifacts preserve per-dispatcher ring quotas. The
client remains 8/8 with identical runtime knobs. Both published best/peer
configurations match the recorded best trials. No Axio processes remain on
either host. A local SSH observation connection disconnected during L-App;
the remote controller continued uninterrupted and was not restarted.

Audit files in the isolated checkout:

- `results/unified-audit-40934a0.jsonl`
- `results/threshold-trace-40934a0.jsonl`

To repeat one case from desktop01 with a fresh output directory:

```bash
cd /home/ubuntu/git_repos/codex/axio-search-cdf44e6
./scripts/artifact_eval/run_e2e.sh \
  --profile smoke \
  --config-dir results/unified-tuning-40934a0/reference-configs \
  --case roce-l-app-req128 \
  --warmup-windows 10 --sample-windows 20 --tuning-rounds 20 \
  --output results/roce-l-app-repeat
```
