# tauri-client Client

## 定位

本文档描述 `tauri-client` 的正式 client 侧目标口径和当前最小闭环收口。

它承接 `docs/network/define` 没有定义的 client 侧内容，包括：

- `tauri-client` 的网络真状态边界
- `DiskRuntime`、`disksession`、`NetworkMedia` 的职责划分
- 创建网络盘对话框的 draft 生命周期
- `remote_backend_id` 的唯一性口径
- `SessionDataChangedNotice` 的宿主处理策略
- 重扫、挂载、拔出、删除的当前宿主策略
- 故障与 connection cleanup 的当前实现口径

这里不重复定义协议字段和 wire 语义。

## 当前对象结构

`tauri-client` 侧固定收口为：

```text
ClientState
  -> NetworkClientState
    -> connection_pool[server_addr]
    -> opened_sessions[(server_addr, remote_disk_id)]
    -> drafts[draft_id]
  -> DiskCatalog
    -> DiskRuntime[*]
  -> BackendRust
    -> managed disk
```

挂载态下额外存在：

```text
DiskRuntime
  -> NetworkMedia
    -> DiskSession
    -> metadata(SessionDescribe)
```

关键边界如下：

- `NetworkClientState` 持有 live connection、live `disksession` 和 draft
- `DiskRuntime` 只描述本地盘卡片与持久化配置
- `NetworkMedia` 只在真正挂载到 `BackendRust` 时构造

## 命名与唯一键

固定命名如下：

- `appsession`
  - 本机 `BackendRust + KMDF` 会话
- `connection`
  - 一条 `client-gateway` 业务连接
- `disksession`
  - 一条已打开的网络盘会话
- `local_disk_id`
  - 本地盘卡片与 `DiskRuntime` 标识
- `server_addr`
  - 服务器地址领域名
- `remote_disk_id`
  - 远端网盘标识
- `remote_backend_id`
  - 最近一次成功 `SessionDescribe` 获得的 backend 身份
- `auth_material`
  - 当前最小闭环直接保存原始 `claim_code`

当前重复拒绝固定为两个条件：

- `(server_addr, remote_disk_id)` 相同
- `(server_addr, remote_backend_id)` 相同

直接结果：

- 不允许重复添加同一远端盘
- 不允许同时添加同一 backend 的 `rw` 和 `ro`
- 不允许同时添加同一 backend 的多个 `ro`
- 检查范围至少覆盖正式 `DiskRuntime` 和当前 draft 项
- `remote_backend_id` 只在完成 `SessionDescribe` 后才能参与判断

### `remote_backend_id` 的当前真状态边界

`remote_backend_id` 当前明确不属于 `DiskRuntime` 持久化模型。

它只存在于三处：

- draft 项的 `SessionMetadata`
- `opened_session` 的 `SessionMetadata`
- 网络重扫四段流水中的 staged candidate

当前固定不做：

- 写入 `DiskRuntime`
- 写入 `client_config`
- 回传前端展示
- 脱离 `SessionDescribe` 单独维护一份 backend 快照

## `NetworkClientState`

`NetworkClientState` 是 `tauri-client` 唯一网络真状态持有者，负责：

- 连接复用
- 已打开 `disksession` 复用
- draft 生命周期
- `SessionDataChangedNotice` / `SessionCloseNotice` / disconnect 收束
- `NetworkMedia` 终态错误失效队列
- 网络盘重扫任务汇总与分发

它固定是多 connection 全局协调层，不是单个 connection 管理器。

并发边界固定为：

- 每个 `server_addr` 拥有独立执行 lane
- 不同 `server_addr` 的建连、auth、open、读写、重扫、故障收束可并发推进
- 同一 `server_addr` 内仍遵守单 connection 的 auth/open lane 互斥

这里固定的是“多 connection + 每 connection 独立 lane”的模型，不写死线程实现方式。

### 当前代码落点

当前实现已经按职责拆成以下几层：

- `src-tauri/src/state/network_client/`
  - `mod.rs`
    - `NetworkClientState` 对外唯一真状态对象
  - `connection_pool.rs`
    - connection 复用与 idle cleanup 判定
  - `opened_sessions.rs`
    - live `disksession` 表
  - `drafts.rs`
    - draft 表与 `draft_id` 分配
  - `pending_events.rs`
    - `SessionDataChangedNotice` / `SessionCloseNotice` / disconnect / media invalidation 暂存队列
