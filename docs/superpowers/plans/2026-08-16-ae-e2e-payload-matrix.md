# AE E2E Payload Matrix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run PipeTune E2E tuning for T-App, L-App, and M-App at 128/512/1024-byte request-frame points on DPDK and RoCE, and report every baseline and historical best.

**Architecture:** `artifact_eval.matrices` owns an explicit frame/payload matrix and constructs 18 immutable `CaseConfiguration` values. `artifact_eval.configuration` materializes the explicit payloads, while `artifact_eval.summary` publishes unambiguous frame/payload columns without changing PipeTune artifacts or tuning policy.

**Tech Stack:** Python 3.10 standard library, existing `unittest` fixtures, `axio-configure`, Meson/Ninja, existing AE/PipeTune remote harness.

## Global Constraints

- Case IDs use request Ethernet-frame sizes: `req128`, `req512`, and `req1024`.
- Request payloads are 86, 470, and 982 bytes respectively.
- T-App response payload is 22 bytes; L-App and M-App responses equal their requests.
- Cover DPDK and RoCE, producing 18 independently selectable cases.
- Both DPDK endpoints start at C1/C2=16/16/C3=32; both RoCE endpoints start at C1/C2=16/8/C3=64.
- Smoke uses two warmup windows, three sample windows, one session, and at most three tuning rounds.
- Preserve uncommitted `config/client.toml` and `docs/artifact-evaluation.md` changes.
- Do not run the complete Python suite; use only focused changed-surface checks and remote compilation/real execution.

---

### Task 1: Explicit E2E case matrix and smoke depth

**Files:**
- Modify: `artifact_eval/configuration.py`
- Modify: `artifact_eval/matrices.py`
- Modify: `artifact_eval/model.py`
- Test: `tests/artifact_eval/test_e2e.py`
- Test: `tests/artifact_eval/test_core.py`

**Interfaces:**
- Produces: `CaseConfiguration.request_frame_bytes`, `request_payload_bytes`, and `response_payload_bytes`.
- Produces: 18 `end_to_end_cases()` entries with unique payload-aware case IDs.

- [x] Replace the old 12-case assertion with an 18-case assertion covering both backends, three handlers, and three frame/payload points; assert smoke rounds equal three.
- [x] Run only the focused E2E matrix test and confirm it fails on the old matrix.
- [x] Add optional explicit payload fields to `CaseConfiguration`, validating positive values and keeping non-E2E figure constructors source compatible.
- [x] Replace `HANDLERS` and `_e2e_initial_c3()` E2E construction with an explicit tuple of `(frame_bytes, request_payload_bytes)` and handler-specific response payloads.
- [x] Change the smoke profile's tuning depth from two to three rounds.
- [x] Run the focused E2E matrix and profile tests.
- [x] Commit as `feat: sweep e2e application payloads`.

### Task 2: Materialized payloads and readable summary

**Files:**
- Modify: `artifact_eval/configuration.py`
- Modify: `artifact_eval/summary.py`
- Test: `tests/artifact_eval/test_core.py`
- Test: `tests/artifact_eval/test_summary.py`

**Interfaces:**
- Consumes: explicit payload fields from `CaseConfiguration`.
- Produces: generated endpoint TOMLs containing the selected payload pair.
- Produces: E2E CSV/Markdown columns `Request frame B`, `Request payload B`, and `Response payload B`.

- [x] Add focused assertions that explicit E2E payloads override handler defaults while existing figure cases retain their current handler payloads.
- [x] Add a focused row-format assertion proving request-frame and payload columns are present and distinct.
- [x] Run the two focused tests and confirm they fail before production changes.
- [x] Make `common_overrides()` prefer explicit request/response payloads and otherwise use the existing handler defaults.
- [x] Add the three payload-identification fields to the E2E top-level and per-case headings.
- [x] Run the focused configuration/summary checks and `compileall` for `artifact_eval`.
- [x] Commit as `feat: report e2e payload points`.

### Task 3: Documentation and exact-SHA reference run

**Files:**
- Modify: `docs/superpowers/specs/2026-08-16-ae-e2e-payload-matrix-design.md` only if implementation names differ from the approved contract.
- Results: `/home/ubuntu/git_repos/codex/axio-emulator/results/ae/e2e-payload-smoke/`.

**Interfaces:**
- Consumes: final pushed exact SHA on `rDesktop_01` and `rDesktop_02`.
- Produces: one 18-row `summary.md` and `summary.csv`, plus native PipeTune artifacts for all sessions.

- [ ] Push the implementation branch and fast-forward both existing remote checkouts without overwriting testbed TOMLs.
- [ ] Compile the client and server binaries at the same exact SHA.
- [ ] Dry-run E2E and verify all 18 payload-aware case IDs and three-round smoke depth.
- [ ] Run `run_e2e.sh --profile smoke --output results/ae/e2e-payload-smoke`.
- [ ] Confirm every case starts with target C1=16, publishes a baseline, and completes or converges within three rounds.
- [ ] Report each case's baseline Mpps, historical-best Mpps, improvement, best C1/C2/C3, P99.9, rounds, stop reason, and result path.
