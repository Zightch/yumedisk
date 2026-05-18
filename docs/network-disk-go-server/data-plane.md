# 数据面最小闭环

## 1. 当前范围

当前第一版数据面只保留最小闭环能力：

- `SessionDescribe`
- `ReadAt`
- `WriteAt`
- `Close`

另外保留三类边界命令：

- `ConnHeartbeat`：只存在于 `client-gateway`
- `SessionCloseNotice`：只存在于 `client-gateway`
- `LinkHeartbeat`：只存在于 `gateway-storer`

当前全系统正式只保留两个心跳方向：

- `client(connection) -> gateway : ConnHeartbeat`
- `gateway -> storer : LinkHeartbeat`

第一版不再保留：

- session-scoped heartbeat
- `gateway -> client` 反向心跳
- `storer -> gateway` 反向心跳

## 2. 进入数据面的前置条件

进入 client 数据面前，必须已经完成以下链路：

1. client bootstrap 完成
2. connection 建立
3. `AuthStart / AuthFinish` 成功
4. 获得有效 `auth_id`
5. `SessionOpen(auth_id)` 成功
6. 获得有效 `session_id`
7. `SessionDescribe(session_id)` 成功
8. 构造客户端盘对象

硬约束：

- 认证成功不等于可直接读写
- `auth_id` 不等于 `session_id`
- `SessionOpen` 是进入数据面的唯一入口
- metadata 通过 `SessionDescribe` 获取
- auth 过程与 `SessionOpen` 在同一 connection 上必须互斥
- 同一 connection 上可以并存多个已签发 `auth_id`
- 同一 connection 上可以并存多个已打开 session
- 上述互斥只约束建会话前阶段

## 3. metadata 口径

### 3.1 作用

client 构造盘对象所需的最小 metadata 只有：

- `disk_size_bytes`
- `read_only`
- `max_io_bytes`

### 3.2 来源

当前正式来源固定为：

1. storer 在注册阶段把 metadata 交给 gateway
2. `SessionOpen` 成功时，gateway 把当前 route metadata 复制进 `session_registry` 作为 session 快照
3. `SessionOpen` 成功响应只返回 `session_id`
4. `SessionDescribe(session_id)` 从 gateway 的 session 快照中返回 metadata

## 4. 客户端盘对象构造口径

客户端盘对象第一版构造时显式保存：

- `disk_id`
- 已打开 session
- `disk_size_bytes`
- `read_only`
- `max_io_bytes`

它不负责：

- 认证
- 建连
- 心跳
- 自动重连
- 自动重新开会话

## 5. ReadAt

### 5.1 输入

- `session_id`
- `offset`
- `length`

### 5.2 输出

- 成功时返回完整数据段
- 失败时返回统一错误

## 6. WriteAt

### 6.1 输入

- `session_id`
- `offset`
- `length`
- `payload`

### 6.2 输出

- 成功时表示远端已接受本次写入
- 失败时返回统一错误

## 7. Close

### 7.1 作用

主动关闭一个 session。

### 7.2 约束

- `Close` 只关闭目标 session
- 不直接关闭整条 connection
- 关闭后再次对该 `session_id` 读写应返回 `ERR_SESSION_UNAVAILABLE`

## 8. ConnHeartbeat

### 8.1 作用

- 维持 client-gateway connection 存活
- 检查连接是否仍有效

### 8.2 边界

它不代表某个盘会话保活。

它的方向固定为：

- client 主动发送
- gateway 返回 response

当前正式模型中，client 不再对每个 session 单独做 heartbeat。

## 9. LinkHeartbeat

### 9.1 作用

- 维持 gateway-storer route connection 存活
- 检测 route 是否可达

### 9.2 边界

它不属于某个 session。

它的方向固定为：

- gateway 主动发送
- storer 返回 response
- `role = storer` 超时未收到 heartbeat 时主动退出

`role = whole` 的本地 fixed route 不走这条外部 heartbeat 链路。

route heartbeat 超时等价于 route connection 死亡。

## 10. 大块 I/O 约束

传输层单帧 payload 最大 `65536` 字节。

因此：

- `max_io_bytes` 必须小于 transport 上限扣除业务头开销后的安全值
- 大块 I/O 必须由客户端盘对象主动拆片

拆片规则：

1. 每个子请求带独立 `request_id`
2. 每个子请求带完整 `session_id + offset + length`
3. 对端只处理单帧业务请求，不承担跨帧业务重组

## 11. gateway 转发语义

当前第一版 `gateway` 同时承担数据面转发。

### 11.1 SessionOpen 之后的数据面

1. client 对 `gateway_session_id` 发数据面请求
2. gateway 查 `session_registry`
3. 得到 `(route_connection_id, storer_session_id)`
4. 改写为 storer 侧 `request_id + session_id`
5. storer 返回结果
6. gateway 恢复 client 侧 `request_id`
7. gateway 回给 client

要求：

- `gateway` 不缓存盘数据
- `gateway` 不改变成功/失败语义
- `gateway` 只负责映射、路由、转发、回传

并发补充：

- 建会话前阶段的“单 auth 过程、单 `SessionOpen` 过程、二者互斥”只约束 auth/open lane
- 已打开 session 上的 `SessionDescribe / ReadAt / WriteAt / Close` 允许并发复用同一条 connection
- 一条 connection 可以在持有多个活跃 session 的同时继续串行打开新的 session

## 12. 错误语义

第一版必须统一至少以下错误：

- `ERR_AUTH_ID_INVALID`
- `ERR_AUTH_ID_EXPIRED`
- `ERR_SESSION_UNAVAILABLE`
- `ERR_SESSION_BUSY`
- `ERR_IO_OUT_OF_RANGE`
- `ERR_IO_TOO_LARGE`
- `ERR_IO_READ_ONLY`
- `ERR_IO_FAILED`

边界原则：

- 协议结构错误由 connection 边界直接拒绝
- 数据面只处理已经成立的业务对象错误
- 客户端盘对象不重复做网络协议级判断

## 13. session / connection / route 失效策略

### 13.1 协议层定义

协议层只定义以下事实：

- connection 死亡时，该 connection 下 session 全部失效
- route 死亡时，该 route 下 session 全部失效，gateway 发送 `SessionCloseNotice`
- 收到 `SessionCloseNotice` 时，目标 session 已确定失效

### 13.2 网络层边界

协议层只定义 session / connection / route 的失效事实。

### 13.3 宿主策略边界

宿主是否定义假死挂起态，以及如何收束自己的盘对象，不属于本文档定义范围。

### 13.4 connection 与 route 失效结果

一条 client-gateway connection 失效时：

- 该连接下全部 session 一起失效
- 相关数据面请求全部失败
- 该连接下未消费 `auth_id` 一起失效

一条 storer route 失效时：

- 该 route 下全部 session 一起失效
- gateway 必须主动发 `SessionCloseNotice`
- 该 route 下未消费 `auth_id` 一起撤销

## 14. 当前最小验收

1. client 可通过 `SessionOpen` 获得 `session_id`。
2. client 可通过 `SessionDescribe` 获得构造盘对象的最小 metadata。
3. 客户端盘对象显式持有 `disk_id + session + metadata`。
4. 客户端盘对象可完成真实 `ReadAt / WriteAt`。
5. 一条 connection 上可并发持有多个已打开 session。
6. connection 或 route 失效后，协议层只定义目标 session 已失效。
