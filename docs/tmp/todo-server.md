# server 重构方案

## 1. 目标

按 `docs/network-disk-go-server` 的正式口径，重建 `server`，把当前“单盘 embedded gateway 一体机”收束为清晰的 `gateway + storer` 分体模型，并保留 `whole` 作为同一套能力的装配形态。

这轮只认新的正式结构，不保留历史兼容、不保留旧协议旁路、不保留补丁式过渡层。

## 2. 当前代码判断

### 2.1 已经可复用的地基

- `server/cmd/gateway` 和 `server/cmd/storer` 已经是两个独立可执行入口。
- `server/internal/transport` 已经提供稳定的 framed transport。
- `server/internal/config` 已经把 gateway / storer 的配置拆开。
- `server/internal/auth` 已经具备 `claim_code -> auth_verifier` 和 challenge/proof 计算能力。
- `server/internal/session` 已经具备本地 session 管理、读写、越界、只读和 busy 基础能力。
- `server/internal/storage/file` 已经是可复用的本地介质后端。
- `server/internal/route` 已经有 route 真源的最小容器。
- `server/internal/gateway` 已经有独立 listener、route registry、session registry、storer 连接管理的雏形。
- `server/internal/storer` 已经有 core / whole / remote role 的装配雏形。

### 2.2 现在仍然错的地方

- `SessionOpen` 还在把 `disk_id` 直接当作入参，成功响应还在回 metadata。
- gateway 连接状态还在维护 `pendingAuthDisk`、`authorizedDisk`、`openSessionID` 这种“单盘 + 单会话”旧模型。
- `SessionDescribe` 还不存在。
- `Ping` 还在扮演 session heartbeat。
- `SessionCloseNotice` 还没有按正式口径拆成独立 notice 语义，`request_id` 也还没收成 notice 规则。
- route 注册仍然携带 `SessionTTLSeconds` 这种不该出现在正式路由模型里的字段。
- `session.Descriptor` 还把 runtime 状态、metadata、过期信息、connection 归属揉在一起。
- `gateway/internal/storer_routes.go` 同时承担 route 表、storer 连接、注册、round-trip、心跳和协议转发，职责过重。
- `storer/internal/data_plane_handler.go` 还按旧协议做 `SessionOpen(disk_id) -> session_id + metadata`。
- `storer/internal/whole_runtime.go` 仍然有特殊的 embedded gateway 装配路径，不是统一结构的纯装配层。
- 现有测试大多还在验证旧语义，不能直接拿来作为新协议的验收标准。

### 2.3 当前高风险文件

这些文件已经是重构热点，不能继续在旧结构上叠补丁：

- `server/internal/gateway/storer_routes.go`
- `server/internal/gateway/session_opener.go`
- `server/internal/gateway/handler.go`
- `server/internal/gateway/runtime.go`
- `server/internal/storer/data_plane_handler.go`
- `server/internal/storer/role_runtime.go`
- `server/internal/storer/whole_runtime.go`
- `server/internal/proto/header.go`
- `server/internal/proto/auth.go`
- `server/internal/proto/session.go`
- `server/internal/proto/storer_register.go`

## 3. 必须遵守的重构原则

- 先删旧语义，再加新语义，不做双轨兼容。
- 一个状态只允许一个真源，一个决策只允许一个决策点。
- 重构不是加壳，不是中转层，不是 helper/service 化。
- 目录和包要表达职责，不要让一个文件同时承担协议、状态、转发和装配。
- `whole` 只能是装配形态，不能再派生第二套协议语义。
- 文档、代码、测试必须同时收束，不能让测试继续验证历史行为。

## 4. 目标结构

### 4.1 gateway 目标分层

- `route_registry`
  - 只保存 `disk_id -> route connection` 的真源关系。
  - 保存 route 的静态 metadata：`disk_size_bytes`、`read_only`、`max_io_bytes`、`auth_verifier`。
  - 不保存 session 语义，不保存 client 语义。

