# KMDF 内核占用压缩最终方案

## 1. 当前结论

当前 `ct -> 压测 -> rm` 闭环、多盘并发、Q2/Q8/Q32 稳定性已经打通，单盘顺序吞吐也已经恢复到约 `600 MB/s`。剩余问题不是 App worker 模型，也不是 SCSI per-target queue 的基本结构，而是单盘压测时内核态 CPU 明显偏高。

当前最可疑的热路径是 `YumeDiskKMDF` 的 async slot transport：

```text
App POST_READ_SLOT / POST_WRITE_SLOT
  -> KMDF ControlProxySubmitSlotAsync
  -> ControlAlloc(asyncRequest)
  -> ControlAlloc(ioctlBuffer)
  -> IoAllocateIrp
  -> IoAllocateWorkItem
  -> IoQueueWorkItem
  -> IoCallDriver(IOCTL_SCSI_MINIPORT)
  -> completion
  -> WdfRequestComplete
  -> IoFreeIrp / IoFreeWorkItem / ControlFree
```

这些动作每个 slot 都执行一次。吞吐上来以后，固定开销会稳定表现为高内核态 CPU。

## 2. 当前锁结构审计

### 2.1 KMDF

当前 KMDF 锁和等待结构：

- `CTRL_DEVICE_CONTEXT.OpenLock`：`WDFSPINLOCK`
- `CTRL_FILE_CONTEXT.SessionLock`：`WDFWAITLOCK`
- `InFlightZeroEvent`：`KEVENT`
- `PendingSlotZeroEvent`：`KEVENT`

当前判断：

- `OpenLock` 只保护 `OpenCount/OpenFileObject`，范围很小，保留 `WDFSPINLOCK` 合理。
- `SessionLock` 用于 open、heartbeat、cleanup、miniport handle/session state，属于 PASSIVE 级生命周期锁，保留 `WDFWAITLOCK` 合理。
- `PendingSlotCount` 和 `InFlightRequestCount` 用 interlocked 计数，cleanup/watchdog 再用 `KEVENT` drain，方向合理。
- 问题在于 `ControlSessionRegisterPendingSlot` 现在每个 slot 都进 `WDFWAITLOCK` 检查 session，这会把高频数据面带回 PASSIVE wait lock 热路径。

最终规则：

- `WDFWAITLOCK` 只用于低频生命周期路径：open、heartbeat、cleanup、watchdog、close miniport handle。
- `POST_READ_SLOT/POST_WRITE_SLOT` 高频路径不得依赖 `SessionLock` 做每次准入。
- 高频路径改成原子准入：读取 session run state、增加 pending slot 引用、再次确认 closing generation/state，失败则回滚引用。
- `IoCallDriver`、`IoQueueWorkItem`、`WdfRequestComplete`、内存分配、等待事件都不能在 `WDFSPINLOCK` 或 `KSPIN_LOCK` 内执行。

### 2.2 SCSI

当前 SCSI 锁结构：

- `DEVICE_CONTEXT.SessionLock`：`KSPIN_LOCK`
- `YUME_DISK.BufferLock`：`KSPIN_LOCK`
- `YUME_DISK_QUEUE_STATE.ReadQueueLock`：`KSPIN_LOCK`
- `YUME_DISK_QUEUE_STATE.WriteQueueLock`：`KSPIN_LOCK`

当前判断：

- Storport/miniport 路径可能运行在较高 IRQL，SCSI 侧使用 `KSPIN_LOCK` 是合理方向。
- 当前 `DiskDrainReadSlots` / `DiskDrainWriteSlots` 已经遵守核心规则：锁内只摘队列和改状态，锁外写 App slot、复制数据、complete SRB。
- `WRITE_ACK_BATCH` 也是锁内落账，完成原始 WRITE SRB 延后到锁外，方向正确。

最终规则：

