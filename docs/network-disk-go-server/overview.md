# 总览

## 1. 文档定位

本文档描述当前 `network-disk-go-server` 正式结构，不再沿用旧的“认证资格隐式绑定 connection”“`SessionOpen` 直接返回全部 metadata”“session 自己做 client 心跳”等口径。

目标是先把层次和职责收清，再让各协议文档在同一结构上展开。

## 2. 唯一主路径

当前正式主路径固定为：

```text
client <-> gateway <-> storer
```

角色边界固定如下：

- `client`
  - 只连接 `gateway`
  - 不直接连接 `storer`
- `gateway`
  - 负责 bootstrap 协商后的业务连接
  - 负责认证、授权、会话创建、路由与转发
- `storer`
  - 持有真实存储介质
  - 执行真实会话与块读写

## 3. 正式分层

当前正式模型固定拆成五层：

```text
bootstrap
  -> connection
    -> auth-grant
      -> session
        -> metadata + data plane
```

### 3.1 bootstrap

负责：

- 建立 TCP
- `Hello`
- 可选 TLS

不负责：

- transport
- request/response 配对
- 认证
- 会话

### 3.2 connection

负责：

- transport runtime
- `request_id` 分配
- pending request map
- 响应配对
- notice 分发
- 连接级心跳

不负责：

- proof 计算
- `auth_id` 生命周期
- `session_id` 生命周期
- 元数据缓存

### 3.3 auth-grant

负责：

- challenge 生命周期
- `auth_id` 分配与失效
- `auth_id -> (connection, disk_id, expire_at)` 绑定

### 3.4 session

负责：

- `SessionOpen(auth_id)`
- `session_id` 分配
- `gateway_session_id -> (client_connection, route_connection, storer_session_id, disk_id)` 映射
- 会话关闭与 close notice

### 3.5 metadata + data plane

负责：

- `SessionDescribe(session_id)`
- `ReadAt / WriteAt / Close`
- `NetworkMedia` 与远端 session 的绑定

## 4. client 侧对象层级

当前正式 client 模型建议理解为：

```text
UI
  -> ClientState
    -> DiskCatalogState
      -> DiskRuntimeStore
        -> DiskRuntime
          -> NetworkMedia
    -> GatewayConnection
      -> ConnectionAuthenticator
      -> SessionOpener
      -> DiskSession
```

其中：

- `GatewayConnection`
  - 表示到某个 gateway endpoint 的一条业务连接
  - 是 connection 复用核心
- `ConnectionAuthenticator`
  - 在已有 connection 上执行 `AuthStart / AuthFinish`
  - 成功后获得 `auth_id`
- `SessionOpener`
  - 用 `auth_id` 请求打开真实远端会话
- `DiskSession`
  - 表示已打开的 gateway session
  - 不表示仅已认证状态
- `NetworkMedia`
  - 绑定一个已打开 `DiskSession`
  - 通过该 session 承接远端块读写

## 5. 关键对象边界

### 5.1 GatewayConnection

`GatewayConnection` 的正式职责只有：

- 持有 transport runtime
- 分配 `request_id`
- 管理 pending request
- 接收循环
- 分发响应和 notice
- connection 级 heartbeat
- 连接死亡广播

它不应直接承担：

- 认证成功资格缓存
- 会话元数据缓存
- 盘级业务状态

同时它必须承接一条明确的过程互斥规则：

- 一个 connection 同时最多只有一个 auth 过程在飞
- 一个 connection 同时最多只有一个 `SessionOpen` 过程在飞
- auth 过程与 `SessionOpen` 过程互斥，不能并存
- 已经进入数据面后的 session 读写请求可继续并发复用该 connection

### 5.2 auth_id

`auth_id` 是认证成功后的显式授权对象。

它至少绑定：

- 当前 connection
- 一个 `disk_id`
- 生效时间
- 过期时间

它不是 session，也不是 metadata。

### 5.3 DiskSession

`DiskSession` 只表示已经打开成功的远端会话。

它至少绑定：

- `session_id`
- `disk_id`
- 归属 `GatewayConnection`

metadata 通过 `SessionDescribe` 获取，不在 `SessionOpen` 成功响应里直接塞入。

### 5.4 NetworkMedia

`NetworkMedia` 只绑定一个已打开 session，并保存必要 metadata：

