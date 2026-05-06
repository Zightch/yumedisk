# Windows驱动问题排查笔记

本文档记录当前 `YumeDiskKMDF`、`YumeDiskSCSI`、`RWTestApp` 在重建 App-owned media queue 过程中已经遇到过的典型问题、确认过的根因、修复方式和后续注意事项。

目标不是复述协议设计，而是把真实踩坑过程收成一本可复用的排查笔记，方便后续重构、回归和多盘并发验证时快速对照。

## 1. 适用范围

- 用户态后端：`windows/tests/RWTestApp/main.cpp`
- KMDF 控制驱动：`windows/YumeDiskKMDF/YumeDiskKMDF`
- Storport miniport：`windows/YumeDiskSCSI/YumeDiskSCSI`
- 当前协议：`POST_READ_SLOT`、`POST_WRITE_SLOT`、`READ_ACK`、`WRITE_ACK_BATCH`

## 2. 统一排查思路

先分层，再判断卡在哪一层，不要一上来把“有设备无盘”“Q8挂死”“1117”都混成一个问题。

### 2.1 四层划分

1. `App` 是否成功打开 `YumeDiskKMDF`，并持续预投 slot。
2. `KMDF` 是否成功持有 miniport handle，并把 slot transport 到 `YumeDiskSCSI`。
3. `SCSI` 是否把系统 `READ/WRITE SRB` 正确挂到 per-target 队列，并完成首个 probe read。
4. 系统存储栈是否已经拿到 probe read 结果，真正把盘枚举成 `Get-Disk` 可见磁盘。

### 2.2 最有价值的观测点

- `ct` / `rm` / `rm all`
- `debug_snapshot`
- `debug_scsi`
- `YumeDiskKMDF` 的 `ControlSession*` / `ControlProxySubmitSlot*` 日志
- `YumeDiskSCSI` 的 `DiskQueueReadSrb` / `DiskDrainReadSlots` / `DiskQueueReadAck` 日志
- Windows 事件查看器中的 `disk 153`、`storport 129`

### 2.3 常见计数判读

如果出现下面这种组合，说明卡在“首个 probe read 没有真正回到系统”：

```text
backend_read_slot_posts > 0
backend_read_slot_completions = 0
backend_read_ack_commands = 0
debug_scsi posted_read_slots = 1
debug_scsi pending_reads = 1
debug_scsi read_completed = 0
```

如果出现下面这种组合，说明高队列深度下有 slot/cancel/reset 没有收干净：

```text
read_post_slot = 32
pending_reads = 8 / 31
backend_command_failures 持续增加
POST_READ_SLOT transient failure, error=1117
磁盘 100%，但无实际读写吞吐
```

## 3. 问题归档

### 3.1 设备打开失败：`open control device failed, error=50`

相关提交：

- `45fb1cf 修复open问题`

现象：

- 用户态打开控制设备失败，Win32 返回 `50`，即 `ERROR_NOT_SUPPORTED`。
- KMDF 日志出现：

```text
ControlSessionTryOpen: create resources failed ... status=C00000BB
ControlEvtFileCreate: ... status=C00000BB
```

定位信号：

- `C00000BB` 是 `STATUS_NOT_SUPPORTED`。
- 失败发生在 `ControlSessionTryOpen` 的“创建 file/session 资源”阶段，还没进入真正的数据面。
- 如果 `QUERY_INFO`/miniport 探测日志还没出现，说明问题在 KMDF session 资源创建，不在 SCSI。

根因：

- file 绑定 session/watchdog 引入后，KMDF 的 watchdog 资源创建方式不成立。
- 本轮修复中，问题点落在 session 资源模型本身：watchdog 定时器和 file/session 生命周期耦合方式不对，导致 `ControlSessionCreateResources` 返回 `STATUS_NOT_SUPPORTED`。
- 同时，旧实现把 session 锁、watchdog、下层 handle 和 in-flight I/O 生命周期缠在一起，后续就算侥幸打开成功，也容易在 cleanup/watchdog 上埋死锁和悬挂 I/O。

修复：

- 把 watchdog 改成 file 绑定的 one-shot timer，不再使用旧的 periodic 方式。
- 心跳到达时手动重新 `WdfTimerStart`，而不是依赖周期性 timer 常驻。
- session 内新增 `InFlightRequestCount + InFlightZeroEvent`，清理顺序固定为：

```text
SessionClose / WatchdogLock
  -> RemoveAllDisks / SessionClose 下发到 SCSI
  -> drain in-flight IO
  -> close miniport handle
```

