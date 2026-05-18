# windows/rust-cli 重构待办

## 1. 目标

`windows/rust-cli` 侧本轮要完成以下收口：

- 连接前置 `HelloRequest / HelloResponse(server_capabilities)` bootstrap
- `client(connection) -> gateway : ConnHeartbeat`
- 与 `whole` 的 client-facing 完整兼容
- 当前宿主策略继续固定为立即卸载并清理

## 2. Phase R1: bootstrap

需要完成：

- 在 transport 启动前增加 `HelloRequest / HelloResponse`。
- 解析 `HelloResponse.server_capabilities`。
- 当前第一版按空能力负载处理。
- `Hello` 成功前不启动 transport runtime。
- 为后续 TLS 保留 `Hello` 之后、transport 之前的位置，但当前不启用 TLS。

验收标准：

- Rust CLI 连 `gateway` 与连 `whole` 时都先走 `Hello`。
- `Hello` 失败会终止连接建立流程。

## 3. Phase R2: `ConnHeartbeat`

需要完成：

- 明确 `ConnHeartbeat` 是唯一 client-facing heartbeat。
- 方向固定为 `client(connection) -> gateway`。
- heartbeat loop 属于 connection，而不是 session / media。
- timeout 后统一把 connection 收束为死亡，并触发 session 失效传播。
- 清理或删除残留的 session-scoped heartbeat 假设。

验收标准：

- `DiskSession` 与 `NetworkMedia` 不自行维护 heartbeat。
- 全树不再存在额外 client-facing heartbeat 分叉。

## 4. Phase R3: host cleanup 策略

当前阶段必须固定以下宿主策略：

- 收到 `SessionCloseNotice`：立即卸载并清理
- 观察到 connection 死亡：立即卸载并清理
- 观察到 terminal session 错误：立即卸载并清理

需要完成：

- 让 `Hello` 失败、heartbeat timeout、notice、disconnect、terminal session error 全部汇入同一 cleanup 入口。
- 保持网络文档层面只定义失效事件，不把当前策略反写回正式网络协议文档。

## 5. Phase R4: 测试

至少补齐：

- `Hello` 正常路径与失败路径测试
- `ConnHeartbeat` 正常路径与 timeout 测试
- connection 死亡后的 session 失效与 cleanup 测试
- 对 `whole` 作为 client-facing endpoint 的兼容测试

最终验收：

- `cargo test` 通过
