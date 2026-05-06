# App-Owned Media Queue TODO

Source:
- [development-principles.md](../development-principles.md)
- [workflow.md](../workflow.md)
- [app-owned-media-queue-protocol-design.md](./app-owned-media-queue-protocol-design.md)
- [progress README](../progress/README.md)

## Current Goal

Validate the rebuilt multi-disk concurrent path, confirm that per-disk App slot engines plus per-target SCSI queues improve dual-disk throughput/fairness without reintroducing hangs, and record the remaining benchmark gaps.

## Current Boundary

- No old-version compatibility.
- No parallel old/new protocol branch.
- No extra abstraction layer unless a real bottleneck forces it.
- The App path is already cut over to one slot engine thread per disk with per-disk `queueDepth` for read and write slots.
- The SCSI path is already cut over to per-target queue locks and per-target posted/pending lists; multi-disk traffic no longer shares one adapter-global read/write queue.
- `WRITE_ACK_BATCH` is already the only write ACK path, and ACK now completes before the corresponding system write SRB is completed.
- The remaining gap is manual multi-disk validation: proving throughput/fairness gains and checking that dual-disk pressure does not regress cancellation or cleanup stability.
- Do not advance to the next substep until the current one is complete, archived, and committed.

## Pending Substeps

1. Run the manual dual-disk benchmark matrix and record throughput, kernel CPU, fairness, cancel behavior, and cleanup stability.

## Current Unique Next Step

Run the manual dual-disk benchmark matrix and record throughput, kernel CPU, fairness, cancel behavior, and cleanup stability.
