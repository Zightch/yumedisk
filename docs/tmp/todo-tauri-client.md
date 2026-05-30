# `tauri-client` 网络 `data changed` / `backend_id` / 判重重建执行清单

## 0. 当前范围

本清单只服务于 `tauri-client` 当前这一轮收口，不覆盖 `rust-cli`、`server`、`network-core` 的历史任务。

本轮只处理三件事：

- 把 `data changed` 正式接入 `tauri-client`
- 把 `remote_backend_id` 的使用收束到 `tauri-client` 的 `network/` 侧
- 补本地文件盘判重，判重条件为完整文件路径

本轮不是在旧实现上叠补丁，而是按重建口径把 `tauri-client` 现有网络盘主线收成唯一正确版本。

## 1. 当前总目标

按最小核心闭环完成 `tauri-client` 这一轮正式收口：

1. `SessionDataChangedNotice` 只在 `tauri-client` 的 `network/` 路径中接收、暂存、收束和下沉。
2. `remote_backend_id` 不进入通用 `DiskRuntime` 持久化模型，不进入文件盘和内存盘范围。
3. 网络盘 draft 添加阶段补一次 `remote_backend_id` 判重。
4. 网络盘重扫改为“四段执行流水”，消除顺序相关行为。
5. 同一 `(server_addr, remote_backend_id)` 冲突时，只保留一个 `ro`，其他 `rw/ro` 一律置为 `invalid`。
6. 文件盘补完整文件路径判重，但这条规则只属于本地文件盘，不混入 `network/` 逻辑。

## 2. 固定边界

### 2.1 本轮只做的事

- `tauri-client` 接 `SessionDataChangedNotice`
- `tauri-client` 使用 `SessionDescribe.metadata.backend_id`
- 网络盘 draft 阶段重复拒绝
- 网络盘重扫阶段 backend 冲突裁决
- 文件盘完整路径判重

### 2.2 本轮明确不做

- 修改 `docs/network` 正式协议定义
- 为 `backend_id` 做跨重启稳定性保证
- 为 `backend_id` 增加持久化字段
- 为网络盘做后台自动重连
- 为文件盘引入与 `backend_id` 类似的额外身份模型
- 把 `data changed` 做成内存盘、文件盘、网络盘共用框架

### 2.3 当前正式口径

- `data changed` 只属于网络盘，不进入内存盘和文件盘主线。
- `remote_backend_id` 只属于网络盘，不进入内存盘和文件盘主线。
- `remote_backend_id` 是非持久化真状态，只以当前成功 `SessionDescribe` 的结果为准。
- `remote_backend_id` 不写入 `DiskRuntime` 通用持久化字段，不写入 `client_config`。
- `remote_backend_id` 当前只用于同一 client 进程内的冲突裁决，不回传 UI，不参与协议建连。
- 文件盘判重单独收在本地文件盘路径，不混入 `network/`。

## 3. 适用原则

本清单严格按 [开发原则](../development/development-principles.md) 执行，重点如下：

- 极简核心原则
  - 这一轮只收一条最小闭环：`notice -> network event -> runtime reconcile -> notify_managed_disk_data_changed`。
  - 不把三类盘抽成统一 `data changed` 平台。
- 激进更新原则
  - 直接重写网络盘重扫裁决逻辑，不保留顺序相关旧行为。
- 单一真实来源原则
  - `remote_backend_id` 只以当前成功 `SessionDescribe` 为真。
  - 网络盘 live session 真状态只由 `NetworkClientState` 持有。
- 边界闸口原则
  - draft 判重收在 `network/uniqueness.rs`。
  - 重扫冲突裁决收在 `network/runtime_flow.rs`。
  - 文件盘路径判重收在 `backend/disk_service.rs` 的文件盘入口。
- 结构重构与层次依赖原则
  - `data changed` 只允许走 `state/network_client -> network/event_reconciler -> BackendRust` 这条层次。
  - 不允许把 `backend_id` 扩散到通用 `DiskRuntime`、通用 DTO、通用持久化层。
- 删除优先原则
  - 删除或替换当前重扫里“边扫边落 runtime”的顺序耦合路径。

## 4. 当前现状与缺口

### 4.1 `data changed` 还没有真正接进 `tauri-client`

当前已有：

