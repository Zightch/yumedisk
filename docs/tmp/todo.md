# Kernel CPU TODO

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

## Pending Substeps

1. 收敛 `KMDF` 热路径对象生命周期，去掉每-slot `alloc/free IRP/work item/buffer`。
2. 保持当前取消模型不变，确认对象池不会破坏 cleanup、session close 和 late ACK 处理。
3. 在不改协议的前提下重新测单盘吞吐与 kernel CPU。

## Current Unique Next Step

先按 [kmdf-kernel-cpu-reduction-draft.md](./kmdf-kernel-cpu-reduction-draft.md) 收敛 `KMDF async slot transport` 的对象池和提交模型，优先去掉每-slot `IoAllocateIrp + IoAllocateWorkItem + ControlAlloc` 热路径固定开销。