- `auth_grant_registry`
  - 只保存 `auth_id` 的生命周期。
  - 绑定 `client_connection_id`、`disk_id`、`issued_at`、`expire_at`、`grant_state`。
  - 不保存 session，不保存 route 数据面信息。

- `session_registry`
  - 只保存 `gateway_session_id -> (client_connection_id, route_connection_id, storer_session_id, disk_id, metadata snapshot)`。
  - `SessionDescribe` 只读这里，不回 storer。

- `client connection runtime`
  - 只负责请求配对、auth/open 互斥、notice 下发。
  - 不再保存“已认证磁盘”或“当前唯一 session”的隐式状态。

- `storer connection runtime`
  - 只负责路由注册、心跳、round-trip、断线回收。
  - 不承担 client 认证语义。

### 4.2 storer 目标分层

- `core`
  - 持有本地介质。
  - 持有本地 session 服务。
  - 持有本地 auth material。

- `register path`
  - 只负责向 gateway 注册 `disk_id`、`auth_verifier` 和 route metadata。

- `data plane`
  - 只处理 gateway 发来的 `SessionOpen / ReadAt / WriteAt / Close`。
  - `SessionOpen` 只返回 `storer_session_id`，不返回 metadata。

- `whole`
  - 只做装配，不做特殊协议。
  - 不能再内建一套“本地 gateway 直绑 handler”的独立语义。

## 5. 实施顺序

### Phase 0. 先冻结 wire 口径

优先改 `server/internal/proto`，把 wire 先收成正式口径，否则后面所有 handler 都会继续朝旧协议回流。

必须完成：

- `header.go`
  - 增加 `SessionDescribe` 的 opcode。
  - 把 `SessionCloseNotice` 改成正式 notice 语义。
  - 增加 `FLAG_NOTICE`。
  - 明确 request / response / notice 的头部校验。

- `auth.go`
  - `AuthFinish` 成功响应必须携带 `auth_id`。
  - `AuthStart` / `AuthFinish` 的 body 结构保持单一职责。

- `session.go`
  - `SessionOpen` 请求改为 `auth_id`。
  - `SessionOpen` 成功响应 body 清空，只返回 `session_id`。
  - 增加 `SessionDescribe(session_id)` 的请求 / 响应编解码。
  - 删除旧的 `Ping` 作为 session heartbeat 的语义。

- `storer_register.go`
  - 删除 `SessionTTLSeconds` 的 wire 字段。
  - 注册只保留 route 真源需要的字段。

这一步的验收标准：

- `SessionOpen` 和 `SessionDescribe` 的 wire 已经和正式文档对齐。
- `SessionCloseNotice` 的 notice 头部规则已经和正式文档对齐。
- 旧的 metadata 直返不再出现在 protocol codec 中。

### Phase 1. 重写 gateway 的连接状态机

目标是把当前 `ConnectionState` 从“单盘单 session 的隐式状态”改成“只管 auth/open 过程互斥的显式状态机”。

要做的事：

- 拆掉 `authorizedDisk` 和 `openSessionID` 这种单会话状态。
- 保留的只应该是：
  - 当前 connection ID
  - 当前是否有 auth in-flight
  - 当前是否有 SessionOpen in-flight
  - 必要的 request / notice 相关临时状态
- `ConnectionState` 不能再充当 session 真源。
- `session_registry` 才是 active session 的真源。

相关文件：

- `server/internal/gateway/handler.go`
- `server/internal/gateway/session_opener.go`
- `server/internal/gateway/runtime.go`

验收标准：

- 一条 connection 可以在已有多个 session 的情况下继续发起新的 auth/open。
- `handler.go` 不再靠一个 `openSessionID` 限死整条连接。
- `SessionDescribe / ReadAt / WriteAt / Close` 不再被旧单会话状态误伤。

### Phase 2. 重写 gateway 的 auth grant 与 session 建立链路