- SCSI per-target read/write queue 继续使用 `KSPIN_LOCK`。
- 不把 SCSI 队列锁换成 wait lock。
- 不在 spin lock 内做大块 copy、StorPort completion、KMDF/miniport control completion、内存分配或等待。
- 如果后续发现 SCSI spin lock CPU 明显偏高，优先优化锁内线性查找和分配次数，而不是把队列锁改成 wait lock。

### 2.3 临界区设计总规则

后续所有 KMDF/SCSI 并发改动统一按“取走即解锁”设计。锁只保护状态所有权转移，不承载实际工作。

固定流程：

```text
锁外准备
  -> 分配内存
  -> 校验不依赖共享状态的参数
  -> 计算长度、offset、seq、capacity

锁内转移状态
  -> 重新校验共享状态仍成立
  -> 从队列摘走 request / slot
  -> 修改计数、bitmap、state、generation
  -> 必要时把对象挂到本地 deferred list
  -> 立即解锁

锁外执行重活
  -> RtlCopyMemory
  -> IoCallDriver
  -> StorPortNotification(RequestComplete)
  -> WdfRequestComplete
  -> ControlFree / DiskFree
  -> KeWaitForSingleObject / WdfTimerStop(wait=TRUE)
  -> signal event
```

禁止项：

- 禁止在 `KSPIN_LOCK/WDFSPINLOCK` 内做任何等待。
- 禁止在 `KSPIN_LOCK/WDFSPINLOCK` 内调用 `IoCallDriver`。
- 禁止在 `KSPIN_LOCK/WDFSPINLOCK` 内完成 SRB 或 WDFREQUEST。
- 禁止在 `KSPIN_LOCK/WDFSPINLOCK` 内做大块 `RtlCopyMemory`。
- 禁止在 `KSPIN_LOCK/WDFSPINLOCK` 内做可能触发复杂路径的分配/释放。
- 禁止持有 `WDFWAITLOCK` 后再等待 pending slot drain 或调用同步 miniport IOCTL。

允许项：

- 在锁内做链表插入/摘除。
- 在锁内做小字段赋值、计数增减、bitmap 标记。
- 在锁内读取当前 session id、state、generation。
- 在锁内把需要完成的对象移动到本地 list。

### 2.4 当前路径的锁边界设计

#### KMDF submit queue

`SubmitQueueLock` 使用 `WDFSPINLOCK` 或 `KSPIN_LOCK` 均可，前提是临界区只包住链表：

```text
enqueue submit object:
  锁外填对象字段和 IRP buffer
  lock SubmitQueueLock
  InsertTailList(SubmitQueue)
  queued++
  unlock
  KeSetEvent(SubmitEvent)

worker dequeue:
  lock SubmitQueueLock
  Move up to N objects to local list
  queued -= N
  unlock
  for each local object:
      IoCallDriver
```

不要在锁内逐个 `IoCallDriver`。worker 每次从共享队列批量取一小段到本地 list，可以减少反复抢锁，又不会长时间占锁。

#### KMDF free list / object pool

对象池也采用“拿对象即解锁”：

```text
acquire object:
  lock FreeListLock
  if free list not empty:
      pop object
      unlock
      return object
  if created < max:
      created++
      unlock
      allocate object outside lock
      return object
  unlock
  return STATUS_DEVICE_BUSY
```

如果锁外分配失败，必须用 interlocked 或短锁回滚 `created`。不能为了等空闲对象而在锁内等待 `FreeEvent`。初版也不建议在 KMDF 内等待池对象，直接返回 busy 给 App 由 App 重投，避免 cleanup 卡死。

对象回池：

```text
complete object:
  锁外完成 WDFREQUEST
  清空 per-request 字段
  lock FreeListLock
  push object
  unlock
  KeSetEvent(FreeEvent)
```

#### KMDF session state

`SessionLock` 只保护低频生命周期变更：

- open 成功后发布 `MiniportHandle/FileObject/DeviceObject/SessionId`
- heartbeat 更新 `LastHeartbeatTick`
- cleanup/watchdog 把 state 从 active 改成 closing/locked/closed
- close miniport handle 前后的状态切换