- `src-tauri/src/network/`
  - `validation.rs`
    - 边界输入校验
  - `uniqueness.rs`
    - `(server_addr, remote_disk_id)` 与 `(server_addr, remote_backend_id)` 唯一键检查
  - `gateway_ops.rs`
    - connect / auth / open / describe 最小网络操作
  - `cleanup.rs`
    - session close、runtime invalidation、draft session cleanup
  - `event_reconciler.rs`
    - drain 事件并把 `NetworkClientState`、`DiskRuntimeStore`、`BackendRust` 收束一致
  - `draft_flow.rs`
    - draft 真状态写入、提交接管、dispose cleanup
  - `runtime_flow/`
    - `mod.rs`
      - network runtime 的挂载、拔出、删除最小编排
    - `rescan.rs`
      - 网络盘重扫四段执行流水与统一裁决
- `src-tauri/src/workflow/`
  - `network_runtime.rs`
    - network event 收束唯一闸口
  - `network_draft.rs`
    - draft command 的上层编排与跨 flow 串接
  - `runtime_disk.rs`
    - local/network runtime 的统一分派入口
- `src-tauri/src/commands/`
  - `network_disk.rs`
    - 网络盘 draft command 薄桥接
  - `disk.rs`
    - 磁盘 command 薄桥接
- `src-tauri/src/lib.rs`
  - 后台 watcher 定时调用 `workflow/network_runtime`

## `DiskRuntime` 与 `NetworkMedia`

网络盘 `DiskRuntime` 最小字段固定为：

- `local_disk_id`
- `disk_name`
- `server_addr`
- `remote_disk_id`
- `auth_material`
- `capacity_bytes`
- `auto_mount`
- `configured_read_only`
- `source_read_only`

`DiskRuntime` 不负责：

- 持有 live `disksession`
- 持有 live connection
- 持有长期常驻 `NetworkMedia`
- 持有 `remote_backend_id`

`NetworkMedia` 只表示挂到 `BackendRust::Media` 的网络盘介质视图，固定显式持有：

- `remote_disk_id`
- `DiskSession`
- `SessionDescribe` 返回的 metadata

`DiskSession` 在这里仍只负责：

- `SessionDescribe`
- `ReadAt`
- `WriteAt`
- `Close`

它不负责：

- `ConnHeartbeat`
- connection 保活

`NetworkMedia` 不负责：

- 建连
- 认证
- 自动重连
- 重扫

### 后续接入 `cache` 的固定风险提示

当前 `tauri-client` 主线也还没有把 `rw` 路径正式接进 `cache`。

但后续一旦接入，固定口径应提前收住为：

- `mux` 先按 `SessionDescribe.disk_size_bytes` 对外层请求做真实越界判断
- `cache` 右侧 `DiskSessionAtIo` 会把真实容量按 cache block size 视为一个向上对齐后的逻辑尾块，只用于服务最后一个不足整块的块
- 命中最后一个不足整块的尾块时，读路径只拉取真实前缀并在本地补 `0`；写路径只推送真实前缀，超出真实 EOF 的尾部直接丢弃
- `rw` 路径的 `write_locked()` 成功，只表示本地 cache 已接受写入，不再等价于远端已经确认写成功
- 若后续 flush 失败，包括最后短尾块的真实前缀写失败，当前项目不把这个延迟失败再追到 kernel，也不额外提供解释或恢复语义

### 当前前端代码落点

创建网络盘 feature 当前已收成：

- `src/features/createNetworkDisk/CreateNetworkDiskDialog.vue`
  - 对话框壳与组合
- `src/features/createNetworkDisk/useNetworkDraftFlow.ts`
  - 本地交互态与 invoke 编排
- `src/features/createNetworkDisk/NetworkDraftForm.vue`
  - 服务器地址、磁盘名、领盘码输入区
- `src/features/createNetworkDisk/NetworkDraftList.vue`
  - draft 列表区
- `src/features/createNetworkDisk/networkDraftError.ts`
  - 错误码到中文文案映射

