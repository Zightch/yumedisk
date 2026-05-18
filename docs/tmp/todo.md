# 当前总表

## 1. 当前进度

### 1.1 已完成

- `server`
  - 已完成正式 wire 收口：
    - `AuthFinish -> auth_id`
    - `SessionOpen(auth_id) -> session_id`
    - `SessionDescribe(session_id) -> metadata`
    - `SessionCloseNotice` notice 头部语义
  - 已完成 gateway/storer 最小主链重建。
  - 已删除旧 `SessionOpen(disk_id) -> metadata` 和 session-scoped `Ping` 的正式语义。
  - `go test ./...` 已通过。

- `windows/rust-cli`
  - 已完成协议 wire 跟进。
  - 已完成 `auth_id -> SessionOpen -> SessionDescribe` 消费主链。
  - `NetworkMedia` 已显式记录 `disk_id`。
  - 当前阶段失效策略已经固定为立即卸载并清理。
  - `cargo test` 之前阶段已通过。

### 1.2 当前判断

现在已经不需要再做“单边先改、另一边以后再追”的工作了。  
下一阶段应该直接按同一口径推进 `server + windows/rust-cli` 的同步重构，但重点不再是 wire，而是内部结构、状态真源和失效链路。

## 2. 剩余工作

### 2.1 第一优先级：收 `windows/rust-cli` 的 connection / session 结构

这是下一阶段最优先的工作，也是和当前 `server` 口径对齐的关键点。

1. 重写 `GatewayConnection` 的状态模型
   - 去掉 `Authorized`、`SessionOpenPending` 这种“单授权 + 单 open 流”的旧状态真源。
   - 保留 connection 级 `auth/open` in-flight 互斥，但不要再把 active session 塞进 phase。
   - 允许一条 connection 下同时持有多个 `DiskSession`。
   - `SessionCloseNotice` 和 connection 死亡只做事件分发，不在网络层里隐式替宿主删对象。

2. 收束 `DiskSession`
   - 只保留 `GatewayConnection + session_id + invalid/terminal state`。
   - 不再承担 metadata 真源。
   - 不再承担旧 keepalive/session heartbeat 模型。

3. 收束 `CliHost`
   - 把 mount 主链固定为：
     - connect
     - auth -> `auth_id`
     - open -> `session_id`
     - describe -> metadata
     - bind `NetworkMedia(disk_id + session + metadata)`
   - 把故障链统一成一个宿主回收入口：
     - connection 断开
     - `SessionCloseNotice`
     - `session-unavailable`
     - 其他明确不能继续 I/O 的终态错误
   - 当前阶段保持“立即卸载并清理”，不实现挂起态。

4. 重写 Rust CLI 测试
   - 删掉仍然依赖旧 `GatewayConnection` phase 的测试辅助路径。
   - 新增一条 connection 多 session 的测试。
   - 新增 notice / disconnect / session terminal error 的立即卸载测试。
   - 最终验收为 `cargo test`。

详细任务见 [todo-rust-cli.md](./todo-rust-cli.md)。

### 2.2 第二优先级：继续收 `server` 的内部结构

`server` 的正式协议和最小主链已经收通，但离“按开发原则完成重建”还差内部职责重构。

1. 拆 `server/internal/gateway/storer_routes.go`
   - 至少拆成：
     - route 真源
     - storer connection runtime
     - round-trip / register client
   - 目标是把 route 注册、连接管理、心跳、数据面转发从一个大文件里拆开。

2. 收束 session 内部模型
   - `session.Descriptor` 现在仍然揉着：
     - disk metadata
     - TTL / ExpiresAt
     - connection ownership
   - 后续要拆成：
     - storer 内部运行记录
     - gateway/session-facing metadata snapshot

3. 收束 `whole`
   - `whole` 虽然已经跟上新协议，但仍然是特殊装配路径。
   - 后续要把它进一步压成“纯装配形态”，不要再保留特殊语义分叉。

4. 继续补齐 server 侧测试结构
   - 当前测试已经跟上新协议，但后续随着结构拆分，需要把测试也按新职责拆开。
   - 不要让 integration test 重新变成替结构问题兜底的地方。

详细任务见 [todo-server.md](./todo-server.md)。

### 2.3 第三优先级：做两侧同步收口

等 `windows/rust-cli` 结构和 `server` 内部结构都到位后，再做这一段：

1. 对齐 `SessionCloseNotice` 失效链
   - server 发 notice
   - rust-cli 网络层上报
   - host 立即卸载并清理

2. 对齐 busy / retry 语义
   - `SessionOpen` busy 时 `auth_id` 可重试
   - Rust CLI 不要误消费或误清理授权对象

3. 对齐一条 connection 下多 session 的实际行为
   - server 不限制单 session
   - rust-cli 不再以 phase 隐式限制单 session

4. 做最后的测试收口
   - `server`: `go test ./...`
   - `windows/rust-cli`: `cargo test`
   - 现阶段先不要求最后程序联调，但代码和测试口径必须一致

## 3. 推荐执行顺序

1. 先做 `windows/rust-cli` 的 `GatewayConnection / DiskSession / CliHost` 收口。
2. 再根据新 client 侧落点，继续拆 `server` 的 `storer_routes / session / whole` 结构。
3. 最后统一收 notice、multi-session、busy retry 和测试。

## 4. 当前下一步

下一步直接进入：

1. `windows/rust-cli` 的 `GatewayConnection` 重构
2. `CliHost` 的 mount / invalidation / cleanup 收口
3. 对应 Rust 测试重写

这是当前最应该先做的一段。
