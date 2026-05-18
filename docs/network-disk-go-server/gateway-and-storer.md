# Gateway-and-Storer 业务层协议

## 1. 文档定位

本文档定义 `gateway <-> storer` 的正式边界。

当前正式口径固定为：

- `gateway-storer` 是私有 route 控制面
- 它不是 `client-gateway` 的镜像协议
- 它不接收 `auth_id`
- 它不接收 `disk_id` 级多路选择
- 它不承接 client-facing metadata 查询
- 它只服务“一个 storer 进程承载一个 disk”的第一版结构

本文档只定义独立 `gateway` 与 `role = storer` 的外部网络边界。

`role = whole` 只复用这里的 route / session / data plane 语义，不额外暴露这条外部网络边。

## 2. 角色与连接方向

### 2.1 whole

- `role = whole`
- 持有本地存储
- 内嵌 gateway
- 对 client 监听
- 不对其他 `storer` 暴露 storer listener
- 不走外部 `StorerRegister` 网络注册协议
- gateway 的 `route_registry` 中只保留自己的 `disk_id`
- client-facing 完整走 `Hello -> transport -> auth -> session -> metadata -> data plane`

### 2.2 storer

- 持有本地存储
- 不对 client 监听
- 主动长连 `gateway`
- 一个进程只注册一个 `disk_id`
- 注册成功后承接该盘的会话和数据面

连接方向固定为：

```text
storer ----主动长连----> gateway
```

## 3. 正式结构边界

当前第一版必须先收死以下边界：

1. 一个 `storer` 进程只承载一个 `disk_id`
2. 一个 route connection 只绑定一个 `disk_id`
3. 同一时刻一个 `disk_id` 只允许一个活跃 route
4. `gateway` 是唯一路由选择点
5. `storer` 只执行本地 session 和 I/O

直接结果：

- `gateway -> storer` 的内部 `SessionOpen` 不携带 `disk_id`
- `gateway -> storer` 的内部 `SessionOpen` 不携带 `auth_id`
- route 一旦注册成功，`disk_id` 选择已经在 gateway 层结束

## 4. 注册阶段

### 4.1 注册目的

注册阶段只解决：

1. 让 gateway 知道这个 storer 是否可信
2. 让 gateway 知道这个 route 承载哪个 `disk_id`
3. 让 gateway 缓存认证材料与基础 metadata

### 4.2 注册字段

`storer` 向 `gateway` 提交：

- `gateway_token`
- `disk_id`
- `auth_verifier`
- `disk_size_bytes`
- `read_only`
- `max_io_bytes`

其中：

- `auth_verifier = SHA512(claim_code_bytes)`
- `claim_code` 原文不上送
- `gateway_token` 只用于 storer 注册信任

### 4.3 route_registry 写入

注册成功后，gateway 至少写入：

- `disk_id`
- `route_connection_id`
- `auth_verifier`
- `disk_size_bytes`
- `read_only`
- `max_io_bytes`
- `route_state = active`

### 4.4 metadata 口径

注册进来的 metadata 在当前 route 生命周期内视为不可变。

第一版口径固定为：

- route 级 metadata 真源在 `gateway.route_registry`
- `SessionOpen` 成功时，gateway 复制当前 route metadata 作为 session 快照
- `SessionDescribe(session_id)` 由 gateway 从 `session_registry` 本地回答
- gateway 不再为 `SessionDescribe` 向 storer 发起额外 round-trip

### 4.5 whole 的本地 fixed route

`role = whole` 不走外部注册消息，但内嵌 gateway 启动后仍需在本地写入一条 fixed route。

该 fixed route 与外部注册成功后的 `route_registry` 字段口径一致，并固定为：

- 只对应本地唯一 `disk_id`
- 只路由到本地 storer core
- 不派生第二套 session / metadata / data plane 语义

## 5. route connection

注册成功后，`gateway <-> storer` 的长连即成为 route connection。

该连接负责：

- `SessionOpen`
- `ReadAt`
- `WriteAt`
- `Close`
- `LinkHeartbeat`

它不负责：

- client 认证
- `auth_id`
- client-facing metadata 查询
- client-facing notice
- 多盘复用选择

## 6. 不下发 client 认证

当前正式边界固定如下：

