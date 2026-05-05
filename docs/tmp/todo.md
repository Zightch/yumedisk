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
- The old data path has been stripped to a control-plane skeleton; current READ/WRITE benchmark traffic is intentionally disconnected until the new queue protocol is rebuilt.
- Do not advance to the next substep until the current one is complete, archived, and committed.

## Pending Substeps

1. Rebuild the protocol entrypoints on top of the skeleton.
2. Rework KMDF session handling around file-bound lifecycle and locked timeout state.
3. Rewrite SCSI queue processing to drain while work is available and split lock scope.
4. Rework App backend scheduling to remove whole-backend serialization and piggyback write ACK.
5. Reconnect the benchmark loop and validate `Q1T1 / Q8 / Q32` runs.

## Current Unique Next Step

Rebuild the protocol entrypoints on top of the skeleton.