slot 高频路径只做原子准入，不拿 `SessionLock`：

```text
read active state/generation
InterlockedIncrement(PendingSlotCount)
read active state/generation again
if changed:
    InterlockedDecrement(PendingSlotCount)
    fail request
```

cleanup 路径：

```text
lock SessionLock
state = Closing
unlock

wake transport runtime
send RemoveAllDisks | SessionClose
wait PendingSlotCount == 0

lock SessionLock
close miniport handle
state = Closed/Locked
unlock
```

不能持有 `SessionLock` 去等 `PendingSlotZeroEvent`。否则 slot completion 如果需要碰 session 状态，很容易形成等待环。

#### SCSI read drain

当前 `DiskDrainReadSlots` 的方向是正确的，保持并强化：

```text
lock ReadQueueLock
find pending read
pop posted read slot
mark SlotIssued / counters
unlock

write read event into App buffer
complete slot SRB
free slot object
```

锁内不能写 App buffer，也不能 complete slot SRB。

后续如果 `DiskFindNextPendingReadLocked` 的线性查找成为热点，优化方向是维护 ready list 或 pending-unissued list，而不是扩大锁范围。

#### SCSI write drain

当前 `DiskDrainWriteSlots` 也基本正确：

```text
lock WriteQueueLock
find writable request
pop posted write slot
seq = NextIssueSeq++
unlock

copy SRB fragment to App write slot
complete slot SRB
free slot object
```

锁内只决定 `seq` 的所有权。`RtlCopyMemory(SRB -> App slot)` 必须继续在锁外。

后续可优化 `DiskFindNextWritableRequestLocked` 的线性查找，但不能把 copy 放回锁内。

#### SCSI READ_ACK

目标结构：

```text
lock ReadQueueLock
find request by EventId
remove request from pending list
update counters
unlock

validate data length / KernelVa
copy App buffer to original SRB
complete original READ SRB
free request
```

当前实现已经符合这个方向。注意 `READ_ACK` 失败时也只能影响该 request，不能在锁内完成，也不能扩大到磁盘级状态。

#### SCSI WRITE_ACK_BATCH

`WRITE_ACK_BATCH` 的锁内工作要控制在“落账”范围：

```text
for each range:
    锁外做 range 基础校验和 endSeq 计算
    lock WriteQueueLock
    find write request
    check seq bounds / duplicate bitmap
    set ack bits
    if request complete or failed:
        remove from pending
        move request to local deferred completion list
    unlock

锁外写 batch result
锁外完成 WRITE_ACK_BATCH ioctl SRB
锁外完成 deferred original WRITE SRB
```

当前实现里 bitmap 检查和置位在锁内是合理的，因为它们是 write request 状态的一部分。但如果单个 ACK range 很大，锁内循环会变长。后续如果这里成为热点，优化方向是限制 batch range 长度或改成位图批量操作，不把 completion/copy 搬进锁内。

#### SCSI cleanup/remove

remove/cleanup 必须采用“两阶段摘除”：

```text
lock target read/write locks
move all pending requests and posted slots to local lists
reset queue counters/state
unlock

complete all local SRBs
complete all local slot SRBs
free all local objects
notify bus change
```

当前 `DiskCompleteTargetPending` 已经是这个方向。后续要避免在持锁状态下调用 `DiskCompleteReadRequest`、`DiskCompleteWriteRequest`、`DiskCompleteSlotSrb`。

#### Disk storage buffer

`DiskResetDiskStorage` 当前在 `BufferLock` 内释放 `Disk->Buffer`。这不是当前热路径，但从锁边界看不够干净。

建议最终改成：

```text
lock BufferLock
oldBuffer = Disk->Buffer
Disk->Buffer = NULL
unlock

DiskFree(oldBuffer)
```

如果以后重新启用内核内存介质或 buffer 快照，这个规则同样适用：锁内只交换指针，锁外分配/释放/清零大块内存。

## 3. 最终优化边界

