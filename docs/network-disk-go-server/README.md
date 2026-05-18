# 网络盘 Go Server

本文档描述按开发原则重建后的唯一正式架构，不保留历史兼容、旧一体机中间态及过渡口径。

## 文档索引

- [总览](overview.md)
- [传输层协议](transport.md)
- [Client ↔ Gateway 业务层协议](client-and-gateway.md)
- [Gateway ↔ Storer 业务层协议](gateway-and-storer.md)
- [认证与路由](auth-routing.md)
- [数据面最小闭环](data-plane.md)

## 当前唯一主路径

```text
client <-> gateway <-> storer
```

第一版固定边界：

- `client`
  - 仅连接 `gateway`
  - 不直连 `storer`
- `gateway`
  - 唯一控制中枢
  - 独占 `route / auth grant / session` 三张真源表
- `storer`
  - 单进程只承载一个 `disk_id`
  - 职责限定为本地介质、本地 session 及 I/O 执行
- `whole`
  - `role = whole`
  - 对 client 暴露完整的 gateway client-facing 入口
  - 不对其他 `storer` 暴露 storer-facing 监听口
  - 内嵌 gateway 固定路由本地 storer
  - `route_registry` 中只保留自己的 `disk_id`

第一版不做：

- `client -> storer` 直连
- 单 `storer` 承载多盘
- locator
- 分片
- 多副本
- 自动重连
- 自动恢复
- 历史协议兼容桥

## 当前正式主链

client 侧主链固定为：

```text
TCP connected
  -> HelloRequest
  -> HelloResponse(server_capabilities = empty)
  -> transport ready
  -> AuthStart / AuthFinish -> auth_id
  -> SessionOpen(auth_id) -> session_id
  -> SessionDescribe(session_id) -> metadata
  -> ReadAt / WriteAt / Close
```

storer 侧主链固定为：

```text
TCP connected
  -> transport ready
  -> StorerRegister
  -> route active
  -> SessionOpen
  -> ReadAt / WriteAt / Close
```

## 当前明确口径

1. `client-gateway` 与 `gateway-storer` 是两套独立的控制面，而非同一套协议的镜像转发。
2. `gateway-storer` 在第一版中是私有 route 控制面，不承接 client 认证语义。
3. `auth_id` 仅存在于 `client-gateway` 边界。
4. `gateway` 独占三张真源表：
   - `route_registry`
   - `auth_grant_registry`
   - `session_registry`
5. 同一时刻一个 `disk_id` 只允许一条活跃 route。
6. 同一时刻一条 connection 上至多一个 auth 过程在飞、至多一个 `SessionOpen` 过程在飞，且二者互斥。
7. 上述互斥仅约束建会话前阶段；已打开的 session 在数据面允许并发复用同一条 connection。
8. `SessionOpen` 仅负责打开会话，不返回 metadata。
9. `SessionDescribe` 单独返回 session 绑定的 metadata。
10. 客户端盘对象显式持有 `disk_id + session + metadata`，不承担认证、建连、心跳和重连。
11. 已签发的 `auth_id` 可以在同一 connection 上并存，已打开的 session 也可以并存；只有建会话前的 in-flight lane 受互斥约束。
12. 当前正式心跳只有两个方向：
   - `client(connection) -> gateway : ConnHeartbeat`
   - `gateway -> storer : LinkHeartbeat`
13. `whole` 对 client 仍完整走 `Hello -> transport -> auth -> session -> metadata -> data plane` 主链，只是 route 固定为本地唯一 `disk_id`。
14. session / connection / route 失效后，协议层只定义失效事实，不定义上层对象的销毁或挂起策略。

## 文档使用原则

这组文档仅描述正式目标结构。

若后续结构发生变化，应直接改写正式文档，不在其中保留“当前也可以”“后续再补”“暂时兼容旧行为”等过渡性表述。
