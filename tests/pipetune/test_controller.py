from __future__ import annotations

import dataclasses
import hashlib
import json
import pathlib
import shutil
import tempfile
import types
import unittest

from pipetune.artifacts import artifact_ref
from pipetune.candidates import Candidate, CandidateAction
from pipetune.controller import (
    ColdStartController,
    ControllerError,
    InfrastructureFailureLimit,
    MeasureTrialExecutor,
    TrialExecutionError,
    TuningLoop,
)
from pipetune.diagnosis import Diagnosis, ProbeSpec, Statistic, summarize_trial
from pipetune.impact import ExpectedImpactComparison
from pipetune.objective import ObjectivePolicy, ObjectiveTrial
from pipetune.reporting import publish_session_outputs
from pipetune.runner import MeasureError, MeasureRequest
from pipetune.search_policy import (
    ComputeBottleneck,
    ImpactSpec,
    SearchAction,
    SearchPhase,
)
from tests.pipetune.test_diagnosis import build_session
from tests.pipetune.test_runner import FakeConfigTool, ScriptedTransport
from tests.pipetune.test_session import create_store


POLICY = ObjectivePolicy(
    latency_slo_us=100.0,
    latency_relative_floor=0.03,
    throughput_relative_floor=0.01,
)


def _statistic(value: float, *, unit: str) -> Statistic:
    return Statistic(
        samples=(value,),
        median=value,
        mad=0.0,
        uncertainty=value * 0.01,
        unit=unit,
    )


def _objective(
    trial_id: str, *, throughput: float, latency: float = 2.0
) -> ObjectiveTrial:
    return ObjectiveTrial(
        trial_id=trial_id,
        status="valid",
        client_p999=_statistic(latency, unit="us"),
        server_throughput=_statistic(throughput, unit="Mpps"),
        rejection_reason=None,
    )


def _rejected_objective(trial_id: str, status: str) -> ObjectiveTrial:
    return ObjectiveTrial(
        trial_id=trial_id,
        status=status,
        client_p999=None,
        server_throughput=None,
        rejection_reason=f"scripted {status}",
    )


def _diagnosis(
    point: str,
    *,
    probe: ProbeSpec | None = None,
    direction: str | None = "tx",
    completed_probe: ProbeSpec | None = None,
) -> Diagnosis:
    return Diagnosis(
        schema="pipetune.diagnosis/v1",
        point=point,
        direction=direction,
        confidence="high",
        evidence=(),
        rejected_evidence=(),
        missing_metrics=(),
        noise_thresholds={},
        required_probe=probe,
        input_hashes={},
        stage_ranking=(),
        completed_probe=completed_probe,
    )


def _impact(candidate_id: str, *, accepted: bool) -> ExpectedImpactComparison:
    return ExpectedImpactComparison(
        candidate_id=candidate_id,
        point="scripted",
        metric="scripted_metric",
        accepted=accepted,
        reason=("scripted decrease" if accepted else "scripted rejection"),
        baseline_value=2.0,
        candidate_value=1.0 if accepted else 2.0,
        observed_reduction=1.0 if accepted else 0.0,
        required_reduction=0.1,
        unit="scripted",
    )


