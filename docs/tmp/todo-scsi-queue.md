# `YumeDiskSCSI` `queue/` 结构重构执行清单

## 0. 触发原因

当前 [queue.c](C:/Users/Zightch/Desktop/driver/windows/YumeDiskSCSI/YumeDiskSCSI/queue/queue.c) 已超过 `1400` 行，已经满足 [开发原则第 5 条](../development/development-principles.md) 的主动重构触发条件：

- 文件接近或超过 `1000` 行
- 职责混杂
- 状态与行为已经缠在一起
- 同类逻辑散落在同一文件内长期平铺

这次工作必须按“重建结构”处理，不做简单搬文件，不做 `queue_helper.c` 这类中转兜底文件，也不做一层拆分后就停止。

## 1. 当前范围

本清单只处理：

- `windows/YumeDiskSCSI/YumeDiskSCSI/queue/`
- 与它直接相连的公开入口和内部组件边界

本清单当前不处理：

- 网络协议
- `KMDF`
- `AppKernel`
- `SCSI` 命令语义
- 新功能

如果重构中需要改引用方，范围只限：

- `adapter/`
- `control/`
- `scsi/`

也就是只做结构收束，不改业务语义。

## 2. 当前现状

当前 [queue.c](C:/Users/Zightch/Desktop/driver/windows/YumeDiskSCSI/YumeDiskSCSI/queue/queue.c) 同时承载了下面几类职责：

1. 私有数据结构与分配：
   - `YUME_POSTED_SLOT`
   - `YUME_READ_REQUEST`
   - `YUME_WRITE_REQUEST`
   - bitmap、event id、payload shape 计算
2. slot 编码与完成：
   - read slot event 写入
   - write slot payload 写入
   - `submit slot` / `cancel slot`
   - slot 完成与释放
3. read 数据面：
   - read request 入队
   - read slot drain
   - read ack 回写与完成
4. write 数据面：
   - write request 入队
   - write slot drain
   - write ack batch 校验、bitmap 标记、最终完成
5. event slot：
   - event slot submit
   - event slot pending 完成
6. 生命周期清理：
   - queue 初始化
   - queue 清理
   - target/all pending 清空
7. debug 聚合：
   - `DiskQueryDebugState`

这说明它已经不是一个“queue 文件”，而是一个未拆开的 `queue` 子系统。

## 3. 当前公开面

当前 [queue.h](C:/Users/Zightch/Desktop/driver/windows/YumeDiskSCSI/YumeDiskSCSI/queue/queue.h) 暴露的入口大致分为四组：

1. 生命周期：
   - `DiskInitializeQueueState`
   - `DiskFreeQueuedState`
   - `DiskCompleteTargetPending*`
   - `DiskCompleteAllPending*`
2. `SCSI` 数据面入口：
   - `DiskQueueReadSrb`
   - `DiskQueueWriteSrb`
3. `control` 控制面入口：
   - `DiskHandleSubmitSlotIoctl`
   - `DiskHandleSubmitEventSlotIoctl`
   - `DiskHandleReadAckIoctl`
   - `DiskHandleWriteAckBatchIoctl`
   - `DiskHandleCancelSlotIoctl`
   - `DiskCompleteDeferredWriteCompletions`
4. debug：
   - `DiskQueryDebugState`

这也进一步说明，当前 `queue/` 的正式对外角色应当是一个 facade，而不是单文件实现。

## 4. 这次重构的正式目标

### 4.1 目标不是“把一个大文件切成几个平铺文件”

这次不能做成：

- `queue_read.c`
- `queue_write.c`
- `queue_event.c`
- `queue_debug.c`

这种仍然依赖前缀平铺来模拟分组的结构。

按照开发原则，既然 `queue` 已经形成稳定子组件，就应该继续下沉成目录层级，让目录本身表达边界。

### 4.2 目标结构

目标目录固定收成：

```text
queue/
  queue.h
  slot/
    slot.h
    slot.c
  read/
    read.h
    read.c
  write/
    write.h
    write.c
  event/
    event.h
    event.c
  lifecycle/
    lifecycle.h
    lifecycle.c
  debug/
    debug.h
    debug.c
```

如果后续发现某个子组件继续膨胀，再继续在对应目录内部下沉，例如：

- `write/ack/`
- `write/request/`
- `slot/payload/`

而不是重新回到前缀平铺。

### 4.3 分层目标

这次重构后的层次固定分成三层：

1. 底层最小能力：
   - slot/request 结构
   - payload 编码
   - list/bitmap 处理
   - event slot 保存与完成
