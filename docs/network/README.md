# Network Protocol

## 定位

`docs/network` 只定义网络协议本身：

- bootstrap 与 transport 边界
- wire header / body / 字段语义
- 消息方向、时序与失效事实
- `connection / request_id / auth_id / session_id / disk_id` 的协议含义

这里不定义：

- rust-cli 的对象结构和清理策略
- gateway 程序或 storer 程序的内部表结构
- `whole` 的部署方式
- 当前项目的 reject / cleanup / watchdog 具体实现策略

这些内容分别落在：

- [Client 实现文档](../client/README.md)
- [Server 实现文档](../server/README.md)

## 文档索引

- [transport](transport.md)
- [client-gateway](client-gateway.md)
- [gateway-storer](gateway-storer.md)
- [auth-routing](auth-routing.md)
- [data-plane](data-plane.md)

## 协议主模型

正式主链固定为：

```text
client <-> gateway <-> storer
```

协议层只承认三类业务对象：

- `connection`
  - 由一条完成 bootstrap 的 transport 连接隐式表示
- `auth_id`
  - `client-gateway` 边界上的显式授权对象
- `session_id`
  - 已打开会话的显式标识

`disk_id` 不是 transport 头字段：

- 在 `client-gateway` 上，它出现在 `AuthStart` 请求体中
- 在 `gateway-storer` 上，它出现在 `StorerRegister` 请求体中

`auth_id` 不是通用头字段：

- 它只出现在 `SessionOpen` 请求体中

`session_id` 是通用头字段：

- 只对已打开 session 的命令生效

## 主链与并发约束

在 `client-gateway` 边界，正式链路为：

```text
TCP
  -> Hello
  -> transport
  -> AuthStart / AuthFinish
  -> auth_id
  -> SessionOpen(auth_id)
  -> session_id
  -> SessionDescribe(session_id)
  -> ReadAt / WriteAt / Close
```

固定约束如下：

- 一条 connection 同时最多一个 auth 过程在飞
- 一条 connection 同时最多一个 `SessionOpen` 过程在飞
- auth 过程与 `SessionOpen` 过程互斥
- 同一条 connection 可以同时持有多个已签发 `auth_id`
- 同一条 connection 可以同时持有多个已打开 session
- 已打开 session 上的 `SessionDescribe / ReadAt / WriteAt / Close` 允许并发
- 已打开 session 的并发存在，不阻止该 connection 后续再次串行发起 auth/open

## 元数据边界

协议层固定收口如下：

- `SessionOpen` 只负责打开会话
- `SessionOpen` 不返回 metadata 语义
- `SessionDescribe(session_id)` 单独返回 session 绑定 metadata

`SessionOpen` 成功时的最小协议结果为：

- 状态 `OK`
- 响应头中的 `session_id`

成功响应体可以携带附加负载，但其业务语义不由 `docs/network` 定义。

## 心跳与失效事实

当前正式只保留两个心跳方向：

- `client(connection) -> gateway : ConnHeartbeat`
- `gateway -> storer : LinkHeartbeat`

协议层只定义以下失效事实：

- client-gateway connection 死亡时，该 connection 下 `auth_id` 与 session 一起失效
- route connection 死亡时，该 route 下 session 一起失效
- `SessionCloseNotice` 一旦到达，目标 session 已经失效

至于：

- client 是否立即卸载盘
- connection 是否立刻关闭
- gateway 如何清理内部表
- storer watchdog 超时后是否退出

都由具体实现文档定义，不在本目录收口。
