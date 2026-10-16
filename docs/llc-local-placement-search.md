# LLC-guided count and placement search

Historical design and experiment record. The independent paired/local policy
below is superseded by [the unified search revision](pipetune-unified-search-revision.md).
Its measurements remain historical evidence, not the current search contract.

Implementation baseline: `68683b0`.

1. Keep paired C1/C2 reduction while preserving one-to-one colocated groups.
   If a one-count reduction does not improve the objective and directional LLC
   miss remains above 40%, seek a smaller count with midpoint steps.
2. The first sample with directional LLC miss at or below 40% ends the coarse
   search. I/O miss remains reported but does not control this boundary.
3. At that count, compare the measured colocated configuration with a same-count
   split configuration, when twice the count fits the NUMA workspace budget.
4. At each following count N-1, evaluate both colocated and legal split layouts
   before deciding whether to continue. The objective gate selects a significant
   improvement or an equivalent result using fewer actual workspaces. These
   local trials do not require another significant LLC/stall reduction.
5. When neither layout improves the objective or saves resources at equivalent
   performance, retain the previous local configuration and enter compute
   expansion. Do not substitute the global historical best for this local seed.
6. Historical best remains independent of the exploratory accepted cursor and
   is the configuration written to `best.toml`.

The search is budgeted and local; a single rejected count does not prove global
optimality. RoCE RC candidates must still satisfy the existing peer-QP mapping
constraints. The client remains fixed at 8/8 except for reciprocal routes.

## Execution record

- Isolated branch: `codex/llc-local-placement-search`.
- User-approved scope: Python search only; no datapath or paper-data changes.
- Verification: lightweight syntax/state checks, branch review, then remote
  DPDK/RoCE x T-App/L-App 128B tuning with 10/20 windows. No full Python suite.
- Ruling: retain the existing inclusive 40% boundary; stop binary refinement at
  its first relieved sample. Same-count split is a placement comparison, not a
  claim that aggregate stall must fall.
- New runs may resume local-search checkpoints. Old-code sessions are not
  migrated: their source identity differs and their binary-refinement state
  belongs to the earlier search policy.
- Review fix: a split local seed may try the existing colocated-fanout profile
  for an application bottleneck. This explicit placement change is still
  checked by both the completion-time gate and the E2E gate.
- Local evidence: syntax compilation and 15 focused paired-state/controller
  checks passed; no full test suite was run.
- The 12 search-policy checks also passed. Review identified and resolved the
  split-seed compute gap described above; no additional important new-session
  findings remained.

## Desktop validation, 2026-10-10

Code commit: `512be38b0ae2b165b3a1fa93969fefd636f52d7f` on both hosts.
Repository: `/home/ubuntu/git_repos/codex/axio-search-cdf44e6`.
The user's main repository and endpoint configurations were not modified.

All four cases built their target and peer binaries successfully. Measurements
used 128-byte request frames, a fixed 8/8 client, 10 warmup windows, 20 sample
windows, and a maximum of 20 rounds. T-App uses 86/22-byte request/response
payloads; L-App uses 86/86. DPDK starts at 16/16 colocated; RoCE RC starts at
16 apps / 8 dispatchers under the existing fixed-peer QP constraint.

| Backend | Handler | Baseline Mpps | Best Mpps | Improvement | Best C1/C2 | Placement | Best P99.9 us | Rounds |
| --- | --- | ---: | ---: | ---: | --- | --- | ---: | ---: |
| DPDK | T-App | 41.69 | 67.42 | +61.70% | 6/6 | colocated | 11.00 | 8 |
| DPDK | L-App | 40.85 | 53.99 | +32.15% | 7/7 | colocated | 26.79 | 7 |
| RoCE | T-App | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | 0 |
| RoCE | L-App | unavailable | unavailable | unavailable | unavailable | unavailable | unavailable | 0 |

C3 remained `(app_rx, app_tx, dispatcher_rx, dispatcher_tx, nic_rx, nic_tx)` =
`(64, 32, 32, 32, 128, 32)` in both DPDK best configurations.
The client retained `(16, 16, 16, 16, 32, 16)`.

### DPDK search paths

L-App:

- 16/16 -> 15/15 improves throughput; 14/14 does not, so seek 7/7.
- 7/7 reaches LLC-load miss **15.686%** while I/O-write miss remains **84.013%**.
  The controller enters local search without requiring I/O relief.
- 7/7 split: 45.24 Mpps versus 53.99 Mpps colocated; reject split.
- 6/6 colocated: 49.60 Mpps versus the fresh 7/7 baseline of 50.59 Mpps;
  accept `equivalent_fewer_cores` under the existing noise gate. The 6/6 split
  trial reaches 41.91 Mpps and is rejected.
- 5/5 colocated/split reach 44.60/34.88 Mpps; reject both and enter compute
  from the accepted 6/6 cursor, not the historical best or initial 16/16.
- 12/6 app-fanout reaches 31.03 Mpps and fails both gates. Stop with
  `all_candidates_invalid`; `best.toml` remains the historical 7/7 configuration.

T-App:

- 16/16 -> 15/15 improves throughput; 14/14 does not, so seek 7/7.
- 7/7 reaches LLC-load miss **10.917%** while I/O-write miss remains **62.255%**.
- 7/7 split: 51.06 Mpps versus 62.63 Mpps colocated; reject split.
- 6/6 colocated: 67.42 Mpps versus the fresh 7/7 baseline of 63.79 Mpps;
  accept. The 6/6 split reaches 41.98 Mpps and is rejected.
