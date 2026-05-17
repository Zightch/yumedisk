# windows/rust-cli 重构方案

## 1. 目标

按 `docs/network-disk-go-server` 的正式口径，重建 `windows/rust-cli` 的网络协议栈、会话对象、`NetworkMedia` 和宿主接线，使它不再消费旧版 `SessionOpen(disk_id) -> session_id + metadata` 模型，而是收敛到：

1. `AuthStart / AuthFinish -> auth_id`
2. `SessionOpen(auth_id) -> session_id`
3. `SessionDescribe(session_id) -> metadata`
4. `NetworkMedia(disk_id + DiskSession + metadata)`

这轮同样不做历史兼容，不保留旧协议分支，不保留“先支持旧 open、以后再切”的过渡层。

## 2. 当前代码判断

### 2.1 已经可复用的地基

- `windows/rust-cli/src/network/transport_client.rs` 已经提供 transport client。
- `windows/rust-cli/src/network/protocol_client.rs` 已经有独立的协议编解码层。
- `windows/rust-cli/src/network/gateway_connection.rs` 已经有 request/response 配对和 notice 分发雏形。
- `windows/rust-cli/src/network/connection_authenticator.rs` 已经有 challenge/proof 认证主链。
- `windows/rust-cli/src/network/session_opener.rs` 已经有会话打开入口。
- `windows/rust-cli/src/network/disk_session.rs` 已经有读写调用封装。
- `windows/rust-cli/src/network/network_media.rs` 已经实现 `backend_rust::Media` 适配。
- `windows/rust-cli/src/cli/host.rs` 已经把 network disk 接到 CLI 宿主上。

### 2.2 现在仍然错的地方

- `protocol_client.rs` 里 `SessionOpenRequest` 还在发送 `disk_id`。
- `SessionOpenResponse` 还在解析 metadata 和 `session_ttl_seconds`。
- `ConnectionAuthenticator::authenticate()` 现在只返回 `disk_id`，没有显式 `auth_id` 对象。
- `SessionOpener::open()` 还把“打开会话”和“拿 metadata”揉成一步。
- `GatewayConnection` 还在维护 `Idle / AuthPending / Authorized / SessionOpen` 这种单盘单会话状态机。
- `GatewayConnection` 当前模型不允许一个 connection 下并行持有多个 `DiskSession`。
- `DiskSession` 还把 `disk_size_bytes / read_only / max_io_bytes / session_ttl_seconds` 都绑在 session 对象上。
- `DiskSession` 还在用 session-scoped `Ping` 做 keepalive。
- `NetworkMedia` 现在只显式记录 `session + metadata`，没有把 `disk_id` 收进自身状态。
- `CliHost::mount_network_disk()` 还在用 `auth -> open -> 用 open 响应直接构造 media` 这一条旧链。
- `SessionCloseNotice` 收到后，宿主侧还没有完全收束到“网络层只上报失效，具体清理策略由宿主决定”的边界。
- 现有 Rust 测试大多还在验证旧 `SessionOpen` / `Ping` / 单 session connection 模型。

### 2.3 当前高风险文件

这些文件已经是 Rust CLI 侧的重构热点，不能继续在旧模型上叠补丁：

- `windows/rust-cli/src/network/protocol_client.rs`
- `windows/rust-cli/src/network/gateway_connection.rs`
- `windows/rust-cli/src/network/connection_authenticator.rs`
- `windows/rust-cli/src/network/session_opener.rs`
- `windows/rust-cli/src/network/disk_session.rs`
- `windows/rust-cli/src/network/network_media.rs`
- `windows/rust-cli/src/network/mod.rs`
- `windows/rust-cli/src/cli/host.rs`

## 3. 必须遵守的重构原则

- Rust CLI 必须直接切到新协议，不做旧协议兼容桥。
- `GatewayConnection` 只做 connection 级职责，不再冒充 session 真源。
- `DiskSession` 和 `NetworkMedia` 要分清：session 是连接上的远端会话句柄，metadata 属于 `NetworkMedia` 的可见盘视图。
- `NetworkMedia` 显式记录 `disk_id + DiskSession + metadata`。
- 失效处理遵守正式文档边界，但当前阶段 `windows/rust-cli` 的宿主策略固定为：收到失效事件后立即卸载并清理，不实现假死挂起态。
- 测试必须跟着新语义重写，不能继续用旧 `SessionOpenResponse` 和 `Ping` 断言兜底。

## 4. 目标结构

### 4.1 protocol_client

- 只负责 client-facing 协议编解码。
- `SessionOpenRequest` 输入改为 `auth_id`。
- `SessionOpenResponse` 只返回 `session_id`。
- 新增 `SessionDescribeRequest / SessionDescribeResponse`。
- 新增正式 `FLAG_NOTICE` 和 notice 头部校验。
- `SessionCloseNotice` 明确按 notice 处理。
- 删掉 session-scoped `Ping` 旧语义。

