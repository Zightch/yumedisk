# Unit Attention / DataChanged 重建执行清单

## 1. 目标

本清单只服务于 `Unit Attention` 与“底层内容已变更”感知链路的重建。

本轮目标不是在现有“盘失效 / invalidation / 热拔”语义上继续打补丁，而是按重建口径把“盘仍然活着，但底层内容被别处写过”单独收成一条正式能力。

执行顺序固定为两阶段：

1. 先按驱动链自底向上顺序打通本地链路：
   - `YumeDiskSCSI`
   - `YumeDiskKMDF`
   - `AppKernel`
   - `BackendRust`
   - `rust-cli`
2. 本地共享 `MemoryMedia` 最小闭环稳定后，再接入网络侧 `rw -> ro` 的变更传播。

当前版本固定只做一种变化：

- `data_changed`

当前版本明确不做：

- `capacity_changed`
- metadata changed
- generic media change framework
- 盘删除 / session close / connection lost 的复用表达

同时固定以下正式口径：

- `AsyncNotificationSupported + StorPortAsyncNotificationDetected(..., RAID_ASYNC_NOTIFY_FLAG_MEDIA_STATUS)` 只做系统感知加速。
- 正式设备语义仍然靠 `Unit Attention` 收口。
- `Unit Attention` 表达的是“该 target 底层内容可能已变”，不是“盘失效”。
- 本轮 `YumeDiskSCSI / YumeDiskKMDF / AppKernel` 版本口径统一升级到 `0.1.0.1`。

## 2. 适用原则

本清单严格按 [开发原则](../development/development-principles.md) 执行，重点适用以下条目：

- 第 1 点“极简核心原则”
  - 当前只收一条最小可验证主路径：`data_changed`
  - 不提前做 `capacity_changed`、多 reason、多模式切换
- 第 2 点“激进更新原则”
  - 旧 `ct` 位置参数口径、旧 `rm all` 口径、旧“invalid 就等于一切外部变化”口径都不保留兼容桥
- 第 3 点“单一真实来源原则”
  - target 的 `pending Unit Attention` 只能由 `YumeDiskSCSI` 持有
  - 共享 `MemoryMedia` 的成员关系只能由 `rust-cli` 本地共享注册表持有
  - 某次系统写是否最终 committed，只能以 `AkEventWriteFinalCommitted` 为准
- 第 4 点“边界闸口原则”
  - CLI 参数互斥约束在 `rust-cli` 命令入口一次收住
  - `NotifyDataChanged` 的 target 有效性在 `AppKernel/KMDF/SCSI` 各自唯一边界入口校验
- 第 5 点“结构重构与层次依赖原则”
  - 新增本地共享内存相关代码时，优先收成稳定目录组件，不继续平铺 `shared_xxx`
  - 变更传播链必须按层次下沉，不允许把 UA 决策散落到 host、KMDF、SCSI 多处并列维护
- 第 6 点“删除优先原则”
  - 不保留旧 `ct <disk-size-mib> [dense|auto]`、`rm all` 的兼容命令面
  - 不保留“用 invalidation 模拟 data_changed”的旧思路
- 第 7 点“测试覆盖原则”
  - 当前重建必须补覆盖：同底层多 target、单次写入后的单次 UA、重复写合并、`REQUEST SENSE`、异步通知加速、共享内存删除拒绝
- 第 8 点“文档跟随实现原则”
  - `todo-UA.md` 只写当前正式打算落地的链路，不记录旧草路

## 3. 当前现状

### 3.1 rust-cli 当前只支持独占本地内存盘

当前 `windows/rust-cli` 的本地盘命令面是：

- `ct <disk-size-mib> [dense|auto] [true|false] [target]`
- `rm <target>`
- `rm all`

当前问题：

- `ct` 只会创建当前盘独占的 `DenseMem`
- 没有 `sm` 命令
- 没有 `smid`
- 没有“多个 target 绑定同一底层内存区”的正式模型
- `rm` 只能按 target 或 `all` 处理，不能按共享内存区处理

相关现状文件：

- `windows/rust-cli/src/cli/shell.rs`
- `windows/rust-cli/src/cli/host.rs`
- `windows/rust-cli/src/cli/local/dense_mem.rs`