第一阶段只重建 KMDF async slot transport 的对象生命周期和提交模型，不改协议语义。

保持不变：

- App 仍然持有唯一介质。
- 当前 `POST_READ_SLOT`、`POST_WRITE_SLOT`、`READ_ACK`、`WRITE_ACK_BATCH` 协议不变。
- 系统侧取消只追到 SCSI。
- late `READ_ACK/WRITE_ACK_BATCH` 仍由 SCSI 判断 stale / cancelled / not found。
- SCSI per-target queue 不回退。
- App per-disk bounded workers 不回退。

本阶段不做：

- 不做协议 v2。
- 不做 slot bundle。
- 不把取消链路扩展到 App。
- 不把 SCSI spin lock 改成 wait lock。
- 不新增旧版本兼容层。

## 4. 最终方案

### 4.1 Session 内 async slot transport runtime

在 `CTRL_FILE_CONTEXT` 下新增一个 session-owned transport runtime：

```text
CTRL_FILE_CONTEXT
  -> TransportRuntime
       -> SubmitQueueLock
       -> FreeListLock
       -> FreeEvent
       -> SubmitEvent
       -> StopEvent
       -> WorkerThread(s)
       -> ActiveCount
       -> PoolHighWater
       -> Closing
       -> FreeList
       -> SubmitQueue
```

这个 runtime 与 App file object 同生命周期：

```text
CreateFile success
  -> open miniport handle
  -> initialize transport runtime
  -> start submit worker(s)

FileCleanup / watchdog
  -> mark session closing
  -> stop accepting new slots
  -> send RemoveAllDisks | SessionClose
  -> wake workers
  -> drain active objects
  -> stop workers
  -> free transport pool
  -> close miniport handle
```

### 4.2 有界对象池

把当前每 slot 临时创建的 `CTRL_ASYNC_SLOT_REQUEST` 改成池对象：

```text
CTRL_ASYNC_SLOT_OBJECT
  -> WDFREQUEST Request
  -> PIRP Irp
  -> PUCHAR IoctlBuffer
  -> ULONG IoctlBufferSize
  -> IO_STATUS_BLOCK / completion fields
  -> SlotId / TargetId / SlotType
  -> DirectBuffer / DirectBufferSize
  -> State
  -> Link
```

池对象长期持有：

- `PIRP`
- `SRB_IO_CONTROL + YUMEDISK_MESSAGE + YUMEDISK_SUBMIT_SLOT` 小 buffer
- completion 上下文

热路径只做：

```text
Acquire pooled object
  -> fill descriptor
  -> IoReuseIrp
  -> initialize stack + completion
  -> enqueue submit queue
  -> return STATUS_PENDING to WDF
```

completion 后：

```text
miniport completion
  -> complete original WDFREQUEST
  -> clear per-request fields
  -> return object to pool
  -> signal FreeEvent if waiters exist
```

池容量规则：

- 不按 `YUMEDISK_MAX_TARGETS * QD * 2` 预分配。
- 按真实并发水位增长，session 内复用。
- 初始池可以很小，例如 `64`。
- 上限按配置或常量限制，例如 `active disks * queue_depth * 2 + margin`，同时设置硬上限。
- 达到上限且无空闲对象时，`POST_*_SLOT` 返回 `STATUS_DEVICE_BUSY` 或 `STATUS_INSUFFICIENT_RESOURCES`，由 App 重投。
- 初版优先返回错误给 App，不在 KMDF 内无限等待池对象，避免 cleanup 卡死窗口扩大。

### 4.3 IRP 复用

池对象的 IRP 由 `IoAllocateIrp` 创建一次，后续用 `IoReuseIrp` 复用。

每次提交前必须重新初始化：

- `Irp->RequestorMode`
- `Irp->AssociatedIrp.SystemBuffer`
- `Irp->UserBuffer`
- `Irp->Flags`
- `Irp->IoStatus`
- next stack location
- `IoSetCompletionRoutine`