def _publish_fixture_trial(
    destination: pathlib.Path,
    trial_id: str,
    *,
    candidate_label: str = "baseline",
) -> None:
    with tempfile.TemporaryDirectory(prefix=".controller-fixture-") as temp_dir:
        session = build_session(pathlib.Path(temp_dir), target_role="server")
        source = session / "trials/trial-0001"
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(source, destination)
    manifest = destination / "trial.json"
    document = json.loads(manifest.read_text(encoding="utf-8"))
    canonical = destination / "configs/canonical/target.json"
    canonical_document = json.loads(canonical.read_text(encoding="utf-8"))
    topology, application_count, dispatcher_count = _fixture_topology(
        candidate_label
    )
    canonical_document["deployment"]["topology"] = topology
    runtime = canonical_document["knobs"]["runtime"]
    runtime["application_core_count"] = application_count
    runtime["dispatcher_queue_count"] = dispatcher_count
    canonical.write_text(
        json.dumps(
            canonical_document,
            allow_nan=False,
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )
    target_endpoint = next(
        endpoint
        for endpoint in document["endpoints"]
        if endpoint["spec"]["endpoint_id"] == "target"
    )
    canonical_artifact = next(
        artifact
        for artifact in target_endpoint["artifacts"]
        if artifact["path"] == "configs/canonical/target.json"
    )
    payload = canonical.read_bytes()
    canonical_artifact["sha256"] = hashlib.sha256(payload).hexdigest()
    canonical_artifact["size_bytes"] = len(payload)
    document["trial_id"] = trial_id
    manifest.write_text(
        json.dumps(document, allow_nan=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def _fixture_topology(
    candidate_label: str,
) -> tuple[dict[str, object], int, int]:
    workspace_ids = list(range(8))
    if candidate_label == "split-1to1":
        applications = [0, 1]
        dispatchers = [2, 3]
        groups = [
            {"dispatcher": dispatcher, "applications": [application]}
            for application, dispatcher in zip(applications, dispatchers)
        ]
    elif candidate_label == "app-fanout-layer":
        applications = [0, 1, 2, 3]
        dispatchers = [0, 1]
        groups = [
            {"dispatcher": 0, "applications": [0, 2]},
            {"dispatcher": 1, "applications": [1, 3]},
        ]
    elif candidate_label == "paired-colocated-growth":
        applications = [0, 1, 2]
        dispatchers = [0, 1, 2]
        groups = [
            {"dispatcher": value, "applications": [value]}
            for value in applications
        ]
    elif candidate_label == "c2-decrease":
        applications = [0, 1]
        dispatchers = [0]
        groups = [{"dispatcher": 0, "applications": applications}]
    elif candidate_label == "c1-decrease":
        applications = [0]
        dispatchers = [0, 1]
        groups = [
            {"dispatcher": 0, "applications": applications},
            {"dispatcher": 1, "applications": []},
        ]
    elif candidate_label in ("c1-increase", "c1-probe"):
        applications = [0, 1, 2]
        dispatchers = [0, 1]
        groups = [
            {"dispatcher": 0, "applications": [0, 2]},
            {"dispatcher": 1, "applications": [1]},
        ]
    else:
        applications = [0, 1]
        dispatchers = [0, 1]
        groups = [
            {"dispatcher": value, "applications": [value]}
            for value in applications
        ]
    topology = {
        "application_workspaces": workspace_ids,
        "dispatcher_workspaces": workspace_ids,
        "workloads": [{"id": 1, "groups": groups}],
        "workspaces": [
            {"id": value, "cpu_core": value} for value in workspace_ids
        ],
    }
    return topology, len(applications), len(dispatchers)


class ScriptedExecutor:
    def __init__(self, root: pathlib.Path):
        self.root = root
        self.calls: list[tuple[str, bytes, bytes]] = []

    def execute(
        self,
        *,
        trial_id: str,
        target_config: pathlib.Path,
        peer_config: pathlib.Path,
        destination: pathlib.Path,
    ) -> object:
        self.calls.append(
            (trial_id, target_config.read_bytes(), peer_config.read_bytes())
        )
        payload = target_config.read_text(encoding="utf-8")
        candidate_label = next(
            (
                label
                for label in (
                    "paired-colocated-growth",
                    "app-fanout-layer",
                    "split-1to1",
                    "c3-tx-increase",
                    "c3-rx-increase",
                    "c3-tx-decrease",
                    "c3-rx-decrease",
                    "c2-decrease",
                    "c1-increase",
                    "c1-decrease",
                    "c1-probe",
                )
                if label in payload
            ),
            "baseline",
        )
        _publish_fixture_trial(
            destination,
            trial_id,
            candidate_label=candidate_label,
        )
        return summarize_trial(destination / "trial.json")


class FlakyExecutor(ScriptedExecutor):
    def __init__(self, root: pathlib.Path, failures: tuple[bool, ...]):
        super().__init__(root)
        self.failures = iter(failures)
        self.attempted: list[str] = []

    def execute(self, **kwargs: object) -> object:
        trial_id = str(kwargs["trial_id"])
        self.attempted.append(trial_id)
        if next(self.failures, False):
            raise TrialExecutionError("scripted infrastructure failure")
        return super().execute(**kwargs)


class PublishThenFailExecutor(ScriptedExecutor):
    def __init__(self, root: pathlib.Path):
        super().__init__(root)
        self.published_failure = False

    def execute(self, **kwargs: object) -> object:
        if not self.published_failure:
            self.published_failure = True
            trial_id = str(kwargs["trial_id"])
            destination = pathlib.Path(kwargs["destination"])
            _publish_fixture_trial(
                destination,
                trial_id,
                candidate_label="baseline",
            )
            raise TrialExecutionError("lost acknowledgement after publish")
        return super().execute(**kwargs)


class CrashDuringComputeExecutor(ScriptedExecutor):
    def __init__(self, root: pathlib.Path):
        super().__init__(root)
        self.crashed = False

    def execute(self, **kwargs: object) -> object:
        trial_id = str(kwargs["trial_id"])
        if "split-1to1" in trial_id and not self.crashed:
            self.crashed = True
            target_config = pathlib.Path(kwargs["target_config"])
            peer_config = pathlib.Path(kwargs["peer_config"])
            self.calls.append(
                (trial_id, target_config.read_bytes(), peer_config.read_bytes())
            )
            _publish_fixture_trial(
                pathlib.Path(kwargs["destination"]),
                trial_id,
                candidate_label="split-1to1",
            )
            raise RuntimeError("scripted process interruption")
        return super().execute(**kwargs)


class ScriptedCandidates:
    def __init__(
        self, order: tuple[str, ...], *, format_drift: bool = False
    ):
        self.order = order
        self.format_drift = format_drift
        self.calls: list[str] = []

    def __call__(
        self,
        diagnosis: Diagnosis,
        *,
        target_config: pathlib.Path,
        peer_config: pathlib.Path,
        output_dir: pathlib.Path,
        config_tool: object,
    ) -> tuple[Candidate, ...]:
        del config_tool
        self.calls.append(diagnosis.point)
        output_dir.mkdir(parents=True)
        if diagnosis.point == "probe_required":
            names = ("c1-probe",)
        elif diagnosis.point == "P4":
            names = self.order
        elif diagnosis.point == "P1":
            names = ("c1-decrease", "c3-tx-increase")
        elif diagnosis.point == "P2":
            names = ("c1-decrease",)
        elif diagnosis.point == "P3":
            names = ("c2-decrease",)
        else:
            names = ()
        result = []
        for index, name in enumerate(names, 1):
            root = output_dir / f"candidate-{index:02d}-{name}"
            root.mkdir()
            target = root / "target.toml"
            peer = root / "peer.toml"
            if name == "c1-probe" and diagnosis.required_probe is not None:
                value = diagnosis.required_probe.candidate_value
                target.write_text(f"target-c1={value}\n", encoding="utf-8")
                target_hash = ("1" if value > 4 else "6") * 64
            elif name == "c1-increase":
                target.write_text(
                    "target-c1 = 5\n"
                    if self.format_drift
                    else "target-c1=5\n",
                    encoding="utf-8",
                )
                target_hash = "1" * 64
            elif name == "c1-decrease":
                target.write_text("target-c1=3\n", encoding="utf-8")
                target_hash = "6" * 64
            else:
                target.write_text(f"target-{name}\n", encoding="utf-8")
                target_hash = ("2" if name == "c2-decrease" else "3") * 64
            if name == "c2-decrease":
                peer.write_text("peer-route-c2=3\n", encoding="utf-8")
                peer_hash = "4" * 64
            else:
                shutil.copyfile(peer_config, peer)
                peer_hash = "5" * 64
            overrides = ()
            if name == "c1-probe":
                required = diagnosis.required_probe
                assert required is not None
                overrides = ((required.knob, required.candidate_value),)
            result.append(
                Candidate(
                    candidate_id=f"candidate-{index:02d}-{name}",
                    action=CandidateAction(
                        name, name.split("-", 1)[0], overrides
                    ),
                    target_config=target,
                    peer_config=peer,
                    target_sha256=target_hash,
                    peer_sha256=peer_hash,
                    canonical_target={},
                    canonical_peer={},
                )
            )
        return tuple(result)


class ScriptedActionMaterializer:
    def __init__(self) -> None:
        self.calls: list[tuple[str, ...]] = []

    def __call__(
        self,
        actions: tuple[SearchAction, ...],
        *,
        target_config: pathlib.Path,
        peer_config: pathlib.Path,
        output_dir: pathlib.Path,
        config_tool: object,
    ) -> tuple[Candidate, ...]:
        del config_tool
        self.calls.append(tuple(action.name for action in actions))
        output_dir.mkdir(parents=True)
        candidates = []
        for index, action in enumerate(actions, 1):
            candidate_id = f"candidate-{index:02d}-{action.name}"
            root = output_dir / candidate_id
            root.mkdir()
            target = root / "target.toml"
            peer = root / "peer.toml"
            target.write_bytes(
                target_config.read_bytes() + f"action={action.name}\n".encode()
            )
            shutil.copyfile(peer_config, peer)
            candidates.append(
                Candidate(
                    candidate_id=candidate_id,
                    action=action,
                    target_config=target,
                    peer_config=peer,
                    target_sha256=hashlib.sha256(target.read_bytes()).hexdigest(),
                    peer_sha256=hashlib.sha256(peer.read_bytes()).hexdigest(),
                    canonical_target={},
                    canonical_peer={},
                )
            )
        return tuple(candidates)


def _compute_action(name: str, *, role: str) -> SearchAction:
    direction = "rx" if role == "application" else "tx"
    metric = (
        "app_rx.completion"
        if role == "application"
        else "dispatcher_tx.completion"
    )
    return SearchAction(
        name=name,
        kind="topology",
        overrides=(),
        profile=(
            "colocated-fanout" if name == "app-fanout-layer" else "split-1to1"
        ),
        phase=SearchPhase.COMPUTE,
        impact=ImpactSpec("component", metric, direction),
    )


class ColdStartControllerTest(unittest.TestCase):
    def _run(
        self,
        root: pathlib.Path,
        *,
        order: tuple[str, ...],
        throughput_by_label: dict[str, float],
        initial_point: str = "probe_required",
        final_point: str = "P4",
        format_drift: bool = False,
        probe_direction: int = 1,
        valid_impact_labels: frozenset[str] | None = None,
    ):
        store, _identity, _accepted, state = create_store(root)
        executor = ScriptedExecutor(root)
        candidates = ScriptedCandidates(order, format_drift=format_drift)
        probe_spec = ProbeSpec(
            knob="knobs.runtime.application_core_count",
            direction=probe_direction,
            baseline_value=4,
            candidate_value=4 + probe_direction,
        )

        def diagnose(summary: object, *, probe_summary: object | None = None):
            del summary
            return (
                _diagnosis(initial_point, probe=probe_spec)
                if probe_summary is None
                else _diagnosis(final_point, completed_probe=probe_spec)
            )

        def objective(summary: object) -> ObjectiveTrial:
            trial_id = summary.trial_id
            label = trial_id.rsplit("-", 1)[-1]
            for candidate_label in throughput_by_label:
                if candidate_label in trial_id:
                    label = candidate_label
                    break
            return _objective(
                trial_id,
                throughput=throughput_by_label[label],
            )

        def compare_impact(
            _diagnosis_value: object,
            _baseline: object,
            _candidate: object,
            *,
            candidate_id: str,
        ) -> ExpectedImpactComparison:
            valid = valid_impact_labels is None or any(
                label in candidate_id for label in valid_impact_labels
            )
            return _impact(candidate_id, accepted=valid)

        controller = ColdStartController(
            root=root,
            store=store,
            config_tool=object(),
            executor=executor,
            policy=POLICY,
            candidate_generator=candidates,
            diagnoser=diagnose,
            objective_factory=objective,
            impact_comparer=compare_impact,
            trial_id_factory=lambda purpose: purpose,
        )
        result = controller.run_round(state, round_index=1)
        return result, executor, store

    def _run_two_phase(
        self,
        root: pathlib.Path,
        *,
        compute_role: str | None,
        compute_names: tuple[str, ...],
        throughput_by_label: dict[str, float],
        valid_impact_labels: frozenset[str],
        compute_order: tuple[str, ...] | None = None,
    ):
        store, _identity, _accepted, state = create_store(root)
        executor = ScriptedExecutor(root)
        memory = ScriptedCandidates(())
        materializer = ScriptedActionMaterializer()
        actions = tuple(
            _compute_action(name, role=compute_role or "application")
            for name in (compute_order or compute_names)
        )

        def objective(summary: object) -> ObjectiveTrial:
            trial_id = summary.trial_id
            label = next(
                (
                    candidate
                    for candidate in throughput_by_label
                    if candidate in trial_id
                ),
                "baseline",
            )
            return _objective(
                trial_id,
                throughput=throughput_by_label[label],
            )

        def compare_impact(
            _impact_spec: object,
            _baseline: object,
            _candidate: object,
            *,
            candidate_id: str,
        ) -> ExpectedImpactComparison:
            return _impact(
                candidate_id,
                accepted=any(
                    label in candidate_id for label in valid_impact_labels
                ),
            )

        controller = ColdStartController(
            root=root,
            store=store,
            config_tool=object(),
            executor=executor,
            policy=POLICY,
            candidate_generator=memory,
            action_materializer=materializer,
            diagnoser=lambda _summary: _diagnosis("P3", direction="rx"),
            compute_bottleneck_factory=(
                (lambda _summary: None)
                if compute_role is None
                else (
                    lambda _summary: ComputeBottleneck(
                        role=compute_role,
                        metric=(
                            "app_rx.completion"
                            if compute_role == "application"
                            else "dispatcher_tx.completion"
                        ),
                    )
                )
            ),
            compute_action_factory=lambda _bottleneck, _topology, _runtime: actions,
            objective_factory=objective,
            impact_comparer=compare_impact,
            trial_id_factory=lambda purpose: purpose,
        )
        result = controller.run_round(state, round_index=1)
        return result, controller, executor, materializer, store

    def test_memory_acceptance_does_not_enter_compute_phase(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            result, _controller, executor, materializer, _store = self._run_two_phase(
                pathlib.Path(temp_dir),
                compute_role="application",
                compute_names=("split-1to1", "app-fanout-layer"),
                throughput_by_label={"baseline": 40.0, "c2-decrease": 42.0},
                valid_impact_labels=frozenset(("c2-decrease",)),
            )

            self.assertIn("c2-decrease", result.accepted_trial_id)
            self.assertEqual(materializer.calls, [])
            self.assertEqual(len(executor.calls), 2)
            self.assertEqual(
                {item["search_phase"] for item in result.state.details["candidate_evaluations"]},
                {"memory"},
            )

    def test_application_compute_runs_split_and_fanout_from_same_anchor(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            result, _controller, executor, materializer, _store = self._run_two_phase(
                pathlib.Path(temp_dir),
                compute_role="application",
                compute_names=("split-1to1", "app-fanout-layer"),
                throughput_by_label={
                    "baseline": 40.0,
                    "c2-decrease": 39.0,
                    "split-1to1": 41.0,
                    "app-fanout-layer": 43.0,
                },
                valid_impact_labels=frozenset(
                    ("split-1to1", "app-fanout-layer")
                ),
            )

            self.assertIn("app-fanout-layer", result.accepted_trial_id)
            self.assertEqual(
                materializer.calls, [("split-1to1", "app-fanout-layer")]
            )
            self.assertEqual(
                tuple(call[0] for call in executor.calls),
                (
                    "round-01-baseline",
                    "round-01-c2-decrease",
                    "round-01-split-1to1",
                    "round-01-app-fanout-layer",
                ),
            )
            evaluations = result.state.details["candidate_evaluations"]
            self.assertEqual(
                tuple(item["search_phase"] for item in evaluations),
                ("memory", "compute", "compute"),
            )

    def test_compute_selection_is_independent_of_candidate_order(self) -> None:
        accepted = []
        for order in (
            ("split-1to1", "app-fanout-layer"),
            ("app-fanout-layer", "split-1to1"),
        ):
            with self.subTest(order=order), tempfile.TemporaryDirectory(
                prefix="pipetune-controller-"
            ) as temp_dir:
                result, _controller, _executor, _materializer, _store = (
                    self._run_two_phase(
                        pathlib.Path(temp_dir),
                        compute_role="application",
                        compute_names=("split-1to1", "app-fanout-layer"),
                        compute_order=order,
                        throughput_by_label={
                            "baseline": 40.0,
                            "c2-decrease": 39.0,
                            "split-1to1": 41.0,
                            "app-fanout-layer": 43.0,
                        },
                        valid_impact_labels=frozenset(
                            ("split-1to1", "app-fanout-layer")
                        ),
                    )
                )
                accepted.append(result.accepted_trial_id)
        self.assertTrue(all("app-fanout-layer" in value for value in accepted))

    def test_equal_compute_candidates_use_an_order_independent_tie_break(self) -> None:
        accepted_actions = []
        for order in (
            ("split-1to1", "app-fanout-layer"),
            ("app-fanout-layer", "split-1to1"),
        ):
            with self.subTest(order=order), tempfile.TemporaryDirectory(
                prefix="pipetune-controller-"
            ) as temp_dir:
                result, _controller, _executor, _materializer, _store = (
                    self._run_two_phase(
                        pathlib.Path(temp_dir),
                        compute_role="application",
                        compute_names=("split-1to1", "app-fanout-layer"),
                        compute_order=order,
                        throughput_by_label={
                            "baseline": 40.0,
                            "c2-decrease": 39.0,
                            "split-1to1": 43.0,
                            "app-fanout-layer": 43.0,
                        },
                        valid_impact_labels=frozenset(
                            ("split-1to1", "app-fanout-layer")
                        ),
                    )
                )
                accepted_actions.append(
                    next(
                        item["action"]
                        for item in result.state.details["candidate_evaluations"]
                        if item["trial_id"] == result.accepted_trial_id
                    )
                )
        self.assertEqual(accepted_actions, ["app-fanout-layer"] * 2)

    def test_accepted_compute_restarts_the_next_round_in_memory_phase(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            root = pathlib.Path(temp_dir)
            first, controller, _executor, materializer, store = self._run_two_phase(
                root,
                compute_role="application",
                compute_names=("split-1to1", "app-fanout-layer"),
                throughput_by_label={
                    "baseline": 40.0,
                    "c2-decrease": 39.0,
                    "split-1to1": 41.0,
                    "app-fanout-layer": 43.0,
                },
                valid_impact_labels=frozenset(
                    ("split-1to1", "app-fanout-layer")
                ),
            )
            self.assertEqual(first.state.details["accepted_search_phase"], "compute")

            second = controller.run_round(store.status(), round_index=2)

            self.assertEqual(
                second.state.details["candidate_evaluations"][0]["search_phase"],
                "memory",
            )
            self.assertEqual(len(materializer.calls), 2)

    def test_recovers_a_published_compute_trial_without_rerunning_its_pair(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _identity, _accepted, state = create_store(root)
            executor = CrashDuringComputeExecutor(root)
            memory = ScriptedCandidates(())
            materializer = ScriptedActionMaterializer()
            actions = (
                _compute_action("split-1to1", role="application"),
                _compute_action("app-fanout-layer", role="application"),
            )

            def objective(summary: object) -> ObjectiveTrial:
                throughput = 40.0
                if "c2-decrease" in summary.trial_id:
                    throughput = 39.0
                elif "split-1to1" in summary.trial_id:
                    throughput = 41.0
                elif "app-fanout-layer" in summary.trial_id:
                    throughput = 43.0
                return _objective(summary.trial_id, throughput=throughput)

            controller = ColdStartController(
                root=root,
                store=store,
                config_tool=object(),
                executor=executor,
                policy=POLICY,
                candidate_generator=memory,
                action_materializer=materializer,
                diagnoser=lambda _summary: _diagnosis("P3", direction="rx"),
                compute_bottleneck_factory=lambda _summary: ComputeBottleneck(
                    role="application", metric="app_rx.completion"
                ),
                compute_action_factory=lambda _bottleneck, _topology, _runtime: actions,
                objective_factory=objective,
                impact_comparer=lambda _impact_spec, _baseline, _candidate, *, candidate_id: _impact(
                    candidate_id, accepted=True
                ),
                trial_id_factory=lambda purpose: purpose,
            )
            with self.assertRaisesRegex(RuntimeError, "process interruption"):
                controller.run_round(state, round_index=1)

            result = TuningLoop(
                store=store,
                round_runner=controller,
                policy=POLICY,
            ).run(store.status(), max_iterations=1)

            split_calls = [
                trial_id for trial_id, _target, _peer in executor.calls
                if "split-1to1" in trial_id
            ]
            self.assertEqual(len(split_calls), 1)
            self.assertTrue(
                any(
                    item["action"] == "split-1to1" and item["reused_visited"]
                    for item in result.state.details["visited_candidates"]
                )
            )

    def test_dispatcher_compute_runs_split_and_paired_growth(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            result, _controller, _executor, materializer, _store = self._run_two_phase(
                pathlib.Path(temp_dir),
                compute_role="dispatcher",
                compute_names=("split-1to1", "paired-colocated-growth"),
                throughput_by_label={
                    "baseline": 40.0,
                    "c2-decrease": 39.0,
                    "split-1to1": 42.0,
                    "paired-colocated-growth": 41.0,
                },
                valid_impact_labels=frozenset(
                    ("split-1to1", "paired-colocated-growth")
                ),
            )

            self.assertIn("split-1to1", result.accepted_trial_id)
            self.assertEqual(
                materializer.calls,
                [("split-1to1", "paired-colocated-growth")],
            )

    def test_memory_exhaustion_without_compute_evidence_rolls_back_cleanly(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            result, _controller, executor, materializer, store = self._run_two_phase(
                pathlib.Path(temp_dir),
                compute_role=None,
                compute_names=(),
                throughput_by_label={"baseline": 40.0, "c2-decrease": 39.0},
                valid_impact_labels=frozenset(),
            )

            self.assertIsNone(result.accepted_trial_id)
            self.assertEqual(
                result.state.details["reason"],
                "memory candidates exhausted without compute-bound evidence",
            )
            self.assertEqual(materializer.calls, [])
            self.assertEqual(len(executor.calls), 2)
            self.assertEqual(store.status().accepted, result.previous_accepted)

    def test_all_compute_candidates_rejected_and_visited_pairs_are_not_rerun(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            root = pathlib.Path(temp_dir)
            result, controller, executor, _materializer, store = self._run_two_phase(
                root,
                compute_role="application",
                compute_names=("split-1to1", "app-fanout-layer"),
                throughput_by_label={
                    "baseline": 40.0,
                    "c2-decrease": 39.0,
                    "split-1to1": 40.1,
                    "app-fanout-layer": 40.1,
                },
                valid_impact_labels=frozenset(
                    ("split-1to1", "app-fanout-layer")
                ),
            )
            self.assertEqual(result.state.details["reason"], "all candidates invalid")
            first_candidate_calls = len(executor.calls) - 1

            repeated = controller.run_round(
                store.status(),
                round_index=2,
            )

            self.assertIsNone(repeated.accepted_trial_id)
            self.assertEqual(len(executor.calls) - 1, first_candidate_calls + 1)

    def test_reuses_equal_probe_and_accepts_observed_best_after_all_trials(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            root = pathlib.Path(temp_dir)
            result, executor, store = self._run(
                root,
                order=("c2-decrease", "c1-increase", "c3-tx-decrease"),
                throughput_by_label={
                    "baseline": 40.0,
                    "probe": 44.0,
                    "c2-decrease": 42.0,
                    "c3-tx-decrease": 41.0,
                },
            )
            self.assertEqual(result.state.phase, "accepted")
            self.assertEqual(result.accepted_trial_id, "round-01-probe")
            self.assertEqual(result.probe_trial_id, "round-01-probe")
            self.assertTrue(result.reused_probe)
            self.assertEqual(len(executor.calls), 4)
            self.assertEqual(
                tuple(call[0] for call in executor.calls),
                (
                    "round-01-baseline",
                    "round-01-probe",
                    "round-01-c2-decrease",
                    "round-01-c3-tx-decrease",
                ),
            )
            self.assertEqual(len(store.status().attempts), 4)
            self.assertTrue(
                (root / store.status().accepted.target.path).read_text().startswith(
                    "target-c1=5"
                )
            )

    def test_best_selection_is_independent_of_candidate_order(self) -> None:
        accepted = []
        for order in (
            ("c1-increase", "c2-decrease", "c3-tx-decrease"),
            ("c3-tx-decrease", "c2-decrease", "c1-increase"),
        ):
            with self.subTest(order=order), tempfile.TemporaryDirectory(
                prefix="pipetune-controller-"
            ) as temp_dir:
                result, _executor, _store = self._run(
                    pathlib.Path(temp_dir),
                    order=order,
                    throughput_by_label={
                        "baseline": 40.0,
                        "probe": 40.1,
                        "c2-decrease": 42.0,
                        "c3-tx-decrease": 43.0,
                    },
                )
                accepted.append(result.accepted_trial_id)
        self.assertEqual(
            accepted,
            ["round-01-c3-tx-decrease", "round-01-c3-tx-decrease"],
        )

    def test_direct_p3_path_skips_probe_and_runs_one_candidate(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            result, executor, _store = self._run(
                pathlib.Path(temp_dir),
                order=(),
                throughput_by_label={"baseline": 40.0, "c2-decrease": 42.0},
                initial_point="P3",
            )
            self.assertIsNone(result.probe_trial_id)
            self.assertFalse(result.reused_probe)
            self.assertEqual(result.accepted_trial_id, "round-01-c2-decrease")
            self.assertEqual(len(executor.calls), 2)

    def test_controller_evidence_flows_into_the_final_report(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            root = pathlib.Path(temp_dir)
            round_result, _executor, store = self._run(
                root,
                order=(),
                throughput_by_label={"baseline": 40.0, "c2-decrease": 42.0},
                initial_point="P3",
            )
            convergence = TuningLoop(
                store=store,
                round_runner=object(),
                policy=POLICY,
            ).run(round_result.state, max_iterations=1)

            publish_session_outputs(root, store, convergence)

            record = json.loads((root / "iterations.jsonl").read_text())
            self.assertEqual(record["diagnosis"]["result"]["point"], "P3")
            self.assertEqual(
                set(record["diagnosis"]["counter_rates"]["baseline"]),
                {"llc_load", "llc_store", "io_read", "io_write"},
            )
            self.assertIn("Diagnosis evidence", (root / "report.md").read_text())

    def test_direct_p1_path_skips_probe_and_selects_best_action(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            result, executor, _store = self._run(
                pathlib.Path(temp_dir),
                order=(),
                throughput_by_label={
                    "baseline": 40.0,
                    "c1-decrease": 41.0,
                    "c3-tx-increase": 42.0,
                },
                initial_point="P1",
            )
            self.assertIsNone(result.probe_trial_id)
            self.assertEqual(result.accepted_trial_id, "round-01-c3-tx-increase")
            self.assertEqual(len(executor.calls), 3)

    def test_p2_probe_is_never_accepted_as_a_candidate(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            result, executor, _store = self._run(
                pathlib.Path(temp_dir),
                order=(),
                throughput_by_label={
                    "baseline": 40.0,
                    "probe": 50.0,
                    "c1-decrease": 42.0,
                },
                final_point="P2",
            )
            self.assertEqual(result.probe_trial_id, "round-01-probe")
            self.assertEqual(result.accepted_trial_id, "round-01-c1-decrease")
            self.assertFalse(result.reused_probe)
            self.assertEqual(len(executor.calls), 3)

    def test_negative_probe_is_not_reused_by_p2_c1_decrease(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            result, executor, _store = self._run(
                pathlib.Path(temp_dir),
                order=(),
                throughput_by_label={
                    "baseline": 40.0,
                    "probe": 50.0,
                    "c1-decrease": 42.0,
                },
                final_point="P2",
                probe_direction=-1,
            )
            self.assertEqual(result.probe_trial_id, "round-01-probe")
            self.assertEqual(result.accepted_trial_id, "round-01-c1-decrease")
            self.assertFalse(result.reused_probe)
            self.assertEqual(len(executor.calls), 3)

    def test_canonical_match_with_different_toml_bytes_does_not_reuse_probe(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            result, executor, _store = self._run(
                pathlib.Path(temp_dir),
                order=("c1-increase",),
                throughput_by_label={
                    "baseline": 40.0,
                    "probe": 44.0,
                    "c1-increase": 43.0,
                },
                format_drift=True,
            )
            self.assertEqual(result.accepted_trial_id, "round-01-c1-increase")
            self.assertFalse(result.reused_probe)
            self.assertEqual(len(executor.calls), 3)

    def test_rolls_back_when_no_candidate_significantly_improves(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            result, executor, store = self._run(
                pathlib.Path(temp_dir),
                order=("c1-increase", "c2-decrease"),
                throughput_by_label={
                    "baseline": 40.0,
                    "probe": 40.1,
                    "c2-decrease": 40.2,
                },
            )
            self.assertEqual(result.state.phase, "rolled_back")
            self.assertIsNone(result.accepted_trial_id)
            self.assertEqual(len(executor.calls), 3)
            self.assertEqual(store.status().accepted, result.previous_accepted)

    def test_tries_later_candidates_when_expected_impact_rejects_the_fastest(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            result, _executor, _store = self._run(
                pathlib.Path(temp_dir),
                order=("c1-increase", "c2-decrease"),
                throughput_by_label={
                    "baseline": 40.0,
                    "c1-increase": 45.0,
                    "c2-decrease": 42.0,
                },
                initial_point="P4",
                valid_impact_labels=frozenset(("c2-decrease",)),
            )

            self.assertEqual(result.state.phase, "accepted")
            self.assertIn("c2-decrease", result.accepted_trial_id)
            self.assertEqual(
                tuple(item.accepted for item in result.expected_impacts),
                (False, True),
            )
            self.assertEqual(
                tuple(
                    item["valid"]
                    for item in result.state.details["candidate_evaluations"]
                ),
                (False, True),
            )

    def test_all_candidates_invalid_when_neither_passes_both_gates(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            result, _executor, _store = self._run(
                pathlib.Path(temp_dir),
                order=("c1-increase", "c2-decrease"),
                throughput_by_label={
                    "baseline": 40.0,
                    "c1-increase": 45.0,
                    "c2-decrease": 39.0,
                },
                initial_point="P4",
                valid_impact_labels=frozenset(("c2-decrease",)),
            )

            self.assertEqual(result.state.phase, "rolled_back")
            self.assertEqual(
                result.state.details["reason"],
                "memory candidates exhausted without compute-bound evidence",
            )
            self.assertIsNone(result.accepted_trial_id)
            self.assertEqual(
                tuple(
                    item["valid"]
                    for item in result.state.details["candidate_evaluations"]
                ),
                (False, False),
            )

    def test_rejects_a_controller_state_outside_round_boundary(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _identity, _accepted, state = create_store(root)
            state = store.transition(state, phase="diagnose")
            controller = ColdStartController(
                root=root,
                store=store,
                config_tool=object(),
                executor=ScriptedExecutor(root),
                policy=POLICY,
            )
            with self.assertRaisesRegex(ControllerError, "round boundary"):
                controller.run_round(state, round_index=1)

    def test_retries_failures_and_resets_counter_after_each_success(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _identity, _accepted, state = create_store(root)
            executor = FlakyExecutor(root, (True, False, True, False))
            candidates = ScriptedCandidates(("c2-decrease",))
            sequence = iter(range(1, 20))
            controller = ColdStartController(
                root=root,
                store=store,
                config_tool=object(),
                executor=executor,
                policy=POLICY,
                candidate_generator=candidates,
                diagnoser=lambda _summary: _diagnosis("P3", direction="rx"),
                objective_factory=lambda summary: _objective(
                    summary.trial_id,
                    throughput=(
                        42.0 if "c2-decrease" in summary.trial_id else 40.0
                    ),
                ),
                impact_comparer=lambda _diagnosis, _baseline, _candidate, *, candidate_id: _impact(
                    candidate_id, accepted=True
                ),
                trial_id_factory=lambda purpose: f"{purpose}-{next(sequence):02d}",
                infrastructure_failure_limit=2,
            )
            result = controller.run_round(state, round_index=1)
            self.assertEqual(result.state.phase, "accepted")
            self.assertEqual(
                tuple(attempt.status for attempt in result.state.attempts),
                ("abandoned", "complete", "abandoned", "complete"),
            )
            self.assertEqual(len(executor.attempted), 4)

    def test_retries_an_unhealthy_baseline_without_advancing_the_round(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _identity, _accepted, state = create_store(root)
            executor = ScriptedExecutor(root)
            statuses = iter(("unhealthy_peer", "valid", "valid"))

            def objective(summary: object) -> ObjectiveTrial:
                status = next(statuses)
                if status != "valid":
                    return _rejected_objective(summary.trial_id, status)
                return _objective(
                    summary.trial_id,
                    throughput=(
                        42.0 if "c2-decrease" in summary.trial_id else 40.0
                    ),
                )

            controller = ColdStartController(
                root=root,
                store=store,
                config_tool=object(),
                executor=executor,
                policy=POLICY,
                candidate_generator=ScriptedCandidates(("c2-decrease",)),
                diagnoser=lambda _summary: _diagnosis("P3", direction="rx"),
                objective_factory=objective,
                impact_comparer=lambda _diagnosis, _baseline, _candidate, *, candidate_id: _impact(
                    candidate_id, accepted=True
                ),
                trial_id_factory=lambda purpose: purpose,
                infrastructure_failure_limit=2,
            )

            result = controller.run_round(state, round_index=1)

            self.assertEqual(result.state.phase, "accepted")
            self.assertEqual(result.baseline_trial_id, "round-01-baseline-health-retry-01")
            self.assertEqual(
                tuple(call[0] for call in executor.calls),
                (
                    "round-01-baseline",
                    "round-01-baseline-health-retry-01",
                    "round-01-c2-decrease",
                ),
            )
            self.assertEqual(len(result.state.attempts), 3)

    def test_bounds_repeated_unhealthy_baselines_as_infrastructure_failure(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _identity, _accepted, state = create_store(root)
            executor = ScriptedExecutor(root)
            controller = ColdStartController(
                root=root,
                store=store,
                config_tool=object(),
                executor=executor,
                policy=POLICY,
                objective_factory=lambda summary: _rejected_objective(
                    summary.trial_id, "unhealthy_peer"
                ),
                trial_id_factory=lambda purpose: purpose,
                infrastructure_failure_limit=2,
            )
            loop = TuningLoop(store=store, round_runner=controller, policy=POLICY)

            result = loop.run(state, max_iterations=4)

            self.assertEqual(result.stop_reason, "infrastructure_failure_limit")
            self.assertEqual(result.completed_rounds, 0)
            self.assertEqual(result.infrastructure_failures, 2)
            self.assertEqual(len(executor.calls), 2)
            self.assertEqual(loop.run(result.state, max_iterations=4).state, result.state)

    def test_combines_unhealthy_and_execution_failures_for_required_evidence(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _identity, _accepted, state = create_store(root)
            executor = FlakyExecutor(root, (False, True))
            controller = ColdStartController(
                root=root,
                store=store,
                config_tool=object(),
                executor=executor,
                policy=POLICY,
                objective_factory=lambda summary: _rejected_objective(
                    summary.trial_id, "unhealthy_peer"
                ),
                trial_id_factory=lambda purpose: purpose,
                infrastructure_failure_limit=2,
            )

            result = TuningLoop(
                store=store,
                round_runner=controller,
                policy=POLICY,
            ).run(state, max_iterations=4)

            self.assertEqual(result.stop_reason, "infrastructure_failure_limit")
            self.assertEqual(result.infrastructure_failures, 2)
            self.assertEqual(result.completed_rounds, 0)
            self.assertEqual(executor.attempted, [
                "round-01-baseline",
                "round-01-baseline-health-retry-01",
            ])

    def test_structurally_invalid_baseline_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _identity, _accepted, state = create_store(root)
            executor = ScriptedExecutor(root)
            controller = ColdStartController(
                root=root,
                store=store,
                config_tool=object(),
                executor=executor,
                policy=POLICY,
                objective_factory=lambda summary: _rejected_objective(
                    summary.trial_id, "invalid"
                ),
                trial_id_factory=lambda purpose: purpose,
            )

            with self.assertRaisesRegex(
                ControllerError, "baseline objective is structurally invalid"
            ):
                controller.run_round(state, round_index=1)
            self.assertEqual(len(executor.calls), 1)
            resumed = TuningLoop(
                store=store,
                round_runner=controller,
                policy=POLICY,
            ).run(store.status(), max_iterations=4)
            self.assertEqual(resumed.stop_reason, "invalid_control_evidence")
            self.assertEqual(resumed.completed_rounds, 0)
            self.assertEqual(len(executor.calls), 1)

    def test_retries_an_unhealthy_required_probe_before_diagnosis(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _identity, _accepted, state = create_store(root)
            executor = ScriptedExecutor(root)
            statuses = iter(("valid", "unhealthy_peer", "valid"))
            probe = ProbeSpec(
                knob="knobs.runtime.application_core_count",
                direction=1,
                baseline_value=4,
                candidate_value=5,
            )

            def objective(summary: object) -> ObjectiveTrial:
                status = next(statuses)
                if status != "valid":
                    return _rejected_objective(summary.trial_id, status)
                return _objective(
                    summary.trial_id,
                    throughput=(40.0 if "baseline" in summary.trial_id else 42.0),
                )

            controller = ColdStartController(
                root=root,
                store=store,
                config_tool=object(),
                executor=executor,
                policy=POLICY,
                candidate_generator=ScriptedCandidates(("c1-increase",)),
                diagnoser=lambda _summary, probe_summary=None: (
                    _diagnosis("probe_required", probe=probe)
                    if probe_summary is None
                    else _diagnosis("P4", completed_probe=probe)
                ),
                objective_factory=objective,
                impact_comparer=lambda _diagnosis, _baseline, _candidate, *, candidate_id: _impact(
                    candidate_id, accepted=True
                ),
                trial_id_factory=lambda purpose: purpose,
                infrastructure_failure_limit=2,
            )

            result = controller.run_round(state, round_index=1)

            self.assertEqual(result.state.phase, "accepted")
            self.assertEqual(
                tuple(call[0] for call in executor.calls),
                (
                    "round-01-baseline",
                    "round-01-probe",
                    "round-01-probe-health-retry-01",
                ),
            )
            self.assertEqual(
                result.probe_trial_id,
                "round-01-probe-health-retry-01",
            )

    def test_bounds_repeated_unhealthy_required_probes(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _identity, _accepted, state = create_store(root)
            executor = ScriptedExecutor(root)
            statuses = iter(("valid", "unhealthy_peer", "unhealthy_peer"))
            probe = ProbeSpec(
                knob="knobs.runtime.application_core_count",
                direction=1,
                baseline_value=4,
                candidate_value=5,
            )

            def objective(summary: object) -> ObjectiveTrial:
                status = next(statuses)
                return (
                    _objective(summary.trial_id, throughput=40.0)
                    if status == "valid"
                    else _rejected_objective(summary.trial_id, status)
                )

            controller = ColdStartController(
                root=root,
                store=store,
                config_tool=object(),
                executor=executor,
                policy=POLICY,
                candidate_generator=ScriptedCandidates(("c1-increase",)),
                diagnoser=lambda _summary, probe_summary=None: (
                    _diagnosis("probe_required", probe=probe)
                    if probe_summary is None
                    else _diagnosis("P4", completed_probe=probe)
                ),
                objective_factory=objective,
                trial_id_factory=lambda purpose: purpose,
                infrastructure_failure_limit=2,
            )

            result = TuningLoop(
                store=store,
                round_runner=controller,
                policy=POLICY,
            ).run(state, max_iterations=4)

            self.assertEqual(result.stop_reason, "infrastructure_failure_limit")
            self.assertEqual(result.completed_rounds, 0)
            self.assertEqual(result.infrastructure_failures, 2)
            self.assertEqual(len(executor.calls), 3)

    def test_structurally_invalid_required_probe_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _identity, _accepted, state = create_store(root)
            executor = ScriptedExecutor(root)
            statuses = iter(("valid", "invalid"))
            probe = ProbeSpec(
                knob="knobs.runtime.application_core_count",
                direction=1,
                baseline_value=4,
                candidate_value=5,
            )

            def objective(summary: object) -> ObjectiveTrial:
                status = next(statuses)
                return (
                    _objective(summary.trial_id, throughput=40.0)
                    if status == "valid"
                    else _rejected_objective(summary.trial_id, status)
                )

            controller = ColdStartController(
                root=root,
                store=store,
                config_tool=object(),
                executor=executor,
                policy=POLICY,
                candidate_generator=ScriptedCandidates(("c1-increase",)),
                diagnoser=lambda _summary: _diagnosis(
                    "probe_required", probe=probe
                ),
                objective_factory=objective,
                trial_id_factory=lambda purpose: purpose,
            )

            with self.assertRaisesRegex(
                ControllerError,
                "required probe objective is structurally invalid",
            ):
                controller.run_round(state, round_index=1)
            self.assertEqual(len(executor.calls), 2)

    def test_stops_after_bounded_consecutive_infrastructure_failures(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _identity, _accepted, state = create_store(root)
            executor = FlakyExecutor(root, (True, True, True))
            sequence = iter(range(1, 20))
            controller = ColdStartController(
                root=root,
                store=store,
                config_tool=object(),
                executor=executor,
                policy=POLICY,
                trial_id_factory=lambda purpose: f"{purpose}-{next(sequence):02d}",
                infrastructure_failure_limit=2,
            )
            with self.assertRaises(InfrastructureFailureLimit) as raised:
                controller.run_round(state, round_index=1)
            self.assertEqual(raised.exception.consecutive_failures, 2)
            self.assertEqual(len(executor.attempted), 2)
            self.assertEqual(
                tuple(
                    attempt.status for attempt in raised.exception.state.attempts
                ),
                ("abandoned", "abandoned"),
            )

    def test_consumes_a_published_trial_after_lost_acknowledgement(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _identity, _accepted, state = create_store(root)
            executor = PublishThenFailExecutor(root)
            candidates = ScriptedCandidates(("c2-decrease",))
            controller = ColdStartController(
                root=root,
                store=store,
                config_tool=object(),
                executor=executor,
                policy=POLICY,
                candidate_generator=candidates,
                diagnoser=lambda _summary: _diagnosis("P3", direction="rx"),
                objective_factory=lambda summary: _objective(
                    summary.trial_id,
                    throughput=(
                        42.0 if "c2-decrease" in summary.trial_id else 40.0
                    ),
                ),
                impact_comparer=lambda _diagnosis, _baseline, _candidate, *, candidate_id: _impact(
                    candidate_id, accepted=True
                ),
                trial_id_factory=lambda purpose: purpose,
                infrastructure_failure_limit=1,
            )
            result = controller.run_round(state, round_index=1)
            self.assertEqual(result.state.phase, "accepted")
            self.assertEqual(
                tuple(attempt.status for attempt in result.state.attempts),
                ("complete", "complete"),
            )

    def _interrupted_round_controller(
        self,
        root: pathlib.Path,
        store: object,
    ) -> ColdStartController:
        sequence = iter(range(1, 30))
        return ColdStartController(
            root=root,
            store=store,
            config_tool=object(),
            executor=ScriptedExecutor(root),
            policy=POLICY,
            candidate_generator=ScriptedCandidates(("c2-decrease",)),
            diagnoser=lambda _summary: _diagnosis("P3", direction="rx"),
            objective_factory=lambda summary: _objective(
                summary.trial_id,
                throughput=(
                    42.0 if "c2-decrease" in summary.trial_id else 40.0
                ),
            ),
            impact_comparer=lambda _diagnosis, _baseline, _candidate, *, candidate_id: _impact(
                candidate_id, accepted=True
            ),
            trial_id_factory=lambda purpose: f"{purpose}-{next(sequence):02d}",
            infrastructure_failure_limit=2,
        )

    def test_recovers_a_cross_process_running_trial_without_manifest(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _identity, _accepted, state = create_store(root)
            state = store.checkpoint(
                state,
                details={
                    "round": 0,
                    "active_trial": {
                        "round_index": 1,
                        "round_attempt": 1,
                        "purpose": "round-01-baseline",
                        "failure_key": None,
                    },
                },
            )
            running = store.start_trial(state, "interrupted-baseline")
            collision = root / "configs/round-0001"
            collision.mkdir(parents=True)
            (collision / "preserve").write_text("old attempt\n", encoding="utf-8")
            controller = self._interrupted_round_controller(root, store)
            result = TuningLoop(
                store=store,
                round_runner=controller,
                policy=POLICY,
            ).run(running, max_iterations=1)
            self.assertEqual(result.stop_reason, "max_iterations")
            self.assertEqual(result.completed_rounds, 1)
            self.assertEqual(
                tuple(attempt.status for attempt in result.state.attempts),
                ("abandoned", "complete", "complete"),
            )
            self.assertTrue((collision / "preserve").is_file())
            self.assertTrue((root / "configs/round-0001-restart-02").is_dir())

    def test_recovers_a_cross_process_published_running_trial(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _identity, _accepted, state = create_store(root)
            state = store.checkpoint(
                state,
                details={
                    "round": 0,
                    "active_trial": {
                        "round_index": 1,
                        "round_attempt": 1,
                        "purpose": "round-01-baseline",
                        "failure_key": None,
                    },
                },
            )
            running = store.start_trial(state, "interrupted-baseline")
            _publish_fixture_trial(
                root / "trials/interrupted-baseline",
                "interrupted-baseline",
            )
            controller = self._interrupted_round_controller(root, store)
            result = TuningLoop(
                store=store,
                round_runner=controller,
                policy=POLICY,
            ).run(running, max_iterations=1)
            self.assertEqual(result.completed_rounds, 1)
            self.assertEqual(
                tuple(attempt.status for attempt in result.state.attempts),
                ("complete", "complete", "complete"),
            )

    def test_classifies_published_required_control_before_recovery(self) -> None:
        for status, prior_failures, expected_stop in (
            ("unhealthy_peer", 1, "infrastructure_failure_limit"),
            ("invalid", 0, "invalid_control_evidence"),
        ):
            with self.subTest(status=status), tempfile.TemporaryDirectory(
                prefix="pipetune-controller-"
            ) as temp_dir:
                root = pathlib.Path(temp_dir)
                store, _identity, _accepted, state = create_store(root)
                state = store.checkpoint(
                    state,
                    details={
                        "round": 1,
                        "round_attempt": 1,
                        "baseline_health_failures": prior_failures,
                        "active_trial": {
                            "round_index": 1,
                            "round_attempt": 1,
                            "purpose": "round-01-baseline",
                            "failure_key": "baseline_health_failures",
                        },
                    },
                )
                running = store.start_trial(state, "published-required-control")
                _publish_fixture_trial(
                    root / "trials/published-required-control",
                    "published-required-control",
                )
                executor = ScriptedExecutor(root)
                controller = ColdStartController(
                    root=root,
                    store=store,
                    config_tool=object(),
                    executor=executor,
                    policy=POLICY,
                    objective_factory=lambda summary, status=status: _rejected_objective(
                        summary.trial_id, status
                    ),
                    infrastructure_failure_limit=2,
                )

                result = TuningLoop(
                    store=store,
                    round_runner=controller,
                    policy=POLICY,
                ).run(running, max_iterations=1)

                self.assertEqual(result.stop_reason, expected_stop)
                self.assertEqual(result.completed_rounds, 0)
                self.assertEqual(executor.calls, [])
                self.assertEqual(result.state.attempts[-1].status, "complete")
                self.assertEqual(
                    TuningLoop(
                        store=store,
                        round_runner=controller,
                        policy=POLICY,
                    ).run(result.state, max_iterations=1).state,
                    result.state,
                )

    def test_recovers_round_two_baseline_from_accepted_boundary(self) -> None:
        for running_trial in (False, True):
            with self.subTest(running_trial=running_trial), tempfile.TemporaryDirectory(
                prefix="pipetune-controller-"
            ) as temp_dir:
                root = pathlib.Path(temp_dir)
                store, _identity, _accepted, state = create_store(root)
                controller = self._interrupted_round_controller(root, store)
                first = controller.run_round(state, round_index=1)
                self.assertIsNotNone(first.accepted_objective)
                state = store.checkpoint(
                    first.state,
                    details=TuningLoop._convergence_details(
                        first.state,
                        completed_rounds=1,
                        best=first.state.accepted,
                        best_objective=first.accepted_objective,
                        stop_reason=None,
                        infrastructure_failures=0,
                    ),
                )
                accepted_target = (root / state.accepted.target.path).read_bytes()
                accepted_peer = (root / state.accepted.peer.path).read_bytes()
                state = store.checkpoint(
                    state,
                    details={
                        **state.details,
                        "active_trial": {
                            "round_index": 2,
                            "round_attempt": 1,
                            "purpose": "round-02-baseline",
                            "failure_key": None,
                        },
                    },
                )
                if running_trial:
                    state = store.start_trial(state, "interrupted-round2-baseline")

                result = TuningLoop(
                    store=store,
                    round_runner=controller,
                    policy=POLICY,
                ).run(state, max_iterations=2)

                self.assertEqual(result.stop_reason, "max_iterations")
                self.assertEqual(result.completed_rounds, 2)
                self.assertEqual(
                    controller._executor.calls[-2][1:],
                    (accepted_target, accepted_peer),
                )
                self.assertTrue(
                    (root / "configs/round-0002-restart-02").is_dir()
                )
                expected_tail = (
                    ("abandoned", "complete")
                    if running_trial
                    else ("complete",)
                )
                self.assertEqual(
                    tuple(
                        attempt.status
                        for attempt in result.state.attempts[-len(expected_tail) :]
                    ),
                    expected_tail,
                )

    def test_resume_honors_a_persisted_infrastructure_failure_limit(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _identity, _accepted, state = create_store(root)
            for trial_id in ("failed-01", "failed-02"):
                state = store.start_trial(state, trial_id)
                state = store.abandon_active_trial(state)
            state = store.checkpoint(
                state,
                details={
                    **state.details,
                    "active_trial": {
                        "round_index": 1,
                        "round_attempt": 1,
                        "purpose": "round-01-baseline",
                        "failure_key": None,
                    },
                },
            )
            controller = self._interrupted_round_controller(root, store)
            loop = TuningLoop(
                store=store,
                round_runner=controller,
                policy=POLICY,
            )

            result = loop.run(state, max_iterations=4)

            self.assertEqual(result.stop_reason, "infrastructure_failure_limit")
            self.assertEqual(result.completed_rounds, 0)
            self.assertEqual(result.infrastructure_failures, 2)
            self.assertEqual(controller._executor.calls, [])
            self.assertNotIn("active_trial", result.state.details)
            resumed = loop.run(result.state, max_iterations=4)
            self.assertEqual(resumed.state, result.state)
            self.assertEqual(resumed.completed_rounds, 0)

    def test_interrupted_required_retry_consumes_the_remaining_failure_budget(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-controller-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _identity, _accepted, state = create_store(root)
            state = store.checkpoint(
                state,
                details={
                    "round": 1,
                    "round_attempt": 1,
                    "baseline_health_failures": 1,
                    "active_trial": {
                        "round_index": 1,
                        "round_attempt": 1,
                        "purpose": "round-01-baseline-health-retry-01",
                        "failure_key": "baseline_health_failures",
                    },
                },
            )
            state = store.start_trial(
                state,
                "interrupted-baseline-health-retry",
            )
            controller = self._interrupted_round_controller(root, store)

            result = TuningLoop(
                store=store,
                round_runner=controller,
                policy=POLICY,
            ).run(state, max_iterations=1)

            self.assertEqual(result.stop_reason, "infrastructure_failure_limit")
            self.assertEqual(result.infrastructure_failures, 2)
            self.assertEqual(result.completed_rounds, 0)
            self.assertEqual(controller._executor.calls, [])
            self.assertEqual(result.state.attempts[-1].status, "abandoned")


class MeasureTrialExecutorTest(unittest.TestCase):
    def test_composes_runner_with_two_isolated_role_ordered_cold_starts(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-executor-") as temp_dir:
            root = pathlib.Path(temp_dir)
            target = root / "target.toml"
            peer = root / "peer.toml"
            target.write_text("target\n", encoding="utf-8")
            peer.write_text("peer\n", encoding="utf-8")
            tool = FakeConfigTool("server")
            for document in tool.documents.values():
                document["tuning"]["warmup_windows"] = 0
                document["tuning"]["sample_windows"] = 1
                document["tuning"]["noise"] = {
                    "throughput_relative_floor": 0.01,
                    "latency_relative_floor": 0.03,
                    "stage_time_relative_floor": 0.05,
                    "stall_time_relative_floor": 0.05,
                    "miss_rate_percentage_point_floor": 0.5,
                }
                document["other"]["iterations"] = 1
            target_spec = tool.resolve(
                endpoint_id="target",
                config_path=target,
                binary_override=None,
            ).spec
            peer_spec = tool.resolve(
                endpoint_id="peer",
                config_path=peer,
                binary_override=None,
            ).spec
            events: list[str] = []
            transports = {
                "target": ScriptedTransport(target_spec, events),
                "peer": ScriptedTransport(peer_spec, events),
            }
            executor = MeasureTrialExecutor(
                request=MeasureRequest(
                    target_config=target,
                    peer_config=peer,
                    output=root / "unused",
                    configure_binary=root / "axio-configure",
                ),
                config_tool=tool,
                transport_factory=lambda spec: transports[spec.endpoint_id],
                sleeper=lambda _seconds: None,
            )
            summaries = []
            for trial_id in ("trial-0001", "trial-0002"):
                summaries.append(
                    executor.execute(
                        trial_id=trial_id,
                        target_config=target,
                        peer_config=peer,
                        destination=root / "tuning/trials" / trial_id,
                    )
                )
            self.assertEqual(
                [summary.trial_id for summary in summaries],
                ["trial-0001", "trial-0002"],
            )
            self.assertEqual(
                [event for event in events if event.startswith("start:")],
                [
                    "start:server:target",
                    "start:client:peer",
                    "start:server:target",
                    "start:client:peer",
                ],
            )
            self.assertEqual(transports["target"].cleaned, 2)
            self.assertEqual(transports["peer"].cleaned, 2)
            self.assertEqual(len(tool.materializations), 4)
            for trial_id in ("trial-0001", "trial-0002"):
                self.assertTrue(
                    (root / "tuning/trials" / trial_id / "trial.json").is_file()
                )

    def test_atomically_publishes_only_the_trial_subtree_and_removes_stage(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-executor-") as temp_dir:
            root = pathlib.Path(temp_dir)
            target = root / "target.toml"
            peer = root / "peer.toml"
            target.write_text("target\n", encoding="utf-8")
            peer.write_text("peer\n", encoding="utf-8")
            destination = root / "tuning/trials/trial-0001"

            def fake_measure(request, **kwargs):
                self.assertEqual(kwargs["trial_id_factory"](), "trial-0001")
                built = build_session(request.output.parent, target_role="server")
                built.replace(request.output)
                return types.SimpleNamespace(
                    manifest=artifact_ref(
                        request.output,
                        request.output / "trials/trial-0001/trial.json",
                        schema="pipetune.trial/v1",
                    )
                )

            executor = MeasureTrialExecutor(
                request=MeasureRequest(
                    target_config=target,
                    peer_config=peer,
                    output=root / "unused",
                    configure_binary=root / "axio-configure",
                ),
                measure_function=fake_measure,
            )
            summary = executor.execute(
                trial_id="trial-0001",
                target_config=target,
                peer_config=peer,
                destination=destination,
            )
            self.assertEqual(summary.trial_id, "trial-0001")
            self.assertTrue((destination / "trial.json").is_file())
            self.assertFalse(any((root / "tuning").glob(".trial-0001.*")))
            self.assertFalse((root / "tuning/session.json").exists())

    def test_failure_removes_stage_without_publishing_a_partial_trial(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-executor-") as temp_dir:
            root = pathlib.Path(temp_dir)
            target = root / "target.toml"
            peer = root / "peer.toml"
            target.write_text("target\n", encoding="utf-8")
            peer.write_text("peer\n", encoding="utf-8")
            destination = root / "tuning/trials/trial-0001"

            def fail_measure(request, **_kwargs):
                request.output.mkdir(parents=True)
                (request.output / "partial").write_text("partial")
                raise MeasureError("scripted cold-start failure")

            executor = MeasureTrialExecutor(
                request=MeasureRequest(
                    target_config=target,
                    peer_config=peer,
                    output=root / "unused",
                    configure_binary=root / "axio-configure",
                ),
                measure_function=fail_measure,
            )
            with self.assertRaisesRegex(TrialExecutionError, "cold-start failure"):
                executor.execute(
                    trial_id="trial-0001",
                    target_config=target,
                    peer_config=peer,
                    destination=destination,
                )
            self.assertFalse(destination.exists())
            self.assertFalse(any((root / "tuning").glob(".trial-0001.*")))

    def test_does_not_reclassify_controller_invariant_as_infrastructure(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-executor-") as temp_dir:
            root = pathlib.Path(temp_dir)
            target = root / "target.toml"
            peer = root / "peer.toml"
            target.write_text("target\n", encoding="utf-8")
            peer.write_text("peer\n", encoding="utf-8")

            def fail_invariant(_request, **_kwargs):
                raise ControllerError("scripted invariant")

            executor = MeasureTrialExecutor(
                request=MeasureRequest(
                    target_config=target,
                    peer_config=peer,
                    output=root / "unused",
                    configure_binary=root / "axio-configure",
                ),
                measure_function=fail_invariant,
            )
            with self.assertRaisesRegex(ControllerError, "scripted invariant") as raised:
                executor.execute(
                    trial_id="trial-0001",
                    target_config=target,
                    peer_config=peer,
                    destination=root / "tuning/trials/trial-0001",
                )
            self.assertIs(type(raised.exception), ControllerError)
            self.assertNotIsInstance(raised.exception, TrialExecutionError)

    def test_rejects_wrong_manifest_identity_without_adopting_its_subtree(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-executor-") as temp_dir:
            root = pathlib.Path(temp_dir)
            target = root / "target.toml"
            peer = root / "peer.toml"
            target.write_text("target\n", encoding="utf-8")
            peer.write_text("peer\n", encoding="utf-8")
            destination = root / "tuning/trials/trial-0001"

            def wrong_measure(request, **_kwargs):
                built = build_session(request.output.parent, target_role="server")
                built.replace(request.output)
                return types.SimpleNamespace(
                    manifest=types.SimpleNamespace(
                        path="trials/trial-other/trial.json"
                    )
                )

            executor = MeasureTrialExecutor(
                request=MeasureRequest(
                    target_config=target,
                    peer_config=peer,
                    output=root / "unused",
                    configure_binary=root / "axio-configure",
                ),
                measure_function=wrong_measure,
            )
            with self.assertRaisesRegex(ControllerError, "unexpected"):
                executor.execute(
                    trial_id="trial-0001",
                    target_config=target,
                    peer_config=peer,
                    destination=destination,
                )
            self.assertFalse(destination.exists())
            self.assertFalse(any((root / "tuning").glob(".trial-0001.*")))

    def test_refuses_a_preexisting_destination_before_starting_measurement(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-executor-") as temp_dir:
            root = pathlib.Path(temp_dir)
            target = root / "target.toml"
            peer = root / "peer.toml"
            target.write_text("target\n", encoding="utf-8")
            peer.write_text("peer\n", encoding="utf-8")
            destination = root / "tuning/trials/trial-0001"
            destination.mkdir(parents=True)
            called = []

            def should_not_run(*_args, **_kwargs):
                called.append(True)

            executor = MeasureTrialExecutor(
                request=MeasureRequest(
                    target_config=target,
                    peer_config=peer,
                    output=root / "unused",
                    configure_binary=root / "axio-configure",
                ),
                measure_function=should_not_run,
            )
            with self.assertRaisesRegex(ControllerError, "already exists"):
                executor.execute(
                    trial_id="trial-0001",
                    target_config=target,
                    peer_config=peer,
                    destination=destination,
                )
            self.assertEqual(called, [])


if __name__ == "__main__":
    unittest.main()
