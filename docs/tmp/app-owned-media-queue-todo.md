# App-Owned Media Queue TODO

Source:
- [development-principles.md](../development-principles.md)
- [app-owned-media-queue-protocol-design.md](./app-owned-media-queue-protocol-design.md)

## Goal

- Rebuild the smallest closed loop for disk benchmark traffic with the new queue protocol.
- Remove the old `WAIT_EVENT` inline path and all compatibility branches.
- Eliminate the `Q32/Q8` hang and stall behavior.
- Improve throughput and reduce kernel CPU versus the current baseline.

## Current blockers

- `YumeDiskKMDF` still synchronously waits on `IOCTL_SCSI_MINIPORT`.
- `YumeDiskSCSI` still uses one `ControlLock` to cover too many queue states.
- `RWTestApp` still serializes medium access with one global `diskLock`.
- Write confirmation still carries too much control traffic.

## Work Plan

| Order | Task | Output | Done When |
| --- | --- | --- | --- |
| 1 | Cut over the protocol entrypoints | Replace old `WAIT_EVENT / READ_REPLY / WRITE_ACK` semantics with slot-based commands | Old inline wait path is gone and no caller depends on old payload layout |
| 2 | Rework KMDF session handling | File-bound session lifecycle, locked state on timeout, no long-pending miniport control request | Session close is deterministic and does not wedge worker threads |
| 3 | Rewrite SCSI queue processing | Split lock scope, drain while work is available, avoid linear scan under a single global lock | Read/write dispatch can keep draining at `Q8` and `Q32` without stall |
| 4 | Rework App backend scheduling | Per-disk concurrency, slot pool, piggyback write ACK, no whole-backend serialization lock | App can keep multiple workers active without blocking on one global lock |
| 5 | Reconnect the benchmark loop | Run sequential 1M read/write through the new path and collect throughput/CPU numbers | `Q1T1 / Q8 / Q32` complete without hang, stuck disk, or session leak |

## Non-goals

- No old-version compatibility.
- No parallel old/new protocol branch.
- No extra abstraction layer unless a real bottleneck forces it.
- No preservation of the `WAIT_EVENT` inline data path.

## Verification

- `RWTestApp` sequential 1M read/write at `Q1T1`, `Q8`, `Q32`.
- Exit and cleanup path must not leave disk stuck at 100%.
- Repeated runs must not accumulate pending sessions, pending SRBs, or leaked handles.
- Throughput and kernel CPU must improve relative to the current baseline.

