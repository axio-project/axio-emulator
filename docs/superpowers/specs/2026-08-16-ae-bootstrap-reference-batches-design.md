# AE Bootstrap Reference Batch Configuration

## Goal

Make AE bootstrap experiments start from the same per-stage runtime batch and
post sizes as the checked-in reference endpoint TOMLs, while preserving the
explicit C3 sweeps used by Figures 3, 6, 7, and 8.

## Design

`CaseConfiguration` records whether a case preserves the reference C3 values.
For those cases, materialization changes target C1/C2 but does not override
`app_rx_batch_size`, `app_tx_batch_size`, `dispatcher_rx_batch_size`,
`dispatcher_tx_batch_size`, `nic_rx_post_size`, or `nic_tx_post_size`.

All E2E cases and both Figure 14 adaptation cases preserve reference C3.
Figure 3, Figure 6, Figure 7, and Figure 8 continue to apply their declared
uniform C3 values. The client remains fixed at C1/C2=8/8 in every AE case.

Dry-run output labels the bootstrap C3 value as `reference` so it does not
misrepresent a mixed six-value configuration as one scalar. E2E summaries
continue reading the materialized canonical target and therefore publish all
six values when they are not uniform.

## Verification

A focused contract test verifies both branches of materialization and the
matrix classification. Remote acceptance runs only `dpdk-l-app-req128` with
10 warmup windows, 20 sample windows, and three tuning rounds. Its generated
server configuration must match the reference `64/32/32/32/128/32` values and
its generated client must remain C1/C2=8/8.