这是这轮最核心的改动。

必须建立新的链路：

1. `AuthStart(disk_id)`
2. `AuthFinish(challenge, proof)`
3. gateway 生成 `auth_id`
4. `SessionOpen(auth_id)`
5. gateway 查 `auth_grant_registry` 和 `route_registry`
6. gateway 向 storer 发起内部 `SessionOpen`
7. storer 返回 `storer_session_id`
8. gateway 分配 `gateway_session_id`
9. gateway 写入 `session_registry` 的 session snapshot
10. `SessionOpen` 只返回 `session_id`
11. client 后续再调用 `SessionDescribe(session_id)`

要删掉的旧假设：

- `SessionOpen` 不再接受 `disk_id`。
- `SessionOpen` 不再返回 metadata。
- `SessionOpen` 不再顺带返回 `TTLSeconds`。
- `auth_id` 不是 connection 上的隐式 long-lived authorized 状态。

相关文件：

- `server/internal/gateway/authenticator.go`
- `server/internal/gateway/session_opener.go`
- `server/internal/gateway/session_registry.go`
- `server/internal/gateway/contracts.go`
- `server/internal/gateway/test_backend_test.go`

验收标准：

- `AuthFinish` 成功后能得到可消费一次的 `auth_id`。
- `SessionOpen` 只消费 `auth_id`，不再消费 `disk_id` 作为 wire 输入。
- `SessionDescribe` 从 `session_registry` 读出 metadata snapshot。
- `busy` 失败时 `auth_id` 仍可在未过期前重试。

### Phase 3. 拆 route 真源和 storer 连接管理

`server/internal/gateway/storer_routes.go` 现在太重，必须拆。

建议拆成至少三块：

- route 真源
- storer connection runtime
- round-trip / register client

必须完成的收口：

- `route.Registry` 只保存 route 真源。
- route entry 只保存 route 需要的静态信息，不再带 session TTL。
- storer 连接断开时，只由 gateway 统一接管：
  - 撤 route
  - 撤 auth grant
  - 关 session
  - 发 `SessionCloseNotice`

相关文件：

- `server/internal/route/registry.go`
- `server/internal/gateway/storer_routes.go`
- `server/internal/gateway/storer_handler.go`
- `server/internal/gateway/runtime.go`

验收标准：

- route 注册和 route 数据面转发不是同一个文件里的“大杂烩”。
- 断线回收逻辑不再散落在多个 handler 里。
- route 失效后，gateway 能一次性收束所有相关 session 和 auth grant。

### Phase 4. 重写 storer 的数据面语义

`server/internal/storer/data_plane_handler.go` 现在还是旧模型，必须改成正式模型。

必须完成：

- `SessionOpen` 请求不再解析 `disk_id`。
- `SessionOpen` 成功响应只返回 `storer_session_id`，不返回 metadata。
- `ReadAt / WriteAt / Close` 继续基于 `storer_session_id`。
- 不再保留 session-scoped `Ping` 语义。
- 如果需要保活，只保留正式定义的 `LinkHeartbeat`，不要把它混成 session heartbeat。

同时要重写本地 session service 的输入输出：

- `session.Service.Open` 不应该再依赖 wire 上来的 `disk_id`。
- session metadata 不应该再带 TTL 作为对外字段。
- `session.Descriptor` 需要拆成“内部运行记录”和“对外 metadata 快照”。

相关文件：

- `server/internal/storer/data_plane_handler.go`
- `server/internal/storer/gateway_local_adapter.go`
- `server/internal/storer/core.go`
- `server/internal/session/service.go`
- `server/internal/session/manager.go`

验收标准：

- storer 只负责本地会话和 I/O，不再承担 client 认证语义。
- `SessionOpen` 不再把 metadata 直接带回 client。
- storer 端只接受 gateway 的 route-facing 请求。

### Phase 5. 收束 `whole` 只做装配

`whole` 现在仍然有特殊路径，后面必须把它收成“同一套 gateway/storer 核心的装配形态”。

