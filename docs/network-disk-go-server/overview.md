# 总览

## 1. 文档定位

本文档描述当前 `network-disk-go-server` 的正式重建结构。

当前口径遵守以下开发原则：

- 先建立最小闭环，不为未来扩展预铺复杂度
- 收敛到唯一新结构，不保留历史兼容桥
- 所有关键状态只允许一个真实来源
- 约束在边界入口集中收口

因此本文档只描述当前唯一正确的结构，不继续保留旧的“认证隐式挂在 connection 上”“`SessionOpen` 顺带返回 metadata”“网络层替宿主决定盘对象清理策略”之类历史中间态。

## 2. 唯一主路径

当前正式主路径固定为：

```text
client <-> gateway <-> storer
```

角色边界固定如下：

- `client`
  - 只连接 `gateway`
  - 只理解 client-facing 控制面、session metadata 和共享数据面
- `gateway`
  - 是唯一控制中枢
  - 持有 `route / auth grant / session` 三张真源表
  - 负责认证、授权、会话建立、metadata 查询、故障传播和数据面转发
- `storer`
  - 一个进程只承载一个 `disk_id`
  - 只负责本地介质、本地 session 和 I/O 执行
  - 只连接 `gateway`
- `whole`
  - `role = whole`
  - 对 client 完整表现为 gateway
  - 不对其他 `storer` 暴露 storer listener
  - 内嵌 gateway 固定路由本地唯一 `disk_id`

## 3. 正式分层

当前正式模型固定拆成四层：

```text
client bootstrap
  -> gateway control core
    -> storer route control plane
      -> session metadata + shared data plane
```

### 3.1 client bootstrap

只存在于 `client -> gateway` 边界，负责：

- 建立 TCP
- `HelloRequest / HelloResponse(server_capabilities)`
- 在 `Hello` 之后预留 TLS 位置
- 启动 transport

当前第一版固定为：

- `HelloRequest` 当前负载为空
- `HelloResponse` 当前 `server_capabilities` 负载为空
- 因当前能力负载为空，`Hello` 完成后直接进入 transport

不负责：

- 认证材料计算
- `auth_id`
- `session_id`
- metadata 读取
- 数据面语义

### 3.2 gateway control core

是当前结构的中心层，负责：

- client connection runtime
- `AuthStart / AuthFinish`
- `auth_id` 生命周期
- `SessionOpen(auth_id)`
- `SessionDescribe(session_id)`
- route 选择
- session 映射
- 故障传播

### 3.3 storer route control plane

是 `gateway <-> storer` 私有控制面，负责：

- `StorerRegister`
- route connection 存活
- gateway 向 storer 发起内部 `SessionOpen`

它不负责：

- client 认证
- `auth_id`
- client-facing metadata 查询
- client-facing notice

### 3.4 session metadata + shared data plane

当前第一版只保留：

- `SessionDescribe`
- `ReadAt`
- `WriteAt`
- `Close`

其中：

- `client-gateway` 额外有 `ConnHeartbeat` 和 `SessionCloseNotice`
- `gateway-storer` 额外有 `LinkHeartbeat`

## 4. 三方内部结构

### 4.1 client

当前正式 client 结构建议理解为：

```text
UI
  -> ClientState
    -> GatewayConnection
      -> ConnectionAuthenticator
      -> SessionOpener
      -> DiskSession[*]
        -> NetworkMedia
```

关键约束：

- `GatewayConnection` 只表示一条可复用业务连接
- `ConnectionAuthenticator` 只负责拿到 `auth_id`
- `SessionOpener` 只负责把 `auth_id` 变成 `session_id`
- `DiskSession` 只表示已打开 session
- `SessionDescribe` 负责在已打开 session 上读取 metadata
- `NetworkMedia` 显式记录 `disk_id + session + metadata`

### 4.2 gateway

当前正式 gateway 结构建议固定为：

```text
gateway runtime
  -> client connection runtime
  -> route registry
  -> auth grant registry
  -> session registry
  -> route connection runtime
```

其中：

- client connection runtime 只负责收发、请求配对、notice 下发和建会话前过程互斥
- registry 层只负责状态归属，不混入网络收发
- route connection runtime 只负责对 storer 长连、注册、喂狗和内部 round-trip