- `AuthStart / AuthFinish / auth_id` 只存在于 `client <-> gateway`
- `gateway` 不把 client challenge/proof 转发给 storer
- storer 不生成 `auth_id`

storer 只看到：

- 合法的 `StorerRegister`
- 来自 gateway 的 `SessionOpen`
- 来自 gateway 的 `ReadAt / WriteAt / Close`

## 7. 内部 SessionOpen

### 7.1 gateway 侧输入

gateway 在 client 边界收到的是：

```text
SessionOpen(auth_id)
```

### 7.2 gateway 侧还原

gateway 用 `auth_id` 还原：

- `client_connection_id`
- `disk_id`
- 目标 `route_connection`

### 7.3 storer 侧输入

gateway 向 storer 发起的是 route-facing `SessionOpen`。

由于当前一个 route 只承载一个 `disk_id`，所以 storer 侧输入不再携带 `disk_id`，只表示：

- 请在当前 route 上为当前盘打开一个真实本地 session

### 7.4 成功返回

成功时 storer 返回：

- `storer_session_id`

gateway 随后：

1. 分配新的 `gateway_session_id`
2. 把当前 route metadata 复制成 session 快照
3. 写入 `session_registry`
4. 回给 client `gateway_session_id`
5. 后续由 client 再调用 `SessionDescribe(gateway_session_id)` 读取 metadata

## 8. 共享数据面复用口径

`gateway-storer` 与 `client-gateway` 共享同一组数据面语义：

- `ReadAt`
- `WriteAt`
- `Close`

但会话 ID 空间不同：

- client 侧看到的是 `gateway_session_id`
- storer 侧看到的是 `storer_session_id`

转发逻辑固定为：

1. client 对 `gateway_session_id` 发请求
2. gateway 查表得到 `(route_connection_id, storer_session_id)`
3. gateway 改写 `request_id` 与 `session_id`
4. storer 返回结果
5. gateway 恢复 client 侧 `request_id`
6. gateway 回给 client

要求：

- `gateway` 不缓存盘数据
- `gateway` 不改成功/失败语义
- `gateway` 只负责映射、转发、回传

## 9. route heartbeat

`gateway <-> storer` 维持 route connection 级心跳。

当前方向固定为：

- gateway 主动喂狗
- storer 收到后必须立即响应 `LinkHeartbeat`
- `role = storer` 必须维护本地 watchdog；超时未收到 gateway heartbeat 时主动退出
- gateway 发送 heartbeat 超时未收到响应，等价于 route connection 死亡

这是 route connection 级能力，不属于某个 session。

补充约束：

- `gateway-storer` 不存在反向 `storer -> gateway` heartbeat
- `gateway-storer` 不存在 session-scoped heartbeat
- `role = whole` 的本地 fixed route 不走外部 `LinkHeartbeat` 网络链路

## 10. route 故障传播

当 route connection 断线或 heartbeat 超时后，gateway 必须立即：

1. 撤销该 route
2. 撤销该 route 名下全部未消费 `auth_id`
3. 找出该 route 名下全部活跃 session
4. 将这些 session 收束为 closed
5. 对仍在线的 client connection 发送 `SessionCloseNotice`
6. 清理本地映射

`role = whole` 下若本地 storer core 失效，内嵌 gateway 也必须走与 route 丢失完全相同的 grant/session 清理路径。

这里的 client-facing 结果固定为：

- gateway 明确判定 session 已失效
- 协议层只定义目标 session 已失效

## 11. 当前正式约束

1. storer 主动连接 gateway。
2. 一个 storer 进程只注册一个 `disk_id`。
3. 一个 route connection 只绑定一个 `disk_id`。
4. 同一时刻同一 `disk_id` 只允许一个活跃 route。
5. `gateway_token` 只用于 storer 注册信任。
6. client 认证不下发到 storer。
7. `auth_id` 只存在于 client-gateway。
8. storer 只处理真实会话和数据面。
9. route metadata 真源在 gateway route 表，`SessionDescribe` 由 gateway 本地回答。
10. `gateway-storer` 唯一心跳方向是 `gateway -> storer : LinkHeartbeat`。
11. `role = whole` 不暴露 storer listener，且 `route_registry` 中只保留本地唯一 `disk_id`。
12. route 断线时 gateway 必须主动接管关闭相关 client session。
