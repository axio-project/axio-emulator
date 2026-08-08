# PipeTune Measurement Controller

PipeTune `measure` runs one bounded Axio experiment and publishes the evidence
needed by the later offline diagnosis and tuner. The controller can run on a
workstation with SSH access to both endpoints, or directly on either endpoint.
Axio, `perf`, and `pcm-pcie` always execute on the configured testbed hosts.

This release collects evidence; it does not yet diagnose P1-P4 or change C1-C6.

## Build requirements

PipeTune uses Python 3.10 or newer and the standard library. Build the native
configuration tool on the controller before invoking the Python CLI:

```bash
meson setup build-tools -Ddatapath=false
ninja -C build-tools axio-configure
```

Build the Axio client and server binaries on their respective hosts with the
same source revision and the endpoint's own TOML file:

```bash
meson setup build-client -Daxio_config="$PWD/config/client.toml"
python3 toolchain/axio_build.py build-client --target axio

meson setup build-server -Daxio_config="$PWD/config/server.toml"
python3 toolchain/axio_build.py build-server --target axio
```

The default provider paths are `/usr/bin/perf` and `/usr/sbin/pcm-pcie`.
`pcm-pcie` also requires PCM/MSR access. Provider permission or capability
failures are recorded as unavailable metrics and do not fabricate zero rates.

## Configure controller placement

`deployment.transport` describes how the controller reaches each endpoint. It
does not change the datapath fingerprint.

For a workstation controlling both hosts, set both endpoint files to SSH. For
example, the client file can contain:

```toml
[deployment]
role = "client"
transport = "ssh"
host = "rDesktop_01"
ssh_port = 22
ssh_user = "ubuntu"
workdir = "/home/ubuntu/git_repos/codex/axio-emulator-e61"
use_sudo = true
numa_node = 1
```

Use the corresponding server role, host, work directory, and NUMA node in the
server file. PipeTune invokes the system `ssh` client, so normal SSH config,
proxy, key, and host-alias rules continue to apply.

When the controller runs on an endpoint, use `local` for that endpoint and
`ssh` for its peer:

```toml
[deployment]
role = "client"
transport = "local"
host = ""
ssh_port = 0
ssh_user = ""
workdir = "/home/ubuntu/git_repos/codex/axio-emulator-e61"
use_sudo = true
numa_node = 1
```

All network, handler, topology, and knob fields still describe the endpoint
where Axio runs. Validate the pair before measuring:

```bash
build-tools/axio-configure validate-pair TARGET.toml PEER.toml
```

Both configs must enable JSONL metrics and must use matching values for
`other.iterations`, `other.window_seconds`, `tuning.warmup_windows`, and
`tuning.sample_windows`. Iterations must cover warmup plus sample windows.

## Run a measurement

The target is the endpoint whose host counters and later tuning knobs are in
scope. The peer is frozen experiment context. Either role can be the target:

```bash
python3 -m pipetune measure \
  --target-config TARGET.toml \
  --peer-config PEER.toml \
  --output results/session-001 \
  --axio-configure build-tools/axio-configure
```

Use `--target-binary` or `--peer-binary` only when the binary is not at
`WORKDIR/build-ROLE/axio` on that endpoint.

PipeTune always starts the server role before the client role. The client still
generates traffic even when the server is the target. Only the target receives
perf and PCM collection. The source TOMLs are copied into the trial unchanged;
materialized copies differ only in runner-owned metrics output fields. PipeTune
rejects any target or peer knob leakage.

Startup readiness means the managed workload exists. Sampling begins only
after both endpoint JSONL streams contain the configured number of valid,
monotonically increasing warmup windows. `perf` attaches to the actual Axio PID,
including when `sudo` uses a separate monitor process.

## Artifact layout

`--output` is a new session directory and must not already exist:

```text
session-001/
  session.json
  trials/
    trial-.../
      trial.json
      host-metrics.json
      configs/
        source/{target,peer}.toml
        materialized/{target,peer}.toml
      endpoints/
        target/{metrics.jsonl,stdout.log,stderr.log}
        peer/{metrics.jsonl,stdout.log,stderr.log}
      providers/
        perf-version.{stdout,stderr}
        perf-{load,store}.{stdout,stderr}
        pcm-version.{stdout,stderr}
        pcm-pcie.{csv,stdout,stderr}
```

`session.json` uses `pipetune.session/v1` and indexes typed trial manifests.
`trial.json` records the target, endpoint roles and transports, process results,
Git/binary/config fingerprints, cleanup status, schemas, and artifact hashes.
`host-metrics.json` records four target rates plus provider availability and
the argv/exit status for every provider command. Each Axio `metrics.jsonl`
remains the compact six-stage `axio.metrics/v1` stream.

Human-facing JSON is already pretty printed. It can also be inspected with:

```bash
python3 -m json.tool results/session-001/session.json
python3 -m json.tool \
  results/session-001/trials/TRIAL_ID/host-metrics.json
```

JSONL intentionally stores one compact object per line. Use `jq` when a
pretty view of individual windows is useful:

```bash
jq . results/session-001/trials/TRIAL_ID/endpoints/target/metrics.jsonl
```

## Failure and cleanup behavior

Every endpoint and provider is launched through a foreground, session-scoped
worker. Cleanup signals only the recorded private process group and removes
only that trial's contained remote scratch directory; PipeTune never uses a
global `pkill`. Cleanup is idempotent.

The local session directory is published atomically only after both endpoint
processes exit successfully, all artifacts validate, remote cleanup completes,
and `session.json` is written. A failed measurement leaves no authoritative
output directory. Raw provider output is retained for successful trials even
when a provider is unavailable. A provider command that succeeds but loses its
declared output artifact is treated as an infrastructure failure.

Do not run two measurements with the same `--output`. Use a fresh directory for
each independent E8 trial; multi-trial session management is introduced by the
later tuner workflow.
