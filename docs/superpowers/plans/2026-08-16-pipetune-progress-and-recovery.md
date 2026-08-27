# PipeTune Progress and Recovery Implementation Plan

> **Superseded policy:** The enqueue-saturation recovery described in Task 2
> was removed. Baseline enqueue drops are observations, candidate enqueue
> drops reject only that candidate, and neither condition changes the search
> phase. NIC completion errors remain health failures.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make long PipeTune commands visibly active, publish a concise best-result summary, and recover paired reduction from target enqueue saturation at an exploratory cursor.

**Architecture:** A small optional progress callback flows through the existing CLI, application, controller, and runner boundaries; direct library callers remain silent. The controller recognizes target enqueue saturation only while paired search is exploring below its last healthy cursor and switches to compute search from that healthy cursor.

**Tech Stack:** Python 3.10 standard library, `unittest`, existing PipeTune immutable session and reporting contracts.

## Global Constraints

- Preserve stdout as the existing final JSON document.
- Write default human progress and summary to stderr; `--quiet` suppresses both.
- Do not modify user TOML files or Axio critical-path C++.
- Keep historical best and exploratory search cursor distinct.
- Form two independently reviewable implementation commits after this plan commit.

---

### Task 1: CLI progress and best summary

**Files:**
- Create: `pipetune/progress.py`
- Modify: `pipetune/__main__.py`
- Modify: `pipetune/application.py`
- Modify: `pipetune/controller.py`
- Modify: `pipetune/runner.py`
- Test: `tests/pipetune/test_cli.py`
- Test: `tests/pipetune/test_runner.py`
- Test: `tests/pipetune/test_controller.py`

**Interfaces:**
- Produces: optional `ProgressSink = Callable[[str], None]` arguments whose default is `None`.
- Produces: a deterministic terminal best summary derived from the verified convergence result and canonical best config.

- [x] Add CLI tests proving progress goes to stderr, JSON remains parseable on stdout, and `--quiet` suppresses human output.
- [x] Run the focused tests and verify they fail because progress/summary are absent.
- [x] Add optional progress emission at runner and controller blocking boundaries.
- [x] Add the final best summary without changing the status JSON schema.
- [x] Run focused CLI, runner, controller, application, and reporting tests.
- [x] Commit as `feat: show pipetune execution progress`.

### Task 2: Recover a saturated exploratory cursor

**Files:**
- Modify: `pipetune/controller.py`
- Modify: `pipetune/paired_search.py` if a persisted transition helper is required.
- Test: `tests/pipetune/test_controller.py`
- Test: `tests/pipetune/test_convergence.py`

**Interfaces:**
- Consumes: persisted paired-search reference and last healthy cursor.
- Produces: a compute-phase round starting from the last healthy cursor when an exploratory baseline has target enqueue drops.

- [x] Add a regression test reproducing a healthy 15/15 cursor followed by a dropped 14/14 exploratory baseline.
- [x] Confirm from the recorded `tune-001` artifacts that the current controller terminates at `infrastructure_failure_limit`.
- [x] Classify only target app/dispatcher enqueue saturation as a paired-search transition signal.
- [x] Restore the healthy cursor and enter compute search without replacing historical best.
- [x] Keep peer drops and non-paired baseline drops on the existing failure path.
- [x] Run only local syntax/diff smoke checks; use exact-SHA remote execution as requested instead of a Python test suite.
- [x] Commit as `fix: recover saturated paired search cursors`.

### Task 3: Exact-SHA remote acceptance

**Files:**
- No production files.
- Results: remote `results/ae/` directories only.

**Interfaces:**
- Consumes: one pushed exact SHA on both reference hosts.
- Produces: three human-readable smoke summaries and their immutable artifacts.

- [ ] Run `python3 -m compileall -q pipetune` locally; do not run the full Python suite.
- [ ] Push the exact SHA and update both existing remote checkouts without changing their testbed TOMLs.
- [ ] Build both endpoint binaries at that SHA.
- [ ] Run DPDK E2E T-App from server 16/16, client 8/8, smoke profile.
- [ ] Run Figure 6 L-App at C1/C2/C3 = 1/1/16, smoke profile.
- [ ] Run RoCE E2E T-App from server 16/16, client 8/8, smoke profile.
- [ ] Report throughput, latency or stage distribution, rounds, best config, stop reason, progress-output behavior, and result paths.
