# App-Owned Media Queue TODO

Source:
- [development-principles.md](../development-principles.md)
- [workflow.md](../workflow.md)
- [app-owned-media-queue-protocol-design.md](./app-owned-media-queue-protocol-design.md)
- [progress README](../progress/README.md)

## Current Goal

Rebuild the smallest closed loop for disk benchmark traffic with the new app-owned media queue protocol, remove the old `WAIT_EVENT` inline path, eliminate the `Q32/Q8` hang and stall behavior, and improve throughput plus stability over the current baseline.

## Current Boundary

- No old-version compatibility.
- No parallel old/new protocol branch.
- No extra abstraction layer unless a real bottleneck forces it.
- Do not advance to the next substep until the current one is complete, archived, and committed.

## Pending Substeps

1. Cut over the protocol entrypoints and delete the old `WAIT_EVENT` inline data path.
2. Rework KMDF session handling around file-bound lifecycle and locked timeout state.
3. Rewrite SCSI queue processing to drain while work is available and split lock scope.
4. Rework App backend scheduling to remove whole-backend serialization and piggyback write ACK.
5. Reconnect the benchmark loop and validate `Q1T1 / Q8 / Q32` runs.

## Current Unique Next Step

Cut over the protocol entrypoints and delete the old `WAIT_EVENT` inline data path.

