# axio-emulator

<!-- PipeTune is an efficient performance-tuning framework for host datapaths.
It correlates datapath configuration, memory efficiency, and performance to
search for effective configuration values. -->

<!-- PipeTune is described in our paper repository:
https://github.com/Huangxy-Minel/Paper-DPerf -->

## Catalog

1. [Features](#features)
2. [Quick Start](#quick-start)
3. [Customize Axio Datapath](#customize-axio-datapath)
4. [Axio Tuner (Coming Soon)](#axio-tuner)
5. [Troubleshooting](#trouble)

## <a name="features"></a>1. Features

- **Datapath:** Axio emulates the performance of real-world host applications
  with message-based and packet-based handlers. A workload can compose
  application, dispatcher, and NIC stages and use either DPDK or RoCE.
- **Tuner (coming soon):** Axio Tuner will search core, queue, batch, and other
  datapath configuration values through PipeTune.

The **Axio Datapath can be used independently** to emulate a specific
application or as a high-speed datapath performance-test tool.

## <a name="quick-start"></a>2. Quick Start

The following instructions set up and manually run the Axio Datapath on a
client host and a server host.

### Test Environment

The reference testbed uses:

- Ubuntu 22.04
- Linux kernel 5.15.x
- DPDK 22.11.x
- Intel Xeon Silver 4309Y CPUs
- two-port 200 Gbit/s NVIDIA/Mellanox ConnectX-7 NICs
- PCIe 4.0 x16
- 512 GB DDR5-3200 memory

Other recent ConnectX-class environments can work, but their DPDK,
libibverbs/OFED, device, and NUMA settings must be configured accordingly.

### Install Prerequisites

Install the basic build and profiling dependencies:

```bash
sudo bash scripts/init.sh
```

Build the vendored DPDK release if DPDK 22.11.x is not already installed in
the expected location:

```bash
tar -xvf third_party/dpdk-22.11.3.tar.xz -C third_party
bash third_party/build_dpdk.sh
```

For RoCE, install Mellanox OFED or compatible libibverbs providers for the NIC.
If the DPDK installation layout differs from the reference environment, update
`dpdk_pc_path` in `meson.build`.

### Configure Axio

Axio uses one TOML file per endpoint. Start from `config/client.toml` on the
client host and `config/server.toml` on the server host. For the first run,
only adapt `[deployment]` and `[network]` to the two machines.

In `[deployment]`, set the endpoint role and NUMA node:

```toml
[deployment]
role = "client"        # use "server" in config/server.toml
numa_node = 0

# Reserved for later PipeTune orchestration; Quick Start runs Axio manually.
host = "legacy-unset"
ssh_port = 22
ssh_user = "legacy-unset"
workdir = "."
use_sudo = true
```

In `[network]`, select the backend and the local NIC. Set `local_*` to this
host and `remote_*` to its peer; reverse them on the other endpoint:

```toml
[network]
backend = "dpdk"       # dpdk | roce
roce_transport = "rc"  # rc | ud; used when backend = "roce"
physical_port = 0
rx_ring_entries = 2048
tx_ring_entries = 2048

local_ip = "10.0.2.101"
remote_ip = "10.0.2.102"
local_mac = "10:70:fd:6b:93:5c"
remote_mac = "10:70:fd:87:0e:ba"
device_pcie = "0000:98:00.0"
device_name = "rocep152s0f0"
```

Use colon-delimited MAC addresses and a domain-qualified PCIe BDF. Confirm the
selected device and port are up before starting Axio. The checked-in ring sizes
are suitable first-run defaults.

Leave `[handler]`, `[knobs.build]`, `[knobs.runtime]`, `[other]`, `[metrics]`,
`[tuning]`, `[[workspaces]]`, and `[[workloads]]` unchanged for the first run.
They are covered in [Customize Axio Datapath](#customize-axio-datapath).

### Build axio-emulator

Configure a separate build directory for each endpoint. `axio_config` must be
an absolute path. On the client host, run:

```bash
meson setup build-client -Daxio_config="$PWD/config/client.toml"
python3 toolchain/axio_build.py build-client --target axio
```

On the server host, run:

```bash
meson setup build-server -Daxio_config="$PWD/config/server.toml"
python3 toolchain/axio_build.py build-server --target axio
```

Meson generates an endpoint-specific configuration header in the build
directory. Always use `toolchain/axio_build.py` for incremental production
builds so the header is updated before compiling Axio.

Low-level development can still compile with the defaults in `src/common.h`:

```bash
meson setup build-fallback
python3 toolchain/axio_build.py build-fallback --target axio
```

On a development machine without DPDK or RDMA libraries, build the
dependency-free smoke suite instead:

```bash
meson setup build-smoke -Ddatapath=false
meson test -C build-smoke --print-errorlogs
```

If the build fails, see [Troubleshooting](#trouble).

### Run Axio Datapath Individually

Run both endpoints manually. **Start the server first**, using the same TOML
file that was bound to its build:

```bash
sudo build-server/axio --config config/server.toml \
  --peer-config config/client.toml
```

Then start the client on the client host:

```bash
sudo build-client/axio --config config/client.toml \
  --peer-config config/server.toml
```

Axio validates the TOML and compares its build fingerprint with the binary
before initializing the NIC. Passing a different build-time configuration
causes startup to fail instead of running a mismatched datapath.

### Outputs of the Datapath

A successful run prints a stage-by-stage performance table. The main fields
are:

1. **Thpl. (Mpps):** throughput in millions of packets per second.
2. **Avg. [/P]:** average execution time per packet at each pipeline stage.
3. **Avg. Stall [/P]:** average pipeline stall time per packet; this is part of
   the stage execution time.
4. **Max/Min/Avg Stall. [/B]:** maximum, minimum, and average stall time per
   batch.
5. **Max/Min/Avg Coml. [/B]:** maximum, minimum, and average completion time per
   batch.

## <a name="customize-axio-datapath"></a>3. Customize Axio Datapath

Axio models application work with a message-based handler and models work in
the dispatcher with a packet-based handler. Existing handlers can be selected
entirely through TOML. A new handler must first be registered in the C++ type
and parser mappings described below.

### Register a Message-based Handler

Message handlers are compile-time templates. Keep the datapath and typed-config
enum values at the same ordinal because the generated header carries the typed
configuration value into `MessageHandlerType`.

1. Declare the private handler kernel in `src/workspace.h` and implement it in
   `src/ws_impl/msg_handlers.cc`. A minimal kernel has this shape:

   ```cpp
   template <class TDispatcher>
   void Workspace<TDispatcher>::_my_handler(
       AXIO_MEMORY_BUFFER_TYPE** buffer_ptr, size_t packet_count,
       udphdr* udp_header, WorkspaceHeader* workspace_header) {
     for (size_t i = 0; i < packet_count; ++i) {
       // Read or transform the request, then write the response.
       this->_write_payload(*buffer_ptr, reinterpret_cast<char*>(udp_header),
                            reinterpret_cast<char*>(workspace_header),
                            kAppRespPayloadSize);
       ++buffer_ptr;
     }
   }
   ```

2. Register the compile-time type in `src/common.h`:

   ```cpp
   enum MessageHandlerType : uint8_t {
     kMessageHandlerEmpty = 0,
     // Existing handlers...
     kMessageHandlerMyHandler,
   };
   ```

3. Add the matching typed value at the same ordinal in
   `include/axio/config/config_types.h`:

   ```cpp
   enum class MessageHandler : uint8_t {
     kEmpty,
     // Existing handlers in the same order...
     kMyHandler,
   };
   ```

4. Map the TOML name in both places used by the strict schema:

   - add `kMyHandler -> "my_handler"` to `to_string(MessageHandler)` in
     `include/axio/config/build_config.h`;
   - add `"my_handler" -> MessageHandler::kMyHandler` to the
     `handler.message_handler` mapping in `src/config/config_loader.cc`.

5. Add a branch for the new type in
   `Workspace<TDispatcher>::_handle_server_messages` in
   `src/ws_impl/msg_handlers.cc`:

   ```cpp
   else if (handler == kMessageHandlerMyHandler) {
     this->_my_handler(mbuf_ptr, pkt_num, &uh, &hdr);
   }
   ```

6. Select the registered TOML name and define its wire behavior on both
   endpoints:

   ```toml
   [handler]
   message_handler = "my_handler"
   packet_handler = "empty"
   apply_new_mbuf = false
   request_payload_bytes = 86
   response_payload_bytes = 86
   app_ticks_per_message = 0
   ```

`request_payload_bytes` describes the client request and
`response_payload_bytes` describes the server response. Set `apply_new_mbuf`
when the handler must allocate a distinct response buffer. Use
`app_ticks_per_message` to model additional per-message processing cost.

### Register a Packet-based Handler

Packet handlers execute inside a dispatcher backend. Registration follows the
same enum and parser rules, plus a backend-specific implementation:

1. Declare the handler in the selected backend dispatcher header and implement
   it in the corresponding packet-handler source. For DPDK, the existing
   implementation is `src/dispatcher_impl/dpdk/dpdk_pkt_handlers.cc`; the RoCE
   wrapper is `src/dispatcher_impl/roce/roce_pkt_handlers.cc`.
2. Add matching ordinal values to `PacketHandlerType` in `src/common.h` and
   `config::PacketHandler` in `include/axio/config/config_types.h`.
3. Extend `to_string(PacketHandler)` in
   `include/axio/config/build_config.h` and the `handler.packet_handler` parser
   mapping in `src/config/config_loader.cc`.
4. Dispatch the new type from the backend's `handle_server_packets` template.
5. Select its canonical name with `handler.packet_handler` and rebuild.

A packet handler is valid only for a backend whose wrapper implements it. The
built-in `echo` packet handler is currently implemented for DPDK; the RoCE
packet wrapper currently accepts only `empty`.

### Customize the Configuration

`config/schema-v1.example.toml` is the annotated source of truth for all
required schema-v1 fields. Copy it when creating a new experiment, then keep a
separate endpoint file for client and server.

The configuration is grouped by purpose:

- `[deployment]`, `[network]`, and `[handler]` deploy the endpoint and select
  application behavior;
- `[knobs.build]` and `[knobs.runtime]` contain the PipeTune C1-C6 values;
- `[other]` controls run windows and memory-pool capacity;
- `[metrics]` and `[tuning]` describe measurement and future tuning policy;
- `[[workspaces]]` and `[[workloads]]` describe the datapath topology.

Build the native configuration tool and validate the endpoint pair before
building the datapath:

```bash
meson setup build-tools -Ddatapath=false
ninja -C build-tools axio-configure
build-tools/axio-configure validate-pair \
  config/client.toml config/server.toml
```

For reproducible scripted changes, materialize a new file instead of editing
the source configuration in place. C1/C2 overrides also update workload groups
from `[tuning.resources]`: candidates are added in listed order and removed in
reverse order, while applications are placed in the least-loaded dispatcher
group (dispatcher ID breaks ties).

```bash
build-tools/axio-configure materialize \
  config/server.toml /tmp/server-custom.toml \
  --set-json '{"knobs.runtime.application_core_count":6,"knobs.runtime.dispatcher_queue_count":6}'
```

Every resource-pool ID must have a matching `[[workspaces]]` declaration. Its
`cpu_core` is a zero-based core ordinal within `deployment.numa_node`; inactive
candidates are declared but are not launched until materialized into a group.

The generated header and build fingerprint contain the endpoint role,
backend/transport/ring settings, all handler fields, build knobs, and
memory-pool size/cache. Changing any of those values requires rebuilding the
affected endpoint. Runtime knobs, physical port and addresses, NUMA placement,
run windows, metrics, tuning, and topology are consumed at startup and do not
change the generated header.

After changing a projected value, rebuild with the TOML that will be passed at
runtime:

```bash
meson configure build-server -Daxio_config=/tmp/server-custom.toml
python3 toolchain/axio_build.py build-server --target axio
sudo build-server/axio --config /tmp/server-custom.toml \
  --peer-config config/client.toml
```

## <a name="axio-tuner"></a>4. Axio Tuner (Coming Soon)

Axio Tuner is the next PipeTune integration stage. It will automatically parse
diagnosis data, complete the P1-P4 decisions, restart the emulator across
multiple cold-start tuning rounds, and report the converged configuration or
the best result at the configured round limit.

The later `libpipetune` integration will provide probe macros, per-thread event
rings, a shared-memory event stream, an independent daemon, and a knob
registration API. The historical Python prototype is not the supported Axio
runtime or tuning workflow.

## <a name="trouble"></a>5. Troubleshooting

### Cannot find the DPDK library

Verify that the vendored DPDK installation completed successfully and that
`dpdk_pc_path` in `meson.build` points to its `pkg-config` directory. Meson
prints the detected DPDK modules and version during setup.

### Configuration validation fails

Run the validator directly:

```bash
build-tools/axio-configure validate config/client.toml
build-tools/axio-configure validate config/server.toml
```

Schema errors report the TOML key and source location. Every declared key is
required and unknown keys are rejected.

### Build fingerprint mismatch

The binary was compiled with different projected values from the TOML passed
at startup. Reconfigure the build directory if necessary, rebuild with
`toolchain/axio_build.py`, and run that binary with the same TOML:

```bash
meson configure build-server \
  -Daxio_config="$PWD/config/server.toml"
python3 toolchain/axio_build.py build-server --target axio
sudo build-server/axio --config config/server.toml \
  --peer-config config/client.toml
```

### Axio starts but no traffic is observed

Start the server before the client. On both hosts, verify the selected backend,
physical port, PCIe BDF, device name, link state, IP/MAC direction, and RoCE
transport. Confirm that each endpoint uses its own role and the peer's address
as `remote_*`.

### Memory-pool exhaustion or allocation stalls

Review `other.mempool_size`, `other.mempool_cache_size`, the RX/TX ring sizes,
the inflight-message budget, and `handler.apply_new_mbuf`. Increasing the pool
can hide an incorrect handler lifetime, so first confirm that newly allocated
response buffers are actually required and released.

The executable name is `axio`.