- `network_core` 已支持 `SessionDataChangedNotice`
- `rust-cli` 已接通 `data changed`
- `tauri-client` 已有后台 watcher 与 `event_reconciler`

当前缺失：

- `connection_pool` 没有注册 `SessionDataChangedNotice` handler
- `pending_events` 没有 `data changed` 事件类型
- `event_reconciler` 没有 `data changed` 收束路径
- 已挂载网络盘没有正式接到 `notify_managed_disk_data_changed(...)`

### 4.2 `remote_backend_id` 之前被想象成通用 runtime 字段

当前正确口径已经收窄为：

- `remote_backend_id` 不持久化
- `remote_backend_id` 不属于文件盘和内存盘

因此当前缺口不是“给通用模型补字段”，而是：

- 网络 draft 项如何拿到并使用 `backend_id`
- 网络 opened session 如何保留并使用 `backend_id`
- 网络重扫如何基于 fresh `backend_id` 做裁决

### 4.3 当前网络判重只有 `(server_addr, remote_disk_id)`

当前已有：

- 同一远端盘重复添加会被拒绝

当前缺失：

- draft 阶段按 `(server_addr, remote_backend_id)` 判重
- 提交前防御性再次判重
- 重扫阶段按 `(server_addr, remote_backend_id)` 做 authoritative 裁决

### 4.4 当前重扫只有前半段收集与分组，后半段仍是即时提交，行为顺序相关

当前 `runtime_flow::rescan_network_runtimes(...)` 的结构是：

- 先收集全部网络盘重扫任务
- 再按 `server_addr` 做一层分组
- 之后逐盘拿 live session 或新建 session
- 一拿到结果就立即落 `runtime_store`
- 一盘失败就立即写 `invalid`

这条路径的问题：

- 当前只有“收集任务”和“按 connection 分组”这两个前置动作
- 缺少独立的 fresh candidate 阶段
- 缺少独立的统一裁决与统一提交阶段
- 无法对同一 `server_addr` 的全量网络盘做统一 backend 冲突裁决
- `backend_id` 冲突结果依赖遍历顺序
- 无法先完整观察“本轮 fresh describe”再统一决策

### 4.5 文件盘当前还没有完整路径判重

当前已有：

- 文件盘创建、恢复、重扫、编辑

当前缺失：

- 创建既有文件盘时的重复拒绝
- 创建新文件盘时的重复拒绝
- 配置恢复时的重复路径收束

## 5. 目标结构

### 5.1 `data changed` 只留在 `network/`

正式落点固定为：

```text
GatewayConnection
  -> state/network_client/pending_events
  -> network/event_reconciler
  -> backend.notify_managed_disk_data_changed(target_id)
```

固定要求：

- `data changed` 不进入文件盘和内存盘的抽象层。
- `data changed` 不驱动 invalidation。
- `data changed` 只对当前已挂载的网络盘产生本地 `BackendRust` 通知。
- 仅已打开未挂载的网络盘收到 `data changed` 时，当前最小闭环直接忽略。

### 5.2 `remote_backend_id` 只留在网络 live 状态与 staged result

正式落点固定为：

- `draft item` 的 `SessionMetadata`
- `opened session` 的 `SessionMetadata`
- `rescan` 过程中的 staged describe result

固定要求：

- 不写入 `DiskRuntime` 通用持久化字段
- 不写入 `client_config`
- 不回传前端展示
- 不给文件盘和内存盘复制一套字段

### 5.3 draft 阶段判重

draft 添加阶段固定流程改为：

```text
auth
  -> open
  -> describe
  -> duplicate check by remote_disk_id
  -> duplicate check by known remote_backend_id
  -> accept or reject
```

这里的“known remote_backend_id”固定只覆盖当前进程内已知真状态：

- 当前 draft 项
- 当前 live opened session
- 当前已经挂载或刚重扫成功并保有 live session 的网络盘

当前明确不做：

- 为了判重专门把所有历史 runtime 预热建连一遍

原因：

- `remote_backend_id` 非持久化
- authoritative backend 冲突判定应统一交给后续重扫裁决

### 5.4 重扫改为四段执行流水

同一 `server_addr` 下的网络盘重扫固定改为：

第一段：收集全部网络盘任务

- 遍历当前全部 runtime
- 只收集网络盘任务
- 此段不做任何连接、认证、会话和状态修改