### 4.3 storer

当前正式 storer 结构建议固定为：

```text
storer runtime
  -> local media host
  -> local session service
  -> gateway route adapter
```

其中：

- local media host 持有真实介质
- local session service 维护本地真实 session
- gateway route adapter 只负责注册、心跳和响应 gateway 请求

### 4.4 whole

当前正式 `whole` 结构建议固定为：

```text
whole runtime
  -> client listener
  -> client bootstrap
  -> transport runtime
  -> embedded gateway core
  -> local fixed route(self disk_id)
  -> local storer core
```

其中：

- client-facing 主链与独立 `gateway` 完全一致
- 不启动外部 storer listener
- 不通过外部 `StorerRegister` 网络注册自己
- gateway 启动后在本地写入唯一一条 fixed route，且只对应自己的 `disk_id`

## 5. 三张真源表

### 5.1 route_registry

`gateway` 路由表是 route 真源，至少保存：

- `disk_id`
- `route_connection_id`
- `auth_verifier`
- `disk_size_bytes`
- `read_only`
- `max_io_bytes`
- `route_state`

当前正式约束：

- 一个 `storer` 进程只承载一个 `disk_id`
- 一个 route connection 只绑定一个 `disk_id`
- 同一时刻一个 `disk_id` 只允许一个活跃 route
- route metadata 在当前 route 生命周期内视为不可变

### 5.2 auth_grant_registry

`gateway` 授权表是 `auth_id` 真源，至少保存：

- `auth_id`
- `client_connection_id`
- `disk_id`
- `issued_at`
- `expire_at`
- `grant_state`

当前正式约束：

- `auth_id` 只存在于 `client-gateway`
- 一个 `auth_id` 只对应一个 `disk_id`
- 一个 `auth_id` 只能消费一次
- 一个 connection 同时最多只保留一个未消费 `auth_id`

### 5.3 session_registry

`gateway` 会话表是 client-visible session 真源，至少保存：

- `gateway_session_id`
- `client_connection_id`
- `route_connection_id`
- `storer_session_id`
- `disk_id`
- `disk_size_bytes`
- `read_only`
- `max_io_bytes`
- `session_state`

这里的 metadata 是 `SessionOpen` 成功后从 route 表复制进来的 session 快照。

它的职责不是维护“当前 route 的最新盘状态”，而是维护“这个已打开 session 对 client 暴露的稳定视图”。

## 6. 建会话前的正式互斥规则

`GatewayConnection` 当前只承接两类短生命周期过程：

1. auth 过程：`AuthStart -> AuthFinish`
2. open 过程：`SessionOpen(auth_id)`

固定约束：

- 一个 connection 同时最多只有一个 auth 过程在飞
- 一个 connection 同时最多只有一个 open 过程在飞
- auth 过程与 open 过程互斥
- 上述互斥只约束建会话前阶段
- 已打开 session 上的 `SessionDescribe / ReadAt / WriteAt / Close` 不受该互斥影响
- 一个 connection 可以在已持有多个活跃 session 的同时，再继续串行发起新的 auth/open

换句话说，`auth_id` 是短生命周期授权对象，不是 connection 的长期“authorized 状态”。

## 7. metadata 收口

当前 metadata 口径固定为：

- route 级基础 metadata 的真源是 `route_registry`
- `SessionOpen` 成功时，gateway 把当前 route metadata 复制为 session 快照
- `SessionOpen` 成功响应只返回 `session_id`
- client 后续通过 `SessionDescribe(session_id)` 读取这份 session 快照

当前第一版只有以下最小字段：

- `disk_size_bytes`
- `read_only`
- `max_io_bytes`

## 8. bootstrap 到数据面的完整主链

client 侧正式主链：

```text
TCP connected
  -> HelloRequest
  -> HelloResponse(server_capabilities = empty)
  -> transport ready
  -> AuthStart(disk_id)
  -> AuthFinish(challenge_token, proof)
  -> auth_id
  -> SessionOpen(auth_id)
  -> gateway_session_id
  -> SessionDescribe(gateway_session_id)
  -> metadata
  -> ReadAt / WriteAt / Close
```

