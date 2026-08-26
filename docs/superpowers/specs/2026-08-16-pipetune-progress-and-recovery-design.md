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

## Enqueue-drop semantics

Application and dispatcher enqueue counters are observations, not contention
diagnoses. A baseline or candidate that reports an enqueue drop remains valid
and is not retried. The drop count is retained as an observation but does not
itself alter the candidate decision, paired-search cursor, or search phase;
the normal expected-impact and end-to-end objective rules still apply. NIC
completion errors remain health failures; endpoint-throughput differences are
warnings.

## Compatibility and verification

- stdout JSON contracts remain unchanged.
- Progress is enabled by default only for CLI use; Python callers remain silent.
- Existing user-edited TOML files are not modified.
- Focused tests cover stderr/stdout separation, `--quiet`, best-summary semantics,
  and the exploratory enqueue-saturation transition.
- Acceptance uses the same three remote smoke cases as the preceding artifact
  evaluation review: DPDK T-App E2E, Figure 6 L-App, and RoCE T-App E2E.
