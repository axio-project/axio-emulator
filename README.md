# axio-emulator

Axio is a high-speed host-datapath emulator used by PipeTune. It composes
application, dispatcher, and NIC stages into configurable workloads and can use
either DPDK or RoCE as the transport backend. Message and packet handlers model
application work while the datapath reports stage-level throughput, completion,
and stall metrics.

The emulator uses one strict TOML configuration for build-time knobs, runtime
knobs, endpoint addresses, workload topology, and the metadata needed by later
PipeTune stages.

## Contents

1. [Requirements](#requirements)
2. [Configure and build](#configure-and-build)
3. [Run the emulator](#run-the-emulator)
4. [TOML schema](#toml-schema)
5. [Configuration CLI](#configuration-cli)
6. [Legacy migration](#legacy-migration)
7. [Handler development](#handler-development)
8. [Development and troubleshooting](#development-and-troubleshooting)

## Requirements

The reference environment is Ubuntu 22.04 with Linux 5.15, Meson, Ninja,
DPDK 22.11.x, Mellanox OFED/libibverbs, and a ConnectX-class NIC. The vendored
DPDK source can be built with:

```bash
tar -xvf third_party/dpdk-22.11.3.tar.xz -C third_party
bash third_party/build_dpdk.sh
```

Adjust the DPDK `pkg-config` path in `meson.build` if the installed layout is
different from the reference environment.

## Configure and build

Start from `config/client.toml`, `config/server.toml`, or the fully annotated
`config/schema-v1.example.toml`. Validate both endpoints before building:

```bash
meson setup build-tools -Ddatapath=false
ninja -C build-tools axio-configure

build-tools/axio-configure validate config/client.toml
build-tools/axio-configure validate config/server.toml
build-tools/axio-configure validate-pair \
  config/client.toml config/server.toml
```

### Generated configuration build

Pass an absolute TOML path through the Meson `axio_config` option. Configure a
separate build directory for each endpoint role:

```bash
meson setup build-client \
  -Daxio_config="$PWD/config/client.toml"
python3 toolchain/axio_build.py build-client --target axio

meson setup build-server \
  -Daxio_config="$PWD/config/server.toml"
python3 toolchain/axio_build.py build-server --target axio
```

Meson generates `generated/axio_config_generated.h` inside the build directory
and force-includes it before `src/common.h`. The generated header contains only
the `[build]` table plus a build fingerprint; it is never edited or checked in.
`src/common.h` remains the source of fallback defaults and is not rewritten.

Always use `toolchain/axio_build.py` for incremental production builds. The
driver first stabilizes the generated header and then builds the requested
target. A runtime-only TOML edit leaves the header timestamp unchanged and does
not rebuild C++; a `[build]` edit changes the header and recompiles the affected
target in the same invocation.

To change which configuration a build directory follows:

```bash
meson configure build-server \
  -Daxio_config="$PWD/config/server.toml"
python3 toolchain/axio_build.py build-server --target axio
```

### `common.h` fallback build

Omitting `axio_config` preserves direct compilation from `src/common.h`:

```bash
meson setup build-fallback
python3 toolchain/axio_build.py build-fallback --target axio
```

The fallback is intended for low-level development and compatibility. The
binary still requires TOML at startup, and its `[build]` table must exactly
match the effective `AXIO_CONFIG_*` defaults from `src/common.h`. Otherwise Axio
rejects the mismatch before initializing the NIC or datapath.

## Run the emulator

Start the server before the client and pass the same TOML file used to configure
each generated build:

```bash
sudo build-server/axio --config config/server.toml
sudo build-client/axio --config config/client.toml
```

Axio parses and validates the complete document, compares the runtime build
fingerprint with the binary, and only then initializes the datapath. A syntax,
schema, validation, or fingerprint error exits with status 2 and includes the
configuration key and source location where possible.

## TOML schema

Schema v1 is strict: every declared key is required and unknown keys are
rejected. The annotated source of truth for users is
`config/schema-v1.example.toml`.

The major sections are:

| Section | Purpose | Rebuild required |
| --- | --- | --- |
| `build` | role, DPDK/RoCE selection, MTU, rings, mempool, handlers, payloads, inflight budget | yes |
| `runtime` | NUMA/port, measurement windows, batch and NIC post sizes | no |
| `network` | IPv4, MAC, PCIe BDF, and device name | no |
| `workspaces` / `workloads` | CPU placement and dispatcher/application topology | no |
| `metrics` | structured and human-readable output policy | no |
| `deployment` / `tuning` | PipeTune deployment and search contract | no |

Allowed build enum values are:

- `role`: `client`, `server`
- `backend`: `dpdk`, `roce`
- `roce_transport`: `rc`, `ud`
- `mempool_handler`: `ring_mp_mc`, `ring_sp_sc`, `ring_mp_sc`,
  `ring_sp_mc`, `ring_mt_rts`, `ring_mt_hts`, `stack`, `lf_stack`,
  `bucket`, `huge_alloc`
- `message_handler`: `empty`, `t_app`, `l_app`, `m_app`, `file_write`,
  `file_read`, `key_value`
- `packet_handler`: `empty`, `echo`
- pipeline phases: `app_tx`, `dispatcher_tx`, `nic_tx`, `nic_rx`,
  `dispatcher_rx`, `app_rx`

MAC addresses must use canonical colon notation, such as
`10:70:fd:6b:93:5c`. PCIe devices must use canonical domain-qualified BDF
notation, such as `0000:98:00.0`.

Application-core and dispatcher-queue counts are derived from
`[[workloads.groups]]`; there are no duplicated count knobs. Each group maps one
local dispatcher workspace to one or more application workspaces, while
`remote_dispatchers` identifies the peer workspaces used by that workload.

## Configuration CLI

`axio-configure` is the single parser/validator shared by developers, Meson, and
the emulator. After building it as shown above, the available commands are:

```text
axio-configure validate CONFIG
axio-configure validate-pair LOCAL PEER
axio-configure dump CONFIG
axio-configure generate CONFIG OUTPUT
axio-configure materialize INPUT OUTPUT --set-json JSON
axio-configure migrate-legacy INPUT OUTPUT --role ROLE --backend BACKEND
```

`dump` emits deterministic canonical JSON for comparison and automation.
`materialize` writes a new validated TOML document without changing its input:

```bash
build-tools/axio-configure materialize \
  config/server.toml /tmp/server-mtu4096.toml \
  --set-json '{"build.mtu":4096,"runtime.iterations":40}'
```

`generate` is normally invoked by Meson; calling it manually is useful only for
inspection or tooling tests.

## Legacy migration

The old colon-delimited `config/send_config` and `config/recv_config` files are
accepted only by the one-shot converter. They are not valid emulator runtime
configuration:

```bash
build-tools/axio-configure migrate-legacy \
  config/send_config config/client.toml \
  --role client --backend dpdk

build-tools/axio-configure migrate-legacy \
  config/recv_config config/server.toml \
  --role server --backend dpdk
```

Review the generated build defaults, canonical MAC/BDF addresses, deployment
metadata, and backend before using the result. Then run `validate-pair` and use
the TOML files for all subsequent builds and launches.

`toolchain/config_parser.py`, `toolchain/tuner.py`, and the tuning mode in
`toolchain/main.py` belong to the historical research prototype. They do not
participate in the Axio runtime path and will be replaced by the PipeTune
controller and library in later development phases.

## Handler development

Message handlers live in `src/ws_impl/msg_handlers.cc`; packet handlers live in
the backend dispatcher implementations. When adding a new handler:

1. Implement and test the handler without changing existing wire behavior.
2. Register the handler in the Axio enums and typed configuration mappings.
3. Select it through `build.message_handler` or `build.packet_handler` and set
   explicit request/response payload sizes in TOML.
4. Rebuild through `toolchain/axio_build.py` and verify the generated build
   fingerprint.

Existing handler selection no longer requires editing `src/common.h`.

## Development and troubleshooting

On a machine without DPDK or RDMA libraries, build the dependency-free suite:

```bash
meson setup build-smoke -Ddatapath=false
meson test -C build-smoke --print-errorlogs
```

Common failures:

- **DPDK is not found:** verify the vendored installation and the `dpdk_pc_path`
  selected in `meson.build`.
- **Build/config fingerprint mismatch:** run the binary with the TOML used by
  that build directory, or reconfigure `-Daxio_config` and rebuild with
  `toolchain/axio_build.py`.
- **No traffic after startup:** verify endpoint role, backend, PCIe BDF, device
  name, link state, addresses, inflight budget, batch sizes, and ring/mempool
  capacity on both endpoints.
- **Mempool exhaustion with `apply_new_mbuf = true`:** increase the pool or
  reduce outstanding traffic after confirming the handler really requires a
  newly allocated response buffer.

The executable name remains `axio`.