storer 侧正式主链：

```text
TCP connected
  -> transport ready
  -> StorerRegister(gateway_token, disk_id, auth_verifier, metadata)
  -> route active
  -> SessionOpen()
  -> storer_session_id
  -> ReadAt / WriteAt / Close
```

whole 侧正式主链：

```text
TCP connected
  -> HelloRequest
  -> HelloResponse(server_capabilities = empty)
  -> transport ready
  -> AuthStart(disk_id)
  -> AuthFinish(challenge_token, proof)
  -> auth_id
  -> SessionOpen(auth_id)
  -> local fixed route(self disk_id)
  -> SessionDescribe(gateway_session_id)
  -> ReadAt / WriteAt / Close
```

## 9. 故障传播总原则

### 9.1 client connection 死亡

等价于：

- 该 connection 下全部未消费 `auth_id` 失效
- 该 connection 下全部活跃 session 失效
- 该 connection 下 pending request 全部失败

网络层边界固定为：

- 网络层只上报“connection / session 已失效”
- 网络层只调用 `NetworkMedia` 的失效处理接口
- 是否立即清理盘对象，还是保留为严格假死挂起态，由 client / 宿主策略决定

### 9.2 route connection 死亡

等价于：

- 该 route 失效
- 该 route 下全部未消费 `auth_id` 撤销
- 该 route 下全部 session 关闭
- gateway 对仍在线 client 下发 `SessionCloseNotice`

client 侧接到这类确定性失效后，网络层仍然只上报 session 失效给 `NetworkMedia` 接口；宿主是否马上删除对象，不由协议层强制规定。

### 9.3 认证失败

只影响本次 auth 过程，不关闭 connection。

### 9.4 open 失败

只影响本次 `SessionOpen`，不关闭 connection。

其中：

- 若失败原因是 `busy`，未过期 `auth_id` 仍可重试
- 若 route 已消失，`auth_id` 直接撤销

### 9.5 假死挂起态约束

如果宿主选择保留 `NetworkMedia` 对象并进入假死挂起态，则必须满足：

- 不主动返回成功
- 不伪装读写已经完成
- 不静默自动重连

## 10. 部署形态

### 10.1 gateway

- 独立进程
- 对 client 提供 client-facing 入口
- 对 storer 提供 storer-facing 长连入口

### 10.2 storer

- `role = storer`
  - 持有真实存储
  - 主动连接 `gateway`
  - 注册单个 `disk_id`

### 10.3 whole

- `role = whole`
  - 同进程内同时装配 `gateway` 和本地 `storer`
  - 对 client 提供与独立 gateway 完全相同的 client-facing 协议
  - 不对其他 `storer` 提供 storer-facing listener
  - `route_registry` 中只保留本地唯一 `disk_id`
  - gateway 对该 `disk_id` 固定路由到本地 storer core

`whole` 是正式角色之一，但不派生第二套协议边界。

## 11. 当前正式约束

1. `gateway` 是唯一控制中枢。
2. `storer` 一个进程只承载一个 `disk_id`。
3. 同一时刻一个 `disk_id` 只允许一个活跃 route。
4. `auth_id` 只存在于 `client-gateway`。
5. `SessionOpen` 仅负责打开会话，成功后只返回 `session_id`。
6. `SessionDescribe(session_id)` 单独返回 session 绑定的 metadata。
7. `gateway-storer` 不接收 client 认证语义，也不承接 client-facing metadata 查询。
8. `NetworkMedia` 显式持有 `disk_id + session + metadata`。
9. 当前正式心跳只有两个方向：`client(connection) -> gateway : ConnHeartbeat`，`gateway -> storer : LinkHeartbeat`。
10. `whole` 对 client 必须走完整 `Hello -> transport -> auth -> session` 主链，但 route 固定为本地唯一 `disk_id`。
11. route 或 connection 失效时，gateway 负责清理授权与会话；网络层只负责向 `NetworkMedia` 上报失效事件，不替宿主决定对象清理策略。
12. 正式文档不保留历史兼容和过渡叙述。