- `ControlSessionRelease` 不再只是解锁，而要和 in-flight 引用计数配对。

注意事项：

- 看到 `error=50` 时，先怀疑 `KMDF` 自己的 session 资源创建，不要先怀疑 `App` 路径或 `CreateFile` 参数。
- file object 下面挂的 KMDF 对象必须严格检查是否支持当前配置，尤其是 timer、serialization、cleanup 顺序。
- session 锁绝对不能长时间包住 `RemoveAllDisks -> drain -> close handle` 这种跨层流程。

### 3.2 有设备无盘：设备管理器里有盘，磁盘管理器/`Get-Disk` 没盘

相关提交：

- `45fb1cf 修复open问题`
- `2a785ba Fix KMDF async slot transport`
- `26e25da Fix probe read slot completion path`
- `8d969bf 修复有设备无盘问题`

现象：

- `ct` 成功，设备管理器里能看到 `Zightch YumeDisk SCSI Disk Device`。
- `visible_path=<pending-enumeration>` 长时间不变。
- `Get-PnpDevice` 能看到设备，`Get-Disk` 看不到新增磁盘。
- 事件查看器常伴随 `disk 153`，目标通常是 `LBA 0`。

典型日志组合：

```text
DiskQueueReadSrb: queue target=0 event=1 lba=0 blocks=1 bytes=4096
DiskDrainReadSlots: dispatch target=0 event=1 slot=1 lba=0 blocks=1 bytes=4096
backend_read_slot_completions=0
backend_read_ack_commands=0
posting_read_slots=1
active_read_slots=1
```

根因：

这个问题至少出现过三类根因。

第一类是接口选错：

- `KMDF` 枚举 `GUID_DEVINTERFACE_STORAGEPORT` 时会扫到大量非本驱动接口。
- 只有通过 `QUERY_INFO` 校验签名、协议版本、service name 后，才能确认目标 miniport。
- 否则容易打开到别的 storageport 接口，`ZwDeviceIoControlFile(IOCTL_SCSI_MINIPORT)` 直接失败。

第二类是首个 probe read 没真正闭环：

- 系统会先对新盘发 `LBA 0 / 4KB` 一类的探测读。
- `SCSI` 已经把这个读排进 `PendingReads`，并且把 read slot 派发给 `KMDF`。
- 但 `App` 没有真正拿到这个 slot completion，或者拿到了 completion 但没有被正确消费。
- 结果是设备节点存在，但 disk.sys/classpnp 一直等不到 probe read 成功返回，因此磁盘永远停在“设备已到达，盘未枚举完成”的状态。

这类问题在异步 transport 上尤其隐蔽：

- `IoCallDriver` 有时会同步 inline 完成 miniport control IRP。
- 如果 `KMDF` 无条件把这次调用当成“真正的异步 pending”，上层 `WDFREQUEST` 就可能永远不被正确完成。
- 从外部看起来就是：`SCSI` 已 dispatch，`KMDF` 似乎也收到了 completion，但 `App` 侧 completion 计数仍然为零。

第三类是本次最终确认的 `POST_READ_SLOT` 提交线程阻塞：

- 新的 App per-disk slot engine 是单线程模型；同一个线程负责预投 read/write slot、等待 overlapped completion、执行 `READ_ACK` / `WRITE_ACK_BATCH`。
- 如果 `KMDF` 在处理 `POST_READ_SLOT` 的 WDF 请求线程里直接向 storage stack 调 `IoCallDriver`，这次调用在当前 miniport control 场景下可能无法及时返回到用户态。
- 结果是用户态 `DeviceIoControl(POST_READ_SLOT)` 没有变成“已投递，等待 overlapped event”的状态，slot engine 卡在 `BeginAsyncIoControl` 内，根本进不了 `WaitForMultipleObjects`。
- 这时 `SCSI` 日志可以看到 slot 已被完成，`KMDF` 也能打印 `finalStatus=00000000 completeInfo=32`，但 App 侧仍然是 `backend_read_slot_completions=0 / backend_read_ack_commands=0`，因为用户态线程还没机会消费 completion。
- `completeInfo=32` 证明这不是输出长度为 0 的问题；`posting_read_slots` 长时间非 0 才是判断“卡在提交阶段”的关键证据。

修复：

