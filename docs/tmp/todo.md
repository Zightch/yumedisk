# 当前待办总表

## 1. 范围

本轮待办只包含以下收口项：

- `HelloRequest / HelloResponse(server_capabilities)` bootstrap 落地
- `ConnHeartbeat` 与 `LinkHeartbeat` 的唯一化
- `whole / storer` 双角色模型按正式文档重建
- `server` 与 `windows/rust-cli` 同步收口

## 2. 当前统一口径

本轮实现必须直接对齐以下正式文档结论：

- `HelloResponse` 携带 `server_capabilities`，当前第一版负载固定为空
- 当前全系统只允许两个心跳方向：
  - `client(connection) -> gateway : ConnHeartbeat`
  - `gateway -> storer : LinkHeartbeat`
- `whole` 对 client 完整表现为 gateway
- `whole` 不暴露 storer listener
- `whole.route_registry` 中只保留本地唯一 `disk_id`
- 同一 connection 上只限制 auth/open 的 in-flight 过程互斥；多个已签发 `auth_id` 与多个已打开 session 可以并存
- 网络文档只定义失效事件；当前宿主的“立即卸载并清理”策略只写入 Rust CLI todo，不反向污染网络文档

## 3. 拆分

- `server` 侧详细任务见 [todo-server.md](todo-server.md)
- `windows/rust-cli` 侧详细任务见 [todo-rust-cli.md](todo-rust-cli.md)

## 4. 全局约束

- 按开发原则重建，不做补丁和历史兼容
- 不长期并存新旧 bootstrap / 心跳 / whole 语义
- rust-cli 的 connection 复用边界是 gateway endpoint，不是 `disk_id`
- `ConnHeartbeat` 只属于 `client-gateway` connection
- `LinkHeartbeat` 只属于 `gateway-storer` route
- 不再保留 session-scoped heartbeat

## 5. 整体验收

本轮完成后至少满足：

1. `server` 的 `go test ./...` 通过。
2. `windows/rust-cli` 的 `cargo test` 通过。
3. `whole` 与独立 `gateway + storer` 对 client 暴露同一条正式主链。
4. 全树不再保留额外心跳分叉。
