# KMDF Kernel CPU TODO

Source:
- [development-principles.md](../development-principles.md)
- [workflow.md](../workflow.md)
- [app-owned-media-queue-protocol-design.md](./app-owned-media-queue-protocol-design.md)
- [kmdf-kernel-cpu-reduction-draft.md](./kmdf-kernel-cpu-reduction-draft.md)
- [progress README](../progress/README.md)

## Current Goal

在保持当前 `ct -> 压测 -> rm` 闭环稳定、单盘顺序吞吐约 `600 MB/s`、多盘并发稳定的前提下，压低单盘压测时明显偏高的内核 CPU 占用。

## Current Boundary

- 不回退当前 `App per-disk bounded workers` 数据面模型。
- 不回退 `SCSI per-target queue` 和当前取消语义。
- 系统侧取消逻辑只追到 `SCSI`，不新增“系统取消一路追到 App”的全链路复杂取消协议。
- 下一阶段优先怀疑 `KMDF async slot transport` 的每-slot 固定开销，不先重写协议，不先改 `SCSI` 基本队列模型。
- 当前协议边界保持不变：`POST_READ_SLOT`、`POST_WRITE_SLOT`、`READ_ACK`、`WRITE_ACK_BATCH`、`CANCEL_SLOT`。
- 只有在 `KMDF` 热路径对象复用之后 CPU 仍明显偏高，才考虑第二阶段协议级降命令数优化。
- `KMDF` 的 `WDFWAITLOCK` 只保留在 open/heartbeat/cleanup/watchdog 等低频生命周期路径，不作为 slot 高频准入锁。
- `SCSI` per-target queue 继续使用 `KSPIN_LOCK`，不改成 wait lock；锁内只做队列和状态操作。
- 按 [development-principles.md](../development-principles.md) 的“激进更新原则”执行：KMDF 热路径优化不是给旧 per-slot transport 打补丁，而是重建 session-owned transport runtime，并在落地后删除旧 per-slot IRP/work item/SessionLock 热路径。

## Pending Substeps

1. 为 `KMDF` session 增加 transport runtime 骨架，open 初始化、cleanup/watchdog 停止。
2. 引入 async slot object pool，让对象长期持有 `IRP + IOCTL buffer + completion context`。
3. 用 `IoReuseIrp` 复用池内 IRP，去掉每-slot `IoAllocateIrp/IoFreeIrp`。
4. 改成 session-owned 长期 submit worker，去掉每-slot `IoAllocateWorkItem/IoFreeWorkItem`。
5. 将 `POST_READ_SLOT/POST_WRITE_SLOT` 高频准入从 `WDFWAITLOCK` 改成原子 state + pending ref。
6. 保持当前取消模型不变，确认对象池不会破坏 cleanup、session close 和 late ACK 处理。
7. 在不改协议的前提下重新测单盘吞吐与 kernel CPU。

## Current Unique Next Step

先按 [kmdf-kernel-cpu-reduction-draft.md](./kmdf-kernel-cpu-reduction-draft.md) 实施 Step 1：增加 `KMDF` session-owned transport runtime 骨架，完成 open 初始化、cleanup/watchdog 停止、编译通过，但暂不改变数据面行为。
