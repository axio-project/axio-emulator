# AE Bootstrap Reference Batch Configuration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve reference per-stage C3 values for E2E and Figure 14 bootstrap cases without changing the paper Figure sweeps.

**Architecture:** Add one explicit case-level materialization policy. `target_overrides()` always applies C1/C2 and conditionally applies the six uniform C3 fields.

**Tech Stack:** Python 3.10 standard library, `unittest`, existing `axio-configure` materialization, remote Axio testbed.

## Global Constraints

- Every AE client remains fixed at C1/C2=8/8.
- E2E and Figure 14 preserve the reference server batch/post values.
- Figures 3, 6, 7, and 8 retain their explicit uniform C3 sweeps.
- Smoke uses 10 warmup windows, 20 sample windows, and at most three E2E rounds.
- Do not run the complete Python suite; use focused checks and one remote case.

---

### Task 1: Separate reference and uniform C3 materialization

**Files:**
- Modify: `artifact_eval/configuration.py`
- Modify: `artifact_eval/matrices.py`
- Modify: `artifact_eval/harness.py`
- Test: `tests/artifact_eval/test_core.py`
- Test: `tests/artifact_eval/test_e2e.py`

**Interfaces:**
- Consumes: checked-in reference target TOMLs and existing `CaseConfiguration` matrices.
- Produces: `CaseConfiguration.preserve_reference_c3: bool` and conditional `target_overrides()` output.

- [ ] **Step 1: Write failing materialization and matrix tests**

Assert that E2E and Figure 14 cases preserve reference C3, Figure 3/6/7/8 do
not, and `target_overrides()` omits all six C3 keys only for reference cases.

- [ ] **Step 2: Run the focused tests and verify the intended failures**

Run the named `unittest` methods only. Expect failures because the policy field
does not yet exist and all cases currently apply uniform C3.

- [ ] **Step 3: Implement the minimal policy**

Add and validate `preserve_reference_c3`; classify the two bootstrap matrices;
conditionally add the six C3 overrides; print `reference` in dry-run output.

- [ ] **Step 4: Run focused tests and syntax checks**

Run the same named tests, `python3 -m compileall -q artifact_eval`, and
`git diff --check`. Expect zero failures.

- [ ] **Step 5: Commit and run remote acceptance**

Commit the behavior separately from this design record, push the exact SHA,
build on both endpoints through the AE harness, and run only
`dpdk-l-app-req128`. Verify generated configs and report baseline/best data.
