# rust-cli Client

## 定位

本文档只描述当前 `rust-cli` 的实现口径。

它承接 `docs/network/define` 没有定义的 client 侧策略，包括：

- connection 复用边界
- auth/open lane 的本地对象组织
- `NetworkMedia` 如何绑定 metadata
- `remote_backend_id` 如何参与本地重复拒绝
- `SessionDataChangedNotice` 的宿主处理方式
- session / connection 故障后的当前清理策略
- session 关闭后何时回收 connection

## 当前对象结构

当前 `rust-cli` 侧主结构固定为：

```text
CliHost
  -> NetworkMountRegistry
    -> GatewayConnection[*]
    -> MountedNetworkDisk[*]
      -> DiskSession
      -> NetworkMedia
```

关键对象职责如下：

### `GatewayConnection`

表示一条可复用的 `client-gateway` 业务连接，负责：

- client `Hello`
- framed transport 收发
- `request_id` 分配与请求配对
- `ConnHeartbeat`
- auth/open in-flight lane 互斥
- connection 内 `auth_id` 与 `session_id` 生命周期追踪
- `SessionDataChangedNotice` / `SessionCloseNotice` / disconnect 事件上报

它不负责：

- 盘对象元数据缓存
- 自动重试认证
- 自动重新打开 session

### `AuthGrant`

表示一次成功认证后的显式授权：

- `disk_id`
- `auth_id`

它只是 `SessionOpen` 的输入，不等于已打开会话。

### `DiskSession`

表示一条已打开 session：

- 持有 `GatewayConnection`
- 持有 `session_id`

它负责：

- `SessionDescribe`
- `ReadAt`
- `WriteAt`
- `Close`

它不负责：

- `ConnHeartbeat`
- connection 保活

### `NetworkMedia`

表示真正挂到 backend 的网络盘介质视图，固定显式持有：

- `disk_id`
- `DiskSession`
- `disk_size_bytes`
- `read_only`
- `max_io_bytes`
- `remote_backend_id`

`NetworkMedia` 不负责：

- 认证
- 建连
- connection heartbeat
- 自动重连
- 自动 reopen

## claim code 口径

当前 `rust-cli` 的 claim code 规则为：

- 总长度至少 `80`
- 全部字符必须是 `0-9a-zA-Z`
- 前 `16` 个字符作为 `disk_id`
- 整个 claim code 字节串计算 `SHA512`，得到 `auth_verifier`
- `algo_version = 1` 时，用该 `auth_verifier` 对 `AuthStart.salt` 做 `HMAC-SHA512`
- `auth_verifier` 和后续 `proof` 在线上传输都使用原始 `64` 字节，不做十六进制编码

这是一条 client 实现规则，不是 `docs/network/define` 强行规定的 UI 输入格式。

## 连接复用边界

当前连接池键固定为 gateway endpoint 地址字符串。

直接结果：

- 同一 gateway 地址上的不同盘认证与会话会优先复用同一条 `GatewayConnection`
- 不同 gateway 地址使用不同连接
- 多条 connection 之间互不共享 `auth_id`、session 或 pending request

当前连接复用不按 `disk_id` 切分。

## 当前挂载主链

`mount_network_disk` 当前固定执行：

```text
acquire/reuse GatewayConnection
  -> authenticate(claim_code)
  -> SessionOpen(auth_id)
  -> build DiskSession
  -> SessionDescribe(session_id)
  -> duplicate check by (server_addr, remote_disk_id) and (server_addr, remote_backend_id)
  -> bind NetworkMedia(disk_id, session, metadata)
  -> create managed disk
```

固定口径：

- `SessionOpen` 成功后一定再做一次 `SessionDescribe`
- rust-cli 不从 `SessionOpen` 推导 metadata
- `remote_backend_id` 只从 `SessionDescribe` 获取
- `NetworkMedia` 一定显式记录 `disk_id`

## 唯一性口径

当前 `rust-cli` 对同一 gateway 地址上的网络盘冲突收口为：

- `(server_addr, remote_disk_id)` 相同则拒绝
- `(server_addr, remote_backend_id)` 相同则拒绝

直接结果：

- 不允许同时添加同一 backend 的 `rw` 和 `ro`
- 不允许同时添加同一 backend 的多个 `ro`
- `remote_backend_id` 只参与本地重复拒绝，不参与协议建连、认证或开会话

