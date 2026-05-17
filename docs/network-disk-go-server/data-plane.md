# 数据面最小闭环

## 1. 当前范围

当前第一版数据面只保留最小闭环能力：

- `SessionDescribe`
- `ReadAt`
- `WriteAt`
- `Close`

另外保留一个 connection 级命令：

- `ConnHeartbeat`

当前正式口径中：

- `ConnHeartbeat` 不属于 session 数据面
- `SessionDescribe` 属于 session 建立后的 metadata 查询
- `ReadAt / WriteAt / Close` 属于真正的数据面

## 2. 进入数据面的前置条件

进入数据面前，必须已经完成以下链路：

1. bootstrap 完成
2. connection 建立
3. `AuthStart / AuthFinish` 成功
4. 获得有效 `auth_id`
5. `SessionOpen(auth_id)` 成功
6. 获得有效 `session_id`
7. `SessionDescribe(session_id)` 获得 metadata

硬约束：

- 认证成功不等于可直接读写
- `auth_id` 不等于 `session_id`
- `SessionOpen` 不直接返回 metadata
- auth 过程与 `SessionOpen` 过程在同一 connection 上必须互斥
- 同一 connection 上 auth 过程最多一个、`SessionOpen` 过程最多一个

## 3. SessionDescribe

### 3.1 作用

在已打开 session 上查询构造 `NetworkMedia` 所需的最小元数据。

### 3.2 返回最小字段

- `disk_size_bytes`
- `read_only`
- `max_io_bytes`

这几个字段构成当前 `NetworkMedia` 的最小依赖。

## 4. ReadAt

### 4.1 输入

- `session_id`
- `offset`
- `length`

### 4.2 输出

- 成功时返回完整数据段
- 失败时返回统一错误

## 5. WriteAt

### 5.1 输入

- `session_id`
- `offset`
- `length`
- `payload`

### 5.2 输出

- 成功时表示远端已接受本次写入
- 失败时返回统一错误

## 6. Close

### 6.1 作用

主动关闭一个 session。

### 6.2 约束

- `Close` 只关闭目标 session
- 不直接关闭整条 connection
- 关闭后再次对该 `session_id` 读写应返回 `ERR_SESSION_UNAVAILABLE`

## 7. ConnHeartbeat

### 7.1 作用

- 维持 client-gateway connection 存活
- 检查连接是否仍有效

### 7.2 边界

它不代表某个盘会话保活。  
当前正式模型中，client 不再对每个 session 单独做 heartbeat。

## 8. 大块 I/O 约束

传输层单帧 payload 最大 `65536` 字节。  
因此：

- `max_io_bytes` 必须小于 transport 上限扣除业务头开销后的安全值
- 大块 I/O 必须由 `NetworkMedia` 主动拆片

拆片规则：

1. 每个子请求带独立 `request_id`
2. 每个子请求带完整 `session_id + offset + length`
3. 对端只处理单帧业务请求，不承担跨帧业务重组

## 9. gateway 转发语义

当前第一版 `gateway` 同时承担数据面转发。

### 9.1 SessionDescribe

1. client 对 `session_id` 发 `SessionDescribe`
2. gateway 查 session 映射
3. 转发到对应 storer session
4. storer 返回 metadata
5. gateway 回给 client

### 9.2 ReadAt / WriteAt / Close

1. client 对 `gateway_session_id` 发请求
2. gateway 查表得到 `(route_connection, storer_session_id)`
3. gateway 改写为 storer 侧 `request_id + session_id`
4. storer 返回结果
5. gateway 恢复 client 侧 `request_id`
6. gateway 回给 client

要求：

- `gateway` 不缓存盘数据
- `gateway` 不改变成功/失败语义
- `gateway` 只负责映射、路由、转发、回传

并发补充：

- 上述“单 connection 单 auth 过程、单 `SessionOpen` 过程、二者互斥”的约束只针对建会话前阶段
- 已打开 session 上的 `SessionDescribe / ReadAt / WriteAt / Close` 允许并发复用同一条 connection

## 10. 错误语义

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
- `NetworkMedia` 不重复做网络协议级判断

## 11. NetworkMedia 接入方式

`NetworkMedia` 第一版采用最小直接实现：

- 构造时保存：
  - 已打开的 `DiskSession`
  - `disk_size_bytes`
  - `read_only`
  - `max_io_bytes`
- `read_locked()` 串行发起一个或多个 `ReadAt`
- `write_locked()` 串行发起一个或多个 `WriteAt`

它不负责：

- 认证
- 建连
- 心跳
- 自动重连
- 自动重新开会话

## 12. 连接与 session 失效口径

### 12.1 connection 失效

一条 `GatewayConnection` 失效时：

- 该连接下全部 session 一起失效
- 相关数据面请求全部失败

### 12.2 route 失效

一条 storer route 失效时：

- 该 route 下全部 session 一起失效
- gateway 应主动发 `SessionCloseNotice`

### 12.3 session close notice

收到 `SessionCloseNotice` 后，client 必须把对应 session 视为已关闭。  
后续对该 `session_id` 的任何读写都不应再继续推进。

## 13. 当前最小验收

第一版只验收以下闭环：

1. client 可通过 `SessionDescribe` 获得构造 `NetworkMedia` 的最小 metadata
2. `NetworkMedia` 可完成真实 `ReadAt / WriteAt`
3. 一个 `GatewayConnection` 上可并发持有多个 `DiskSession`
4. connection 断开后，其下全部 session 失效
5. route 断开后，gateway 可定向通知相关 client session 关闭
6. 建会话前阶段满足“单 auth 过程、单 `SessionOpen` 过程、二者互斥”，数据面阶段保持可并发
