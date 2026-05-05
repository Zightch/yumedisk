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
- The new protocol command space, KMDF file-bound session/watchdog, SCSI while-drain queue path, and App-side parallel slot workers are already in place.
- App-side write ACK piggyback and idle flush fallback are already in place.
- The remaining gap is reconnecting the benchmark loop and proving the new path no longer hangs under `Q1T1 / Q8 / Q32`.
- Do not advance to the next substep until the current one is complete, archived, and committed.

## Pending Substeps

1. Reconnect the benchmark loop and validate `Q1T1 / Q8 / Q32` runs.

## Current Unique Next Step

Reconnect the benchmark loop and validate `Q1T1 / Q8 / Q32` runs.