### 3.2 本地内存介质本身已具备共享基础，但没有注册表和成员关系

当前 `DenseMem` 本质上已经是：

- `Arc<RwLock<Vec<u8>>>`

这意味着：

- 底层字节区天然可以被多个对象共享

但当前缺失：

- 共享内存区 ID 分配
- `smid -> media` 注册表
- `smid -> bound targets` 成员表
- target 删除后的解绑收口
- 共享内存区删除规则

也就是说：

- 目前“能共享底层字节”
- 但“不能以正式产品语义管理共享介质”

### 3.3 BackendRust 当前没有“外部内容已变”下行 API

当前 `windows/BackendRust` 的事件线程只处理：

- `AkEventDiskOnline`
- `AkEventDiskRemoved`
- `AkEventWriteFinalCommitted`
- `AkEventWriteFinalRejected`
- `AkEventSessionBroken`

当前问题：

- 没有 `notify_disk_data_changed(target)` 这类对下 API
- `DiskRuntime` 不携带本地共享内存组信息
- `WriteFinalCommitted` 后只能做当前盘的 staged write commit
- 不能在 commit 后继续 fanout 到 sibling target

相关现状文件：

- `windows/BackendRust/src/runtime.rs`
- `windows/BackendRust/src/media.rs`
- `windows/BackendRust/src/types.rs`

### 3.4 AppKernel 当前没有“通知某盘内容已变”的公开接口

当前 `windows/AppKernel/include/appkernel.h` 公开 API 只有：

- `AkOpen / AkClose`
- `AkCreateDisk / AkRemoveDisk / AkRemoveAllDisks`
- `AkWaitEvent / AkPollEvent`
- query state / stats

当前问题：

- 没有 `AkNotifyDiskDataChanged(AK_DISK*)`
- 事件队列里也没有“内容已变”相关 event
- `AK_EVENT_TYPE` 当前只承载 lifecycle 和 write final

当前结论：

- 本轮不需要新增上行 event
- 只需要补一条从 host 下行到驱动的显式 API

相关现状文件：

- `windows/AppKernel/include/appkernel.h`
- `windows/AppKernel/src/appkernel.c`
- `windows/AppKernel/src/disk/ak_disk.c`
- `windows/AppKernel/src/protocol/ak_protocol.c`

### 3.5 KMDF 当前命令白名单里没有 NotifyDataChanged

当前 `YumeDiskKMDF` 的 `ControlEvtIoDeviceControl` 已正式支持：

- `QueryKmdfInfo`
- `QueryScsiInfo`
- `CreateDisk`
- `RemoveDisk`
- `RemoveAllDisks`
- `Heartbeat`
- `PostReadSlot`
- `PostWriteSlot`
- `ReadAck`
- `WriteAckBatch`
- `CancelSlot`
- `QueryDebugState`

当前问题：

- 没有 `YumeDiskCommandNotifyDataChanged`
- 没有对应 payload 校验与代理路径

但当前有利点：

- `QueryScsiInfo / CreateDisk / RemoveDisk / RemoveAllDisks / QueryDebugState` 已经走稳定同步代理路径
- `NotifyDataChanged` 可以复用这条同步控制面
- 不需要介入 slot transport runtime

相关现状文件：

- `windows/YumeDiskKMDF/YumeDiskKMDF/control/ioctl.c`
- `windows/YumeDiskKMDF/YumeDiskKMDF/session/session.c`
- `windows/YumeDiskKMDF/YumeDiskKMDF/transport/transport.c`

### 3.6 SCSI 当前没有 per-target Unit Attention 状态

当前 `YumeDiskSCSI` 已经具备：

- per-target queue
- 标准 `READ/WRITE` 数据面
- `ReadOnly` 写保护 sense
- `BusChangeDetected` 建盘删盘通知
- `AutoRequestSense = TRUE`

当前缺失：

- per-target `pending_data_changed` 状态
- `TEST UNIT READY` 对外部内容变化的提前失败能力
- `REQUEST SENSE` 对 pending UA 的正式返回
- `READ/READ CAPACITY/MODE SENSE/VERIFY` 前置 UA
- `StorPortSetUnitAttributes`
- `StorPortAsyncNotificationDetected`

