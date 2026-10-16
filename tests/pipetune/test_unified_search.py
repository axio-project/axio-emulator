"""Small policy replay: no endpoint processes or full-suite dependency."""
import dataclasses
import types
import unittest

from pipetune.search_policy import C1, C2, ComputeBottleneck, compute_actions, memory_actions
from pipetune.topology_state import TopologyState


def topology(n, budget=16, split=False, layers=1):
    apps = tuple(range(n * layers))
    dispatchers = tuple(range(n * layers, n * layers + n)) if split else tuple(range(n))
    return TopologyState(apps, dispatchers, 0 if split else n,
                         n * layers + (n if split else 0), budget,
                         dict.fromkeys(dispatchers, layers), 0 if split else n)


def runtime(t):
    return dict(application_core_count=t.application_count,
                dispatcher_queue_count=t.dispatcher_count,
                app_rx_batch_size=64, dispatcher_rx_batch_size=32, nic_rx_post_size=128,
                app_tx_batch_size=32, dispatcher_tx_batch_size=32, nic_tx_post_size=32)


def diagnosis(point):
    return types.SimpleNamespace(point=point, direction="rx", required_probe=None,
                                 hypotheses=("P2", "P4") if point == "inconclusive" else ())


class UnifiedSearchTest(unittest.TestCase):
    def test_midpoint_respects_workload_lower_bound_and_logs_skipped_growth(self):
        from pipetune.search_state import SearchState
        t = dataclasses.replace(topology(7), minimum_dispatcher_count=4)
        state = SearchState().at_cursor("rx", t, 80)
        state = dataclasses.replace(state, accelerate=True)
        skipped = []
        actions = state.actions(diagnosis("inconclusive"), t, runtime(t), skipped=skipped)
        self.assertEqual(dict(actions[0].overrides), {C1: 5, C2: 5})
        self.assertTrue(any("P4:c1-increase" in reason for reason in skipped))

    def test_boundary_and_midpoint_resume_replay(self):
        from pipetune.search_state import SearchState
        state = SearchState().at_cursor("rx", topology(16), 80.0)
        state = state.after_selection(topology(15), 79.0, objective_accepted=False)
        actions = state.actions(diagnosis("inconclusive"), topology(15), runtime(topology(15)))
        self.assertEqual(dict(actions[0].overrides), {C1: 8, C2: 8})
        self.assertTrue(any(action.kind == "c3" for action in actions))
        self.assertEqual(SearchState.from_document(state.to_document()), state)
        state = state.after_selection(topology(8), 40.0, objective_accepted=False)
        state = state.at_cursor("rx", topology(8), 40.0)
        actions = state.actions(diagnosis("P1"), topology(8), runtime(topology(8)))
        self.assertEqual(SearchState.from_document(state.to_document()).actions(
            diagnosis("P1"), topology(8), runtime(topology(8))), actions)
        self.assertEqual([a.profile for a in actions if a.kind == "topology"],
                         ["colocated-1to1", "split-1to1"])
        state = state.after_local_comparison(topology(8), accepted=False)
        self.assertEqual(state.local_count, 7)
        state = state.after_local_comparison(topology(8), accepted=False)
        self.assertTrue(state.local_exhausted)
        self.assertTrue(state.compute_eligible(40.0))
        self.assertFalse(state.compute_eligible(None))
        self.assertFalse(state.compute_eligible(40.01))

    def test_missing_llc_and_exhausted_count_do_not_unlock_compute(self):
        from pipetune.search_state import SearchState
        t = topology(1)
        for rate in (None, 80.0):
            state = SearchState().at_cursor("rx", t, rate)
            self.assertEqual(state.actions(diagnosis("P2"), t, runtime(t)), ())
            self.assertFalse(state.compute_eligible(rate))
        state = SearchState().at_cursor("rx", topology(8), 35.0)
        state = state.at_cursor("rx", topology(8), None)
        self.assertIsNone(state.local_count)
        self.assertFalse(state.compute_eligible(None))

    def test_memory_count_intentions_preserve_one_to_one_layout(self):
        for split in (False, True):
            t = topology(8, split=split)
            for point in ("P1", "P2", "P3"):
                actions = memory_actions(diagnosis(point), t, runtime(t))
                self.assertEqual(dict(actions[0].overrides), {C1: 7, C2: 7})
                self.assertEqual(actions[0].profile, "split-1to1" if split else "colocated-1to1")
                self.assertEqual(actions[0].impact.metric, point)

    def test_pending_hypotheses_keep_c3_and_deduplicate_count(self):
        t = topology(16)
        actions = memory_actions(diagnosis("inconclusive"), t, runtime(t))
        self.assertEqual(len(actions), 2)
        self.assertEqual(dict(actions[0].overrides), {C1: 15, C2: 15})
        self.assertEqual(dict(actions[1].overrides)["knobs.runtime.nic_rx_post_size"], 64)

    def test_dispatcher_growth_preserves_split_and_never_grows_queue_alone(self):
        t = topology(6, split=True)
        actions = compute_actions(ComputeBottleneck.dispatcher("dispatcher_rx.completion"), t, runtime(t))
        self.assertEqual(len(actions), 1)
        self.assertEqual(dict(actions[0].overrides), {C1: 7, C2: 7})
        self.assertEqual(actions[0].profile, "split-1to1")

    def test_existing_fanout_can_add_another_complete_layer(self):
        t = topology(4, layers=2)
        actions = compute_actions(ComputeBottleneck.application("app_rx.completion"), t, runtime(t))
        self.assertEqual(dict(actions[-1].overrides), {C1: 12})


if __name__ == "__main__":
    unittest.main()
