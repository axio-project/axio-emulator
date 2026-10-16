# Unified search and asymmetric RC: verification record

## Scope and source

Implementation branch: `codex/pipetune-unified-search`; baseline: `26b7dc7`.
DPDK tuning and the symmetric performance comparison use `4459b5a`; RoCE tuning
uses `02a6926`. Earlier RC functional runs use `718172b`. These revisions share
the same RC I/O implementation; the final changes concern controller recovery
and rebuilding the AE configuration tool.

Both endpoints use the existing isolated checkout:

```text
/home/ubuntu/git_repos/codex/axio-search-cdf44e6
```

The controller is desktop01; desktop02 is reached through its `Desktop_02` SSH
alias. No user checkout, user configuration, or paper data was modified.

## Review map

| Change | Commits |
| --- | --- |
| Approved specification and supersession | `468fcfe` |
| Unified policy; pending hypotheses; legal search bounds | `983f876`, `febf579`, `4459b5a` |
| RC resource ownership and coordinated startup | `d6d8a93`, `9f3ff07` |
| Multi-QP I/O, exact completion ownership and response routing | `60ae631` |
| Full-budget AE start and connection audit artifacts | `71c265e`, `aad20e3`, `718172b` |
| Source-bound resume, historical inspection and probe reuse | `cc34f3a` |
| Incrementally refresh AE's configuration tool | `02a6926` |

## RC functional acceptance

L-App, 86-byte request/response payloads, MTU 4096, fixed client 8/8; two warmup
and three sample windows. These short runs verify functionality, not a tuning
ranking. Each case completed ten windows per endpoint with zero RX completion
errors, including repeated reuse of the bounded RX/TX slots.

| Server layout | QP pairs | Response buffer | Target Mpps |
| --- | ---: | --- | ---: |
| 16/16 colocated | 16 | In place | 35.51 |
| 15/15 colocated | 22 | In place | 42.970 |
| 8/8 colocated | 8 | In place | 42.367 |
| 7/7 colocated | 14 | In place | 36.475 |
| 1/1 colocated | 8 | In place | 0.984 |
| 7/7 split | 14 | In place | 35.716 |
| 15/15 colocated | 22 | Newly allocated | 43.144 |

Artifacts: `results/unified-718172b/`. Each trial includes per-endpoint
`rc-connections.json`, with actual workspace IDs, weights and RX/TX slot quotas.
The split run uses dispatcher IDs distinct from application IDs. The
standalone C++ graph check also covers nonzero client IDs and equal aggregate
weights at 16, 15, 8, 7 and 1 server dispatchers.

Configuration checks reject a 16 KiB request on the 16↔8 graph with the explicit
multi-QP single-packet error. The same request remains valid on the symmetric
4↔4 graph. This check does not claim multi-QP large-message support.

## Symmetric single-QP performance regression

Four colocated cores per endpoint, T-App, 86-byte request and 22-byte response,
RoCE RC, MTU 4096, identical runtime knobs, ten warmup and twenty sample windows.
Each result is a separate cold start; the reported median is over three runs.

| Revision | Run 1 Mpps | Run 2 Mpps | Run 3 Mpps | Median Mpps |
| --- | ---: | ---: | ---: | ---: |
| `26b7dc7` | 27.6775 | 27.6750 | 27.6700 | 27.6750 |
| `4459b5a` | 27.1270 | 27.0355 | 27.4220 | 27.1270 |

Median change: **−1.98%**, within the specified 5% regression threshold.
Artifacts: `results/unified-regression-4459b5a/`. The first attempt at the new
revision's third run lost its SSH connection before publication; only its
successfully published replacement is included above.

## Four-case tuning acceptance

DPDK/RoCE × T-App/L-App, 128-byte request frames, server 16/16 colocated,
fixed client 8/8, ten warmup and twenty sample windows, at most twenty rounds.
Reference C3 values are preserved; each backend uses the same search code.

Artifacts: `results/unified-tuning-4459b5a/` for DPDK and
`results/unified-tuning-02a6926/` for RoCE. Each case retains native session state,
trial manifests, candidate configurations, iterations and its report.

To repeat one case from desktop01, use a new output directory:

```bash
cd /home/ubuntu/git_repos/codex/axio-search-cdf44e6
./scripts/artifact_eval/run_e2e.sh \
  --profile smoke \
  --config-dir results/unified-tuning-4459b5a/reference-configs \
  --case dpdk-t-app-req128 \
  --warmup-windows 10 --sample-windows 20 --tuning-rounds 20 \
  --output results/unified-repeat-dpdk-t-app
```

| Backend | Handler | Baseline Mpps | Best Mpps | Gain | Best layout | RX C3 | TX C3 | P99.9 µs | Rounds |
| --- | --- | ---: | ---: | ---: | --- | --- | --- | ---: | ---: |
| DPDK | T-App | 43.43 | 68.73 | +58.26% | 7/7 colocated | 32/16/64 | 32/32/32 | 7.63 | 7 |
| DPDK | L-App | 45.07 | 53.00 | +17.58% | 7/7 colocated | 64/32/128 | 32/32/32 | 27.48 | 6 |
| RoCE | T-App | 45.99 | 45.99 | +0.00% | 16/16 colocated | 16/16/16 | 16/16/16 | 6.35 | 2 |
| RoCE | L-App | 24.40 | 35.34 | +44.84%* | 16/16 colocated | 16/16/16 | 16/16/16 | 103.28 | 2 |

C3 triples are application batch / dispatcher batch / NIC post size, by direction.
*RoCE L-App's published difference comes from a repeated baseline measurement
of the same configuration, not an accepted tuning change. Its published best
still exceeds the 100 µs latency SLO. See the limitation below.