当前实际行为：

- `TEST UNIT READY` 直接成功
- `REQUEST SENSE` 只回零填充 sense header
- `INQUIRY / REPORT LUNS / READ CAPACITY / MODE SENSE` 都不考虑外部内容变化状态

相关现状文件：

- `windows/YumeDiskSCSI/YumeDiskSCSI/scsi/scsi.c`
- `windows/YumeDiskSCSI/YumeDiskSCSI/control/control.c`
- `windows/YumeDiskSCSI/YumeDiskSCSI/core/defs.h`
- `windows/YumeDiskSCSI/YumeDiskSCSI/adapter/adapter.c`

### 3.7 当前没有正式“内容已变但盘仍有效”的系统模型

当前本地与网络两侧已有的失效表达更偏向：

- session close
- connection lost
- media invalidation -> runtime invalid

这些都不适合当前目标。

当前要补的是一条新语义：

- target 仍然存在
- session 仍然存在
- 盘不 invalid
- 只是底层内容已经被别处写过

## 4. 重建边界

### 4.1 先本地，后网络，且本地固定自底向上

顺序固定为：

1. 本地 `Unit Attention / data_changed` 驱动链
   - `YumeDiskSCSI`
   - `YumeDiskKMDF`
   - `AppKernel`
   - `BackendRust`
   - `rust-cli`
2. 再把同一条 `data_changed` 语义接到网络链路

本清单中的阶段 A-F 属于第一步。

网络接入只列准备项，不在本轮先做。

### 4.2 当前只收 data_changed

当前版本固定为：

- dedicated command
- dedicated host API
- dedicated SCSI pending bit

当前不做：

- `enum change_kind` 下挂多种未来 reason
- 通用 media change router
- capability switch
- 配置开关

也就是说：

- 对外公开面直接叫 `NotifyDataChanged`
- 不先造一套“未来也许能扩展”的大抽象

### 4.2.1 当前版本升级口径

由于本轮会同时改动：

- `YumeDiskSCSI` 对系统暴露的正式设备语义
- `YumeDiskKMDF <-> YumeDiskSCSI` 私有控制命令集合
- `AppKernel` 对宿主暴露的正式公开 API

因此这一版版本号不再停留在 `0.1.0.0`，而是统一升级到：

- `0.1.0.1`

当前口径固定为：

- 版本升级动作只在真正开始实现本轮 UA / data_changed 主线时执行
- 不做“先改行为、后补版本”的滞后做法
- 不保留旧版本并存兼容桥
- `YumeDiskSCSI / YumeDiskKMDF / AppKernel` 三者必须保持同一 `VersionBe`

### 4.3 Async Notification 只是加速层

当前正式顺序固定为：

1. 先把 target 标成 `pending UA`
2. 再 best-effort 发异步通知加速系统感知

约束：

- 即使异步通知失败，也不能影响 pending UA 正式语义
- 即使系统没立刻来探测命令，pending UA 也必须继续保留

### 4.4 当前不承诺文件系统级实时一致

当前能力只到块设备层：

- 目标是让 Windows storage stack 感知“设备内容可能已变”

当前不承诺：

- 已挂载文件系统像集群文件系统一样强一致
- 任意用户态缓存、卷缓存、文件系统缓存的业务级同步

### 4.5 当前不在本地共享内存阶段引入单 writer 约束

本地共享 `MemoryMedia` 阶段只是驱动链验证 harness。

因此：

- 允许多个 target 绑定同一 `smid`
- 允许多个 target 都是可写

当前明确不做：

- 本地 `smid` 的单 writer 独占策略
- 本地 claim code / auth / ro-rw 导出模型

这些属于后续网络共享盘语义，不在本地 UA harness 中提前展开。

## 5. 目标链路

第一阶段的目标链路固定如下：