要求：

- `whole` 不再注册一套单独的本地 route 假数据结构。
- `whole` 不再靠特殊 adapter 伪造另一套协议语义。
- `whole` 只是把同一套 gateway 核心和 storer 核心装到同一个进程里。

相关文件：

- `server/internal/storer/whole_runtime.go`
- `server/internal/storer/role_runtime.go`
- `server/internal/storer/gateway_local_adapter.go`

验收标准：

- `whole` 和独立 `gateway + storer` 在协议层没有两套语义。
- 只有装配方式不同，没有协议语义不同。

### Phase 6. 补齐正式的失效处理

正式文档要求的是：

- 协议层只定义 session / connection / route 已失效。
- 网络层只调用 `NetworkMedia` 的失效接口。
- 立即清理还是保留严格假死挂起态，由宿主决定。

所以代码里要补的不是“强制删除对象”，而是“把失效事件显式传到上层”。

必须完成：

- `SessionCloseNotice` 用正确的 notice 头部发送。
- route / connection 失效时，gateway 能对 client 下发通知。
- client 侧后续会话对象必须有统一失效入口。
- 不要在网络层里把“清理”和“挂死”硬编码成唯一行为。

相关文件：

- `server/internal/gateway/runtime.go`
- `server/internal/gateway/session_registry.go`
- `server/internal/gateway/session_opener.go`
- 对接的 client 侧代码

验收标准：

- 失效事件能穿透到上层。
- 协议层不替宿主做对象生命周期决策。

### Phase 7. 更新测试，而不是沿用旧测试

现有测试要分两类处理：

#### 必须重写的测试

- `server/internal/gateway/session_opener_test.go`
- `server/internal/gateway/session_mapping_test.go`
- `server/internal/gateway/storer_handler_test.go`
- `server/internal/storer/integration_test.go`
- `server/internal/storer/role_runtime_test.go`
- `server/internal/storer/data_plane_handler_test.go`

这些测试大概率还在验证：

- `SessionOpen` 直接回 metadata
- `Ping` 作为 session heartbeat
- 单连接单 session 的隐式状态
- 旧 notice / 旧 opcode 语义

这些都必须删除旧断言后重写。

#### 可以保留或轻改的测试

- `server/internal/config/*`
- `server/internal/route/registry_test.go`
- `server/internal/session/service_test.go`
- `server/internal/storage/file/*`
- `server/internal/transport/*`

这些测试如果只覆盖底层纯逻辑，可以保留，但要确认它们没有偷偷依赖旧 wire 语义。

新的必测场景：

- `AuthFinish -> auth_id`
- `SessionOpen(auth_id) -> session_id`
- `SessionDescribe(session_id) -> metadata`
- 同一 connection 上可串行打开多个 session
- route 断开后 auth grant 和 session 一起失效
- notice 的头部和 reason 编码正确
- `SessionOpen` busy 时 `auth_id` 仍可重试
- `SessionOpen` 成功后不能再把 metadata 当成 open 响应返回

## 6. 推荐执行顺序

1. 先改 `server/internal/proto`，把 wire 固定下来。
2. 再改 `gateway` 的 `ConnectionState`、`auth_grant_registry`、`session_registry`。
3. 再改 `storer` 的 `SessionOpen` 与本地 session 服务。
4. 再拆 `storer_routes.go`，把 route / connection / round-trip 分开。
5. 再把 `whole` 收成纯装配层。
6. 最后重写 server 侧测试。

## 7. 完成标准

这轮 `server` 重构完成时，至少要满足：

- `SessionOpen` 只返回 `session_id`。
- `SessionDescribe` 单独返回 metadata snapshot。
- route / connection / session / auth grant 的真源都只有一份。
- `whole` 和独立 `gateway + storer` 只在装配方式上不同。
- 旧测试和旧 wire 语义全部清掉。
- `go test ./...` 通过。
