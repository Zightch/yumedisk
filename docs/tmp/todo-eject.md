# Windows 单盘系统弹出执行清单

## 0. 当前范围

本清单只处理 Windows 本地虚拟 SCSI 盘“像 U 盘一样由系统发起弹出”的能力。

本清单默认以下前置条件已经成立：

- [todo-reconstruction.md](./todo-reconstruction.md) 已完成
- `AkResponse / AkSessionNotice / AK_DISK_OPS`
- 单盘级 `event slot`
- `AK_DISK_OPS.on_event`
- `AkDiskEventSystemEjected`

也就是说，本清单不再重复处理事件语义拆分、协议重命名、`event slot` 骨架搭建这些前置重建工作，只处理真正的“系统单盘弹出”实现。

本轮明确保持不变：

- 主动删盘仍走现有 `AkRemoveDisk -> RemoveDisk` 主线
- 主动删盘现有行为、现有清理、现有 UI/CLI 入口不改
- `tauri-client` 仍然属于最后一阶段，前面一律先用 `rust-cli` 做验证

## 1. 总目标

按最小核心闭环完成“系统单盘弹出”正式收口：

1. `YumeDiskSCSI` 对系统注册单盘可弹出能力。
2. Windows 对某个 `PhysicalDrive` 发起弹出时，只影响对应 `target/lun`。
3. `SCSI` 先同步清理该 target 的 pending I/O、队列、可见性和局部运行态。
4. 清理完成后，使用该盘已预投的 `event slot` 上报 `AkDiskEventSystemEjected`。
5. `BackendRust` 收到盘级事件后，做“被动拔出”清理，不调用主动 `AkRemoveDisk`。
6. app 最终把对应盘从 `mounted` 收成 `unmounted`，不删除盘，不置 `invalid`。

## 2. 固定边界

### 2.1 本轮只做的事

- `YumeDiskSCSI` 单盘可弹出能力
- `YumeDiskSCSI` 接受系统单盘弹出并做 target 级清理
- 弹出后完成对应盘的 `event slot`
- `BackendRust` 被动拔出路径
- `rust-cli` 黑盒验证
- `tauri-client` 最后接状态同步

### 2.2 本轮明确不做

- 修改 `todo-reconstruction.md` 中已经收口的协议与公开 API
- 把系统弹出做成整个控制器移除
- 因单盘系统弹出而关闭整个 `AppKernel session`
- 因单盘系统弹出而影响其他盘
- 因单盘系统弹出而自动删除 runtime
- 因单盘系统弹出而自动置 `invalid`
- 把该能力并入网络协议、网络 close、network runtime 故障语义

## 3. 正式语义

### 3.1 单盘级作用范围

系统弹出语义固定为：

- Windows 暴露出来的单个 `PhysicalDrive`
- 对应一个 `target/lun`

它不等于：

- 整个 Storport adapter
- 整个 `YumeDiskSCSI` 控制器
- 整个 `AppKernel session`

### 3.2 清理时序

时序固定为：

1. `SCSI` 识别该盘被系统弹出
2. 先同步清理该 target
3. 再完成该盘的 `event slot`
4. `AppKernel` 通过 `AK_DISK_OPS.on_event` 把 `AkDiskEventSystemEjected` 交给上层
5. `BackendRust` / app 做被动拔出收口

不能反过来：

- 不能先通知 app，再等待 app 指挥内核清理

### 3.3 app 最终结果

收到 `AkDiskEventSystemEjected` 后，app 固定动作是：

- 清理对应盘当前 mounted 运行态
- 收成 `unmounted`

明确不做：

- 自动删除盘
- 自动置 `invalid`
- 自动关闭整个 app session

## 4. 当前缺口

### 4.1 `YumeDiskSCSI`

当前已有：

- 主动 `CreateDisk / RemoveDisk / RemoveAllDisks`
- `BusChangeDetected`
- 读写与 `NotifyDataChanged -> UA`

当前缺失：

- 单盘可弹出设备能力
- 接受系统单盘弹出的 target 级逻辑
- 系统弹出后的 target 局部清理
- 清理后完成该盘 `event slot`

### 4.2 `BackendRust`

当前已有：

- 主动移除路径
- app 主动拔出时把 media 返还给上层

当前缺失：

- 收到 `AkDiskEventSystemEjected` 后的被动拔出路径
- 不走 `AkRemoveDisk` 的 media handoff / runtime handoff

### 4.3 `rust-cli`

当前缺失：

- 对 `DiskSystemEjected` 的打印与清理
- 本地盘、网络盘的被动拔出黑盒验证入口

### 4.4 `tauri-client`

当前缺失：

- 本地 `BackendRust` 盘事件 watcher
- 从 `mounted` 自动切回 `unmounted` 的 runtime/UI 收口

## 5. 目标链路

正式链路固定为：

```text
Windows 弹出单个 PhysicalDrive
  -> YumeDiskSCSI 识别对应 target/lun
  -> 只清理该 target 的 pending I/O / 队列 / 可见性
  -> 完成该盘预投的 event slot，负载为 SystemEjected
  -> AppKernel disk event worker 收到并调用 AK_DISK_OPS.on_event
  -> BackendRust 收到后执行被动拔出清理
  -> rust-cli / tauri-client 把对应盘从 mounted 收成 unmounted
```

固定要求：

- 该链路只对一个 target 生效
- 该链路不影响 session 中的其他盘
- 该链路不要求 app 先确认，驱动清理必须先发生
- app 只承接结果，不参与内核同步决策