- `disk_size_bytes`
- `read_only`
- `max_io_bytes`

它不负责：

- 认证
- 打开连接
- heartbeat
- 自动重连
- 自动重新认证

## 6. gateway 职责

`gateway` 只负责：

- `Hello` 应答与 bootstrap 能力宣告
- 本地验证 challenge/proof
- 为认证成功生成 `auth_id`
- 用 `auth_id` 打开真实 session
- 维护 session 映射
- 转发数据面请求
- 在 route 故障时向 client 发送 `SessionCloseNotice`

`gateway` 不负责：

- 真实数据存储
- claim code 原文保存
- 本地磁盘缓存
- 自动重建 client session

## 7. storer 职责

`storer` 只负责：

- 持有真实介质
- 主动长连 `gateway`
- 注册 `disk_id` 与认证材料
- 响应会话打开
- 执行真实 `ReadAt / WriteAt / Close`
- 维持 route connection 存活

`storer` 不负责：

- 直接面向 client 的认证协议
- client connection 的管理
- `auth_id` 生成

## 8. route 与 session 的唯一真实来源

当前必须明确单一真实来源。

### 8.1 route

`gateway` 路由表是真实来源，至少保存：

- `disk_id`
- `auth_verifier`
- `route_connection`
- route 存活状态
- 基础元数据缓存

同一时刻，一个 `disk_id` 只能指向一个活跃 route。

### 8.2 auth grant

`gateway` 授权表是真实来源，至少保存：

- `auth_id`
- `connection_id`
- `disk_id`
- `expire_at`
- 当前状态

### 8.3 session

`gateway` 会话表是真实来源，至少保存：

- `gateway_session_id`
- `client_connection_id`
- `route_connection_id`
- `storer_session_id`
- `disk_id`
- 当前状态

## 9. bootstrap 到数据面的完整主链

当前正式主链应理解为：

```text
TCP connected
  -> HelloRequest
  -> HelloResponse
  -> optional TLS handshake
  -> transport ready
  -> connection established
  -> AuthStart(disk_id)
  -> AuthFinish(challenge_token, proof)
  -> auth_id
  -> SessionOpen(auth_id)
  -> session_id
  -> SessionDescribe(session_id)
  -> metadata
  -> ReadAt / WriteAt / Close
```

其中并发约束固定为：

- `AuthStart -> AuthFinish` 这一整段属于一个 auth 过程
- `SessionOpen` 属于一个会话打开过程
- 同一条 connection 上，任意时刻最多存在一个 auth 过程或一个会话打开过程
- 两类过程不能重叠
- 进入已打开 session 后，`ReadAt / WriteAt / Close` 可与其他已打开 session 的数据面请求并发交错

## 10. 故障传播总原则

### 10.1 client-gateway connection 死亡

等价于：

- 该 connection 下全部 `auth_id` 失效
- 该 connection 下全部 session 失效

### 10.2 gateway-storer route 死亡

等价于：

- 该 route 下全部 session 失效
- gateway 定向通知相关 client session 已关闭

### 10.3 认证失败

只影响本次认证流程，不影响 connection 存活。

### 10.4 会话打开失败

只影响本次 `SessionOpen`，不影响 connection 存活。

## 11. 部署形态

### 11.1 gateway

- 独立进程
- 监听 `client.listen_addr`
- 监听 `storer.listen_addr`

### 11.2 storer

- `role = storer`
  - 持有真实存储
  - 主动连接 `gateway.storer.listen_addr`
- `role = whole`
  - 持有真实存储
  - 内嵌 `gateway`
  - 对 client 提供同一套协议

`whole` 只是部署方式不同，不派生第二套协议边界。

## 12. 当前正式约束

1. `Hello` 在 transport 之前。
2. TLS 如果存在，不走 transport framing。
3. 认证成功必须生成显式 `auth_id`。
4. `SessionOpen` 必须以 `auth_id` 为输入。
5. `SessionOpen` 只返回 `session_id`。
6. metadata 必须通过 `SessionDescribe` 获取。
7. client 心跳属于 connection，不属于 session。
8. `gateway <-> storer` 心跳属于 route connection。
9. route 断线后 gateway 必须主动接管关闭相关 session。
10. `NetworkMedia` 只绑定 session，不负责 connection 管理。
