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
4. [PipeTune Diagnose and Tune](#axio-tuner)
5. [Troubleshooting](#trouble)

## <a name="features"></a>1. Features

- **Datapath:** Axio is a programmable traffic generator and application emulator. Each workload defines a configurable pipeline composed of application, dispatcher, and NIC stages, using either DPDK or RoCE as the network backend.
- **PipeTune:** the Python controller measures, diagnoses, and cold-start tunes Axio core, queue, and batch/post configuration values. The PipeTune paper has been accepted to NSDI '27. This repository integrates its script-based diagnosis and tuning workflow with Axio to support result reproduction and artifact evaluation. The planned `libpipetune` runtime integration will be released in the future.

The **Axio Datapath can be used independently** to emulate a specific application or as a load generator / high-speed datapath for benchmarking.

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
- 512 GB DDR4-3200 memory

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

In `[deployment]`, specify the endpoint role and NUMA node. The configured `numa_node` must match the NUMA node of the target NIC.

```toml
[deployment]
role = "client"        # use "server" in config/server.toml
numa_node = 0

# Quick Start runs Axio manually on this host.
transport = "local"
host = ""
ssh_port = 0
ssh_user = ""
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

Confirm the selected device and port are up before starting Axio.

Leave `[deployment.topology]`, `[handler]`, `[knobs.build]`,
`[knobs.runtime]`, `[other]`, and `[metrics]` unchanged for the first run.
`[tuning]` is optional and is not needed for a manual Axio run.

### Build axio-emulator

Configure a separate build directory for each endpoint. `axio_config` must be an absolute path. On the client host, run:

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
file that was bound to its build. Before the first `sudo` run, create the shared results directory as your normal user. Otherwise, Axio may create it as root and prevent subsequent PipeTune commands from writing their output:

```bash
mkdir -p results
sudo build-server/axio --config config/server.toml
```

Then start the client on the client host:

```bash
mkdir -p results
sudo build-client/axio --config config/client.toml
```

Axio validates the TOML and compares its build fingerprint with the binary
before initializing the NIC. If any build-time option differs, Axio exits with an error rather than running with an incompatible configuration.

### Outputs of the Datapath

A successful run prints a stage-by-stage performance table.

At server host:
```bash
Axio Metrics Window 17
End-to-end throughput (Mpps): 12.732
app_tx throughput (Mpps): 0.000, completion (/packet us): N/A, stall (/packet us): N/A
app_rx throughput (Mpps): 12.732, completion (/packet us): 0.021, stall (/packet us): 0.000
dispatcher_tx throughput (Mpps): 12.732, completion (/packet us): 0.006, stall (/packet us): 0.010
dispatcher_rx throughput (Mpps): 12.732, completion (/packet us): 0.013, stall (/packet us): 0.016
NIC TX throughput (Mpps): 12.732, NIC TX submit (/packet us): 0.010
NIC RX throughput (Mpps): 12.732, NIC RX completion interval (ns): 314.178
Latency p50/p99/p99.9 (us): N/A/N/A/N/A
```

At client host:
```bash
Axio Metrics Window 17
End-to-end throughput (Mpps): 12.714
app_tx throughput (Mpps): 12.714, completion (/packet us): 0.184, stall (/packet us): 0.003
app_rx throughput (Mpps): 12.714, completion (/packet us): 0.016, stall (/packet us): 0.000
dispatcher_tx throughput (Mpps): 12.714, completion (/packet us): 0.016, stall (/packet us): 0.013
dispatcher_rx throughput (Mpps): 12.714, completion (/packet us): 0.017, stall (/packet us): 0.015
NIC TX throughput (Mpps): 12.714, NIC TX submit (/packet us): 0.013
NIC RX throughput (Mpps): 12.714, NIC RX completion interval (ns): 314.625
Latency p50/p99/p99.9 (us): 4.483/6.102/6.807
```

When `metrics.enabled = true`, Axio also writes one machine-readable JSON record per measurement window to the file specified by `metrics.jsonl_path`. The parent directory is created automatically if it does not already exist.
For example:

```toml
[metrics]
enabled = true
human_output = true
jsonl_path = 'results/axio.jsonl'
```

Each line is one compact JSON object. The example below is pretty-printed only for readability:

```json
{
  "window_id": 10,
  "throughput": {
    "e2e_mpps": 27.74
  },
  "latency": {
    "p50_us": 2.06,
    "p99_us": 4.31,
    "p999_us": 4.80
  },
  "stages": {
    "app_tx": {
      "completion_time_per_packet_us": 0.02,
      "stall_time_per_packet_us": 0.00
    },
    "app_rx": {
      "completion_time_per_packet_us": 0.01,
      "stall_time_per_packet_us": 0.00
    },
    "dispatcher_tx": {
      "completion_time_per_packet_us": 0.03,
      "stall_time_per_packet_us": 0.02
    },
    "dispatcher_rx": {
      "completion_time_per_packet_us": 0.02,
      "stall_time_per_packet_us": 0.03
    },
    "nic_tx": {
      "throughput_mpps": 27.74,
      "submit_time_per_packet_us": 0.02
    },
    "nic_rx": {
      "throughput_mpps": 27.74,
      "completion_interval_cycles": 403.59,
      "completion_interval_ns": 144.14,
      "slowest_interval_cycles": 409.38,
      "capacity_interval_cycles": 100.90
    }
  },
  "counters": {
    "app_enqueue_drop_count": 0,
    "dispatcher_enqueue_drop_count": 0,
    "nic_rx_completion_error_count": 0
  }
}
```

Pretty-print the records for interactive inspection without changing the source file:

```bash
python3 toolchain/axio_metrics.py pretty results/axio.jsonl
```

Use `--array` when a single, standard JSON document is more convenient. The
result can be redirected to a separate readable file:

```bash
python3 toolchain/axio_metrics.py pretty --array results/axio.jsonl \
  > results/axio.pretty.json
```
The NIC RX stage is difficult to measure. We use the statistics from all active RX queues (the interval between RX completions) to estimate the NIC RX throughput.
- `completion_interval_cycles` and `completion_interval_ns`: count-weighted average interval between successful RX completions (for one RX queue).
- `slowest_interval_cycles`: longest interval between successful RX completions.
- `capacity_interval_cycles`: aggregate completion interval derived from the combined rates of all RX queues.

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

`config/schema-v1.example.toml` is the annotated schema-v1 reference. Copy it
when creating an experiment, then keep a separate endpoint file for client and
server.

The configuration is grouped by purpose:

- `[deployment]` places the endpoint, while `[deployment.topology]` owns the
  workspace pools, workload mapping, and NUMA-local CPU-core declarations;
- `[network]` and `[handler]` select the transport, NIC, and application
  behavior;
- `[knobs.build]` and `[knobs.runtime]` contain the PipeTune's C1-C6 knobs;
- `[other]` controls run windows and memory-pool capacity;
- `[metrics]` controls output, and optional `[tuning]` contains only tuner
  policy and noise thresholds.

For a complete configuration reference, including multi-workload topology and workspace mappings, see the [Axio Configuration Guide](docs/configuration.md).

Build the native configuration tool and validate the endpoint pair before building the datapath:

```bash
meson setup build-tools -Ddatapath=false
ninja -C build-tools axio-configure
build-tools/axio-configure validate-pair \
  config/client.toml config/server.toml
```

## <a name="axio-tuner"></a>4. PipeTune Diagnose and Tune

PipeTune runs as a controller that manages two pre-deployed Axio endpoints.
Before using PipeTune:

1. Clone the same Axio revision on both hosts.
2. Build the client binary on the client host and the server binary on the
   server host.
3. Configure non-interactive SSH access from the controller to every endpoint
   using `transport = "ssh"`.
4. Configure passwordless `sudo` on endpoints where `use_sudo = true`.
5. Set each endpoint's `deployment.workdir` to its Axio repository path.

PipeTune can run on your workstation with SSH access to both Axio hosts, or on
either host with a local connection to itself and SSH to its peer. Build the
small configuration tool once on the controller:

```bash
meson setup build-tools -Ddatapath=false
ninja -C build-tools axio-configure
```

For the first run, use the reference testbed pair:

- `config/pipetune/server-16c.toml` is the 16-core colocated tuning target (`rDesktop_02`);
- `config/pipetune/client-8c.toml` is the fixed 8-core colocated load generator (`rDesktop_01`).

The files are ready for the reference testbed when the controller runs on
`rDesktop_01`. On another testbed, change only their `[deployment]` and
`[network]` fields first.

```toml
# client-8c.toml: controller and client are on the same host
[deployment]
transport = "local"
role = "client"
workdir = "/path/to/axio-emulator"
use_sudo = true

# server-16c.toml: controller reaches the server over SSH
[deployment]
transport = "ssh"
role = "server"
host = "Desktop_02"
ssh_user = "ubuntu"
ssh_port = 30041
workdir = "/path/to/axio-emulator"
use_sudo = true
```

Because Quick Start already created the endpoint
build directories, validate the pair, then bind those directories to the
PipeTune configs and rebuild:

```bash
# rDesktop_01
build-tools/axio-configure validate-pair \
  config/pipetune/server-16c.toml \
  config/pipetune/client-8c.toml

# rDesktop_01
meson configure build-client \
  -Daxio_config="$PWD/config/pipetune/client-8c.toml"
python3 toolchain/axio_build.py build-client --target axio

# rDesktop_02
meson configure build-server \
  -Daxio_config="$PWD/config/pipetune/server-16c.toml"
python3 toolchain/axio_build.py build-server --target axio
```

### Diagnose one run

First collect one bounded target/peer trial:

```bash
python3 -m pipetune measure \
  --target-config config/pipetune/server-16c.toml \
  --peer-config config/pipetune/client-8c.toml \
  --output results/measure-001
```

Then diagnose it offline. This command does not start Axio or contact either
host:

```bash
python3 -m pipetune diagnose --session results/measure-001
```

The command prints a short human summary and writes the complete diagnosis JSON
under the session's `diagnoses/` directory. Add `--json` when the full document
is also needed on stdout.

```bash
PipeTune diagnosis
  Result: inconclusive (rx, confidence none)
  Throughput: target 41.62 Mpps, peer 41.44 Mpps
  Longest stage: app_rx.completion = 0.12 us/packet
  Counters: LLC load 82.42%, LLC store 84.31%, I/O read 2.40%, I/O write 90.63%
  Next: longest completion has no legal C1 perturbation
  Details: /home/ubuntu/git_repos/codex/axio-emulator/results/measure-001/diagnoses/trial-c55df5b9193b4b398499d6829fc46c0a.json
```

The P1-P4 result is a hypothesis, not permission to keep a new configuration.
Automatic tuning validates that hypothesis with a fresh cold-start candidate.
This standalone diagnosis is a preflight check; `bootstrap` starts a new
session and does not consume `results/measure-001`.

### Tune until no useful candidate remains

Start a new resumable tuning session from the largest C1/C2 pool you want PipeTune to explore:

```bash
python3 -m pipetune bootstrap \
  --target-config config/pipetune/server-16c.toml \
  --peer-config config/pipetune/client-8c.toml \
  --max-iterations 4 \
  --output results/tune-001
```

Inspect progress without changing anything, or resume safely after an
interruption:

```bash
python3 -m pipetune status --session results/tune-001
python3 -m pipetune resume --session results/tune-001
```

Run the returned pair as `results/tune-001/best.toml` and
`results/tune-001/peer.toml`. Read `report.md` for the diagnosis, four counter
rates, both acceptance gates, rollback/accept decisions, stop reason, and the
remaining manual C4-C6 suggestions.

See [Script-Based PipeTune](docs/pipetune.md) for controller placement, provider
requirements, target/peer semantics, P1-P4 rules, recovery, output schemas, and
the complete tuning artifact layout.

For artifact evaluation, use the one-command E2E and Figure 3/6/7/8/14
entry points described in [Axio and PipeTune Artifact Evaluation](docs/artifact-evaluation.md).
They publish numeric CSV/Markdown summaries and raw evidence without modifying
the paper repository or assigning PASS/WARN labels to observed trends.

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
sudo build-server/axio --config config/server.toml
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
