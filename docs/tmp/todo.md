# App-Owned Media Queue TODO

Source:
- [development-principles.md](../development-principles.md)
- [workflow.md](../workflow.md)
- [app-owned-media-queue-protocol-design.md](./app-owned-media-queue-protocol-design.md)
- [progress README](../progress/README.md)

## Current Goal

Validate the rebuilt async KMDF slot transport on the VM, confirm that the first probe read now completes end-to-end, and verify that disk enumeration plus high-queue read pressure no longer regress into the previous pending-slot stall.

## Current Boundary

- No old-version compatibility.
- No parallel old/new protocol branch.
- No extra abstraction layer unless runtime evidence forces it.
- KMDF `POST_READ_SLOT` / `POST_WRITE_SLOT` has been cut over to one async long-pending path; the old synchronous proxy behavior is no longer the target design.
- SCSI per-target queue structure and App per-disk slot engine structure remain the only active data path.
- This round is runtime validation only: do not start a new protocol or fairness optimization before confirming the async transport closes the current enumeration / pending-slot hole.
- Do not advance to the next substep until the current one is complete, archived, and committed.

## Pending Substeps

1. Redeploy the rebuilt drivers and App, then manually verify `ct -> 盘枚举 -> 首个 probe read completion -> Q1/Q8 read pressure` on the VM.

## Current Unique Next Step

Redeploy the rebuilt drivers and App, then manually verify `ct -> 盘枚举 -> 首个 probe read completion -> Q1/Q8 read pressure` on the VM.