```text
Windows WRITE(target=A)
  -> YumeDiskSCSI queue
  -> AppKernel staged write
  -> WRITE_ACK_BATCH complete
  -> AkEventWriteFinalCommitted(target=A, event_id)
  -> BackendRust commit staged write to shared MemoryMedia(smid=X)
  -> BackendRust finds sibling targets bound to smid=X
  -> BackendRust notifies every sibling target except A
  -> AppKernel NotifyDataChanged(target=B)
  -> YumeDiskKMDF proxy command
  -> YumeDiskSCSI mark target B pending_data_changed
  -> YumeDiskSCSI best-effort StorPortAsyncNotificationDetected(...MEDIA_STATUS)
  -> Windows later probes/reads target=B
  -> YumeDiskSCSI returns CHECK CONDITION + UNIT ATTENTION + ASC/ASCQ 28/00 once
  -> pending_data_changed cleared
  -> subsequent READ reads new bytes from shared MemoryMedia
```

这个链路里各层职责固定为：

- `rust-cli`
  - 管共享内存区注册表、CLI 命令面、target 绑定
- `BackendRust`
  - 在 `WriteFinalCommitted` 后做 sibling fanout
- `AppKernel`
  - 对 host 暴露单盘 `NotifyDataChanged` API
- `KMDF`
  - 只做同步控制面代理
- `SCSI`
  - 真正持有 `pending UA` 状态并对系统命令表达标准设备语义

## 6. 公开接口与协议收口

### 6.1 rust-cli 命令面

新命令面固定为：

- `sm <size-mib>`
- `ct size=<mib> [ro=<true|false>] [target=<id>]`
- `ct smid=<id> [ro=<true|false>] [target=<id>]`
- `rm target=<id>`
- `rm smid=<id>`

固定约束：

- `ct`：
  - `size` 和 `smid` 互斥
  - 必须二选一
- `rm`：
  - `target` 和 `smid` 互斥
  - 必须二选一
- 不再保留：
  - `ct <disk-size-mib> ...`
  - `rm all`

`rm smid=<id>` 当前固定策略：

- 如果该 `smid` 仍绑定任何 target
  - 直接拒绝
  - 返回 `smid-in-use`
- 不做“顺手删所有 target”的副作用

### 6.2 BackendRust 对 host 的新 API

当前建议补一条极窄公开 API：

- `notify_disk_data_changed(target_id)`

建议对外形式：

- `pub fn notify_disk_data_changed(&self, target_id: u32, out_error_text: Option<&mut String>) -> bool`

要求：

- 这条 API 只负责把 target 的“内容已变”下发到 `AppKernel`
- 不负责 sibling 查找
- 不负责共享内存注册表

### 6.3 BackendRust 的本地绑定信息

为了在 `WriteFinalCommitted` 后找到 sibling targets，需要在 BackendRust runtime 内补一个很窄的 host 绑定信息。

当前建议不要把这个塞进 `DiskConfig` 的正式通用字段。

建议新增一层 host-side 运行时绑定：

- `ManagedDiskBinding`
  - `local_shared_media_id: Option<u64>`

约束：

- 它只属于宿主运行时
- 不下发给 `AppKernel`
- 不写入 driver 协议
- 不变成跨宿主持久化配置字段

### 6.4 AppKernel 公开 API

当前建议补一条 dedicated API：

- `AK_STATUS AkNotifyDiskDataChanged(AK_DISK* disk);`

固定边界：

- 不新增上行 event type
- 不新增 generic change kind 参数
- 一条 dedicated API 直接对应当前唯一能力

### 6.5 AppKernel <-> KMDF 私有协议

当前建议在 `windows/shared/yumedisk_proto.h` 中新增：

- `YumeDiskCommandNotifyDataChanged`

当前建议固定为：

- 零 payload
- 使用 `Header.TargetId` 指定目标盘

这样做的原因：

- 当前只支持 `data_changed`
- target already exists in header
- 不需要额外 body 和 future-proof 包袱

### 6.6 KMDF 行为

`YumeDiskKMDF` 对 `NotifyDataChanged` 固定行为为：

- 验证 session 有效
- 验证 `Header.TargetId`
- 走现有同步控制代理路径下发给 miniport
- 返回同步成功或失败

明确不做：

- slot runtime
- async completion
- 状态缓存
- 本地 pending UA 镜像

