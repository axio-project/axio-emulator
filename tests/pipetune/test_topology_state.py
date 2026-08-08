from __future__ import annotations

import unittest

from pipetune.topology_state import TopologyState, TopologyStateError


def _config(
    application_ids: list[int],
    dispatcher_ids: list[int],
    workspace_ids: list[int],
    groups: list[dict[str, object]],
) -> dict[str, object]:
    return {
        "deployment": {
            "topology": {
                "application_workspaces": application_ids,
                "dispatcher_workspaces": dispatcher_ids,
                "workloads": [{"id": 1, "groups": groups}],
                "workspaces": [
                    {"id": workspace_id, "cpu_core": workspace_id}
                    for workspace_id in workspace_ids
                ],
            }
        },
        "knobs": {
            "runtime": {
                "application_core_count": sum(
                    len(group["applications"]) for group in groups
                ),
                "dispatcher_queue_count": len({group["dispatcher"] for group in groups}),
            }
        },
    }


def config_16a_16d_colocated() -> dict[str, object]:
    ids = list(range(16))
    return _config(
        ids,
        ids,
        ids,
        [{"dispatcher": value, "applications": [value]} for value in ids],
    )


def config_8a_8d_colocated() -> dict[str, object]:
    ids = list(range(8))
    return _config(
        ids,
        ids,
        ids,
        [{"dispatcher": value, "applications": [value]} for value in ids],
    )


def config_8a_8d_split() -> dict[str, object]:
    applications = list(range(8))
    dispatchers = list(range(8, 16))
    return _config(
        applications,
        dispatchers,
        applications + dispatchers,
        [
            {"dispatcher": dispatcher, "applications": [application]}
            for application, dispatcher in zip(applications, dispatchers)
        ],
    )


def config_16a_8d_fanout() -> dict[str, object]:
    applications = list(range(16))
    dispatchers = list(range(8))
    return _config(
        applications,
        dispatchers,
        applications,
        [
            {"dispatcher": dispatcher, "applications": applications[index::8]}
            for index, dispatcher in enumerate(dispatchers)
        ],
    )


class TopologyStateTest(unittest.TestCase):
    def test_derives_colocated_topology_state(self) -> None:
        colocated = TopologyState.from_config(config_16a_16d_colocated())
        self.assertEqual(
            (colocated.application_count, colocated.dispatcher_count), (16, 16)
        )
        self.assertEqual(colocated.overlap_count, 16)
        self.assertEqual(colocated.physical_core_count, 16)
        self.assertEqual(colocated.physical_core_budget, 16)
        self.assertEqual(set(colocated.fanout_by_dispatcher.values()), {1})
        with self.assertRaises(TypeError):
            colocated.fanout_by_dispatcher[0] = 2
        with self.assertRaises(TypeError):
            dict.__setitem__(colocated.fanout_by_dispatcher, 0, 2)

        compact = TopologyState.from_config(config_8a_8d_colocated())
        self.assertEqual(
            (compact.application_count, compact.dispatcher_count), (8, 8)
        )
        self.assertEqual(compact.overlap_count, 8)

    def test_derives_split_and_balanced_fanout_topologies(self) -> None:
        split = TopologyState.from_config(config_8a_8d_split())
        self.assertEqual(split.overlap_count, 0)
        self.assertEqual(split.physical_core_count, 16)

        fanout = TopologyState.from_config(config_16a_8d_fanout())
        self.assertTrue(fanout.balanced_fanout)
        self.assertEqual(set(fanout.fanout_by_dispatcher.values()), {2})

    def test_reports_unbalanced_fanout(self) -> None:
        document = config_16a_8d_fanout()
        groups = document["deployment"]["topology"]["workloads"][0]["groups"]
        groups[0]["applications"].append(groups[1]["applications"].pop())
        state = TopologyState.from_config(document)
        self.assertFalse(state.balanced_fanout)

    def test_rejects_undefined_workspace_ids(self) -> None:
        document = config_8a_8d_split()
        document["deployment"]["topology"]["workloads"][0]["groups"][0][
            "applications"
        ] = [99]
        with self.assertRaises(TopologyStateError):
            TopologyState.from_config(document)

    def test_rejects_role_pool_and_active_id_absent_from_workspaces(self) -> None:
        document = config_8a_8d_colocated()
        document["deployment"]["topology"]["workspaces"].append(
            {"id": 100, "cpu_core": 100}
        )
        document["deployment"]["topology"]["application_workspaces"][0] = 99
        document["deployment"]["topology"]["workloads"][0]["groups"][0][
            "applications"
        ] = [99]
        with self.assertRaises(TopologyStateError):
            TopologyState.from_config(document)

    def test_rejects_knob_and_active_count_mismatch(self) -> None:
        document = config_8a_8d_colocated()
        document["knobs"]["runtime"]["application_core_count"] = 7
        with self.assertRaises(TopologyStateError):
            TopologyState.from_config(document)

    def test_rejects_physical_budget_violation(self) -> None:
        document = config_8a_8d_split()
        document["deployment"]["topology"]["workspaces"] = [
            {"id": value, "cpu_core": value} for value in range(15)
        ]
        with self.assertRaises(TopologyStateError):
            TopologyState.from_config(document)

    def test_rejects_duplicate_application_ownership(self) -> None:
        document = config_8a_8d_colocated()
        document["deployment"]["topology"]["workloads"][0]["groups"][1][
            "applications"
        ] = [0]
        with self.assertRaises(TopologyStateError):
            TopologyState.from_config(document)

    def test_rejects_duplicate_workspace_ids_and_empty_active_roles(self) -> None:
        duplicate_workspaces = config_8a_8d_colocated()
        duplicate_workspaces["deployment"]["topology"]["workspaces"].append(
            {"id": 0, "cpu_core": 0}
        )
        with self.assertRaises(TopologyStateError):
            TopologyState.from_config(duplicate_workspaces)

        empty_roles = config_8a_8d_colocated()
        empty_roles["deployment"]["topology"]["workloads"][0]["groups"] = []
        empty_roles["knobs"]["runtime"]["application_core_count"] = 0
        empty_roles["knobs"]["runtime"]["dispatcher_queue_count"] = 0
        with self.assertRaises(TopologyStateError):
            TopologyState.from_config(empty_roles)


if __name__ == "__main__":
    unittest.main()