- `KMDF` 打开 miniport 时，不只保存 `HANDLE`，还保存同生命周期的 `FILE_OBJECT + DEVICE_OBJECT`。
- `POST_READ_SLOT` / `POST_WRITE_SLOT` 不再走同步代理，而是走真正的异步 slot transport。
- `submit slot` 的 completion 必须回到原始 `App WDFREQUEST`，不能只完成 miniport control IRP。
- 对 `IoCallDriver` 必须区分两种情况：
  - 真正返回 `STATUS_PENDING`
  - 同步 inline completion
- `ControlProxySubmitSlotAsync` 必须先让原始 `WDFREQUEST` 进入长 pending，再把实际 `IoCallDriver` 放到 `IO_WORKITEM` 中执行，避免阻塞用户态 `DeviceIoControl` 提交流程。
- 手工分配的 slot IRP 由 completion routine 和 work item 用单一 `CompletionState` 竞争收尾；completion routine 返回 `STATUS_MORE_PROCESSING_REQUIRED`，由 KMDF transport 自己完成原始 App request、释放 IRP/work item、注销 pending slot 引用。
- `WdfRequestCompleteWithInformation` 必须返回真实输出长度：
  - read slot 返回 `sizeof(YUMEDISK_READ_SLOT_EVENT)`
  - write slot 返回 `YUMEDISK_WRITE_SLOT_HEADER_BASE_SIZE + DataLength`
- App 侧保留 `posting_read_slots / posting_write_slots` 调试计数，用来区分“已经投递但没完成”和“还卡在投递调用内部”。
- storageport 接口选择必须以 `QUERY_INFO` 签名为最终裁决，而不是“第一个能打开的接口”。

注意事项：

- `visible_path=<pending-enumeration>` 只是结果，不是根因。
- 设备管理器有设备，不代表系统已经把盘枚举成功。
- 如果 `SCSI` 已经看到 `event=1/lba=0`，优先看 read slot completion 是否真的回到 `App`。
- 如果 `posting_read_slots` 长时间非 0，先查 `KMDF POST_*_SLOT` 是否在 WDF 请求线程内同步等待了下层 storage stack，不要先改 SCSI 队列。

直观解释：

- 这个问题可以简单理解成“两段式登记”。
- 第一段是“系统知道来了一个设备”，所以设备管理器里能看到设备。
- 第二段是“系统再问一遍：你是不是一块真盘，前几个扇区能不能正常读出来”，这一步通常就是首个 `LBA 0 / 4KB` probe read。
- 我们当时卡住的是第二段：设备已经到了，但这第一笔探测读没有完整走完，系统一直等不到成功答复。
- 所以外在表现就是“有设备，但没有盘”；不是盘没创建出来，而是系统还不敢把它正式枚举成可用磁盘。

### 3.3 高队列挂死：`Q2/Q8/Q32` 跑一半变成 100% 无读写

相关提交：

- `c63531a debug Q8/Q32`
- `aebb108 修复Q8/Q32问题`
- `430e364 修复1117问题`

现象：

- `Q1` 能跑，`Q2/Q8/Q32` 更容易在跑一段后挂死。
- 磁盘 100%，但没有实际吞吐。
- `diskspd` `Ctrl+C` 也停不下来，经常只能强杀 `RWTestApp`。
- `debug_snapshot` 里常见：

```text
read_post_slot=32
pending_reads=8 / 31
backend_command_failures 持续上升
backend_read_ack_commands 落后 backend_read_slot_completions
POST_READ_SLOT transient failure, error=1117
```

根因：

这不是一个“单点 bug”，而是一组取消/并发/ACK 耦合问题叠加出来的。

第一类根因是 issued slot 取消后没有准确回收原始请求：

- 旧实现只知道“posted slot 在哪”，不知道“这个 slot 已经发给了哪个系统请求”。
- 一旦 slot 已经发出，再发生 reset/remove/cancel，就找不回原始 `READ/WRITE SRB`。
- 结果就是一边 slot 丢了，一边系统 SRB 永远 pending，队列深度被慢慢吃光。

第二类根因是 piggyback ACK 设计把两代状态绑死：

- 旧 `POST_WRITE_SLOT` 同时承担“申请新 slot”和“顺带确认旧 ACK”。
- 一旦 ACK 中夹着 stale/cancelled request，或者系统 write 已结束，这个 `POST_WRITE_SLOT` 的语义就变脏。
- 在取消模型下，这种设计非常容易把 steady-state 队列卡死。

第三类根因是 reset/cleanup 没有真正 complete 全部 pending：