第二段：按 `connection/server_addr` 汇总分组

- 将第一段收集到的任务按 `server_addr` 聚合
- 每组共享同一条 connection 复用策略
- 此段仍不修改 `runtime_store` 和 `opened_sessions`

第三段：获取 fresh candidate

- 遍历该 `server_addr` 下全部网络盘任务
- 优先复用可用 live session
- 没有 live session 时做 `connect -> auth -> open -> describe`
- 把每个盘的 fresh result 暂存为 staged candidate
- 此段不立即修改 `runtime_store`
- 此段不立即修改 `opened_sessions` 最终真状态

第四段：统一裁决与统一提交

- 基于这一轮全部 staged candidate 统一做 backend 冲突分组
- 统一决定每个盘最终是 `unmounted`、`refresh metadata` 还是 `invalid`
- 统一接管新 session 或清理被判失败的 session
- 最后一次性把结果提交到 `runtime_store` 和 `NetworkClientState`

### 5.5 `(server_addr, remote_backend_id)` 冲突裁决

同一 `server_addr` 下，如果本轮 fresh result 中出现多个相同 `remote_backend_id`，固定按如下矩阵处理：

- 若该冲突组没有 `ro`，则该组全部 `invalid`
- 若该冲突组有一个或多个 `ro`，则只保留一个 `ro`
- 该组内其余全部 `rw/ro` 一律 `invalid`

保留哪一个 `ro`，当前最小闭环固定为：

- 按 `local_disk_id` 升序选择第一个 `ro` 作为唯一保留者

当前明确不做：

- mounted 优先
- 最近成功优先
- 最近创建优先
- UI 交互式让用户选择

这样收的原因：

- 行为必须确定且可测试
- 不引入额外状态偏好和历史包袱

### 5.6 文件盘完整路径判重

文件盘判重固定只属于本地文件盘。

正式规则：

- 判重键为完整文件路径
- 现有文件盘创建时拒绝重复路径
- 新建文件盘创建时拒绝重复路径

建议的路径归一化口径：

- 转为绝对路径
- 清理相对段
- Windows 下按大小写不敏感比较

配置恢复时若发现重复路径，当前最小闭环固定为：

- 保留第一条
- 其余重复文件盘标为 `invalid`
- 原因写明“文件路径重复”

当前明确不做：

- 为文件盘额外引入唯一 `backend_id`
- 为文件盘做网络式 live state

## 6. 分阶段执行方案

### 第一阶段：接通 `data changed` 事件链

目标：

- `tauri-client` 能接到 `SessionDataChangedNotice`
- 已挂载网络盘能收到本地 `notify_managed_disk_data_changed(...)`

需要修改：

- `windows/tauri-client/src-tauri/src/state/network_client/connection_pool.rs`
- `windows/tauri-client/src-tauri/src/state/network_client/pending_events.rs`
- `windows/tauri-client/src-tauri/src/state/network_client/mod.rs`
- `windows/tauri-client/src-tauri/src/network/event_reconciler.rs`

固定要求：

- `connection_pool` 注册 `set_session_data_changed_handler(...)`
- `pending_events` 增加 dedicated `data changed` 事件
- `event_reconciler` 只在目标 runtime 已挂载时下发 `notify_managed_disk_data_changed(...)`
- `data changed` 与 `SessionCloseNotice`、disconnect 收束分离

测试：

- `data changed` 到达已挂载网络盘时调用一次 `notify_managed_disk_data_changed(...)`
- `data changed` 到达未挂载网络盘时不触发 invalidation
- 同一轮重复 `data changed` 事件可去重，不重复刷同一 target

### 第二阶段：补网络 live 状态上的 `backend_id` 判重

目标：

- draft 添加阶段补 `remote_backend_id` 判重
- 不把 `backend_id` 扩散到通用持久化层

需要修改：

- `windows/tauri-client/src-tauri/src/network/uniqueness.rs`
- `windows/tauri-client/src-tauri/src/network/draft_flow.rs`
- `windows/tauri-client/src-tauri/src/state/network_client/mod.rs`

固定要求：

- `uniqueness` 明确拆开两类约束：
  - `(server_addr, remote_disk_id)`
  - 当前已知 `(server_addr, remote_backend_id)`