固定口径仍然是：

- 后端 draft 才是真状态
- 前端只持有当前对话框交互态
- 前端不复制一套独立网络运行态

## 创建网络盘对话框主链

### 测试连接

固定流程：

```text
find connection by server_addr
  -> reuse if exists
  -> else Hello -> transport
```

失败只提示“测试连接失败”，不展开详细错误。

### 添加临时网盘

固定流程：

```text
authenticate(claim_code)
  -> SessionOpen(auth_id)
  -> SessionDescribe(session_id)
  -> duplicate check by (server_addr, remote_disk_id) and (server_addr, remote_backend_id)
  -> write draft item
```

若命中重复：

- 当前添加直接失败
- 关闭这次新打开出来的 `disksession`
- 不写入 draft
- 不额外显式做 connection cleanup

### 删除临时卡片

固定流程：

- 关闭该 draft 项对应的 `disksession`
- 从 draft 列表移除该项

当前明确不做：

- 删除临时卡片时立即做 connection cleanup

draft 产生的 connection cleanup 统一延后到对话框消失时处理。

### 提交

固定流程：

1. 把保留的 draft 项写成正式 `DiskRuntime`
2. 先持久化 `DiskRuntime`
3. 再把 draft 持有的 live `disksession` 接管进 `opened_sessions`
4. 不自动创建 `NetworkMedia`
5. 最后立即执行一次网络盘重扫

当前实现结果是：

- 提交成功后，网络盘会在同一条工作流里被重扫收束
- 若接管过来的 live `disksession` 可复用，则对应 `DiskRuntime` 会转成 `unmounted`
- 若后续重扫失败，则对应 `DiskRuntime` 仍会落回 `invalid`

提交前要再次执行防御性重复检查，不能只依赖“添加时检查一次”。

### 对话框消失

当前额外增加一个统一 cleanup 闸口：

- 创建网络盘对话框消失时

它覆盖：

- 点击取消
- 点击提交
- 点击右上角关闭

固定口径：

- 取消或直接关闭时，关闭 draft 持有的全部临时 `disksession`
- 提交时，保留已被正式接管的 live `disksession`
- 最后统一做一次 draft 级 connection cleanup sweep

## 重扫、挂载、拔出、删除

### 重扫

网络盘重扫当前已经收成四段执行流水：

1. 收集全部网络盘任务
2. 按 `server_addr` 汇总分组
3. 为每个盘获取 fresh candidate
4. 统一裁决并统一提交

第三段固定流程是：

- 优先复用当前 live `opened_session`
- live `opened_session` 不可用时，再做 `connect -> auth -> open -> describe`
- 当前轮次只暂存 candidate，不立即写回 `runtime_store`

第四段固定流程是：

- 以本轮 fresh `SessionDescribe.metadata.backend_id` 为唯一 backend 真源
- 按 `(server_addr, remote_backend_id)` 做 authoritative 冲突裁决
- 统一清理 stale session / loser session
- 最后一次性更新 `DiskRuntime`、`opened_sessions` 和 connection cleanup

当前冲突矩阵固定为：

- 同 `(server_addr, remote_backend_id)` 若只有一个 candidate，则直接保留
- 若有多个 candidate 且存在一个或多个 `ro`，只保留一个 `ro`
- 保留者固定为 `local_disk_id` 数值最小的那个 `ro`
- 其余 `rw/ro` 一律置为 `invalid(网络盘后端冲突)`
- 若该组没有任何 `ro`，则整组全部 `invalid(网络盘后端冲突)`

mounted runtime 在重扫中的当前实现还有一条额外约束：

- 只有复用原 live `opened_session` 时，才允许保持 mounted
- 只要本轮拿到的是 fresh reopened session，即使原来是 mounted，也必须回到 `unmounted`
- 这样可以避免挂载侧 `NetworkMedia` 继续绑定旧 session

### 挂载

固定流程：

```text
find DiskRuntime by local_disk_id
  -> find live disksession by (server_addr, remote_disk_id)
  -> build NetworkMedia(disksession + metadata)
  -> create managed disk
```

若找不到 live `disksession`，该盘直接视为 `invalid`。

### 拔出

当前最小闭环固定为：