## 6. SCSI 侧任务

### 6.1 单盘可弹出能力

需要完成：

- 对单盘注册可弹出设备能力
- 让 Windows 把该 `PhysicalDrive` 视为可弹出的设备，而不是固定不可弹出盘

固定要求：

- 能力必须是单盘级
- 不能把整个控制器做成统一可弹出对象

### 6.2 target 级系统弹出处理

当某个 target 被系统弹出时，`SCSI` 必须同步完成：

- 将该 target 标记为 removing / not present
- 停止继续接受该 target 的新 I/O
- 失败完成该 target 现有 pending I/O
- 清理该 target 队列、pending UA、关联运行态
- 让该 target 不再被继续枚举
- 触发该 target 从系统视图中消失

固定要求：

- 只清理该 target
- 不碰其他 target
- 不发全局 `RemoveAll`
- 不结束整个控制器或进程

### 6.3 `event slot` 完成时机

固定要求：

- 只有该 target 同步清理已经完成后，才能完成对应 `event slot`
- 完成的事件负载固定为 `SystemEjected`
- 一次系统弹出只允许上报一次

## 7. AppKernel / BackendRust / app 任务

### 7.1 `AppKernel`

本轮只需要确保：

- `SystemEjected` 事件种类能从 disk event worker 正确传到 `AK_DISK_OPS.on_event`
- 盘关闭或 session 关闭后不再误回调已经失效的 disk runtime

本轮不再重复处理：

- `AkResponse / AkSessionNotice`
- `event slot` 协议与 worker 骨架

### 7.2 `BackendRust`

需要新增：

- `ManagedDiskEventType::DiskSystemEjected`
- 被动拔出清理路径

固定要求：

- 不调用 `AkRemoveDisk`
- 如果内核已经先摘掉 target，`BackendRust` 只做上层 runtime/media 收口
- 对应盘从 mounted 转为 unmounted

### 7.3 `rust-cli`

当前最小闭环要求：

- 能收到并打印 `DiskSystemEjected`
- 能对对应 target 做被动拔出清理
- 后续仍能观察其他 target 正常工作

对不同盘型的收口：

- 本地内存盘
  - 解除 target 绑定
  - 保留内存介质本体
  - 后续允许重新挂载
- 本地文件盘
  - 解除 target 绑定
  - 文件仍留在原路径
  - 后续允许重新挂载
- 网络盘
  - 解除当前 target 绑定
  - 当前阶段只要求不再把它视作 mounted target

### 7.4 `tauri-client`

属于最后一阶段。

最终要求：

- 收到 `DiskSystemEjected` 后，把对应 runtime 从 `mounted` 改为 `unmounted`
- 内存盘恢复 media
- 文件盘释放 mounted media 并恢复本地未挂载状态
- 网络盘释放 mounted `NetworkMedia` 并恢复网络未挂载状态
- 持久化配置并刷新主页

## 8. 分阶段执行方案

### 第一阶段：补 `SCSI` 单盘可弹出与 target 级清理

目标：

- 让某个 target 对应的 `PhysicalDrive` 能被系统弹出
- 弹出后只清理该 target

验收：

- 两个本地盘同时存在时，弹出 A 不影响 B
- A 对应 `PhysicalDrive` 从系统视图消失

### 第二阶段：完成对应盘的 `event slot`

目标：

- 系统弹出后，驱动能完成该盘已预投的 `event slot`
- 上报固定 `SystemEjected`

验收：

- 单盘系统弹出后，对应盘收到一条 `SystemEjected`
- 非弹出盘不收到误事件
- 同一次弹出不重复上报

### 第三阶段：补 `BackendRust` 被动拔出

目标：

- `BackendRust` 能在 `on_event` 中接住 `SystemEjected`
- 完成被动拔出清理

验收：

- 收到事件后不调用 `AkRemoveDisk`
- 被弹出的盘完成被动拔出收口

### 第四阶段：接 `rust-cli` 并做黑盒验证

目标：

- `rust-cli` 能观察到该事件
- `rust-cli` 能完成最小 app 清理

验收：

- 单本地内存盘：系统弹出后自动解除绑定，后续可重挂
- 单本地文件盘：系统弹出后自动解除绑定，后续可重挂
- 双盘并存：弹出 A，B 继续正常

### 第五阶段：最后接 `tauri-client`

目标：

- 正式桌面客户端完成 runtime/UI 收束

验收：

- 卡片从“已挂载”自动转为“未挂载”
- 不误删盘
- 不误置 `invalid`

## 9. 测试矩阵

### 9.1 必测黑盒

- 单盘系统弹出
- 双盘同时存在，只弹出其中一个
- 弹出后其他盘继续读写
- 弹出后对应 target 不再可见
- 弹出事件只上报一次

### 9.2 `rust-cli` 阶段必测

- 本地内存盘
- 本地文件盘
- 同控制器多盘互不影响

### 9.3 `tauri-client` 阶段必测

- 卡片状态自动切换
- 配置持久化后不分叉
- 重扫后仍保持正确状态

## 10. 当前阶段结论

当前最合理的推进顺序是：

1. 先完成 `todo-reconstruction.md`
2. 再做 `SCSI` 单盘可弹出与 target 级清理
3. 然后完成对应盘的 `event slot`
4. 先用 `rust-cli` 做全链路黑盒验证
5. 最后才接 `tauri-client`

这样可以把“驱动链是否真正支持系统单盘弹出”先独立验明，避免再次把协议重建、驱动事件语义和 UI/runtime 工作混在一起。