### 4.2 GatewayConnection

- 只表示一条可复用的 gateway connection。
- 只负责：
  - transport 生命周期
  - request_id 分配
  - pending request 配对
  - notice 分发
  - auth/open in-flight 互斥
- 不再负责：
  - 保存单一 authorized disk
  - 保存单一 open session
  - 要求后续读写必须匹配“当前唯一 session”

### 4.3 ConnectionAuthenticator

- `authenticate(claim_code)` 的正式结果应是 `auth_id`，不是 `disk_id`。
- 它只负责：
  - `AuthStart`
  - 本地 proof 计算
  - `AuthFinish`
  - 返回可供 `SessionOpen` 消费的授权对象
- 不负责：
  - 直接进入 session
  - metadata 查询

### 4.4 SessionOpener

- `SessionOpen(auth_id)` 只负责把授权对象变成 `session_id`。
- metadata 获取必须拆成后续独立步骤。
- 可以有一个更上层的 orchestration helper，把：
  - `SessionOpen(auth_id)`
  - `SessionDescribe(session_id)`
  - `DiskSession + NetworkMedia` 构造
  串起来，但不能再把 metadata 回到 `SessionOpenResponse` 里。

### 4.5 DiskSession

- 只表示已打开 session 的最小句柄。
- 它应该显式持有：
  - `Arc<GatewayConnection>`
  - `session_id`
  - 失效/终态标记
- 它不应该继续承担：
  - metadata 真源
  - session TTL 对外语义
  - session-scoped keepalive 主循环

### 4.6 NetworkMedia

- 显式持有：
  - `disk_id`
  - `DiskSession`
  - `disk_size_bytes`
  - `read_only`
  - `max_io_bytes`
- 不承担：
  - 认证
  - 建连
  - 自动重连
  - 自动重新开会话
- 失效时只暴露给宿主一个明确的无效状态入口，不伪装成功。
- 当前阶段一旦进入失效状态，宿主立即卸载并清理对应网络盘，不保留挂起对象。

### 4.7 CliHost

- `mount_network_disk()` 主链改成：
  - connect
  - auth -> `auth_id`
  - open -> `session_id`
  - describe -> metadata
  - build `DiskSession`
  - build `NetworkMedia(disk_id + session + metadata)`
  - mount
- 故障策略固定为：
  - `connection` 断开时立即卸载并清理
  - 收到 `SessionCloseNotice` 时立即卸载并清理
  - 读写返回 `session-unavailable` 等终态错误时立即卸载并清理
  - 不实现待回收状态，不实现严格假死态

## 5. 实施顺序

### Phase 0. 对齐 server wire 冻结点

Rust CLI 不能先拍脑袋改协议，必须以 `todo-server.md` 里 `server/internal/proto` 的收口结果为准。

开始 Rust CLI 重构前，至少要确认：

- `SessionOpen` 的正式 wire 已经改成 `auth_id -> session_id`
- `SessionDescribe` 已经有正式 wire
- `SessionCloseNotice` 的 notice 头部规则已经固定

### Phase 1. 重写 protocol_client

必须完成：

- 新增 `FLAG_NOTICE`
- `ClientOperationCode` 增加 `SessionDescribe`
- `SessionOpenRequest` 从 `disk_id` 改为 `auth_id`
- `SessionOpenResponse` 从“带 metadata 的响应体”改成“只读 header 里的 session_id”
- 新增 `SessionDescribeResponse`
- 删除 `session_ttl_seconds` 对外字段
- 删除 session-scoped `Ping` 的 client-facing 编解码

相关文件：

- `windows/rust-cli/src/network/protocol_client.rs`
- `windows/rust-cli/src/network/mod.rs`

验收标准：

- Rust 侧编解码与正式 server wire 一致。
- `SessionOpenResponse` 不再解析 metadata。
- `SessionDescribe` 的 body 结构与正式文档一致。

### Phase 2. 重写 GatewayConnection

必须完成：

- 去掉 `ConnectionPhase::Authorized` 和 `ConnectionPhase::SessionOpen` 这种单会话模型。
- 保留 connection 级 auth/open in-flight 互斥，但不把 active session 塞进 phase 里。
- active session 的生命周期由 `DiskSession` / 宿主对象自己持有，不再由 `GatewayConnection` 充当真源。
- notice 收到后只做事件分发，不直接替宿主删对象。

相关文件：

- `windows/rust-cli/src/network/gateway_connection.rs`

验收标准：

- 一个 `GatewayConnection` 可以承载多个活跃 `DiskSession`。
- `GatewayConnection` 不再要求“当前只有一个 session_id 可用”。
- `SessionCloseNotice` 只作为失效事件流出。

### Phase 3. 重写 auth/open/describe 主链

必须完成：

