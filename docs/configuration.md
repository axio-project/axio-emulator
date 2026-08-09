# Axio Configuration Guide

Axio uses one TOML file for each endpoint. The file defines deployment,
networking, handler behavior, datapath topology, tunable parameters, and
measurement output. This guide explains how these parts are related. The
complete annotated field reference is
[`config/schema-v1.example.toml`](../config/schema-v1.example.toml).

## 1. Configure an Endpoint Pair

An Axio experiment has one client and one server:

- the server is started first and waits for traffic;
- the client initiates the workload;
- `local_*` fields describe the current endpoint;
- `remote_*` fields describe its peer.

Start from [`config/client.toml`](../config/client.toml) and
[`config/server.toml`](../config/server.toml). For an initial deployment,
change only `[deployment]` and `[network]`.

Build the configuration utility once:

```bash
meson setup build-tools -Ddatapath=false
ninja -C build-tools axio-configure
```

Validate each file and then validate the pair:

```bash
build-tools/axio-configure validate config/client.toml
build-tools/axio-configure validate config/server.toml
build-tools/axio-configure validate-pair \
  config/client.toml config/server.toml
```

Pair validation checks endpoint roles, backend compatibility, reciprocal
addresses, and remote dispatcher routes. It does not contact either host.

## 2. Deployment and Network

`[deployment]` describes where the endpoint runs:

```toml
[deployment]
transport = "local"       # local | ssh
role = "client"           # client | server
numa_node = 1
host = ""
ssh_port = 0
ssh_user = ""
workdir = "."
use_sudo = true
```

Use `transport = "local"` when the controller runs commands directly on this
host. Use `transport = "ssh"` for a remote endpoint, and set `host`,
`ssh_port`, `ssh_user`, and `workdir`. `numa_node` must match the NUMA node of
the selected NIC.

`[network]` selects DPDK or RoCE and identifies the local port:

```toml
[network]
backend = "dpdk"           # dpdk | roce
roce_transport = "rc"      # rc | ud; used only by RoCE
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

For the peer file, exchange the local and remote addresses. `device_pcie` is
used by DPDK; `device_name` is used by RoCE. The selected interface, physical
port, PCIe function, and address must refer to the same NIC port.

## 3. Handler Selection

`[handler]` defines the emulated application and optional dispatcher work:

```toml
[handler]
message_handler = "t_app"
packet_handler = "empty"
apply_new_mbuf = false
request_payload_bytes = 86
response_payload_bytes = 22
app_ticks_per_message = 0
```

`message_handler` selects application-level processing. `packet_handler`
selects processing inside the dispatcher. The built-in `echo` packet handler
is available for DPDK; RoCE currently accepts `empty`. A newly registered
handler becomes selectable by its canonical TOML name after Axio is rebuilt.
The README describes the handler registration procedure.

## 4. Topology

Topology uses logical workspace IDs. Each workspace maps to a core ordinal
within `deployment.numa_node`; users do not enter a system-wide CPU number.

The two arrays below are ordered resource pools:

```toml
[deployment.topology]
application_workspaces = [0, 1, 2, 3]
dispatcher_workspaces = [0, 1, 2, 3]
```

They may contain inactive workspaces reserved for later materialization. The
active application and dispatcher sets are the workspace IDs referenced by
workload groups. Their distinct counts must equal
`knobs.runtime.application_core_count` (C1) and
`knobs.runtime.dispatcher_queue_count` (C2).

Every pool member must have one CPU mapping:

```toml
[[deployment.topology.workspaces]]
id = 0
cpu_core = 0
```

A workload defines a pipeline and its route to the peer. A group assigns one
local dispatcher to one or more local applications:

```toml
[[deployment.topology.workloads]]
id = 1
pipeline = ["nic_rx", "dispatcher_rx", "app_rx", "app_tx",
            "dispatcher_tx", "nic_tx"]
remote_dispatchers = [0, 1, 2, 3]

[[deployment.topology.workloads.groups]]
dispatcher = 0
applications = [0]
```

`remote_dispatchers` contains dispatcher workspace IDs from the corresponding
workload on the peer. Therefore, a C2 change on one endpoint may require a
route update in the peer file.

### Colocated applications and dispatchers

Use the same workspace IDs for both roles to run four application/dispatcher
pairs on four cores:

```toml
[deployment.topology]
application_workspaces = [0, 1, 2, 3]
dispatcher_workspaces = [0, 1, 2, 3]

[knobs.runtime]
application_core_count = 4
dispatcher_queue_count = 4
# Retain the six batch/post fields from the reference file.

[[deployment.topology.workloads]]
id = 1
pipeline = ["nic_rx", "dispatcher_rx", "app_rx", "app_tx",
            "dispatcher_tx", "nic_tx"]
remote_dispatchers = [0, 1, 2, 3]

[[deployment.topology.workloads.groups]]
dispatcher = 0
applications = [0]

[[deployment.topology.workloads.groups]]
dispatcher = 1
applications = [1]