completion routine 继续返回 `STATUS_MORE_PROCESSING_REQUIRED`，由 KMDF transport 自己完成原始 App `WDFREQUEST` 并回收池对象。

注意：

- 只复用自己通过 `IoAllocateIrp` 分配的 IRP。
- completion 之后、对象回池之前，必须保证 miniport 不再持有该 IRP。
- session cleanup 必须等待 active 对象归零后才能 `IoFreeIrp`。

### 4.4 去掉每-slot work item

当前每 slot 都 `IoAllocateWorkItem + IoQueueWorkItem`，这会引入系统 worker queue 固定开销。

最终改成：

- 每 session 一个长期 submit worker。
- 如单 worker 仍压不住，再扩成少量固定 worker，例如 `2`，不按 QD 创建线程。
- worker 等待 `SubmitEvent`，批量 drain submit queue。
- worker 在 PASSIVE_LEVEL 调用 `IoCallDriver`。

worker 主循环：

```text
while (!Stopping) {
    wait SubmitEvent / StopEvent
    while (dequeue submit object) {
        prepare IRP if needed
        status = IoCallDriver(miniportDeviceObject, irp)
        handle immediate completion race
    }
}
```

关键点：

- `IoCallDriver` 不在 spin lock 内执行。
- dequeue 后对象进入 active 状态，直到 completion routine 或 immediate completion path 回收。
- cleanup 先禁止新入队，再唤醒 worker，再等待 active 对象归零。

### 4.5 高频 session 准入去锁化

当前 `ControlSessionRegisterPendingSlot` 每个 slot 都拿 `SessionLock`，对象池改完后它可能成为新的热点。

最终把数据面准入拆成两层：

低频生命周期层：

- `SessionLock` 只负责改变 session state、miniport handle、watchdog、closing。

高频数据面层：

- 用原子 state/generation 判断 session 是否 active。
- 用 interlocked 增加 `PendingSlotCount`。
- 增加后再次确认 session 未 closing 且 generation 未变化。
- 如果失败，回滚 `PendingSlotCount`。

伪代码：

```c
if (ReadState() != Active) return STATUS_DELETE_PENDING;

InterlockedIncrement(&PendingSlotCount);
if (ReadState() != Active || GenerationChanged()) {
    InterlockedDecrement(&PendingSlotCount);
    return STATUS_DELETE_PENDING;
}
```

cleanup/watchdog：

```text
set state = Closing
wake transport runtime
send session cleanup to SCSI
wait PendingSlotCount == 0
free runtime
close miniport handle
```

这一步必须保持单一真实来源：session state 仍归 `CTRL_FILE_CONTEXT`，只是读路径用原子快照，不复制第二套 session 状态机。

## 5. ACK 命令优化边界

`READ_ACK` 当前已经是小描述符 + `KernelVa`，不会把 1MB 数据复制进 `SRB_IO_CONTROL`。

`WRITE_ACK_BATCH` 是小控制命令。

因此第一阶段不需要把 ACK 也改成 async 池化路径。但可以做低风险复用：

- 为同步 `ControlProxyCommand` 增加 session-local 小 buffer 缓存，避免短命 `ControlAlloc`。
- 不改变命令同步语义。
- 不把 ACK completion 顺序改回旧模型。

优先级低于 async slot transport 池化。

## 6. 实施顺序

### Step 1：补 transport runtime 骨架

- 在 `CTRL_FILE_CONTEXT` 增加 transport runtime 字段。
- session open 初始化 runtime。
- cleanup/watchdog 停止 runtime。
- 不改变数据面行为。
- 编译通过。

### Step 2：引入 async slot object pool

- 新增池对象结构。
- 对象创建时分配 IRP 和 IOCTL buffer。
- completion 后对象回池。
- 暂时仍可沿用现有每-slot work item 投递，先验证池对象生命周期。
- 编译通过后测 `ct -> Q1 -> rm`。

### Step 3：IRP 复用

