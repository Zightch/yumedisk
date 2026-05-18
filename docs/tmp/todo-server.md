# server 重构待办

## 1. 目标

`server` 侧本轮要完成以下收口：

- client-facing `HelloRequest / HelloResponse(server_capabilities)` bootstrap
- `client(connection) -> gateway : ConnHeartbeat`
- `gateway -> storer : LinkHeartbeat`
- `whole / storer` 双角色模型重建

## 2. Phase S1: client bootstrap

需要完成：

- 在 client-facing TCP 上增加 pre-transport `HelloRequest / HelloResponse`。
- `HelloResponse` 结构中保留 `server_capabilities` 负载。
- 当前第一版固定返回空能力负载。
- `Hello` 完成前拒绝进入 transport 与业务协议。
- 在 `Hello` 之后保留 TLS 位置，但当前能力负载为空时直接进入 transport。

验收标准：

- 未完成 `Hello` 就发送 transport / 业务包会被拒绝。
- `role = gateway` 与 `role = whole` 的 client listener 都走同一套 bootstrap。

## 3. Phase S2: `whole` 角色重建

需要完成：

- `role = whole` 仅启动 client listener，不启动 storer listener。
- `whole` 内嵌 gateway core 与 local storer core。
- gateway 启动时向本地 `route_registry` 写入唯一一条 fixed route。
- 该 fixed route 只对应本地唯一 `disk_id`。
- client 对 `whole` 发起 `AuthStart / AuthFinish / SessionOpen / SessionDescribe / ReadAt / WriteAt / Close` 时，完整走 gateway 侧正式主链。
- `whole` 不再走外部 `StorerRegister` 网络注册自己。

验收标准：

- `whole` 不派生第二套 client-facing 协议。
- `whole` 对 client 的表现与独立 gateway 一致，只是 route 固定为本地。

## 4. Phase S3: `ConnHeartbeat`

需要完成：

- 明确 `ConnHeartbeat` 是唯一 client-facing heartbeat。
- 方向固定为 `client(connection) -> gateway`。
- gateway 只返回 response，不主动发反向 heartbeat。
- heartbeat timeout 等价于 client connection 死亡。
- connection 死亡路径统一清理 pending request、auth grant、session。

验收标准：

- `client-gateway` 不再存在 session-scoped heartbeat。
- 不再存在其他 client-facing heartbeat 分叉。

## 5. Phase S4: `LinkHeartbeat`

需要完成：

- gateway 保持 route connection 级主动 heartbeat。
- heartbeat request 超时未收到 storer response 时，gateway 立即走 route 丢失清理路径。
- `role = storer` 维护本地 watchdog。
- storer 超时未收到 gateway heartbeat 时主动退出。
- route 丢失路径统一撤销 grant、关闭 session、发送 `SessionCloseNotice`。

验收标准：

- `gateway-storer` 不再存在反向 heartbeat。
- `gateway-storer` 不再存在 session-scoped heartbeat。

## 6. Phase S5: 测试

至少补齐：

- `Hello` 正常路径与非法顺序测试
- `whole` 角色 client-facing 主链测试
- `ConnHeartbeat` 正常路径与 timeout 清理测试
- `LinkHeartbeat` 正常路径、gateway timeout、storer watchdog 退出测试

最终验收：

- `go test ./...` 通过
