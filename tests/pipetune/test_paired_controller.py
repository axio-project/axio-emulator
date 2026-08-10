from __future__ import annotations

import dataclasses
import pathlib
import shutil
import tempfile
import unittest

from pipetune.candidates import Candidate, canonical_config_sha256
from pipetune.controller import ColdStartController, ControllerError, TuningLoop
from pipetune.diagnosis import Statistic, summarize_trial
from pipetune.objective import ObjectiveTrial
from pipetune.paired_search import PairedSearchState, SearchMode
from pipetune.search_policy import ComputeBottleneck, SearchAction
from pipetune.topology_state import TopologyState
from tests.pipetune.test_controller import (
    POLICY,
    _diagnosis,
    _fixture_canonical_peer,
    _fixture_tuning,
    _publish_fixture_trial,
)
from tests.pipetune.test_search_policy import _config
from tests.pipetune.test_session import create_store


def _target(count: int) -> dict[str, object]:
    document = _config(
        application_count=count,
        dispatcher_count=count,
        budget=16,
        profile="colocated-1to1",
    )
    document["deployment"]["role"] = "server"
    document["tuning"] = _fixture_tuning()
    return document


def _rate(value: float) -> Statistic:
    return Statistic(
        samples=(value,),
        median=value,
        mad=0.0,
        uncertainty=0.5,
        unit="percentage points",
    )


class TrajectoryExecutor:
    rates = {
        16: (82.0, 92.0),
        15: (80.0, 90.0),
        8: (35.0, 38.0),
        11: (45.0, 55.0),
        9: (39.0, 42.0),
    }

    def __init__(self) -> None:
        self.counts: list[int] = []

    @staticmethod
    def _count(path: pathlib.Path) -> int:
        payload = path.read_text(encoding="utf-8")
        return int(payload.split("=", 1)[1]) if payload.startswith("count=") else 16

    def execute(
        self,
        *,
        trial_id: str,
        target_config: pathlib.Path,
        peer_config: pathlib.Path,
        destination: pathlib.Path,
    ) -> object:
        del peer_config
        count = self._count(target_config)
        self.counts.append(count)
        _publish_fixture_trial(destination, trial_id, candidate_label="baseline")
        summary = summarize_trial(destination / "trial.json")
        llc, io = self.rates[count]
        return dataclasses.replace(
            summary,
            canonical_target=_target(count),
            canonical_peer=_fixture_canonical_peer(),
            counters={
                "llc_load": _rate(llc),
                "io_write": _rate(io),
                "llc_store": _rate(10.0),
                "io_read": _rate(10.0),
            },
            missing_counters=(),
        )


class TrajectoryMaterializer:
    def __init__(self) -> None:
        self.calls: list[tuple[int, int]] = []

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
        if not actions:
            return ()
        source = TrajectoryExecutor._count(target_config)
        output_dir.mkdir(parents=True)
        candidates = []
        for index, action in enumerate(actions, 1):
            overrides = dict(action.overrides)
            count = overrides["knobs.runtime.application_core_count"]
            self.calls.append((source, count))
            root = output_dir / f"candidate-{index:02d}-{action.name}"
            root.mkdir()
            target_path = root / "target.toml"
            peer_path = root / "peer.toml"
            target_path.write_text(f"count={count}\n", encoding="utf-8")
            shutil.copyfile(peer_config, peer_path)
            canonical_target = _target(count)
            canonical_peer = _fixture_canonical_peer()
            candidates.append(
                Candidate(
                    candidate_id=f"candidate-{index:02d}-{action.name}",
                    action=action,
                    target_config=target_path,
                    peer_config=peer_path,
                    target_sha256=canonical_config_sha256(canonical_target),
                    peer_sha256=canonical_config_sha256(canonical_peer),
                    canonical_target=canonical_target,
                    canonical_peer=canonical_peer,
                )
            )
        return tuple(candidates)


