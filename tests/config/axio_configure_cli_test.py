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


def main() -> int:
    if len(sys.argv) != 3:
        print(
            "usage: axio_configure_cli_test.py <axio-configure> <source-root>",
            file=sys.stderr,
        )
        return 2

    binary = pathlib.Path(sys.argv[1])
    source_root = pathlib.Path(sys.argv[2])
    valid = source_root / "tests/config/schema-v1.valid.toml"

    with tempfile.TemporaryDirectory(prefix="axio-configure-test-") as temp_dir:
        temp = pathlib.Path(temp_dir)

        validated = run(binary, "validate", valid)
        require_success(validated, "validate")
        require(validated.stdout == "valid\n", "validate output must be stable")

        pair = run(binary, "validate-pair", valid, valid)
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
            "runtime.unknown_option" in rejected.stderr,
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
        require(document["schema_version"] == 1, "dump must include schema")
        require(document["build"]["backend"] == "dpdk", "dump must type enums")
        require(document["runtime"]["iterations"] == 30, "dump must include runtime")
        require(len(document["workspaces"]) == 2, "dump must include workspaces")

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
            '{"runtime.iterations":31}',
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

        build_changed = temp / "build-changed.toml"
        build_override = run(
            binary,
            "materialize",
            valid,
            build_changed,
            "--set-json",
            '{"build.mtu":4096}',
        )
        require_success(build_override, "materialize build config")
        build_generate = run(binary, "generate", build_changed, generated)
        require_success(build_generate, "generate build config")
        require(generated.read_text() != header, "build knob did not change header")

        materialized = temp / "materialized.toml"
        overrides = json.dumps(
            {
                "runtime.iterations": 41,
                "build.inflight_messages": 512,
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
        require(updated["runtime"]["iterations"] == 41, "runtime override lost")
        require(updated["build"]["inflight_messages"] == 512, "build override lost")
        require(
            updated["network"]["local_mac"] == "10:70:fd:00:00:0a",
            "string override lost",
        )
        require(
            json.loads(run(binary, "dump", valid).stdout)["runtime"]["iterations"] == 30,
            "materialize must not mutate its input",
        )

        rejected_output = temp / "rejected.toml"
        bad_override = run(
            binary,
            "materialize",
            valid,
            rejected_output,
            "--set-json",
            '{"runtime.not_a_knob":1}',
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
        require(client["build"]["role"] == "client", "client role lost")
        require(
            client["build"]["mempool_cache_size"] == 512,
            "client mempool cache default lost",
        )
        require(client["runtime"]["app_tx_batch_size"] == 16, "client tuning lost")
        require(client["network"]["local_mac"] == "10:70:fd:6b:93:5c", "MAC migration lost")
        require(client["network"]["device_pcie"] == "0000:98:00.0", "BDF migration lost")
        require(len(client["workloads"]) == 4, "client workloads lost")
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
        require(server["build"]["role"] == "server", "server role lost")
        require(server["build"]["backend"] == "roce", "server backend lost")
        require(
            server["build"]["mempool_handler"] == "huge_alloc",
            "RoCE allocator default lost",
        )
        require(server["runtime"]["app_rx_batch_size"] == 64, "server tuning lost")
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
