# App-Owned Media Queue TODO

Source:
- [development-principles.md](../development-principles.md)
- [workflow.md](../workflow.md)
- [app-owned-media-queue-protocol-design.md](./app-owned-media-queue-protocol-design.md)
- [progress README](../progress/README.md)

## Current Goal

按 `app-owned-media-queue-protocol-design.md` 的最终模型，直接重建面向多盘的唯一数据面结构：`KMDF` 负责单一会话状态管理与 slot transport，`SCSI` 负责 per-target 单盘队列，`App` 负责 per-disk slot engine 与全并发执行；不再先做一套“临时单盘结构”再二次改造成多盘，也不再继续在 `cdcb7e25` 之后形成的复合状态机上补丁推进。

## Current Boundary

- 不保留旧 `WAIT_EVENT` inline data 路径，也不保留任何旧 payload 兼容。
- 不继续在当前多盘并发数据面上修修补补；当前不稳定数据面已经被删回控制面骨架，不再作为继续演进的基础。
- 结构从第一步就按多盘目标落，但只能保留一条实现路径：`KMDF session state -> SCSI per-target queue -> App per-disk engine`。
- 保留并复用已经相对稳定的控制面边界：`session lifecycle`、`QueryInfo`、`CreateDisk`、`RemoveDisk`、`RemoveAllDisks`、`QueryDebugState`。
- 数据面只允许一条协议链路：`POST_READ_SLOT`、`POST_WRITE_SLOT`、`READ_ACK`、`WRITE_ACK_BATCH`。
- KMDF 继续只承担 `session + direct-IO slot transport`，不引入新的协议层或兼容桥。
- SCSI 不允许回到 adapter 级共享数据面锁和共享队列；每个 target 必须自带独立读写队列和推进状态。
- App 不允许回到全局共享 worker pool 或全局共享数据面队列深度；并发粒度必须按盘独立。
- 当前代码现实边界：`KMDF` 会话状态管理和 `SCSI` per-target 队列都已经重建完成；miniport 侧已经具备 `SubmitSlot / ReadAck / WriteAckBatch / CancelSlot` 与 per-target `pending read/write` 的真实实现，但 `KMDF` 前门的 `POST_READ_SLOT / POST_WRITE_SLOT / READ_ACK / WRITE_ACK_BATCH / CANCEL_SLOT` 仍然是骨架，`RWTestApp` 也仍然只是控制台骨架。
- 当前 SCSI 写分片已经收敛到“同一 target 的活跃 write slots 使用单一 payload 大小”这一唯一边界；下一步 App 必须按盘固定 slot bytes，不再尝试多种 write slot 形状混跑。
- 每完成一个子步骤，必须先归档、更新当前 `todo`、提交，然后停止，等待下一轮推进。

## Pending Substeps

1. 重建 `KMDF + App` per-disk 全并发执行：把 `POST_READ_SLOT / POST_WRITE_SLOT / READ_ACK / WRITE_ACK_BATCH / CANCEL_SLOT` 从控制骨架重新接回到 `session-owned slot transport`，并让每盘独立持有 slot depth、read/write/ack 推进和固定 slot bytes。
2. 重新接通最终链路：`建盘 -> 枚举 -> probe read -> steady-state read/write -> ctrl+c / rm all / App 退出`，并把取消、读写单请求失败、session close 都收进唯一边界。
3. 按最终目标验证多盘并发：先双盘 `Q1T1`，再 `Q2/Q8/Q32`，要求不再出现 100% 无读写假死、盘被误判死亡、会话残留或全局串扰。

## Current Unique Next Step

重建 `KMDF + App` per-disk 全并发执行：在现有 `KMDF session state + SCSI per-target queue` 边界上，把 `POST_READ_SLOT / POST_WRITE_SLOT / READ_ACK / WRITE_ACK_BATCH / CANCEL_SLOT` 真正接回 `RWTestApp` 的 per-disk slot engine，并固定每盘自己的 slot depth 与 write slot bytes，不恢复任何全局共享 worker 或全局共享队列深度。