class PairedControllerTrajectoryTest(unittest.TestCase):
    def test_compute_cursor_must_match_the_accepted_topology(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="pipetune-paired-controller-"
        ) as temp_dir:
            root = pathlib.Path(temp_dir)
            paired_state = PairedSearchState(
                direction="rx",
                mode=SearchMode.COMPUTE,
                next_count=None,
                selected_count=8,
            )
            store, _identity, _accepted, state = create_store(
                root,
                details={"round": 0, "paired_search": paired_state.to_document()},
            )
            executor = TrajectoryExecutor()

            def objective(summary: object) -> ObjectiveTrial:
                return ObjectiveTrial(
                    trial_id=summary.trial_id,
                    status="valid",
                    client_p999=Statistic((2.0,), 2.0, 0.0, 0.02, "us"),
                    server_throughput=Statistic((50.0,), 50.0, 0.0, 0.5, "Mpps"),
                    rejection_reason=None,
                )

            controller = ColdStartController(
                root=root,
                store=store,
                config_tool=object(),
                executor=executor,
                policy=POLICY,
                candidate_generator=lambda *args, **kwargs: (),
                action_materializer=TrajectoryMaterializer(),
                diagnoser=lambda _summary: _diagnosis(
                    "paired_reduction_required", direction="rx"
                ),
                compute_bottleneck_factory=lambda _summary: (
                    ComputeBottleneck.application("app_rx.completion")
                ),
                compute_action_factory=lambda *_args: (),
                objective_factory=objective,
                trial_id_factory=lambda purpose: purpose,
            )

            with self.assertRaisesRegex(ControllerError, "selected count"):
                TuningLoop(
                    store=store,
                    round_runner=controller,
                    policy=POLICY,
                ).run(state, max_iterations=1)

    def test_binary_cursor_crosses_lower_e2e_trials_and_computes_from_eight(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-paired-controller-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _identity, accepted, state = create_store(root)
            executor = TrajectoryExecutor()
            materializer = TrajectoryMaterializer()
            compute_sources: list[int] = []
            throughput = {16: 50.0, 15: 49.0, 8: 45.0, 11: 44.0, 9: 46.0}

            def objective(summary: object) -> ObjectiveTrial:
                count = TopologyState.from_config(
                    summary.canonical_target
                ).application_count
                return ObjectiveTrial(
                    trial_id=summary.trial_id,
                    status="valid",
                    client_p999=Statistic((2.0,), 2.0, 0.0, 0.02, "us"),
                    server_throughput=Statistic(
                        (throughput[count],),
                        throughput[count],
                        0.0,
                        throughput[count] * 0.01,
                        "Mpps",
                    ),
                    rejection_reason=None,
                )

            def compute_actions(
                _bottleneck: ComputeBottleneck,
                topology: TopologyState,
                _runtime: dict[str, object],
            ) -> tuple[SearchAction, ...]:
                compute_sources.append(topology.application_count)
                return ()

            controller = ColdStartController(
                root=root,
                store=store,
                config_tool=object(),
                executor=executor,
                policy=POLICY,
                candidate_generator=lambda *args, **kwargs: (),
                action_materializer=materializer,
                diagnoser=lambda _summary: _diagnosis(
                    "paired_reduction_required", direction="rx"
                ),
                compute_bottleneck_factory=lambda _summary: (
                    ComputeBottleneck.application("app_rx.completion")
                ),
                compute_action_factory=compute_actions,
                objective_factory=objective,
                trial_id_factory=lambda purpose: purpose,
            )

            result = TuningLoop(
                store=store,
                round_runner=controller,
                policy=POLICY,
            ).run(state, max_iterations=5)

            self.assertEqual(
                materializer.calls,
                [(16, 15), (15, 8), (8, 11), (8, 9)],
            )
            self.assertEqual(compute_sources, [8])
            self.assertEqual(result.stop_reason, "no_legal_candidate")
            self.assertEqual(result.best, accepted)
            self.assertEqual(result.best_trial_id, "round-01-baseline")

    def test_target_enqueue_drop_switches_compute_from_last_healthy_cursor(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-paired-controller-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _identity, _accepted, state = create_store(root)
            executor = TrajectoryExecutor()
            executor.rates = {
                16: (82.0, 92.0),
                15: (80.0, 90.0),
                8: (20.0, 80.0),
                4: (8.0, 55.0),
            }
            materializer = TrajectoryMaterializer()
            compute_sources: list[int] = []
            throughput = {16: 50.0, 15: 49.0, 8: 55.0}

            def objective(summary: object) -> ObjectiveTrial:
                count = TopologyState.from_config(
                    summary.canonical_target
                ).application_count
                if count == 4:
                    return ObjectiveTrial(
                        trial_id=summary.trial_id,
                        status="unhealthy_peer",
                        client_p999=None,
                        server_throughput=None,
                        rejection_reason="drop: target dispatcher enqueue",
                    )
                return ObjectiveTrial(
                    trial_id=summary.trial_id,
                    status="valid",
                    client_p999=Statistic((2.0,), 2.0, 0.0, 0.02, "us"),
                    server_throughput=Statistic(
                        (throughput[count],),
                        throughput[count],
                        0.0,
                        throughput[count] * 0.01,
                        "Mpps",
                    ),
                    rejection_reason=None,
                )

            def compute_actions(
                _bottleneck: ComputeBottleneck,
                topology: TopologyState,
                _runtime: dict[str, object],
            ) -> tuple[SearchAction, ...]:
                compute_sources.append(topology.application_count)
                return ()

            controller = ColdStartController(
                root=root,
                store=store,
                config_tool=object(),
                executor=executor,
                policy=POLICY,
                candidate_generator=lambda *args, **kwargs: (),
                action_materializer=materializer,
                diagnoser=lambda _summary: _diagnosis(
                    "paired_reduction_required", direction="rx"
                ),
                compute_bottleneck_factory=lambda _summary: (
                    ComputeBottleneck.application("app_rx.completion")
                ),
                compute_action_factory=compute_actions,
                objective_factory=objective,
                trial_id_factory=lambda purpose: purpose,
            )

            result = TuningLoop(
                store=store,
                round_runner=controller,
                policy=POLICY,
            ).run(state, max_iterations=4)

            self.assertEqual(materializer.calls, [(16, 15), (15, 8), (8, 4)])
            self.assertEqual(compute_sources, [8])
            self.assertEqual(result.stop_reason, "no_legal_candidate")


if __name__ == "__main__":
    unittest.main()
