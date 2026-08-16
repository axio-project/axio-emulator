# AE End-to-End Payload Matrix Design

## Objective

Replace the handler-only artifact-evaluation E2E matrix with the payload sweep
used to cover the Axio Figure 12/13 experiment space. The experiment remains a
PipeTune bootstrap run and reports the initial 16-core baseline and historical
best for every case.

## Matrix

The explicit request matrix is:

| Case suffix | Request payload | Request Ethernet frame |
| --- | ---: | ---: |
| `req128` | 86 B | 128 B |
| `req512` | 470 B | 512 B |
| `req1024` | 982 B | 1024 B |

Each request point runs `t_app`, `l_app`, and `m_app` on DPDK and RoCE, for 18
independent cases. T-App always uses a 22 B response payload (64 B Ethernet
frame). L-App and M-App use a response payload equal to the request payload.

Case IDs use the paper-facing Ethernet frame size, for example
`dpdk-t-app-req128`, rather than the message payload stored in the TOML.

File-read, file-write, and key-value handlers remain supported by Axio but are
not part of this E2E artifact matrix.

## Starting configurations

DPDK starts both endpoints at C1/C2=16/16. RoCE starts both endpoints at
C1/C2=16/8 so the RC dispatcher/QP counts match. All 16 application workspaces
are active on both endpoints and backends, so the target and load generator
begin with the complete 16-core NUMA budget in use.

The existing C3 defaults remain unchanged: DPDK uses 32 and RoCE uses 64. Only
the target is tuned after the initial pair is materialized; the load-generator
knobs remain fixed except for reciprocal route updates.

The smoke profile performs two warmup windows, three sample windows, one
session per case, and at most three tuning rounds. A session may stop earlier
when PipeTune converges.

## Configuration and output contracts

`CaseConfiguration` carries the request-frame label and the actual request and
response payload sizes explicitly. Configuration materialization writes those
payload values into both endpoint TOMLs; it no longer infers E2E payloads from
one handler-wide constant.

The E2E terminal table, `summary.md`, and `summary.csv` identify the request
frame size and response payload so the three points of one handler are
unambiguous. Existing baseline/best throughput, improvement, C1/C2/C3, P99.9,
round count, and stop-reason fields remain unchanged.

Generated TOMLs continue to be stored under
`<output>/generated-configs/<case-id>/`, and each bootstrap session retains the
native PipeTune report, iteration log, best config, peer config, and immutable
trial artifacts.

## Verification

Use focused matrix/materialization/summary checks during implementation, then
perform syntax validation and compile both endpoint binaries at one exact SHA.
The acceptance run is the complete 18-case E2E smoke matrix on the reference
testbed. The final report transcribes every case's baseline and historical-best
performance without judging whether it matches a paper trend.
