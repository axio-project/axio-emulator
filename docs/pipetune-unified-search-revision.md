# Unified PipeTune search and asymmetric RoCE RC

Approved implementation specification, 2026-10-17. Base: `26b7dc7`.
Branch: `codex/pipetune-unified-search`.

This specification supersedes the independent paired-search and local-search
controllers described in earlier design notes. There is one search policy with
two phases: memory efficiency and compute capacity. Topology constrains actions;
it does not select a different diagnosis algorithm.

## 1. Initialization and diagnosis

A new bootstrap materializes a fully colocated U/U target from the configured
NUMA workspace budget U. It snapshots this configuration without editing input
files. A resource pool that cannot express U/U is an error. Measure and diagnose
use their inputs unchanged; resume uses the persisted cursor.

Every round performs diagnosis. The leading component proposes a hypothesis;
there is no significance gate between the two leading components.

| Hypothesis | Candidate intentions | Expected impact |
| --- | --- | --- |
| P1 | C1 decrease; directional C3 double | Relevant stall decreases |
| P2 | C1 decrease | Directional LLC miss decreases |
| P3 | C2 decrease | Directional I/O miss decreases |
| P4 | C1 increase; C2 decrease; directional C3 halve | Directional LLC miss decreases |

A legal pure-C1 probe distinguishes P2/P4. If topology protection makes that
probe impossible, retain an inconclusive diagnosis with hypotheses P2 and P4
and evaluate their candidate union. Paired adjustments are compound evidence,
not a pure-C1 causal experiment. Evaluate all legal siblings before selection;
deduplicate identical configurations and reuse existing trials, including probes.

## 2. Topology adapter

Apply protection between candidate intention and configuration materialization.
For one-to-one colocated or split placement, C1 or C2 reduction removes a pair
and preserves placement. Memory actions must not introduce sharing or uneven
fanout. Unsupported C1 growth is skipped without discarding other candidates.
On uniform fanout, remove an entire application layer or dispatcher group.
Record the original hypothesis/intention and the actual count/layout change.

## 3. Memory boundary and local layouts

Keep the exploratory cursor separate from historical best. Ordinary acceptance
requires expected impact and the E2E objective (including latency SLO, noise,
and equivalent performance with fewer physical cores).

The boundary is directional LLC miss <=40%: LLC-load for RX and LLC-store for
TX. I/O counters remain diagnostic evidence, not boundary conditions. Above
the boundary, one E2E decrease or insignificant LLC change must not terminate
legal diagnosis-directed count exploration. Start with minus one; after an
unproductive E2E step, use the midpoint of the legal lower bound and current
count. Diagnosis-driven C3 candidates remain available. Missing LLC is not
relief. Exhausted resources, candidates or rounds above the boundary mean the
boundary was not found, not that compute limitation was established.

At the first relieved cursor compare its count in colocated and split placement
(split requires 2N<=U), then compare N-1 in both layouts. These local comparisons
use E2E, without requiring another LLC or aggregate-stall decrease. C3 retains
its causal gate. Stop local descent when neither smaller layout improves;
retain the previous local cursor. Compute eligibility uses that cursor's own
LLC and stage evidence, never another trial's relief.

## 4. Compute phase

Enter compute only after LLC relief, exhausted memory/local benefits, and a
leading application or dispatcher completion component. For an application
bottleneck, keep C2 and add a complete application layer within the NUMA budget.
For a dispatcher bottleneck on true one-to-one placement, add one app/dispatcher
pair and preserve layout. Do not grow C2 alone or apply pair growth to fanout.
An untested same-count split can also be evaluated; do not jump directly to a
budget-boundary split. Reuse previous layout trials.

Compute expansion requires relevant completion improvement and E2E benefit.
After acceptance return to memory diagnosis. A memory causal gate passing while
E2E fails does not prohibit compute. Only memory/compute are persisted phases;
step size, local count and relief evidence are data in the same search state.
Remove PairedSearchState, paired_reduction_required, probe bypasses and duplicate
acceptance branches. The controller executes and records; policy owns transitions.

## 5. Asymmetric RoCE RC

Support target server 16/16 with a fixed 8/8 client. Per workload, sort actual
dispatcher IDs and partition [0,lcm(C,S)) into C client and S server intervals.
Overlapping intervals define QP pairs and overlap lengths define request weights.
This gives 16 links for 8:16, 22 for 8:15, 14 for 8:7 and eight for 8:8. Infer
the graph from topology and reciprocal routes; add no configuration knob.
Relax RC equal-count/equal-ID constraints only. UD is outside this extension.

Each dispatcher shares its PD, MR, pool and CQs across links. Partition its
existing ring budget into per-link slots/credits; do not multiply the budget by
QP count. Use WR IDs for exact completion ownership, never cross-QP FIFO order.
Progress TX and RX repost fairly. Preserve ingress link and originating client
application on each message, including newly allocated response buffers; return
each response on its original connection to its original application.

Listen on the management connection before creating resources. Create all local
QPs, exchange bounded length-framed manifests through endpoint coordinators,
connect QPs, confirm posted RX readiness, then measure. Retain the three-second
connect deadline. Initialization failure must wake local waiters and release
owned resources. Use one resource/handshake model, with a single-link fast path
and no cross-dispatcher datapath locks.

If any dispatcher in a workload's graph has multiple QPs, both request and
response must fit one packet. Validate before startup/materialization. Fully
single-QP graphs retain existing multi-packet support. This revision covers
T/L/M-App 128/512/1024B and single-packet KV. Multi-QP file read/write reassembly
and Figure 14's multi-packet RoCE adaptation remain out of scope; report an
explicit error instead of silently reverting to 16/8. Reports describe derived
links and per-dispatcher quotas: fixed client threads do not mean fixed QP count.

## 6. Reviewable implementation units

1. Commit this specification and retire contradictory design descriptions.
2. Unify diagnosis, topology adaptation, LLC exploration and compute transitions;
   remove legacy paired state and report paths.
3. Implement RC graph, resource ownership and coordinated startup.
4. Implement multi-QP I/O, WR ownership, response routing and validation.
5. Update AE initialization, CLI/report/docs and remote verification evidence.

Keep CLI commands and primary artifact names. Serialize and validate one search
state. Old sessions remain readable, but source identity rejects resume across
algorithm revisions; no compatibility algorithm or state migration is added.

## 7. Verification scope

Use Python syntax smoke and focused artifact replay, not full Python suites.
Cover P1/P3 precedence, pending P2/P4 candidates, high-LLC exploration despite
E2E decline, exact 40%, missing LLC, resource exhaustion, cursor/best separation,
split and N-1 comparisons, paired growth, and deterministic resume.

Compile C++ feature commits on both hosts at exact SHA. Test RC target dispatcher
counts 16/15/8/7/1 against fixed client eight, split IDs, interleaved completions,
ring reuse, response ownership, both buffer modes and multi-packet restrictions.
Compare symmetric single-QP 4c throughput against 26b7dc7 over three cold starts;
investigate median regression greater than 5% before accepting it.

Run DPDK/RoCE x T-App/L-App 128B, server 16/16, client 8/8, 10 warmup and 20
sample windows, at most 20 rounds. Record hypotheses, original/actual actions,
LLC, causal/E2E gates, cursor and best. Replay branches not naturally reached;
do not repeat long tests merely for coverage. No fixed optimal count or historic
peak is an algorithm-correctness gate. Leave user checkouts/configurations and
paper data untouched; the primary session implements and subagents only review.
