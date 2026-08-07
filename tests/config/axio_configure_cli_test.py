#!/usr/bin/env python3
"""Integration contract for the axio-configure command-line tool."""

import json
import pathlib
import subprocess
import sys
import tempfile


def run(binary: pathlib.Path, *arguments: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(binary), *(str(argument) for argument in arguments)],
        check=False,
        capture_output=True,
        text=True,
    )


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def require_success(result: subprocess.CompletedProcess[str], command: str) -> None:
    require(
        result.returncode == 0,
        f"{command} failed ({result.returncode}): {result.stderr}",
    )


def leaf_paths(value: object, prefix: str = "") -> set[str]:
    if isinstance(value, dict):
        paths: set[str] = set()
        for key, child in value.items():
            child_prefix = f"{prefix}.{key}" if prefix else key
            paths.update(leaf_paths(child, child_prefix))
        return paths
    if isinstance(value, list):
        if value and isinstance(value[0], dict):
            paths: set[str] = set()
            for child in value:
                paths.update(leaf_paths(child, prefix + "[]"))
            return paths
        return {prefix}
    return {prefix}


def main() -> int:
    if len(sys.argv) != 3:
        print(
            "usage: axio_configure_cli_test.py <axio-configure> <source-root>",
            file=sys.stderr,
        )
        return 2

    binary = pathlib.Path(sys.argv[1])
    source_root = pathlib.Path(sys.argv[2])
    legacy_topology = (
        source_root / "tests/config/schema-v1.legacy-topology.toml"
    )
    valid = source_root / "tests/config/schema-v1.valid.toml"
    no_tuning = source_root / "tests/config/schema-v1.no-tuning.toml"
    documented_example = source_root / "config/schema-v1.example.toml"

    with tempfile.TemporaryDirectory(prefix="axio-configure-test-") as temp_dir:
        temp = pathlib.Path(temp_dir)

        validated = run(binary, "validate", valid)
        require_success(validated, "validate deployment topology")
        require(validated.stdout == "valid\n", "validate output must be stable")

        deployment_dump = run(binary, "dump", valid)
        require_success(deployment_dump, "dump deployment topology")
        deployment_document = json.loads(deployment_dump.stdout)
        require(
            deployment_document["deployment"]["topology"]
            ["application_workspaces"] == [4]
            and deployment_document["deployment"]["topology"]
            ["dispatcher_workspaces"] == [0],
            "canonical dump must preserve deployment topology resources",
        )
        require(
            deployment_document["deployment"]["topology"]
            ["workloads"][0]["groups"][0]
            == {"dispatcher": 0, "applications": [4]},
            "canonical dump must preserve the default workload mapping",
        )

        legacy_rejected = run(binary, "validate", legacy_topology)
        require(
            legacy_rejected.returncode == 2,
            "top-level workload/workspace arrays must be rejected",
        )
        require(
            "work" in legacy_rejected.stderr,
            "legacy topology diagnostic must name the rejected path",
        )

        documented = run(binary, "validate", documented_example)
        require_success(documented, "validate documented schema example")

        no_tuning_validated = run(binary, "validate", no_tuning)
        require_success(no_tuning_validated, "validate without tuning policy")
        no_tuning_dump = run(binary, "dump", no_tuning)
        require_success(no_tuning_dump, "dump without tuning policy")
        require(
            "tuning" not in json.loads(no_tuning_dump.stdout),
            "canonical dump must not synthesize an absent tuning policy",
        )

        no_tuning_materialized = temp / "no-tuning-materialized.toml"
        require_success(
            run(
                binary,
                "materialize",
                no_tuning,
                no_tuning_materialized,
                "--set-json",
                '{"other.iterations":31}',
            ),
            "materialize without tuning policy",
        )
        require(
            "tuning" not in json.loads(
                run(binary, "dump", no_tuning_materialized).stdout
            ),
            "materialize must preserve absent tuning policy",
        )

        no_tuning_header = temp / "no-tuning-generated.h"
        require_success(
            run(binary, "generate", no_tuning, no_tuning_header),
            "generate without tuning policy",
        )
        require(
            "#define AXIO_CONFIG_MTU 2048" in no_tuning_header.read_text(),
            "optional tuning must not affect build-header generation",
        )

        no_tuning_peer = temp / "no-tuning-peer.toml"
        no_tuning_peer.write_text(
            no_tuning.read_text()
            .replace('role = "server"', 'role = "client"')
            .replace('host = "axio-server.example.net"',
                     'host = "axio-client.example.net"')
            .replace('local_ip = "10.0.0.1"', 'local_ip = "10.0.0.2"')
            .replace('remote_ip = "10.0.0.2"', 'remote_ip = "10.0.0.1"')
            .replace('local_mac = "10:70:fd:00:00:01"',
                     'local_mac = "10:70:fd:00:00:02"')
            .replace('remote_mac = "10:70:fd:00:00:02"',
                     'remote_mac = "10:70:fd:00:00:01"')
        )
        require_success(
            run(binary, "validate-pair", no_tuning, no_tuning_peer),
            "validate pair without tuning policy",
        )
        no_tuning_pair_local = temp / "no-tuning-pair-local.toml"
        no_tuning_pair_peer = temp / "no-tuning-pair-peer.toml"
        require_success(
            run(
                binary,
                "materialize-pair",
                no_tuning,
                no_tuning_peer,
                no_tuning_pair_local,
                no_tuning_pair_peer,
                "--set-json",
                "{}",
            ),
            "materialize pair without tuning policy",
        )
        for output in (no_tuning_pair_local, no_tuning_pair_peer):
            dumped_output = run(binary, "dump", output)
            require_success(dumped_output, "dump no-tuning pair output")
            require(
                "tuning" not in json.loads(dumped_output.stdout),
                "pair materialization must preserve absent tuning policy",
            )

        pair = run(
            binary,
            "validate-pair",
            source_root / "config/client.toml",
            source_root / "config/server.toml",
        )
        require_success(pair, "validate-pair")
        require(pair.stdout == "valid pair\n", "pair output must be stable")

        invalid = temp / "unknown.toml"
        invalid.write_text(
            valid.read_text().replace(
                "window_seconds = 1",
                "window_seconds = 1\nunknown_option = 7",
                1,
            )
        )
        rejected = run(binary, "validate", invalid)
        require(rejected.returncode == 2, "invalid config must return status 2")
        require(
            "other.unknown_option" in rejected.stderr,
            "invalid config diagnostic must name the key",
        )

        dumped_once = run(binary, "dump", valid)
        dumped_twice = run(binary, "dump", valid)
        require_success(dumped_once, "dump")
        require_success(dumped_twice, "dump repeat")
        require(
            dumped_once.stdout == dumped_twice.stdout,
            "canonical JSON must be deterministic",
        )
        document = json.loads(dumped_once.stdout)
        expected_leaf_paths = {
            "schema_version",
            "deployment.role", "deployment.numa_node", "deployment.host",
            "deployment.ssh_port", "deployment.ssh_user",
            "deployment.workdir", "deployment.use_sudo",
            "network.backend", "network.roce_transport",
            "network.physical_port", "network.rx_ring_entries",
            "network.tx_ring_entries", "network.local_ip",
            "network.remote_ip", "network.local_mac",
            "network.remote_mac", "network.device_pcie",
            "network.device_name",
            "handler.message_handler", "handler.packet_handler",
            "handler.apply_new_mbuf", "handler.request_payload_bytes",
            "handler.response_payload_bytes", "handler.app_ticks_per_message",
            "knobs.build.inflight_limit_enabled",
            "knobs.build.inflight_messages", "knobs.build.mtu",
            "knobs.build.mempool_handler",
            "knobs.runtime.application_core_count",
            "knobs.runtime.dispatcher_queue_count",
            "knobs.runtime.app_tx_batch_size",
            "knobs.runtime.app_rx_batch_size",
            "knobs.runtime.dispatcher_tx_batch_size",
            "knobs.runtime.dispatcher_rx_batch_size",
            "knobs.runtime.nic_tx_post_size",
            "knobs.runtime.nic_rx_post_size",
            "other.iterations", "other.window_seconds",
            "other.mempool_size", "other.mempool_cache_size",
            "metrics.jsonl_path", "metrics.human_output",
            "tuning.max_iterations", "tuning.latency_slo_us",
            "tuning.warmup_windows", "tuning.sample_windows",
            "tuning.infrastructure_failure_limit",
            "tuning.noise.throughput_relative_floor",
            "tuning.noise.latency_relative_floor",
            "tuning.noise.stage_time_relative_floor",
            "tuning.noise.stall_time_relative_floor",
            "tuning.noise.miss_rate_percentage_point_floor",
            "deployment.topology.application_workspaces",
            "deployment.topology.dispatcher_workspaces",
            "deployment.topology.workspaces[].id",
            "deployment.topology.workspaces[].cpu_core",
            "deployment.topology.workloads[].id",
            "deployment.topology.workloads[].pipeline",
            "deployment.topology.workloads[].remote_dispatchers",
            "deployment.topology.workloads[].groups[].dispatcher",
            "deployment.topology.workloads[].groups[].applications",
        }
        require(
            leaf_paths(document) == expected_leaf_paths,
            "canonical dump schema leaves changed: "
            f"missing={sorted(expected_leaf_paths - leaf_paths(document))}, "
            f"extra={sorted(leaf_paths(document) - expected_leaf_paths)}",
        )
        require(document["schema_version"] == 1, "dump must include schema")
        require(document["network"]["backend"] == "dpdk", "dump must type enums")
        require(document["other"]["iterations"] == 30, "dump must include other")
        require(document["knobs"]["runtime"]["application_core_count"] == 1,
                "dump must include C1")
        require("build" not in document and "runtime" not in document,
                "dump must reject the old top-level layout")
        require(
            len(document["deployment"]["topology"]["workspaces"]) == 2,
            "dump must include deployment topology workspaces",
        )

        generated = temp / "axio_config_generated.h"
        generated_result = run(binary, "generate", valid, generated)
        require_success(generated_result, "generate")
        header = generated.read_text()
        generated_stat = generated.stat()
        require(
            "#define AXIO_CONFIG_SCHEMA_VERSION 1" in header,
            "generated header must include schema metadata",
        )
        require(
            '#define AXIO_CONFIG_BUILD_FINGERPRINT "fnv1a64:' in header,
            "generated header must include a build fingerprint",
        )
        require(
            "#define AXIO_CONFIG_MTU 2048" in header,
            "generated header must include build knobs",
        )
        require(
            "AXIO_CONFIG_RUNTIME" not in header and "10.0.0.1" not in header,
            "generated header must exclude runtime and network values",
        )

        runtime_only = temp / "runtime-only.toml"
        runtime_override = run(
            binary,
            "materialize",
            valid,
            runtime_only,
            "--set-json",
            '{"other.iterations":31}',
        )
        require_success(runtime_override, "materialize runtime-only config")
        runtime_generate = run(binary, "generate", runtime_only, generated)
        require_success(runtime_generate, "generate runtime-only config")
        runtime_stat = generated.stat()
        require(generated.read_text() == header, "runtime knob changed build header")
        require(
            runtime_stat.st_mtime_ns == generated_stat.st_mtime_ns
            and runtime_stat.st_ino == generated_stat.st_ino,
            "unchanged build header must preserve mtime and inode",
        )

        runtime_network = temp / "runtime-network.toml"
        runtime_network_override = run(
            binary,
            "materialize",
            valid,
            runtime_network,
            "--set-json",
            '{"network.physical_port":1}',
        )
        require_success(runtime_network_override, "materialize runtime network config")
        runtime_network_generate = run(binary, "generate", runtime_network, generated)
        require_success(runtime_network_generate, "generate runtime network config")
        require(
            generated.read_text() == header,
            "runtime network setting changed build header",
        )

        build_changed = temp / "build-changed.toml"
        build_override = run(
            binary,
            "materialize",
            valid,
            build_changed,
            "--set-json",
            '{"knobs.build.mtu":4096}',
        )
        require_success(build_override, "materialize build config")
        build_generate = run(binary, "generate", build_changed, generated)
        require_success(build_generate, "generate build config")
        require(generated.read_text() != header, "build knob did not change header")

        handler_changed = temp / "handler-changed.toml"
        handler_override = run(
            binary,
            "materialize",
            valid,
            handler_changed,
            "--set-json",
            '{"handler.message_handler":"l_app"}',
        )
        require_success(handler_override, "materialize handler config")
        handler_generate = run(binary, "generate", handler_changed, generated)
        require_success(handler_generate, "generate handler config")
        require(generated.read_text() != header, "handler did not change build header")

        materialized = temp / "materialized.toml"
        overrides = json.dumps(
            {
                "other.iterations": 41,
                "knobs.build.inflight_messages": 512,
                "network.local_mac": "10:70:fd:00:00:0a",
            },
            separators=(",", ":"),
        )
        materialize_result = run(
            binary,
            "materialize",
            valid,
            materialized,
            "--set-json",
            overrides,
        )
        require_success(materialize_result, "materialize")
        materialized_dump = run(binary, "dump", materialized)
        require_success(materialized_dump, "dump materialized")
        updated = json.loads(materialized_dump.stdout)
        require(updated["other"]["iterations"] == 41, "other override lost")
        require(updated["knobs"]["build"]["inflight_messages"] == 512,
                "build knob override lost")
        require(
            updated["network"]["local_mac"] == "10:70:fd:00:00:0a",
            "string override lost",
        )
        require(
            json.loads(run(binary, "dump", valid).stdout)["other"]["iterations"] == 30,
            "materialize must not mutate its input",
        )

        scalable = temp / "scalable.toml"
        scalable.write_text(
            valid.read_text()
            .replace(
                "application_workspaces = [4]",
                "application_workspaces = [4, 5]",
            )
            .replace(
                "dispatcher_workspaces = [0]",
                "dispatcher_workspaces = [0, 1]",
            )
            .replace(
                "[[deployment.topology.workspaces]]\nid = 4\ncpu_core = 4",
                "[[deployment.topology.workspaces]]\n"
                "id = 4\ncpu_core = 4\n\n"
                "[[deployment.topology.workspaces]]\n"
                "id = 1\ncpu_core = 1\n\n"
                "[[deployment.topology.workspaces]]\n"
                "id = 5\ncpu_core = 5",
            )
        )
        scalable_peer = temp / "scalable-peer.toml"
        scalable_peer.write_text(
            scalable.read_text()
            .replace('role = "server"', 'role = "client"')
            .replace('host = "axio-server.example.net"',
                     'host = "axio-client.example.net"')
            .replace('local_ip = "10.0.0.1"', 'local_ip = "10.0.0.2"')
            .replace('remote_ip = "10.0.0.2"', 'remote_ip = "10.0.0.1"')
            .replace('local_mac = "10:70:fd:00:00:01"',
                     'local_mac = "10:70:fd:00:00:02"')
            .replace('remote_mac = "10:70:fd:00:00:02"',
                     'remote_mac = "10:70:fd:00:00:01"')
        )
        scaled = temp / "scaled.toml"
        scaled_peer = temp / "scaled-peer.toml"
        scale_result = run(
            binary,
            "materialize-pair",
            scalable,
            scalable_peer,
            scaled,
            scaled_peer,
            "--set-json",
            '{"knobs.runtime.application_core_count":2,'
            '"knobs.runtime.dispatcher_queue_count":2}',
        )
        require_success(scale_result, "materialize C1/C2 topology pair")
        scaled_dump = json.loads(run(binary, "dump", scaled).stdout)
        scaled_peer_dump = json.loads(run(binary, "dump", scaled_peer).stdout)
        groups = scaled_dump["deployment"]["topology"]["workloads"][0]["groups"]
        require(
            groups
            == [
                {"applications": [4], "dispatcher": 0},
                {"applications": [5], "dispatcher": 1},
            ],
            "C1/C2 materialization did not produce the deterministic topology",
        )
        require(
            scaled_dump["deployment"]["topology"]["workloads"][0]["remote_dispatchers"] == [0, 1]
            and scaled_peer_dump["deployment"]["topology"]["workloads"][0]["remote_dispatchers"]
            == [0, 1],
            "pair materialization did not rebuild reciprocal remote routes",
        )
        require_success(
            run(binary, "validate-pair", scaled, scaled_peer),
            "validate materialized C1/C2 pair",
        )

        rejected_local = temp / "rejected-local.toml"
        rejected_peer = temp / "rejected-peer.toml"
        rejected_pair = run(
            binary,
            "materialize-pair",
            scalable,
            scalable_peer,
            rejected_local,
            rejected_peer,
            "--set-json",
            '{"knobs.runtime.application_core_count":1,'
            '"knobs.runtime.dispatcher_queue_count":2}',
        )
        require(rejected_pair.returncode == 2,
                "invalid pair materialization must fail")
        require(
            not rejected_local.exists() and not rejected_peer.exists(),
            "failed pair materialization must not publish either output",
        )

        rejected_single_topology = temp / "rejected-single-topology.toml"
        single_topology = run(
            binary,
            "materialize",
            scalable,
            rejected_single_topology,
            "--set-json",
            '{"knobs.runtime.application_core_count":2}',
        )
        require(single_topology.returncode == 2,
                "single-endpoint C1/C2 materialization must fail")
        require("materialize-pair" in single_topology.stderr,
                "single-endpoint topology error must direct users to pair mode")
        require(not rejected_single_topology.exists(),
                "rejected single-endpoint topology output must be atomic")

        scaled_checked_client = temp / "checked-client-3x3.toml"
        scaled_checked_server = temp / "checked-server-3x3.toml"
        checked_scale = run(
            binary,
            "materialize-pair",
            source_root / "config/client.toml",
            source_root / "config/server.toml",
            scaled_checked_client,
            scaled_checked_server,
            "--set-json",
            '{"knobs.runtime.application_core_count":3,'
            '"knobs.runtime.dispatcher_queue_count":3}',
        )
        require_success(checked_scale, "scale checked multi-workload pair")
        require_success(
            run(binary, "validate-pair", scaled_checked_client,
                scaled_checked_server),
            "validate scaled checked multi-workload pair",
        )
        checked_scaled_dump = json.loads(
            run(binary, "dump", scaled_checked_client).stdout
        )
        require(
            sum(len(workload["groups"])
                for workload in checked_scaled_dump["deployment"]["topology"]["workloads"]) == 3,
            "checked multi-workload pair did not materialize C2=3",
        )

        independent_client = source_root / "config/client.toml"
        independent_server = source_root / "config/server.toml"
        for dispatcher_count in (3, 1, 4):
            output_client = temp / f"checked-client-4x{dispatcher_count}.toml"
            output_server = temp / f"checked-server-4x{dispatcher_count}.toml"
            independent_scale = run(
                binary,
                "materialize-pair",
                independent_client,
                independent_server,
                output_client,
                output_server,
                "--set-json",
                '{"knobs.runtime.application_core_count":4,'
                f'"knobs.runtime.dispatcher_queue_count":{dispatcher_count}'
                "}",
            )
            require_success(
                independent_scale,
                f"materialize checked pair at C1=4,C2={dispatcher_count}",
            )
            require_success(
                run(binary, "validate-pair", output_client, output_server),
                f"validate checked pair at C1=4,C2={dispatcher_count}",
            )
            independent_dump = json.loads(
                run(binary, "dump", output_client).stdout
            )
            active_dispatchers = {
                group["dispatcher"]
                for workload in independent_dump["deployment"]["topology"]["workloads"]
                for group in workload["groups"]
            }
            require(
                independent_dump["knobs"]["runtime"]
                ["application_core_count"] == 4
                and len(active_dispatchers) == dispatcher_count,
                "independent C2 materialization changed C1 or missed C2",
            )
            for application, workload in enumerate(
                    independent_dump["deployment"]["topology"]["workloads"]):
                workload_applications = sorted(
                    member
                    for group in workload["groups"]
                    for member in group["applications"]
                )
                require(
                    workload_applications == [application],
                    "independent C2 materialization moved an application "
                    "between workloads",
                )
            independent_client = output_client
            independent_server = output_server

        rejected_output = temp / "rejected.toml"
        bad_override = run(
            binary,
            "materialize",
            valid,
            rejected_output,
            "--set-json",
            '{"knobs.runtime.not_a_knob":1}',
        )
        require(bad_override.returncode == 2, "unknown override must fail")
        require(not rejected_output.exists(), "failed materialize must be atomic")

        migrated_client = temp / "client.toml"
        migrate_client = run(
            binary,
            "migrate-legacy",
            source_root / "config/send_config",
            migrated_client,
            "--role",
            "client",
            "--backend",
            "dpdk",
        )
        require_success(migrate_client, "migrate client")
        require(
            "0.029999999999999999" not in migrated_client.read_text(),
            "migration output must use human-readable float precision",
        )
        client_dump = run(binary, "dump", migrated_client)
        require_success(client_dump, "dump migrated client")
        client = json.loads(client_dump.stdout)
        require(client["deployment"]["role"] == "client", "client role lost")
        require(
            client["other"]["mempool_cache_size"] == 512,
            "client mempool cache default lost",
        )
        require(client["knobs"]["runtime"]["app_tx_batch_size"] == 16,
                "client tuning lost")
        require(client["network"]["local_mac"] == "10:70:fd:6b:93:5c", "MAC migration lost")
        require(client["network"]["device_pcie"] == "0000:98:00.0", "BDF migration lost")
        require(len(client["deployment"]["topology"]["workloads"]) == 4, "client workloads lost")

        dotted_legacy = temp / "dotted-send-config"
        dotted_legacy.write_text(
            (source_root / "config/send_config")
            .read_text()
            .replace("10:70:fd:6b:93:5c", "10.70.fd.6b.93.5c")
            .replace("0000:98:00.0", "0000.98.00.0")
        )
        rejected_legacy = run(
            binary,
            "migrate-legacy",
            dotted_legacy,
            temp / "rejected-legacy.toml",
            "--role",
            "client",
            "--backend",
            "dpdk",
        )
        require(rejected_legacy.returncode == 2,
                "dotted legacy addresses must not be rewritten silently")

        checked_client = run(binary, "dump", source_root / "config/client.toml")
        require_success(checked_client, "dump checked client")
        require(
            json.loads(checked_client.stdout) == client,
            "checked client TOML differs from a fresh legacy migration",
        )

        migrated_server = temp / "server.toml"
        migrate_server = run(
            binary,
            "migrate-legacy",
            source_root / "config/recv_config",
            migrated_server,
            "--role",
            "server",
            "--backend",
            "roce",
        )
        require_success(migrate_server, "migrate server")
        server_dump = run(binary, "dump", migrated_server)
        require_success(server_dump, "dump migrated server")
        server = json.loads(server_dump.stdout)
        require(server["deployment"]["role"] == "server", "server role lost")
        require(server["network"]["backend"] == "roce", "server backend lost")
        require(
            server["knobs"]["build"]["mempool_handler"] == "huge_alloc",
            "RoCE allocator default lost",
        )
        require(server["knobs"]["runtime"]["app_rx_batch_size"] == 64,
                "server tuning lost")
        migrated_checked_server = temp / "server-checked.toml"
        migrate_checked_server = run(
            binary,
            "migrate-legacy",
            source_root / "config/recv_config",
            migrated_checked_server,
            "--role",
            "server",
            "--backend",
            "dpdk",
        )
        require_success(migrate_checked_server, "migrate checked server")
        checked_server_migration = run(binary, "dump", migrated_checked_server)
        require_success(checked_server_migration, "dump migrated checked server")
        checked_server = run(binary, "dump", source_root / "config/server.toml")
        require_success(checked_server, "dump checked server")
        require(
            json.loads(checked_server.stdout)
            == json.loads(checked_server_migration.stdout),
            "checked server TOML differs from a fresh legacy migration",
        )

    print("axio-configure CLI test passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, json.JSONDecodeError) as error:
        print(f"axio-configure CLI test failed: {error}", file=sys.stderr)
        raise SystemExit(1)
