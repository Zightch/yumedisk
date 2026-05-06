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
- App-side write ACK has been simplified to dedicated `WRITE_ACK_BATCH`; `POST_WRITE_SLOT` no longer carries piggyback ACK state.
- The benchmark loop is reconnected on the tooling side: `RWTestApp` now resolves the visible `PhysicalDrive` and prints the suggested `diskspd` commands.
- The remaining gap is manual validation: proving the new path no longer hangs under `Q1T1 / Q8 / Q32`, and collecting throughput plus kernel CPU data.
- Do not advance to the next substep until the current one is complete, archived, and committed.

## Pending Substeps

1. Run the manual `Q1T1 / Q8 / Q32` benchmark matrix and record throughput, kernel CPU, and cleanup stability.

## Current Unique Next Step

Run the manual `Q1T1 / Q8 / Q32` benchmark matrix and record throughput, kernel CPU, and cleanup stability.
