# M-App Cache-Line Store Design

## Goal

Restore M-App as a memory-store workload without adding an artificial serial
compute dependency. The default per-application working set is 256 KiB, so 16
application workspaces collectively use 4 MiB.

## Workload semantics

For each received message, M-App selects one random 1 KiB block from the
workspace-local working set. The block offset is aligned to 1 KiB and is
selected by the existing deterministic pseudo-random generator. M-App then
overwrites the entire block sequentially. This touches 16 consecutive 64-byte
cache lines.

The store operation does not read the previous contents, calculate a checksum,
or carry a data dependency between cache lines or messages. A single fixed-size
`memset` expresses the intended store-only operation and lets the compiler emit
efficient vector stores or an equivalent bulk-store sequence.

The TOML fields remain configurable:

```toml
[handler.m_app]
state_bytes = 262144
access_bytes_per_message = 1024
random_seed = 1
```

`access_bytes_per_message` must be a multiple of the 64-byte cache-line size,
must not exceed `state_bytes`, and must divide `state_bytes` evenly. The
selected offset remains aligned to `access_bytes_per_message`.

## Code changes

- Change the default M-App state size from 4 MiB to 256 KiB in configuration
  defaults and checked-in TOML files.
- Replace `MemoryWorkload::read_modify_write()` with
  `MemoryWorkload::store_block()`.
- Remove the checksum state and per-byte read/modify/write loop.
- Update the M-App handler call site and focused workload/config tests.

## Verification

The focused test must demonstrate that the old implementation fails the new
store-only contract before production code changes. The final test verifies
deterministic offsets, alignment, exactly one changed 1 KiB block, and
cache-line configuration validation. Both remote endpoints must compile the
M-App DPDK configuration after the change.