- `ConnectionAuthenticator::authenticate()` 返回 `auth_id`，不再返回 `disk_id`
- `SessionOpener::open(auth_id)` 只得到 `session_id`
- 新增 metadata 查询器，负责 `SessionDescribe(session_id)`
- 定义清晰的 open pipeline，明确哪一层拿到：
  - `disk_id`
  - `auth_id`
  - `session_id`
  - metadata

相关文件：

- `windows/rust-cli/src/network/connection_authenticator.rs`
- `windows/rust-cli/src/network/session_opener.rs`
- 可新增新的 `session_describer.rs` 或 `opened_session.rs`

验收标准：

- Rust CLI 不再依赖 `SessionOpenResponse` 直接构造完整 session 对象。
- `AuthFinish -> auth_id -> SessionOpen -> SessionDescribe` 的顺序固定下来。

### Phase 4. 重写 DiskSession 和 NetworkMedia

必须完成：

- `DiskSession` 去掉 `session_ttl_seconds` 和 keepalive 线程。
- `DiskSession` 不再自己维护 session heartbeat。
- `NetworkMedia` 增加 `disk_id` 字段。
- `NetworkMedia::bind()` 改成基于 `disk_id + DiskSession + metadata` 构造。
- `read_locked / write_locked` 继续承担按 `max_io_bytes` 拆片。
- 失效后：
  - 不返回伪成功
  - 不伪装 I/O 已完成
  - 不静默自动重连

相关文件：

- `windows/rust-cli/src/network/disk_session.rs`
- `windows/rust-cli/src/network/network_media.rs`

验收标准：

- metadata 从 `DiskSession` 中退出，成为 `NetworkMedia` 的显式输入。
- `NetworkMedia` 显式记录 `disk_id`。
- Rust CLI 侧不再有 session-scoped keepalive 模型。

### Phase 5. 重写 CliHost 挂载/回收链

必须完成：

- `CliHost::mount_network_disk()` 改成新主链：
  - connect
  - auth -> `auth_id`
  - open -> `session_id`
  - describe -> metadata
  - `NetworkMedia::bind(disk_id, session, metadata)`
- `pending_closed_sessions` 和 `mounted_network_disks` 的回收路径收紧到正式失效边界。
- notice / connection 死亡时，宿主层统一执行立即卸载并清理。
- 当前阶段固定覆盖以下场景：
  - transport 连接断开
  - `SessionCloseNotice`
  - `SessionDescribe / ReadAt / WriteAt / Close` 返回 `session-unavailable`
  - 其他已确定无法继续推进 I/O 的终态错误

相关文件：

- `windows/rust-cli/src/cli/host.rs`

验收标准：

- host 侧不再假设 open 响应自带 metadata。
- host 侧的盘对象构造严格基于 `disk_id + session + metadata`。
- host 侧对故障盘不保留挂起态，直接卸载并清理。

### Phase 6. 重写 Rust CLI 测试

必须重写的重点：

- `protocol_client.rs` 里的 `SessionOpenResponse` 测试
- `gateway_connection.rs` 里的单 session phase 测试
- `session_opener.rs` 里的 old open flow 测试
- `disk_session.rs` 里的 keepalive/ping 相关测试
- `network_media.rs` 里基于旧 `DiskSession` metadata 的测试

新的必测场景：

- `AuthFinish` 返回 `auth_id`
- `SessionOpen(auth_id)` 只返回 `session_id`
- `SessionDescribe(session_id)` 返回 metadata
- 一个 connection 下可持有多个 session
- `SessionCloseNotice` 只作为失效事件，而不是直接伪造 cleanup
- `NetworkMedia` 显式持有 `disk_id`
- `connection` 死亡、`SessionCloseNotice`、`session-unavailable` 等故障场景下，宿主立即卸载并清理

## 6. 推荐执行顺序

1. 先等 `server/internal/proto` 收完。
2. 先改 `protocol_client.rs`，固定 Rust 侧 wire。
3. 再改 `GatewayConnection`，拆掉单 session 状态机。
4. 再改 `ConnectionAuthenticator / SessionOpener / SessionDescribe` 主链。
5. 再改 `DiskSession / NetworkMedia`。
6. 最后改 `CliHost` 和 Rust 测试。

## 7. 完成标准

这轮 `windows/rust-cli` 重构完成时，至少要满足：

- `ConnectionAuthenticator` 返回 `auth_id`。
- `SessionOpener` 不再消费 `disk_id`，而是消费 `auth_id`。
- `SessionOpen` 和 `SessionDescribe` 已经拆成两步。
- `GatewayConnection` 不再限制一个 connection 只能持有一个 session。
- `DiskSession` 不再承担 session metadata 和 keepalive TTL 模型。
- `NetworkMedia` 显式持有 `disk_id + DiskSession + metadata`。
- 当前阶段 notice / connection 死亡 / session 终态错误统一直接卸载并清理。
- `cargo test` 在 `windows/rust-cli` 下通过。
