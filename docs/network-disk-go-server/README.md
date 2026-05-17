# 网络盘 Go Server

当前正式文档统一描述 `client <-> gateway <-> storer` 的最小闭环协议与结构，不再保留旧的隐式认证资格、`SessionOpen` 直接回元数据、session 自己做 client 心跳等旧口径。

## 文档索引

- [总览](overview.md)
- [传输层协议](transport.md)
- [Client-and-Gateway 业务层协议](client-and-gateway.md)
- [Gateway-and-Storer 业务层协议](gateway-and-storer.md)
- [认证与路由](auth-routing.md)
- [数据面最小闭环](data-plane.md)

## 当前主路径

唯一主路径固定为：

```text
client <-> gateway <-> storer
```

当前部署形态固定为：

- `gateway`
  - 独立进程
  - 对 client 提供入口
  - 对 storer 提供注册与长连入口
- `storer`
  - `role = storer` 时主动长连 `gateway`
  - `role = whole` 时内嵌 `gateway`

第一版不做：

- `client -> storer` 直连
- 分片
- 多副本
- locator
- 自动重连
- 自动恢复

## 当前正式模型

当前正式模型固定拆成五层：

1. `bootstrap`
2. `connection`
3. `auth-grant`
4. `session`
5. `metadata + data plane`

对应主链如下：

```text
TCP connected
  -> Hello
  -> optional TLS
  -> transport ready
  -> connection established
  -> AuthStart/AuthFinish -> auth_id
  -> SessionOpen(auth_id) -> session_id
  -> SessionDescribe(session_id) -> metadata
  -> ReadAt / WriteAt / Close
```

## 当前明确口径

1. `Hello` 位于 transport 之前。
2. 如果启用 TLS，则 TLS 握手发生在 `Hello` 之后、transport 之前。
3. transport 只负责 framed payload，不负责业务语义。
4. `GatewayConnection` 只代表一条可复用业务连接，不代表某个盘或某个 session。
5. 认证成功后必须返回显式 `auth_id`。
6. `auth_id` 只表示“可以申请打开某个盘会话”的资格。
7. `SessionOpen` 只负责打开会话，不返回盘元数据。
8. `SessionDescribe` 单独返回 `session` 绑定的盘元数据。
9. client 心跳上移到 `connection` 级，不再属于 `DiskSession`。
10. `gateway <-> storer` 维持逐 route connection 的链路心跳。
11. route 断开时，gateway 必须定向关闭对应 client session。
12. `NetworkMedia` 只绑定一个已打开 session，不负责认证、建连、心跳与重连。
13. 一个 connection 同时最多只允许一个 auth 过程。
14. 一个 connection 同时最多只允许一个 `SessionOpen` 过程。
15. auth 过程与 `SessionOpen` 过程互斥，不能同时存在。
16. 已打开 session 上的数据面请求允许并发复用同一条 connection。

## 当前文档使用原则

这组文档当前只描述正式目标结构，不继续为旧实现保留双轨兼容叙述。

如果代码仍未完全跟上文档，应按文档收敛实现，而不是回头在文档中继续保留历史中间态。