- `draft_flow` 在 `describe` 后立即执行两类判重
- 被拒绝的新 session 立即走 close + cleanup 路线
- 不修改 `DiskRuntime` 通用持久化模型

测试：

- 同 `(server_addr, remote_disk_id)` 的 draft 添加被拒绝
- 同 `(server_addr, known remote_backend_id)` 的 draft 添加被拒绝
- 拒绝后 draft 不写入，临时 session 被关闭

### 第三阶段：重建网络重扫为四段执行流水

目标：

- 网络重扫不再顺序相关
- `backend_id` 冲突统一在重扫裁决阶段收口

需要修改：

- `windows/tauri-client/src-tauri/src/network/runtime_flow.rs`
- `windows/tauri-client/src-tauri/src/network/cleanup.rs`
- `windows/tauri-client/src-tauri/src/state/network_client/opened_sessions.rs`

建议内部结构：

- `runtime_flow/rescan/` 目录化拆分，避免继续把所有逻辑堆在单文件中
- 最少拆成：
  - `collector`
  - `candidate`
  - `resolver`
  - `committer`

固定要求：

- 先收集全部网络盘任务
- 再按 `server_addr` 汇总分组
- 再对每组逐盘获取 fresh candidate
- 最后统一裁决并统一提交
- fresh `describe` 是唯一 backend 真源
- `(server_addr, remote_backend_id)` 冲突时只保留一个 `ro`
- 冲突失败盘如果当前已挂载，先 eject 再 `invalid`
- 冲突失败盘如果本轮新开了 session，必须 close + cleanup
- 不允许“某盘刚拿到 describe 就立即落最终状态”

测试：

- 同 backend 的 `rw + ro`，重扫后只保留一个 `ro`
- 同 backend 的多个 `ro`，重扫后只保留一个 `ro`
- 同 backend 全是 `rw`，全部 `invalid`
- 冲突组内保留者固定按 `local_disk_id` 最小 `ro`
- 行为不依赖遍历顺序

### 第四阶段：补文件盘完整路径判重

目标：

- 文件盘创建与恢复时不允许重复路径 silently 共存

需要修改：

- `windows/tauri-client/src-tauri/src/backend/disk_service.rs`
- `windows/tauri-client/src-tauri/src/backend/persistence_service.rs`

固定要求：

- 创建既有文件盘时按完整路径拒绝重复
- 创建新文件盘时按完整路径拒绝重复
- 配置恢复时重复项转 `invalid`，不让整个 restore 失败
- 这条规则只属于文件盘，不进入 `network/`

测试：

- 两个文件盘指向同一路径时创建失败
- 新建文件盘与已存在文件盘路径相同则失败
- restore 遇到重复路径时只保留第一条，其余为 `invalid`

### 第五阶段：同步正式文档与行为说明

目标：

- 正式文档与当前实现一致

需要修改：

- `docs/network/client/tauri-client.md`

需要收住的点：

- `remote_backend_id` 不持久化，只属于网络 live 状态与重扫 staged result
- draft 判重与重扫裁决的分工边界
- `data changed` 只属于网络盘
- 文件盘判重是独立本地规则

## 7. 验收标准

以下结果同时成立，视为本清单完成：

- `tauri-client` 已正式接到 `SessionDataChangedNotice`
- 已挂载网络盘收到 `data changed` 后，本地下沉 `notify_managed_disk_data_changed(...)`
- `remote_backend_id` 没有进入通用持久化层
- draft 添加阶段已能拒绝当前已知 backend 冲突
- 网络重扫已改成两阶段模型
- 同 `(server_addr, remote_backend_id)` 冲突时只保留一个 `ro`
- 文件盘已按完整路径判重
- 正式文档已同步说明当前实现边界

## 8. 实施顺序建议

建议严格按以下顺序做，避免边做边发散：

1. 先接 `data changed` 事件链
2. 再补 draft 阶段 `backend_id` 判重
3. 再重建网络重扫两阶段裁决
4. 再补文件盘路径判重
5. 最后同步正式文档与测试归档

原因：

- `data changed` 是独立增量，最容易先落成闭环
- draft 判重不依赖重扫重构，可先完成一半 backend 约束
- authoritative backend 冲突最终仍在重扫裁决，必须单独完整收口
- 文件盘判重与网络逻辑无依赖，放后面最干净