### 6.7 SCSI 内部状态

当前建议给每个 `YUME_DISK` 增加：

- `BOOLEAN PendingDataChangedUa;`

如果实现 `StorPortSetUnitAttributes` 需要本地注册状态，也可再补：

- `BOOLEAN AsyncNotificationRegistered;`

但必须注意：

- 设备级“是否已设置单位属性”如果能在一次性初始化点固定完成，就不要再并行缓存到多处

### 6.8 SCSI 对系统命令的正式语义

当前目标固定为：

- `INQUIRY`
  - 不消费 pending UA
- `REPORT LUNS`
  - 不消费 pending UA
- `REQUEST SENSE`
  - 若 target 有 pending UA，则返回 `UNIT ATTENTION / 28 00`
  - 返回后清除 pending
- `TEST UNIT READY`
  - 若 target 有 pending UA，则先返回 `CHECK CONDITION + UNIT ATTENTION`
  - 返回后清除 pending
- `READ / WRITE / READ CAPACITY / MODE SENSE / VERIFY`
  - 若 target 有 pending UA，则先返回 `CHECK CONDITION + UNIT ATTENTION`
  - 返回后清除 pending

当前 `ASC/ASCQ` 固定为：

- `28h/00h`

## 7. 结构与模块收口

### 7.1 rust-cli 本地共享内存组件

按开发原则第 5 点，当前不应继续平铺：

- `shared_mem_xxx`
- `memory_xxx`

建议直接收为：

- `windows/rust-cli/src/cli/local/memory/`
  - `media.rs`
  - `registry.rs`
  - `binding.rs`
  - `mod.rs`

其中：

- `media.rs`
  - 唯一正式本地内存介质实现
- `registry.rs`
  - `smid` 分配、查询、删除、成员关系
- `binding.rs`
  - `target <-> smid` 或 host-side create binding 组织

当前 `dense_mem.rs` 建议直接删除或重命名并并入该目录，不保留双轨。

### 7.2 BackendRust 结构

当前如果只是补极少量代码，可先收在现有文件中：

- `src/runtime.rs`
- `src/types.rs`

但如果新增“创建绑定 + sibling fanout + notify downward”逻辑明显扩张，则应继续按稳定前缀收目录，而不是把 `runtime.rs` 继续平铺拉长。

建议候选目录：

- `windows/BackendRust/src/runtime/`
  - `context.rs`
  - `events.rs`
  - `data_changed.rs`
  - `mod.rs`

是否执行这一步，取决于本轮改动后 `runtime.rs` 是否已明显失控。

### 7.3 AppKernel / KMDF / SCSI

这三层本轮新增能力都很窄，当前建议：

- 优先接入现有组件，不额外新造一层 generic `media_change service`
- 若某层出现稳定前缀集合，再按目录原则继续拆

也就是说：

- `AppKernel`
  - 直接在现有 `disk/`、`protocol/` 中补 dedicated path
- `KMDF`
  - 直接在 `control/ioctl.c` + proxy path 补 dedicated command
- `SCSI`
  - 直接在 `scsi/`、`control/`、`adapter/` 补 dedicated path

## 8. 分阶段工作

### 阶段 A：先重建 SCSI 的 Unit Attention 正式语义

目标：

- 先让最底层设备模型具备正式 `pending data_changed -> Unit Attention` 语义

任务：

- 将 `YumeDiskSCSI / YumeDiskKMDF / AppKernel` 共享版本口径从 `0.1.0.0` 升级到 `0.1.0.1`
- `YUME_DISK` 增加 `PendingDataChangedUa`
- `control/control.c` 处理 `YumeDiskCommandNotifyDataChanged`
- target 首次从 `false -> true` 时：
  - 置 pending
  - best-effort 调 `StorPortAsyncNotificationDetected(...MEDIA_STATUS)`
- 在 `scsi/scsi.c` 增加：
  - `FillUnitAttentionSenseDataChanged`
  - `TryConsumePendingDataChangedUa`
  - `REQUEST SENSE` 专门处理
  - `TEST UNIT READY` / `READ` / `WRITE` / `READ CAPACITY` / `MODE SENSE` / `VERIFY` 前置 UA
