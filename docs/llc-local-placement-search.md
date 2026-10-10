# LLC-guided count and placement search

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
