# App-Owned Media Queue TODO

Source:
- [development-principles.md](../development-principles.md)
- [workflow.md](../workflow.md)
- [app-owned-media-queue-protocol-design.md](./app-owned-media-queue-protocol-design.md)
- [progress README](../progress/README.md)

## Current Goal

Rebuild the smallest closed loop for disk benchmark traffic with the new app-owned media queue protocol, eliminate the old `WAIT_EVENT` inline model, remove the `Q32/Q8` hang and stall class, and improve throughput plus stability over the old baseline.

## Current Boundary

- No old-version compatibility.
- No parallel old/new protocol branch.
- No extra abstraction layer unless a real bottleneck forces it.
- The new protocol command space and KMDF/SCSI entrypoints are already in place; current gap is session lifecycle and timeout ownership, not protocol naming.
- Do not advance to the next substep until the current one is complete, archived, and committed.

## Pending Substeps

1. Rework KMDF session handling around file-bound lifecycle and locked timeout state.
2. Rewrite SCSI queue processing to drain while work is available and split lock scope.
3. Rework App backend scheduling to remove whole-backend serialization and piggyback write ACK.
4. Reconnect the benchmark loop and validate `Q1T1 / Q8 / Q32` runs.

## Current Unique Next Step

Rework KMDF session handling around file-bound lifecycle and locked timeout state.
