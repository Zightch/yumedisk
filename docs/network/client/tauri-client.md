# tauri-client Client

## 定位

本文档描述 `tauri-client` 的正式 client 侧目标口径和当前最小闭环收口。

它承接 `docs/network/define` 没有定义的 client 侧内容，包括：

- `tauri-client` 的网络真状态边界
- `DiskRuntime`、`disksession`、`NetworkMedia` 的职责划分
- 创建网络盘对话框的 draft 生命周期
- 重扫、挂载、拔出、删除的当前宿主策略
- 故障与 connection cleanup 的当前实现口径

这里不重复定义协议字段和 wire 语义。

## 当前对象结构

`tauri-client` 侧固定收口为：

```text
ClientState
  -> NetworkClientState
    -> connection_pool[server_addr]
    -> opened_disk_sessions[(server_addr, remote_disk_id)]
    -> network_create_drafts[draft_id]
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
- `auth_material`
  - 当前最小闭环直接保存原始 `claim_code`

当前全局唯一键固定为：

```text
(server_addr, remote_disk_id)
```

直接结果：

- 不允许重复添加同一远端盘
- 检查范围至少覆盖正式 `DiskRuntime` 和当前 draft 项

## `NetworkClientState`

`NetworkClientState` 是 `tauri-client` 唯一网络真状态持有者，负责：

- 连接复用
- 已打开 `disksession` 复用
- draft 生命周期
- `SessionCloseNotice` / disconnect 收束
- `NetworkMedia` 终态错误失效队列
- 网络盘重扫任务汇总与分发

它固定是多 connection 全局协调层，不是单个 connection 管理器。

并发边界固定为：

- 每个 `server_addr` 拥有独立执行 lane
- 不同 `server_addr` 的建连、auth、open、读写、重扫、故障收束可并发推进
- 同一 `server_addr` 内仍遵守单 connection 的 auth/open lane 互斥

这里固定的是“多 connection + 每 connection 独立 lane”的模型，不写死线程实现方式。

## `DiskRuntime` 与 `NetworkMedia`

网络盘 `DiskRuntime` 最小字段固定为：

- `local_disk_id`
- `disk_name`
- `server_addr`
- `remote_disk_id`
- `auth_material`
- `capacity_bytes`
- `read_only`
- `auto_mount`

`DiskRuntime` 不负责：

- 持有 live `disksession`
- 持有 live connection
- 持有长期常驻 `NetworkMedia`

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
  -> duplicate check by (server_addr, remote_disk_id)
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
2. 默认状态写为 `invalid`
3. 不自动创建 `NetworkMedia`
4. 自动触发一次网络盘重扫任务

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

网络盘重扫固定为“先汇总，再分发”：

1. 遍历全部 `DiskRuntime`
2. 网络盘先汇总成 `network_rescan_task`
3. 按 `server_addr` 去重并建连
4. 先查 `opened_disk_sessions`
5. 没有 live `disksession` 时再做 `auth -> open -> describe`
6. 成功标为 `unmounted`
7. 失败标为 `invalid`

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

## 故障与 cleanup

### 留白口径

网络层只负责上报事件，不直接规定 client 必须采用哪种清理策略。

固定事实：

- `disksession` 关闭后，只调用 `DiskRuntime` 或宿主的事件通知接口
- 具体是立即清理、挂死保留还是别的处理，交给 client 当前实现决定

### 当前最小闭环实现

当前 `tauri-client` 采用的具体策略是：

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
- 挂载时才创建 `NetworkMedia`
- 拔出后保留 live `disksession`
- 删除后关闭 live `disksession`
- 故障后盘直接转 `invalid`
- 恢复入口统一回到 rescan