- 5/5 colocated: 58.23 Mpps versus a fresh 6/6 baseline of 53.44 Mpps;
  accept as the local cursor. Its split trial reaches 37.12 Mpps and is rejected.
  The historical 6/6 best is not overwritten.
- 4/4 colocated/split reach 47.59/41.32 Mpps; reject both and enter compute
  from 5/5. The 10/5 app-fanout trial reaches 33.09 Mpps and fails both gates.
- Stop with `all_candidates_invalid`; `best.toml` remains 6/6 colocated.

Cold-start variability is material: T-App 14/14 measured 34.86 and 49.05 Mpps,
and 6/6 measured 67.42 and 53.44 Mpps in separate trials. These are single-session
historical-best results, not a claim of repeatable global optimality.

The 22 materialized DPDK peer configurations were checked and all retain C1=C2=8.
Every persisted paired-search record passed `PairedSearchState.from_document()`.
Published `best.toml` and `peer.toml` SHA-256 values match the historical-best
configuration references in the final session state. A live interrupted-resume
experiment was not added to this run.

### RoCE runtime blocker

Both RoCE cases built successfully but exhausted the two-attempt infrastructure
limit before completing baseline warmup. They have no valid performance result.

A bounded manual L-App run reproduced the client abort:

```text
Failed to open device 1
openat(..., "/dev/infiniband/uverbs1", O_RDWR|O_CLOEXEC) = 4
ioctl(4, RDMA_VERBS_IOCTL, ...) = -1 ENOSPC (No space left on device)
```

The initial `ENOSPC` observation did not identify the root cause. A later trace
including `close()` showed that initialization continued after that ioctl.
`TcpClient::disconnect()` closed TCP descriptor 6, another workspace reused 6
for `/dev/infiniband/uverbs1`, and the first client's destructor closed 6 again.
The other workspace then received `EBADF` and aborted inside `ibv_open_device()`.
This is an in-process descriptor-reuse race, not evidence of RDMA exhaustion.
The old `426d5d8` code contains the same double close.

The targeted fix makes `TcpClient::disconnect()` idempotent and reuses it from
the destructor. No lock or packet-processing change is required. A regression
check reuses the closed socket descriptor and verifies that both a repeated
disconnect and destruction leave the replacement descriptor open.

The single-connection `ib_write_bw` preflight succeeded (~125.62 Gb/s), and
`rocep152s0f1` reports active MTU 4096 on the client. No NIC reset, driver reload,
or MTU workaround was attempted.

Diagnostic files on desktop01:

- `/tmp/axio-llc-roce-client.log`
- `/tmp/axio-llc-roce-open.log`
- `/tmp/axio-llc-roce-open.trace` (sudo is required to read the trace)
- `/tmp/axio-roce-retry-close-01.trace` (the descriptor-reuse evidence)

The bounded manual server process was stopped after diagnosis. No Axio,
artifact-evaluation, PCM, or ib_write_bw processes remained at final inspection.
RoCE performance validation remains pending resolution of this runtime blocker.

### Socket fix validation

Commit `aa171e8` fixes the repeated close. The descriptor-reuse regression check
failed against the old header and passed against the fix on both hosts. Both
L-App and T-App endpoint binaries rebuilt successfully at this exact commit.
A manual L-App run initialized all eight RoCE dispatchers on each host without
the device-open failure. It subsequently reported `management connection failed`;
both AE reruns also exited before completing warmup. A short traced manual run
connected to the start-synchronization port 20102 successfully, so the remaining
startup failure is timing-dependent and has not been fixed in this commit.
No complete performance result is claimed for these runs.

Evidence on desktop01: `/tmp/axio-roce-close-fix-aa171e8.log`,
`/tmp/axio-close-fix-manual-client-aa171e8.log`, and
`/tmp/axio-close-fix-connect-aa171e8.trace`. AE result roots are
`results/ae/e2e-roce-{l,t}-app-req128-aa171e8/`. All diagnostic processes were
stopped; no full Python suite, NIC reset, or network configuration change was used.

### Commands and artifacts

Run from desktop01 in the repository above, replacing `CASE` with one of
`dpdk-t-app-req128`, `dpdk-l-app-req128`, `roce-t-app-req128`, or
`roce-l-app-req128`. Use a new output directory for a fresh run.

```bash
./scripts/artifact_eval/run_e2e.sh \
  --profile smoke --case CASE \
  --warmup-windows 10 --sample-windows 20 --tuning-rounds 20 \
  --config-dir results/validation-e2e-full-68683b0/inputs/reference-configs \
  --output results/ae/e2e-CASE-512be38
```

Result roots: `results/ae/e2e-<case>-512be38/`.
Completed DPDK cases provide `summary.md`, `summary.csv`, and the native
`cases/<case>/session-01/{report.md,iterations.jsonl,best.toml,peer.toml}`.
RoCE roots contain build evidence and failure session states, not completed
performance summaries.

Execution logs:

- `/tmp/axio-llc-local-512be38.log` (DPDK cases and the RoCE L-App attempt)
- `/tmp/axio-llc-roce-tapp-512be38.log` (the separate RoCE T-App attempt)