The final audit covers 53 completed cold-start trials and 3,710 endpoint windows.
Every peer retains C1/C2=8/8 and identical runtime knobs; each target starts at
16/16. All NIC RX completion-error counters are zero. Each published best/peer
pair matches the canonical configuration of its recorded historical-best trial.
All sixteen RC connection artifacts preserve the per-dispatcher RX/TX ring
budgets instead of multiplying them by the number of QPs. Audit output is saved
as `results/unified-audit-{dpdk,roce}-02a6926.jsonl` in the isolated checkout.

DPDK T-App trace:

- Round 1 evaluates both the protected count reduction and the P4 RX-batch
  reduction, then selects the latter: 53.02 versus the initial 43.43 Mpps.
- Round 2 finds no ordinary winner while LLC-load remains 81.70%. The 15/15
  exploratory cursor advances despite lower E2E throughput; historical best
  remains separate.
- Round 3 uses the midpoint 8/8 and retains the diagnosis-generated C3 candidate.
- At round 4, the 8/8 cursor has 34.22% LLC-load but 60.56% I/O-write miss.
  The LLC boundary is reached independently of I/O; same-count split improves
  throughput from 53.86 to 58.60 Mpps.
- Round 5 compares 7/7 colocated (68.73 Mpps) and split (46.41 Mpps), selecting
  colocated. Round 6 evaluates 6/6 in both layouts and preserves the 7/7 result
  as historical best. Its colocated candidate passes equivalent-fewer-cores,
  not a significant-throughput-improvement claim.
- Round 7 rejects both 5/5 layouts. The C3 candidate passes its causal gate but
  fails E2E; this does not block compute. The controller reuses the previously
  measured 6/6 split and tests a full application layer at 12/6 (34.37 Mpps).
  Both compute candidates fail; the session stops after seven rounds. Final
  cursor is 6/6, whereas `best.toml` remains 7/7 at 68.73 Mpps.

DPDK L-App follows the same policy: initial LLC-load is 84.95%; the first count
reduction advances an exploratory cursor, then the midpoint reaches 8/8. The
boundary observation is 38.87%. A subsequent cursor measurement at 41.05%
re-enables memory exploration instead of borrowing relief from the earlier
trial. The search reaches 7/7 and preserves its 53.00 Mpps result. Both 6/6
layouts and the application-layer compute extension fail E2E; the final compute
extension is 32.90 Mpps. The session ends after six rounds.

RoCE T-App starts at 16/16 with 45.99 Mpps and 7.92% LLC-load. Diagnosis is P1;
the RX C3 increase improves its causal metric but not E2E. The next round tests
15/15 (43.16 Mpps) and reuses the prior C3 trial. Neither improves E2E. The
current cursor remains below the LLC boundary, but its longest component is a
stall, not application/dispatcher completion. It therefore stops with
`memory_candidates_exhausted_without_compute_evidence`, not a compute-bound
claim. No independent RoCE search policy or 16/8 fallback is used.

### Remaining objective-policy limitation exposed by RoCE L-App

RoCE L-App also diagnoses P1. Its repeated 16/16 baseline changes from 24.40 to
35.34 Mpps. The 15/15 candidate reaches 41.21 Mpps with P99.9=7.42 µs, but the
existing latency significance rule rejects it:

| Quantity | Value |
| --- | ---: |
| Current baseline P99.9 median | 103.276 µs |
| Baseline P99.9 MAD | 68.066 µs |
| Required improvement, dominated by 3×MAD | 204.198 µs |
| Candidate P99.9 median | 7.415 µs |
| Observed improvement | 95.861 µs |

Thus a candidate that crosses into SLO feasibility is rejected as statistically
insignificant. The controller reports `memory_candidates_exhausted_without_compute_evidence`
because the cursor's dominant component remains a stall. This is not a QP-count
restriction or a missing memory-search transition. The approved revision retains
the existing E2E gate, so this work records the limitation without changing its
threshold or introducing another search branch. Choosing whether SLO-feasibility
transitions should override this uncertainty gate requires a separate policy
decision. The reported +44.84% must not be interpreted as a verified tuning gain.

## Focused checks and review

No complete Python test suite was run. Syntax smoke and narrow policy/artifact
replays cover P1/P3 precedence, pending P2/P4, exact 40% relief, midpoint steps,
missing LLC, exhausted counts, split/N−1 comparisons, uniform expansion,
persisted-state round trips, skipped intentions and historical-best publication.
C++ builds completed on both endpoints at the recorded revisions.

A read-only review found that an inconclusive completed C1 probe could lose its
pending P2/P4 hypotheses. `febf579` fixes this and checks the actual probe path.
The first asymmetric measurement also exposed an unregistered connection-artifact
schema; `718172b` registers it. Neither fix changes the datapath's packet handling.

Final review also identified controller-source resume identity, historical
candidate read compatibility, and persisted probe reuse gaps. `cc34f3a` binds
resume to the Python source hash, permits historical records without the new
intention field to be inspected, and publishes the canonical-pair reuse index
atomically with trial finalization. A focused interruption check stops after
finalization and verifies reuse after controller reconstruction. These changes
do not change the candidate/gate definitions or RC datapath tested at `4459b5a`.

The first AE RoCE attempts stopped before starting Axio: AE reused a configuration
tool built before asymmetric RC support. `02a6926` always invokes incremental
Ninja preparation instead of trusting an existing executable. Both endpoints
were compiled at that SHA before retrying RoCE. All 76 generated DPDK configs
produce identical canonical documents under the old and new tools; the completed
DPDK measurements are therefore retained. The failed pre-launch RoCE directories
remain available under the earlier result root for audit.
