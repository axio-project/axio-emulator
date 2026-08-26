# PipeTune Progress and Exploratory-Cursor Recovery Design

## Scope

This change makes long-running `pipetune measure`, `bootstrap`, and `resume`
commands visibly active and fixes the paired-reduction transition observed in
`results/tune-001`. It does not change Axio datapath processing or configuration
defaults.

## CLI progress

PipeTune libraries remain silent unless a progress callback is supplied. The CLI
supplies one by default and writes concise human-readable messages to stderr, so
the existing final JSON document remains the only stdout content. `--quiet`
disables progress and the final human summary.

Progress is emitted at operations that can block for seconds:

- resolving and materializing the endpoint pair;
- starting the server and client;
- waiting for warmup windows;
- collecting sample windows and host counters;
- finalizing a trial;
- starting each tuning round and trial;
- reporting diagnosis, candidate action, decision, retry, and best-so-far.

The terminal completion summary reports the historical-best target throughput,
client P99.9, active C1/C2 and C3 values, completed rounds, stop reason, and
`report.md` path. Workspace-pool capacity is not presented as the active core
count. `generation` remains in the machine-readable status as the crash-recovery
checkpoint sequence number.

## Exploratory-cursor recovery

Paired reduction may deliberately move from the historical best to a lower-count
exploratory cursor while both directional memory-pressure rates remain above the
40% threshold. If the next required baseline at that cursor reports target app or
dispatcher enqueue drops, that sample is search evidence rather than an
infrastructure failure.

PipeTune shall:

1. classify target enqueue saturation separately from peer, completion, and
   provider failures;
2. stop retrying the saturated exploratory baseline;
3. restore the last healthy paired-search cursor;
4. enter compute search from that cursor;
5. keep the historical best unchanged unless a later objective-valid candidate
   improves it.

Target enqueue saturation outside an active paired exploratory search retains the
existing unhealthy-trial behavior. Peer drops, NIC completion errors, missing
metrics, process failures, and provider failures remain infrastructure failures.

## Compatibility and verification

- stdout JSON contracts remain unchanged.
- Progress is enabled by default only for CLI use; Python callers remain silent.
- Existing user-edited TOML files are not modified.
- Focused tests cover stderr/stdout separation, `--quiet`, best-summary semantics,
  and the exploratory enqueue-saturation transition.
- Acceptance uses the same three remote smoke cases as the preceding artifact
  evaluation review: DPDK T-App E2E, Figure 6 L-App, and RoCE T-App E2E.