- 提交前使用 `IoReuseIrp`。
- 每次重建 IRP stack 和 completion routine。
- 删除每-slot `IoAllocateIrp/IoFreeIrp`。
- 验证 Q1/Q8/Q32 和 Ctrl+C/rm all。

### Step 4：长期 submit worker

- 删除每-slot `IoAllocateWorkItem/IoFreeWorkItem`。
- 每 session 启动固定 submit worker。
- worker 批量 drain submit queue。
- 验证吞吐、kernel CPU、cleanup。

### Step 5：高频 session 准入去 wait lock

- 将 `POST_*_SLOT` 的 pending slot 准入从 `WDFWAITLOCK` 调整为原子 state + pending ref。
- `SessionLock` 留在 open/heartbeat/cleanup/watchdog。
- 验证 cleanup/watchdog 不引入 use-after-close。

## 7. 验收指标

功能：

- `ct -> disk visible -> diskspd/DiskMark -> rm all` 正常。
- Q1/Q2/Q8/Q32 读写压力不挂死。
- Ctrl+C 能退出压测。
- App exit / `rm all` / heartbeat timeout 都能完成 cleanup。
- late `READ_ACK/WRITE_ACK_BATCH` 仍由 SCSI 返回 stale/cancelled/not found，不影响后续 I/O。

性能：

- 单盘顺序吞吐不低于当前约 `600 MB/s` 水平。
- 单盘 kernel CPU 明显低于当前约 `50%`。
- 多盘总吞吐不因全局锁或单 worker 被串行化。

调试：

- 保留低频 debug counters。
- 不恢复热路径 print。
- 新增 transport counters：
  - pool objects created
  - pool high water
  - pool acquire failures
  - submit queued
  - submit completed
  - immediate completions
  - completion races
  - active objects
  - cleanup drain wait count

## 8. 主要风险

### 8.1 IRP completion race

`IoCallDriver` 可能同步完成，也可能异步完成。当前代码已经用 `CompletionState` 处理 race。对象池后仍要保留等价状态机，不能因为复用对象而简化掉。

### 8.2 cleanup 与对象回池竞争

cleanup 必须先停止新提交，再等待 active 对象归零，最后释放池。否则 completion routine 可能访问已释放 runtime。

### 8.3 miniport handle 生命周期

池对象持有的 IRP stack 会引用 `MiniportFileObject/MiniportDeviceObject`。关闭 miniport handle 前必须保证没有 active submit 对象。

### 8.4 spin lock 误用

submit queue/free list 可以用 spin lock 保护链表，但锁内只能做链表操作和状态切换。任何 `IoCallDriver`、WDF request completion、内存分配、等待都必须在锁外。

### 8.5 wait lock 继续留在热路径

如果只做对象池，不处理 `ControlSessionRegisterPendingSlot` 的 per-slot `WDFWAITLOCK`，CPU 可能只能下降一部分。最终方案必须把高频 session 准入从 wait lock 中拿出来。

## 9. 官方 API 依据

- `IoReuseIrp` 可用于复用由驱动自己分配的 IRP：
  https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-ioreuseirp
- `IoQueueWorkItem` 会把 work item 放入系统 worker thread 队列，当前每 slot 使用它会形成固定调度成本：
  https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-ioqueueworkitem
- WDF wait lock 适合 PASSIVE_LEVEL 数据同步，WDF spin lock 适合 IRQL <= DISPATCH_LEVEL 的短临界区：
  https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/using-framework-locks

## 10. 最终结论

KMDF 内核占用压缩的第一优先级不是改协议，而是重建 async slot transport：

```text
每-slot 分配/释放 + 每-slot work item
  -> session-owned 有界对象池
  -> IRP/IOCTL buffer 复用
  -> 长期 submit worker 批量投递
  -> 高频 session 准入去 wait lock
```

SCSI 侧队列继续保持 per-target `KSPIN_LOCK`，因为它处在 Storport/miniport 调度路径上；真正需要压的是 KMDF 在每个 slot 上制造的对象生命周期、worker 调度和 wait lock 热路径成本。