## 并发与 lane 规则

当前 `GatewayConnection` 本地生命周期状态固定有四块：

- `auth_in_flight`
- `open_in_flight`
- `auth_grants`
- `active_sessions`

本地执行规则与 `docs/network/define` 对齐：

- 同时最多一个 auth 过程
- 同时最多一个 `SessionOpen` 过程
- auth 与 open 互斥
- 已签发 `auth_id` 可以并存
- 已打开 session 可以并存
- 已打开 session 的读写可与后续 auth/open 并发

因此当前目标模型是：

- 一条 connection 可以挂多个盘 session
- 同一条 connection 上多盘读写可以并发
- 新 auth/open 不需要先关闭已经打开的其他 session

## I/O 与 metadata 落地

当前 `NetworkMedia` 在本地直接使用 `SessionDescribe` 返回的：

- `disk_size_bytes`
- `read_only`
- `max_io_bytes`
- `backend_id`

大块 I/O 的当前策略为：

- `NetworkMedia` 在 client 本地按 `max_io_bytes` 主动拆片
- 每片都走一次独立 `ReadAt / WriteAt`
- server 不负责替 client 做跨请求重组

## 当前故障收束策略

`docs/network/define` 只定义 session / connection / route 失效事实。当前 `rust-cli` 采用的具体策略是：

- session 被主动关闭时：直接卸载并清理该盘
- 收到 `SessionCloseNotice` 时：标记对应盘等待清理，然后卸载
- connection 断开或 `ConnHeartbeat` 超时时：标记该 connection 名下全部盘等待清理，然后卸载
- `NetworkMedia` 读写遇到终态错误时：触发 invalidation handler，标记该盘等待清理，然后卸载

当前不引入：

- 假死挂起态
- 自动重连
- 自动 reopen
- 失效后继续保留挂载对象

## connection 清理触发点

connection 清理工作当前只发生在 session 关闭路径。

固定规则：

1. session 关闭或卸载时，先尝试 `Close(session_id)`
2. 然后调用 `release_connection_after_session_close`
3. 只有当该 connection 同时满足以下条件时，才关闭连接：
   - 没有活跃 session
   - 没有 auth 过程在飞
   - 没有 `SessionOpen` 过程在飞
   - 没有未消费 `auth_id`

也就是说：

- 不是只要 connection 暂时空闲就立即关闭
- 不是任意错误路径只要发现“当前没 session”就直接关
- 没有独立后台 idle sweeper
- 关闭判断入口固定在 session close / remove 路径

## `SessionOpen` 当前 client 口径

`docs/network/define` 不为 `SessionOpen` 失败定义统一业务语义。当前 `rust-cli` 的实现口径是：

- 只认 `OK + session_id` 为成功
- 成功后始终再做 `SessionDescribe`
- 当前若收到项目自定义状态 `0x1202`，则映射为 `open-rejected`
- `open-rejected` 不会让 rust-cli 主动丢弃该 `auth_id`
- 该 `auth_id` 仍可在同一 connection 上继续等待后续重试或自然过期
- 其他 open 失败仍按 server 定义的 open failure 处理
- client 不从失败响应中推导 metadata
- client 不在协议层额外发明其他失败语义

若挂载流程在 open 之后、建盘之前失败，当前会：

- 尝试关闭刚打开的 session
- 再按 session close 路径决定是否释放 connection

## notice 与 disconnect 传播

当前 `NetworkMountRegistry` 的事件传播固定为：

- `SessionDataChangedNotice(session_id)` 只作用于匹配 `(addr, session_id)` 的挂载
- 若目标 session 当前已挂载到本地 managed disk，则调用本地 data-changed 通知入口
- 若目标 session 当前仅处于已打开未挂载状态，则当前最小闭环直接忽略
- `SessionCloseNotice(session_id)` 只标记匹配 `(addr, session_id)` 的挂载
- disconnect 只标记匹配 `addr` 的全部挂载
- 真实卸载动作在 `reap_dead_network_disks` / remove 流程中完成

这保证了：

- `SessionDataChangedNotice` 只表达数据变化，不触发 invalidation
- `SessionCloseNotice` / disconnect 只负责标记失效
- 真正的 backend 卸载仍走宿主自己的删除路径