2. 中层协作组件：
   - read 流水
   - write 流水
   - lifecycle 清理
   - debug 聚合
3. 上层对外 facade：
   - `queue.h`
   - 统一导出当前给 `adapter/control/scsi` 使用的入口

也就是：

- 底层不直接承载 `SCSI`/`control` 业务编排
- 上层不再直接接触内部链表和 bitmap

## 5. 组件职责收口

### 5.1 `slot/`

`slot/` 只负责：

- `YUME_POSTED_SLOT`
- posted slot 分配与释放
- read slot event 写入
- write slot payload 写入
- slot 完成
- slot cancel 时的公共清理

`slot/` 不负责：

- read/write request 生命周期
- ack 处理
- target 清盘

### 5.2 `read/`

`read/` 只负责：

- `YUME_READ_REQUEST`
- read request 分配与完成
- pending read 查找
- `DiskQueueReadSrb`
- `DiskDrainReadSlots`
- `DiskHandleReadAckIoctl`

`read/` 不负责：

- write bitmap
- event slot
- debug 汇总

### 5.3 `write/`

`write/` 只负责：

- `YUME_WRITE_REQUEST`
- bitmap 与 seq 相关逻辑
- write request 分配与完成
- pending write 查找
- `DiskQueueWriteSrb`
- `DiskDrainWriteSlots`
- `DiskHandleWriteAckBatchIoctl`
- `DiskCompleteDeferredWriteCompletions`

这是当前最复杂的一块，也是本轮最高风险组件。

### 5.4 `event/`

`event/` 只负责：

- event slot submit
- event slot pending 完成
- event slot 状态保存与释放

当前虽然正式事件类型为空，但 `event slot` 骨架仍然是稳定组件，不能继续挂在大杂烩文件里。

### 5.5 `lifecycle/`

`lifecycle/` 只负责：

- queue 初始化
- queue 清理
- target pending 清盘
- all pending 清盘

这里固定只收“收尾、归零、释放、完成”语义，不混入 request 分配或 ack 校验。

### 5.6 `debug/`

`debug/` 只负责：

- `DiskQueryDebugState`
- 针对 read/write queue 的统计聚合

它不能继续和热路径实现混在一起。

## 6. 公开面收束规则

### 6.1 `queue.h` 继续作为唯一对外入口

当前引用方仍然只 include：

- `queue/queue.h`

这次不把内部子组件头文件直接暴露给：

- `control/`
- `scsi/`
- `adapter/`

也就是：

- 外部继续只认 facade
- 子组件头只在 `queue/` 内部互相引用

### 6.2 子组件间依赖方向

建议的依赖方向固定为：

- `slot/` 只依赖更底层公共定义
- `read/` 依赖 `slot/`
- `write/` 依赖 `slot/`
- `event/` 依赖 `slot/`
- `lifecycle/` 可以依赖 `slot/read/write/event`
- `debug/` 只读各组件状态
- `queue.h` 最后统一收口导出

禁止做成：

- `slot/` 反向依赖 `read/` 或 `write/`
- `debug/` 反向驱动业务逻辑
- `lifecycle/` 承担 payload 编码

## 7. 分阶段实施步骤

### Phase A. 先建立目录骨架和内部头文件

目标：

- 建出 `slot/read/write/event/lifecycle/debug`
- 明确每个目录的 `h/c`
- 保持 `queue.h` 暂时不变

要求：

- 这一步先不追求大量迁移
- 先把正式结构搭起来
- 明确哪些私有类型放到哪个目录

验收：

- `queue/` 目录结构成型
- 代码仍可编译

### Phase B. 先抽 `slot/`

目标：

- 把所有 posted slot 与 payload 编码逻辑先抽到底层

原因：

- `slot/` 是 `read/write/event` 的共同底层
- 先抽它，后面其他组件的边界才稳定

迁移内容：

- `YUME_POSTED_SLOT`
- `DiskAllocPostedSlot`
- `DiskCompleteSlotSrb`
- `DiskCompleteEventSlotSrb`
- `DiskWriteReadSlotEvent`
- `DiskWriteWriteSlotPayload`
- cancel 里纯 slot 侧的公共部分

验收：

- read/write/event 原有行为不变
- `YumeDiskSCSI` 编译通过

### Phase C. 抽 `read/`

目标：

- 把 read 从大文件中完整独立

迁移内容：

- `YUME_READ_REQUEST`
- read request 分配与完成
- pending read 查找
- `DiskDrainReadSlots`
- `DiskQueueReadSrb`
- `DiskHandleReadAckIoctl`

