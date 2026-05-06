# KMDF 内核占用压缩草案

## 背景

当前链路已经达到以下状态：

- `ct -> 压测 -> rm` 闭环稳定
- 多盘并发通过
- 单盘顺序吞吐恢复到约 `600 MB/s`

但新的现象也已经明确：

- 单盘压测时 CPU 占用可到约 `50%`
- 其中大部分是内核态

这说明当前主瓶颈已经从 App 数据面模型转移到了内核热路径。

## 当前判断

当前最可疑的位置不是 `SCSI per-target queue`，也不是 App worker 数量，而是 `YumeDiskKMDF` 的 async slot transport 固定开销。

当前 `POST_READ_SLOT` / `POST_WRITE_SLOT` 每次都要走一套重路径：

- `ControlAlloc(asyncRequest)`
- `ControlAlloc(ioctlBuffer)`
- `IoAllocateIrp`
- `IoAllocateWorkItem`
- `IoQueueWorkItem`
- `IoCallDriver`
- completion 后再整套释放

这套动作在高 IOPS / 高 QD 下会稳定制造大量内核态 CPU。

## 当前不改的边界

- 不回退当前 App 每盘 worker 模型
- 不回退 `SCSI` per-target queue
- 不新增旧协议兼容
- 不新增“系统取消一路追到 App”的全链路取消协议
- 取消仍然只追到 `SCSI`，App ACK 晚到后由 `SCSI` 判断 stale / cancelled / not found

## 第一阶段目标

不改协议语义，只压 `KMDF` 每-slot 固定成本。

目标是：

- 保持当前吞吐不掉
- 降低单盘 kernel CPU
- 不破坏 cleanup / session close / late ACK

## 推荐方案

推荐把 `KMDF async slot transport` 重建成：

- 每 session 一个有界 async slot request 对象池
- 每个对象长期持有：
  - `CTRL_ASYNC_SLOT_REQUEST`
  - `IRP`
  - `IOCTL buffer`
  - 必要的 completion 上下文
- 热路径只做：
  - 从池里取对象
  - 填 descriptor
  - 投递到 miniport
  - completion 后回收到池

同时去掉“每 slot 一个临时 work item”的模型，改成：

- 每 session 一个长期存在的 submit worker
- 或少量长期存在的 submit workers
- 由这个 worker 消费有界提交队列并调用 `IoCallDriver`

这样能同时去掉两类固定开销：

- 每-slot 内存分配/释放
- 每-slot work item 分配/排队

## 为什么不优先改协议

当前协议已经证明能把吞吐拉回到约 `600 MB/s`，所以现阶段更像“实现成本太高”，不是“协议条数绝对太多”。

只要每-slot 固定开销足够低，现有协议还可以继续撑一段。

所以第一阶段不建议立刻去做：

- 合并 `POST_READ_SLOT` / `READ_ACK`
- 合并 `POST_WRITE_SLOT` / `WRITE_ACK_BATCH`
- 新增更复杂的 slot bundle / multi-event ACK 协议

这些都属于第二阶段候选，而不是当前第一优先级。

## 对取消模型的要求

对象池和 submit worker 改造后，取消模型必须保持不变：

- 系统侧取消只追到 `SCSI`
- `SCSI` 负责完成或取消原始系统请求
- `App` 可以继续回 `READ_ACK` / `WRITE_ACK_BATCH`
- ACK 晚到时由 `SCSI` 判定 stale / cancelled / not found

也就是说：

- 这次优化只能改热路径对象生命周期
- 不能顺手把取消模型重新复杂化

## 落地步骤

1. 为 `KMDF` session 引入有界 async slot request 池
2. 让池对象长期持有 `IRP + buffer + context`
3. 把每-slot `IoAllocateWorkItem` 改成长期存在的 submit worker 模型
4. 保持现有 completion / cleanup / in-flight drain 语义不变
5. 重新测单盘 `Q1/Q8/Q32` 的吞吐和 kernel CPU

## 验收预期

- 单盘吞吐不低于当前水平
- 单盘 kernel CPU 明显低于当前约 `50%` 的水平
- `Ctrl+C`、`rm all`、App 退出不引入新的 cleanup 卡住
- late `READ_ACK` / `WRITE_ACK_BATCH` 仍按当前取消语义工作