- 从 `BackendRust` 移除 managed disk
- 状态回到 `unmounted`
- 不关闭 live `disksession`
- 不主动回收 connection

### 删除

当前最小闭环固定为：

1. 若当前已挂载，先 eject
2. 若存在 live `disksession`，则关闭该 `disksession`
3. 删除对应 `DiskRuntime` 与持久化记录

固定要求：

- 删除后清理 live `disksession`
- 删除逻辑不额外显式调用 connection cleanup
- connection 是否释放只交给 session-close 路径自动判定

## 编辑边界

网络盘编辑当前只允许修改：

- `disk_name`
- `auto_mount`

当前不允许通过编辑修改：

- `server_addr`
- `remote_disk_id`
- `auth_material`

如果要改变这些字段，固定走“删除旧盘，再重新添加新盘”。

`remote_backend_id` 不在这里的原因是：

- 它当前不是可编辑字段
- 它不是持久化配置字段
- 它只属于当前 live/staged `SessionDescribe` 真状态

## 文件盘路径判重边界

文件盘完整路径判重当前已经正式落地，但它只属于本地文件盘。

固定规则：

- 判重键为完整文件路径
- 归一化口径为：
  - 转绝对路径
  - 折叠 `.` / `..`
  - Windows 下按大小写不敏感比较
- `create_file_disk(...)` 按完整路径拒绝重复
- `create_new_file_disk(...)` 按完整路径拒绝重复
- 配置恢复遇到重复路径时，保留第一条，其余标为 `invalid(文件路径重复)`
- 本地文件盘重扫不会把这些重复项重新洗回可用状态

这条规则当前明确不进入：

- `network/`
- `remote_backend_id`
- 网络盘重扫裁决

## 故障与 cleanup

### 留白口径

网络层只负责上报事件，不直接规定 client 必须采用哪种清理策略。

固定事实：

- `disksession` 关闭后，只调用 `DiskRuntime` 或宿主的事件通知接口
- 具体是立即清理、挂死保留还是别的处理，交给 client 当前实现决定

### 当前最小闭环实现

当前 `tauri-client` 采用的具体策略是：

- 收到 `SessionDataChangedNotice` 时：
  - 若目标 session 当前已挂载，则调用本地 data-changed 通知入口
  - 若目标 session 仅处于已打开未挂载状态，则当前最小闭环直接忽略
  - 同一轮重复 notice 会先去重，再下沉一次本地通知
- 已挂载或未挂载网络盘收到 `SessionCloseNotice` 或 connection 死亡后，直接转为 `invalid`
- `NetworkMedia` 读写遇到终态错误时，只上报失效事件，再由后台收束器统一转为 `invalid`
- 若当前已挂载，则先 eject
- 清理对应 live `disksession`
- 清理挂载侧 `NetworkMedia`
- 不主动重连
- 不后台感知服务器是否恢复上线
- 只能通过后续 rescan 重新走 `bootstrap -> auth -> open -> describe`

### connection cleanup 触发点

当前 `tauri-client` 只有两个 cleanup 闸口：

1. `disksession` 关闭路径
2. 创建网络盘对话框消失路径

当前明确不做：

- 任意时刻只要 connection 暂时空闲就立即关闭
- 独立后台 idle sweeper
- 删除临时卡片时立即回收 connection

## 当前最小闭环结论

当前 `tauri-client` 网络盘目标模型固定为：

- 一个 `NetworkClientState` 管多个 `server_addr` connection
- 一个 connection 可以并存多个已打开 `disksession`
- 已打开 `disksession` 可以被重扫复用
- `remote_backend_id` 只属于当前 live/staged `SessionDescribe` 真状态，不持久化
- 挂载时才创建 `NetworkMedia`
- 网络重扫已经固定为四段执行流水
- 同 `(server_addr, remote_backend_id)` 冲突时只保留一个 `ro`
- 拔出后保留 live `disksession`
- 删除后关闭 live `disksession`
- `SessionDataChangedNotice` 只驱动本地 data-changed 通知，不驱动 invalidation
- 文件盘完整路径判重是独立本地规则，不混入网络盘逻辑
- 故障后盘直接转 `invalid`
- 恢复入口统一回到 rescan