验收：

- `POST_READ_SLOT -> READ_ACK` 完整闭环不变
- 相关测试通过

### Phase D. 抽 `write/`

目标：

- 把 write 的 request/ack/drain 全部独立

迁移内容：

- `YUME_WRITE_REQUEST`
- bitmap/seq/payload shape
- write request 分配与完成
- pending write 查找
- `DiskDrainWriteSlots`
- `DiskQueueWriteSrb`
- `DiskHandleWriteAckBatchIoctl`
- `DiskCompleteDeferredWriteCompletions`

注意：

- 这一步风险最高
- 必须盯住 `AckedBitmap`、`AckedCount`、`NextIssueSeq`、`TotalSeq`
- 不能引入 seq 越界、重复 ack、提前完成、漏完成

验收：

- `POST_WRITE_SLOT -> WRITE_ACK_BATCH -> deferred completion` 闭环不变
- 编译和测试通过

### Phase E. 抽 `event/` 和 `lifecycle/`

目标：

- 把“event slot 骨架”和“pending 清盘”从主线中拆开

迁移内容：

- `DiskHandleSubmitEventSlotIoctl`
- `DiskCompletePendingEventSlot`
- `DiskInitializeQueueState`
- `DiskFreeQueuedState`
- `DiskCompleteTargetPendingInternal`
- `DiskCompleteTargetPending*`
- `DiskCompleteAllPending*`

注意：

- 当前没有正式盘级事件类型，不等于可以删掉 `event slot`
- 要保留空骨架，不要误删协议面或 ioctl 面

验收：

- session close / target cleanup / event slot 提交行为不变

### Phase F. 抽 `debug/` 并收紧 facade

目标：

- 最后把 debug 聚合抽走
- 同时收紧 `queue.h`

迁移内容：

- `DiskQueryDebugState`
- facade 对外只保留现有正式入口
- 删除内部 helper 的多余可见性

验收：

- `queue.c` 不再存在，或者只剩极薄 facade，不再承载真实实现
- 外部引用面比现在更窄，不更宽

## 8. 测试与验收要求

### 8.1 最低编译验收

每个阶段后至少要重新编：

- `YumeDiskSCSI`

### 8.2 行为验收重点

必须重点回归下面几条链：

1. `DiskQueueReadSrb -> DiskDrainReadSlots -> DiskHandleReadAckIoctl`
2. `DiskQueueWriteSrb -> DiskDrainWriteSlots -> DiskHandleWriteAckBatchIoctl`
3. `DiskHandleSubmitSlotIoctl -> cancel`
4. `DiskCompleteTargetPending*`
5. `DiskHandleSubmitEventSlotIoctl -> pending completion`
6. `DiskQueryDebugState`

### 8.3 建议补的测试

如果当前测试不足，优先补：

- read ack 找不到 request
- write ack batch 重复 ack
- write ack batch 越界 seq
- posted write slot payload shape 不一致
- cancel 不存在 slot
- target cleanup 后 posted/pending 全部完成

## 9. 本轮禁止项

这轮明确禁止：

- 只把 `queue.c` 拆成 `queue_read.c`、`queue_write.c` 这种前缀平铺
- 新增一个 `queue_helper.c` 或 `queue_service.c` 继续兜底复杂度
- 为了少改引用，把内部头文件直接暴露给 `adapter/control/scsi`
- 保留旧实现，再包一层新接口
- 一边拆目录，一边顺手改协议或行为语义
- 因为当前 event 为空，就把 `event slot` 链路删掉

## 10. 当前建议执行顺序

推荐顺序固定为：

1. `slot/`
2. `read/`
3. `write/`
4. `event/`
5. `lifecycle/`
6. `debug/`
7. 收紧 `queue.h`

原因：

- `slot/` 是最稳定底层
- `read/` 相对简单，适合先验证拆分模式
- `write/` 复杂度最高，要在结构已经稳定后再收
- `event/lifecycle/debug` 适合最后统一归位

## 11. 完成态判断

这次重构完成后，应满足：

- `queue/` 不再依赖单个超大实现文件
- 目录层级本身能直接表达组件边界
- `read/write/event/lifecycle/debug` 职责单一
- `queue.h` 成为唯一稳定 facade
- 外部引用不感知内部子组件细节
- 现有行为语义不变

如果最后只是：

- 文件名变多了
- 目录变深了
- 但 read/write/event 仍互相缠绕

那就不算满足开发原则第 5 条。