- `adapter/adapter.c` 或合适初始化点接入 `StorPortSetUnitAttributes`

固定约束：

- 同一 target 连续多次外部写入，在 pending 尚未消费时只保留一个 pending 位
- 不把内容变化扩大成 `BusChangeDetected`
- 不删盘
- 不 complete pending read/write 失败

### 阶段 B：重建 KMDF 的同步代理命令

目标：

- 在 miniport 语义就位后，把 host 的 `NotifyDataChanged` 正式代理到 miniport

任务：

- `yumedisk_proto.h` 增加 `YumeDiskCommandNotifyDataChanged`
- `control/ioctl.c` 接入 command 白名单
- 走现有 `ControlProxyMessage` 同步代理路径
- 在唯一入口校验：
  - payload length must be zero
  - `Header.TargetId` 合法

固定约束：

- 不引入异步 slot transport
- 不在 KMDF 镜像维护 target pending UA
- 不新增 generic “device event” 总线
- 继续沿用“兼容性只看版本相等”的当前正式口径；既然命令集合已变化，本轮版本必须已经提升到 `0.1.0.1`

### 阶段 C：重建 AppKernel 的 NotifyDataChanged API

目标：

- 给 host 一个正式下行入口

任务：

- 在 `appkernel.h` 补 `AkNotifyDiskDataChanged`
- 在 `src/appkernel.c` 暴露实现
- 在 `src/disk/ak_disk.c` 为单盘做入口校验：
  - disk handle 有效
  - 生命周期允许
  - target 已存在
- 在 `src/protocol/ak_protocol.c` 新增 dedicated protocol sender

固定约束：

- 不新增 event queue 上行事件
- 不新增 generic change kind
- 不把“数据已变”伪装成 remove/recreate
- `AK_VERSION_BE` 必须随本轮同步提升到 `0.1.0.1`

### 阶段 D：重建 BackendRust 的 host-side 绑定与 fanout

目标：

- 在下行 API 就位后，于 `WriteFinalCommitted` 后找出同 `smid` 的 sibling target

任务：

- 补 `ManagedDiskBinding` 或等价 host-side binding
- `create_managed_disk` 时把 `local_shared_media_id` 写入 runtime 真状态
- event thread 处理 `AkEventWriteFinalCommitted` 时：
  - 先 commit 当前盘 staged write
  - 再判断是否属于共享内存组
  - 找出 sibling targets
  - 对除自己外的 sibling target 调 `notify_disk_data_changed(target)`

固定约束：

- sibling notify 失败只记日志
- 不能回滚已 committed 的写
- 不允许把 sibling group 状态复制到多个地方并行维护

### 阶段 E：最后重建 rust-cli 本地共享内存命令面

目标：

- 在下层链路全部稳定后，再建立正式的本地共享内存区模型与验收入口

任务：

- 删除旧 `ct <disk-size-mib> ...` 解析口径
- 删除旧 `rm all` 口径
- 新增 `sm <size-mib>`
- 新增 `ct size=...`
- 新增 `ct smid=...`
- 新增 `rm target=...`
- 新增 `rm smid=...`
- 建立 `smid` 注册表
- 建立 `smid -> bound targets` 成员关系

边界：

- `sm` 只创建共享内存区，不自动建盘
- `ct size=...` 不进入共享注册表
- `ct smid=...` 必须复用已存在共享内存区

### 阶段 F：重建本地最小闭环验收

目标：

- 用共享 `MemoryMedia` 验证整条驱动链

最小验收步骤固定为：

1. `sm 64`
2. `ct smid=1 target=3 ro=false`
3. `ct smid=1 target=4 ro=false`
4. 通过系统对 target 3 发起真实写入
5. 观察 target 4：
   - 不被删除
   - 不 invalid
   - 下一次系统探测或读命令先收到一次 `Unit Attention`
   - 后续正常读能读到新内容

若系统黑盒路径不足以稳定判断，可补极小调试命令面，但范围必须严格收窄：

- 只允许补帮助本轮验证的最小命令
- 不顺手扩成完整块设备测试 shell

建议的最小可选调试命令：