- 只清理了部分 pending I/O，而没有连 issued request、posted slot 一起收干净。
- 这会导致高并发压测后遗留悬挂状态，下次 dispatch 时继续踩到脏状态。

修复：

- `SCSI` 为 read/write request 增加 `IssuedSlotId` / `LastIssuedSlotId`。
- `CancelSlot` 不只取消“还在 posted 队列里的 slot”，还要能反查并结束“已经发出去的原始请求”。
- `WRITE_ACK_BATCH` 成为唯一 ACK 入口，彻底移除 `POST_WRITE_SLOT` 输入里的 piggyback ACK。
- `WRITE_ACK_BATCH` 支持逐项失败返回，stale/cancelled `eventId` 返回 `STATUS_NOT_FOUND`，而不是把整条命令打崩。
- reset / session close / remove 路径要 complete 全部 pending，而不是只 complete 一部分“看起来像 I/O 的对象”。
- 保持 `drain while available`，不要退回“一次只处理一个，等下次再唤醒”的模式。

注意事项：

- 这类挂死很容易被误判成“纯锁死”，实际上更常见的是 slot 丢失、request 悬挂、ACK 失配导致的逻辑性假死。
- `Ctrl+C` 停不掉 `diskspd`，通常不是 `diskspd` 自己坏了，而是底层 raw I/O 没有被正确 cancel/complete。
- 取消和清理必须按“单请求失败不判盘死”的原则实现，不能因为单次读写错误直接把整盘打死。

### 3.4 `POST_READ_SLOT` / `POST_WRITE_SLOT` 出现 `1117`、`995`

相关提交：

- `430e364 修复1117问题`
- `aebb108 修复Q8/Q32问题`

现象：

- 用户态打印：

```text
POST_READ_SLOT transient failure, error=1117
POST_READ_SLOT transient failure, error=995
```

- WinDbg 断点确认过，一次典型返回落在 `ZwDeviceIoControlFile`，内核状态是 `0xC0000185`。

错误含义：

- `1117` = `ERROR_IO_DEVICE`
- `995` = `ERROR_OPERATION_ABORTED`
- `0xC0000185` = `STATUS_IO_DEVICE_ERROR`

实际判读：

- 在当前 slot transport 里，`1117` 不一定代表真实介质坏块。
- 更常见的语义是：对应的 miniport control request 因 reset/remove/session close/cancel 被打断，最后在用户态表面上看成 I/O device error。
- `995` 则更接近“这次 I/O 被系统主动终止/取消”。

修复：

- `KMDF` 对 `YumeDiskCommandSubmitSlot` 的 `STATUS_IO_DEVICE_ERROR` 做特殊映射，转成更接近真实语义的 `STATUS_CANCELLED`。
- `App` 侧把 `1117` / `995` / `ERROR_NOT_READY` / `ERROR_DEVICE_NOT_CONNECTED` 当作可恢复 slot 错误处理：
  - 先尝试 `CancelSlot`
  - 短暂延迟后重投
  - 只在持续出现或 session 已关闭时当成致命错误
- `SCSI` 的 reset/remove/session close 路径必须补齐 complete pending。

注意事项：

- 在这个链路里，用户态看到 `1117` 时，第一反应不应该是“介质出错”，而应该先看最近是否发生了 `reset`、`remove`、`session cleanup`、`Ctrl+C`。
- 如果 `1117` 伴随 `pending_reads`、`posted_read_slots` 卡住，更像取消语义泄漏，不像真正的读盘错误。

### 3.5 性能只有约 `20 MB/s`，内核 CPU 很高

现象：

- 顺序 `1M Q1T1` 只有约 `20 MB/s`。
- CPU 占用很高，而且大部分是内核时间。
- 优化 miniport handle cache 后，性能仍几乎不变。

根因：

这个问题有两层。

第一层是结构性瓶颈：

- 旧 `WAIT_EVENT -> inline data -> READ_REPLY/WRITE_ACK` 模型导致每次 I/O 都要走多次同步往返。
- `KMDF` 还会参与大 buffer 分配、清零、复制，内核态纯搬运成本太高。

第二层是测量期被日志污染：

- 在 `DiskQueueReadSrb`、`DiskDrainReadSlots`、`ControlProxySubmitSlotAsync` 这类热路径上打大量 `DbgPrint`，虚拟机吞吐会断崖式下降。
- 本轮实际观察中，清掉热路径 print 之后，吞吐可以从几十 MB/s 直接回到数百 MB/s 级别。

修复：

