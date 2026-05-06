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
- 不继续在当前多盘并发数据面上修修补补；当前不稳定数据面视为待替换实现，而不是继续演进的基础。
- 结构从第一步就按多盘目标落，但只能保留一条实现路径：`KMDF session state -> SCSI per-target queue -> App per-disk engine`。
- 保留并复用已经相对稳定的控制面边界：`session lifecycle`、`QueryInfo`、`CreateDisk`、`RemoveDisk`、`RemoveAllDisks`、`QueryDebugState`。
- 数据面只允许一条协议链路：`POST_READ_SLOT`、`POST_WRITE_SLOT`、`READ_ACK`、`WRITE_ACK_BATCH`。
- KMDF 继续只承担 `session + direct-IO slot transport`，不引入新的协议层或兼容桥。
- SCSI 不允许回到 adapter 级共享数据面锁和共享队列；每个 target 必须自带独立读写队列和推进状态。
- App 不允许回到全局共享 worker pool 或全局共享数据面队列深度；并发粒度必须按盘独立。
- 每完成一个子步骤，必须先归档、更新当前 `todo`、提交，然后停止，等待下一轮推进。

## Pending Substeps

1. 删回控制面骨架，并按最终多盘模型重定状态归属：`KMDF` 持有唯一 session 状态，`SCSI` 持有 per-target 队列状态，`App` 持有 per-disk slot/medium/ACK 状态。
2. 重建 `KMDF` 状态管理：file 绑定 session、watchdog 锁定、`RemoveAllDisks | SessionClose` 清理顺序、pending slot 生命周期，确保多盘共享同一 session 时状态不分叉。
3. 重建 `SCSI` per-target 单盘队列：每个 target 独立 `posted read/write slots`、`pending read/write requests`、`eventId` 推进和 `drain while available`。
4. 重建 `App` per-disk 全并发执行：每盘独立 slot depth、独立 read/write/ack 推进，不共享全局数据面 worker 池，不共享全局队列深度。
5. 重新接通最终链路：`建盘 -> 枚举 -> probe read -> steady-state read/write -> ctrl+c / rm all / App 退出`，并把取消、读写单请求失败、session close 都收进唯一边界。
6. 按最终目标验证多盘并发：先双盘 `Q1T1`，再 `Q2/Q8/Q32`，要求不再出现 100% 无读写假死、盘被误判死亡、会话残留或全局串扰。

## Current Unique Next Step

删回控制面骨架，并按最终多盘模型重定状态归属：只保留 `KMDF session`、`CreateDisk/RemoveDisk/RemoveAllDisks`、`QueryInfo/QueryDebugState`，删除当前不符合“KMDF 状态单源 + SCSI per-target 队列单源 + App per-disk 并发单源”的数据面实现。