- `dbg-read target=<id> offset=<bytes> length=<bytes>`
- `dbg-write target=<id> offset=<bytes> hex=<...>`

只有在纯依赖 Windows 自动探测无法稳定验收时，才补这层。

### 阶段 G：网络接入准备项

这一阶段不立即实施，但本轮 todo 先明确后续接法：

- server `rw` 写 committed 后，对同 backend 的其他活跃 `ro session` 发 dedicated notice
- gateway 只透传
- client 收到后只调用本地 `notify_disk_data_changed(target)`
- 不 invalid
- 不 close session

当前 dedicated notice 名称与协议细节，等第一阶段稳定后再单独收文档和代码。

## 9. 测试与验收

### 9.1 rust-cli / host 命令面测试

必须补：

- `ct size=...` 与 `ct smid=...` 互斥测试
- `rm target=...` 与 `rm smid=...` 互斥测试
- 缺少 `size/smid` 的报错测试
- 不存在 `smid` 的报错测试
- `rm smid=` 在仍有 bound targets 时拒绝
- target 删除后 `smid` 解绑

### 9.2 BackendRust 测试

必须补：

- 普通独占 `MemoryMedia` 写 committed 不触发 sibling notify
- 同 `smid` 下 writer committed 会通知其他 target
- writer 自己不通知自己
- sibling notify 失败不影响当前写 committed

### 9.3 AppKernel / KMDF / SCSI 测试

必须补：

- `NotifyDataChanged` 对不存在 target 返回错误
- target pending UA 后，`TEST UNIT READY` 首次返回 UA，第二次恢复正常
- target pending UA 后，`REQUEST SENSE` 返回 `UNIT ATTENTION / 28 00`
- 多次 `NotifyDataChanged` 在 pending 未消费前只保留一次
- `INQUIRY` 不消费 pending UA
- `REPORT LUNS` 不消费 pending UA
- `READ CAPACITY` 可触发 pending UA

### 9.4 本地共享内存闭环验收

至少覆盖：

- 双 target 绑定同一 `smid`
- target A 写入，target B 收到一次 UA
- target B 再读能看到新数据
- target A 删除后，target B 仍继续可用
- `rm smid=` 在 target 未删除前拒绝
- target 全删后 `rm smid=` 成功

### 9.5 网络接入停止线

在第一阶段完成前，以下内容都不进入本轮：

- 新 network notice
- gateway/client/server 行为联调
- tauri-client runtime data_changed 传播

## 10. 完成判定

满足以下条件才算 UA 第一阶段重建完成：

- `YumeDiskSCSI / YumeDiskKMDF / AppKernel` 已统一升级到版本 `0.1.0.1`
- `SCSI` 已持有 per-target `PendingDataChangedUa`
- `SCSI` 已能对系统命令返回一次正式 `Unit Attention`
- `StorPortAsyncNotificationDetected(...MEDIA_STATUS)` 已作为加速层接入
- `KMDF` 已正式代理 `YumeDiskCommandNotifyDataChanged`
- `AppKernel` 已提供 `AkNotifyDiskDataChanged`
- `BackendRust` 已能在 `WriteFinalCommitted` 后对 sibling target 发起 dedicated downward notify
- `rust-cli` 已按 `sm / ct(size|smid) / rm(target|smid)` 收成唯一正式命令面
- 本地共享内存区已成为正式注册表模型，而不是临时对象拼接
- 本地双 target 共享 `MemoryMedia` 已完成最小闭环验收
- 文档与测试已同步到当前唯一正式口径

## 11. 当前不应做的事

- 不把 `data_changed` 和 `session close`、`connection lost` 混成一套 notice
- 不把 `data_changed` 做成 remove/recreate target
- 不把 `AsyncNotificationSupported` 当作唯一正式语义
- 不提前设计 `capacity_changed`、`metadata_changed`、`flush_required` 等未来 reason
- 不为旧 `ct` / `rm` 命令面保留兼容解析
- 不让 `BackendRust`、`KMDF`、`SCSI` 三层并行缓存同一份 pending UA 事实
- 不在本地共享内存阶段顺手做网络 auth/session/shared storer 语义