- 用 App-owned slot queue 替换旧 wait-event 大 payload 往返。
- `KMDF` 只传 slot descriptor，不再持有自己的大数据面协议。
- `App` 与 `KMDF` 之间使用 direct I/O，减少额外复制。
- 压测前删掉热路径 print，尤其是：
  - `SCSI` 每次 read/write dispatch
  - `KMDF` 每次 slot submit/complete
  - `App` 每次 slot complete / ack send

注意事项：

- 性能测试前先确认当前驱动不是“调试日志版”。
- 在虚拟机里，`DbgPrint` 的性能杀伤经常比代码结构问题还更夸张。
- 如果吞吐突然从 `500 MB/s` 掉回 `20 MB/s`，先查 print，不要先改协议。

### 3.6 `Ctrl+C` / `rm all` / App 退出时的清理挂住

现象：

- 正常压测时直接 `Ctrl+C`，也可能把虚拟盘打成 100% 无读写。
- `rm all` 曾出现卡住或误判“链路坏了”。
- 结束 `RWTestApp` 后，`diskspd` 才被迫返回。

根因：

- 这些现象本质上还是取消和 session cleanup 的一部分，不是和 `Q8/Q32` 完全无关的新问题。
- 旧链路里，系统 I/O cancel、App slot cancel、session close、miniport handle close 没有被收进唯一边界。
- 一旦先后顺序反了，就会出现：
  - session 已关，但 slot 还活着
  - slot 已取消，但原始 SRB 还 pending
  - `diskspd` 想停，但底层请求没人 complete

修复原则：

- `KMDF` watchdog / cleanup 统一走：

```text
RemoveAllDisks | SessionClose
  -> drain in-flight slot/control IO
  -> close miniport handle
  -> 锁定 session，直到 file object 真正关闭
```

- `SCSI` 对取消的处理原则固定为：
  - 只完成对应读写请求为错误
  - 不判盘死
  - 不影响后续独立 I/O

注意事项：

- “系统 cancel I/O 时，App 回 ACK 后 SCSI 应以错误完成”这个原则必须贯穿读写两条链路。
- 不建议单独再堆一套“全链路复杂取消协议”；先把取消语义收在 SCSI request completion 边界即可。

### 3.7 多盘并发的常见误区

现象：

- 双盘 `Q1T1` 能过，但单盘速度被平分。
- 一块盘的高并发异常会拖慢另一块盘。

根因：

- App 端如果共享全局 worker 池、共享全局队列深度，天然会互相抢资源。
- SCSI 端如果共享 adapter 级读写锁和队列，多 target 并发时一定会互相串扰。

当前结论：

- `KMDF` 共享 session 状态即可，不应该共享数据面调度。
- `SCSI` 必须是 per-target 读写队列、per-target pending 状态。
- `App` 必须是 per-disk slot engine、per-disk queue depth、per-disk ACK 状态。

注意事项：

- 多盘性能问题很多时候不是“算法不够快”，而是状态归属错了。
- 只要还有 adapter 级全局数据面锁或 App 级全局队列深度，多盘就不可能真正并发。

## 4. 调试和压测注意事项

### 4.1 `diskspd` 使用注意

- 直接对 `\\.\PhysicalDriveN` 压测时，可能出现 `Error getting file size`。
- 当前调试中更稳定的方式是使用 `diskspd` 的 `#N` 目标。
- 压测前要先确认磁盘已经真正进入 `Get-Disk`，而不是只在设备管理器里可见。

### 4.2 首个 probe read 的价值最高

- 只要 `LBA 0 / 4KB` 的读没闭环，后面的 QDepth 压测结论都没有意义。
- “有设备无盘”优先抓 probe read，不要先看大块顺序读写。

### 4.3 先看单请求边界，再看全局行为

- 读错误：只影响该 `eventId`。
- 写错误：只影响该大 write 对应的 `eventId`。
- 任何单请求错误都不应该直接把整盘打死。

## 5. 当前最重要的长期规则

1. `KMDF` 只做 session/watchdog/direct-IO transport，不做自己的数据面状态机。
2. `SCSI` 只用 per-target 单盘队列推进请求，不回到 adapter 级共享数据面锁。
3. `App` 只按盘并发，不共享全局数据面 worker 池。
4. `WRITE_ACK_BATCH` 是唯一 ACK 入口，不再恢复 piggyback ACK。
5. 热路径 print 只用于短期定位，定位完成后必须删除，否则性能数据不可信。
