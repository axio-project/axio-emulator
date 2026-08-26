# Artifact Evaluation Workflow Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the existing artifact-evaluation workflow clean, resumable from one directory argument, observable during long runs, and unambiguous in its human-readable reports.

**Architecture:** Keep `artifact_eval` as the importable Python package, move public shell entry points under `scripts/artifact_eval`, and keep transient remote control data below the already-ignored `build-ae` tree. New runs snapshot their reference TOMLs into the result manifest; resume reconstructs the exact matrix and input directory from that manifest. Progress is emitted to stderr, while numeric summaries remain on stdout.

**Tech Stack:** Python 3.10 standard library, Bash, TOML materialization through `axio-configure`, `unittest`.

## Global Constraints

- The main README remains the environment and dependency prerequisite; do not duplicate setup or add a standalone preflight command.
- Do not modify paper-repository data.
- Preserve the current experiment matrices and datapath behavior.
- Use focused tests during implementation and one final `tests/artifact_eval` run.
- Remote verification uses the existing `/home/ubuntu/git_repos/codex/axio-emulator` checkout on both hosts.

---

### Task 1: Clean public and runtime paths

**Files:**
- Move: `artifact-eval/run_*.sh` to `scripts/artifact_eval/run_*.sh`
- Modify: `artifact_eval/runtime.py`
- Modify: `config/artifact/reference-200g/*.toml`
- Modify: `docs/artifact-evaluation.md`
- Test: `tests/artifact_eval/test_core.py`

**Interfaces:**
- Public commands become `./scripts/artifact_eval/run_<experiment>.sh`.
- Remote control and generated build TOMLs live below `<workdir>/build-ae/.artifact_eval/`.

- [ ] Add a failing test asserting that runtime paths are descendants of `build-ae/.artifact_eval` and never use a root `.artifact-eval` directory.
- [ ] Run `python3 -m unittest tests.artifact_eval.test_core` and verify the new assertion fails against the old paths.
- [ ] Centralize the runtime-state root in `artifact_eval.runtime` and update command evidence, RoCE preflight control, and build-config paths.
- [ ] Move the wrappers, update `run_all.sh`, update the guide, and change all four reference workdirs to `/home/ubuntu/git_repos/codex/axio-emulator`.
- [ ] Run the focused test and shell syntax checks; verify the tracked tree contains no root `artifact-eval` directory.
- [ ] Commit the path cleanup.

### Task 2: Make resume manifest-driven and add progress

**Files:**
- Modify: `artifact_eval/manifest.py`
- Modify: `artifact_eval/harness.py`
- Modify: `artifact_eval/__main__.py`
- Modify: `scripts/artifact_eval/run_all.sh`
- Test: `tests/artifact_eval/test_core.py`

**Interfaces:**
- `ExperimentCase.from_document(document)` reconstructs a validated persisted case.
- `RunManifest.inspect(root)` returns the persisted experiment, profile, and matrix before execution.
- New manifests record hashed reference TOMLs under `inputs/reference-configs/`; resume always consumes that snapshot.
- `--resume DIR` rejects matrix-changing overrides and does not require `--profile`, `--case`, or `--config-dir`.

- [ ] Add failing tests for persisted-case reconstruction, input hash validation, and manifest-only resume identity.
- [ ] Run the focused tests and verify they fail for the missing interfaces.
- [ ] Implement strict case deserialization, input snapshot evidence, and manifest inspection.
- [ ] Update the CLI to reconstruct resume options from the manifest and reject conflicting overrides.
- [ ] Add a stderr progress helper and emit preflight, build, case, repeat/session, skip, and summary-stage messages.
- [ ] Run the focused tests and a dry-run/resume CLI fixture; verify stdout remains reserved for matrix or summary output.
- [ ] Commit the resume and progress changes.

### Task 3: Clarify tuning outcomes and finish the guide

**Files:**
- Modify: `artifact_eval/summary.py`
- Modify: `docs/artifact-evaluation.md`
- Test: `tests/artifact_eval/test_summary.py`

**Interfaces:**
- E2E detail tables expose `Search outcome`; accepted exploratory paired reductions are rendered as `exploratory_memory_cursor`, not generic `accepted`.
- Objective winners retain their concrete acceptance mode, such as `equivalent_fewer_cores`; rejected candidates render as `rolled_back`.

- [ ] Add a failing test covering an accepted exploratory cursor whose expected-impact and E2E gates both reject.
- [ ] Run `python3 -m unittest tests.artifact_eval.test_summary` and verify the assertion fails with the old generic label.
- [ ] Implement one summary-outcome formatter shared by detail rows and update the table heading.
- [ ] Rewrite the running and resume examples with the unified paths and manifest-only resume command.
- [ ] Run all `tests/artifact_eval` tests, `bash -n` on every wrapper, and `rg` checks for stale `artifact-eval/`, `.artifact-eval`, and `axio-artifact-evaluation` paths.
- [ ] Commit the reporting and documentation cleanup.

### Task 4: Exact-SHA remote smoke

**Files:**
- No source changes.

**Interfaces:**
- The pushed exact SHA is built and exercised from the existing checkout on each host.

- [ ] Push `codex/artifact-evaluation-cleanup` and switch both existing remote checkouts to the exact SHA without replacing user configuration files.
- [ ] Run one DPDK T-App E2E smoke, one Figure 6 L-App point, and one RoCE T-App E2E smoke.
- [ ] Resume the completed DPDK session with only `--resume DIR`.
- [ ] Verify summaries, progress output, no active experiment processes, and a clean tracked worktree on both hosts.