[[deployment.topology.workloads.groups]]
dispatcher = 2
applications = [2]

[[deployment.topology.workloads.groups]]
dispatcher = 3
applications = [3]
```

Define workspace IDs `0` through `3` with `cpu_core` values `0` through `3`.

### Split applications and dispatchers

Use disjoint IDs to run four applications and four dispatchers on eight cores:

```toml
[deployment.topology]
application_workspaces = [0, 1, 2, 3]
dispatcher_workspaces = [4, 5, 6, 7]

[knobs.runtime]
application_core_count = 4
dispatcher_queue_count = 4
# Retain the six batch/post fields from the reference file.

[[deployment.topology.workloads]]
id = 1
pipeline = ["nic_rx", "dispatcher_rx", "app_rx", "app_tx",
            "dispatcher_tx", "nic_tx"]
remote_dispatchers = [4, 5, 6, 7]

[[deployment.topology.workloads.groups]]
dispatcher = 4
applications = [0]

[[deployment.topology.workloads.groups]]
dispatcher = 5
applications = [1]

[[deployment.topology.workloads.groups]]
dispatcher = 6
applications = [2]

[[deployment.topology.workloads.groups]]
dispatcher = 7
applications = [3]
```

Define workspace IDs `0` through `7` with `cpu_core` values `0` through `7`.
For fanout, retain the dispatcher groups and assign additional applications
evenly across their `applications` arrays. The materializer uses the same
least-loaded assignment rule.

Multiple workloads may use different pipelines and groups. Workspace IDs must
remain unique within each role, and every active workspace must belong to a
valid workload group.

## 5. PipeTune Knobs

PipeTune names six classes of datapath parameters:

| Knob | TOML field | Meaning | Build requirement |
| --- | --- | --- | --- |
| C1 | `application_core_count` | Number of active application workspaces | Runtime |
| C2 | `dispatcher_queue_count` | Number of active dispatcher workspaces and NIC queues | Runtime |
| C3 | Six batch/post fields in `[knobs.runtime]` | Application, dispatcher, and NIC batch sizes | Runtime |
| C4 | `inflight_limit_enabled`, `inflight_messages` | Client in-flight request limit | Rebuild |
| C5 | `mtu` | Datapath buffer and packet-size limit | Rebuild |
| C6 | `mempool_handler` | DPDK or RoCE buffer-pool implementation | Rebuild |

The six C3 fields are:

```toml
[knobs.runtime]
app_tx_batch_size = 16
app_rx_batch_size = 16
dispatcher_tx_batch_size = 16
dispatcher_rx_batch_size = 16
nic_tx_post_size = 16
nic_rx_post_size = 32
```

C1 and C2 are validated against the explicit topology. Change them with the
paired materialization commands unless the groups and peer routes are edited
manually at the same time.

## 6. Materialize Reproducible Configurations

`materialize` writes a new file and leaves its input unchanged. Use it for a
runtime value that does not alter topology:

```bash
build-tools/axio-configure materialize \
  config/server.toml /tmp/server-bs32.toml \
  --set-json '{"knobs.runtime.app_rx_batch_size":32}'
```

Use `materialize-pair` to apply the same C1/C2 values to both endpoints and
regenerate reciprocal routes:

```bash
build-tools/axio-configure materialize-pair \
  config/client.toml config/server.toml \
  /tmp/client-3c.toml /tmp/server-3c.toml \
  --set-json '{"knobs.runtime.application_core_count":3,"knobs.runtime.dispatcher_queue_count":3}'
```

Use `materialize-target-pair` when only the tuning target changes. The peer's
active topology remains fixed, but its remote route is updated:

```bash
build-tools/axio-configure materialize-target-pair \
  config/pipetune/server-16c.toml config/pipetune/client-8c.toml \
  /tmp/server-12c.toml /tmp/client-peer.toml \
  --target-set-json '{"knobs.runtime.application_core_count":12,"knobs.runtime.dispatcher_queue_count":12}'
```

The resource-pool order determines which workspace is added or removed.
Applications are assigned to the least-loaded valid group. Validate the
generated pair before building or running it.

## 7. When to Rebuild

The generated header and build fingerprint contain:

- endpoint role;
- backend, RoCE transport, and RX/TX ring sizes;
- every `[handler]` field;
- every `[knobs.build]` field;
- `other.mempool_size` and `other.mempool_cache_size`.

Changing one of these values requires rebuilding the affected endpoint.
Changing the configuration path also requires rebinding the Meson build
directory:

```bash
meson configure build-server -Daxio_config="$PWD/config/server.toml"
python3 toolchain/axio_build.py build-server --target axio
```

Deployment, NUMA placement, topology, physical port and addresses, C1-C3,
measurement windows, metrics, and tuning policy are read at startup. They do
not change the generated header. Axio still records the complete effective
configuration, so use the TOML intended for that run.

If Meson is configured without `-Daxio_config`, Axio compiles with the defaults
in `src/common.h`. This fallback is intended for low-level development; the
generated configuration is preferred for reproducible experiments.
