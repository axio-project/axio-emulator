from __future__ import annotations

import dataclasses
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
    MeasureTrialExecutor,
)
from pipetune.diagnosis import Diagnosis, ProbeSpec, Statistic
from pipetune.objective import ObjectivePolicy, ObjectiveTrial
from pipetune.runner import MeasureRequest
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


def _publish_fixture_trial(
    destination: pathlib.Path, trial_id: str
) -> None:
    with tempfile.TemporaryDirectory(prefix=".controller-fixture-") as temp_dir:
        session = build_session(pathlib.Path(temp_dir), target_role="server")
        source = session / "trials/trial-0001"
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(source, destination)
    manifest = destination / "trial.json"
    document = json.loads(manifest.read_text(encoding="utf-8"))
    document["trial_id"] = trial_id
    manifest.write_text(
        json.dumps(document, allow_nan=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


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
        _publish_fixture_trial(destination, trial_id)
        return types.SimpleNamespace(trial_id=trial_id)


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

        controller = ColdStartController(
            root=root,
            store=store,
            config_tool=object(),
            executor=executor,
            policy=POLICY,
            candidate_generator=candidates,
            diagnoser=diagnose,
            objective_factory=objective,
            trial_id_factory=lambda purpose: purpose,
        )
        result = controller.run_round(state, round_index=1)
        return result, executor, store

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
                raise RuntimeError("scripted cold-start failure")

            executor = MeasureTrialExecutor(
                request=MeasureRequest(
                    target_config=target,
                    peer_config=peer,
                    output=root / "unused",
                    configure_binary=root / "axio-configure",
                ),
                measure_function=fail_measure,
            )
            with self.assertRaisesRegex(RuntimeError, "cold-start failure"):
                executor.execute(
                    trial_id="trial-0001",
                    target_config=target,
                    peer_config=peer,
                    destination=destination,
                )
            self.assertFalse(destination.exists())
            self.assertFalse(any((root / "tuning").glob(".trial-0001.*")))

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
