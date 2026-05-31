# Data Plane

## 定位

本文档只定义会话打开后的 metadata 查询与共享数据面，不定义 client 盘对象或 server 内部转发表。

固定边界：

- 本文档只讨论业务层的 `SessionDescribe / ReadAt / WriteAt / Notice` 语义
- transport 分帧、实际帧长和连接级收发规则不在这里重复定义

## 进入数据面的前置条件

client 进入数据面前必须已经完成：

1. client-gateway bootstrap
2. `AuthStart / AuthFinish`
3. 得到有效 `auth_id`
4. `SessionOpen(auth_id)` 成功
5. 得到有效 `session_id`
6. `SessionDescribe(session_id)` 获取 metadata

固定事实：

- 认证成功不等于可直接读写
- `auth_id` 不等于 `session_id`
- `SessionOpen` 是进入数据面的唯一入口
- metadata 不从 `SessionOpen` 直接取得

## 共享数据面复用模型

`SessionDescribe / ReadAt / WriteAt` 的 body 与业务 `status_code` 语义，固定作为一套共享数据面定义复用。

这意味着：

- `client-gateway`
- `gateway-storer`

两条边上的这三类 op 使用同一份业务 body 模型。

若某条链路存在中间层，例如 gateway，则固定口径为：

- 中间层可以改写 `request_id`
- 中间层可以改写 `session_id`
- 中间层可以做 session ownership 与路由校验
- 中间层不应再生第二套共享数据面 body 形状
- 中间层不应重新定义第二套共享数据面 `status_code` 语义

## Compression Code

共享数据面里的压缩选择固定使用 `compress:u8`。

当前定义如下：

| 码值 | 含义 |
| --- | --- |
| `0` | `raw` |
| `1` | `zstd-1` |
| `2` | `zstd-3` |
| `255` | `solid-byte block` |

补充口径：

- `3..254` 当前未分配
- `compress=255` 时，`payload` 固定长度为 `1`
- `compress=255` 时，`payload[0]` 表示“当前块所有字节都等于这个值”
- `compress=255` 的解码结果长度由 `length` 决定
- 协议层只定义码表与 wire 形状，不定义发送端何时选择哪种压缩等级

## Raw Data

本文中的 `raw 数据` 固定指一次 `ReadAt / WriteAt` 真正读写的逻辑数据 payload。

固定口径：

- 它只包含这次 I/O 的实际数据字节串
- 它不包含通用业务头
- 它不包含 `ReadAt / WriteAt` 的固定 body 头，如 `offset / length / compress`
- 对发送端来说，看编码前的输入数据
- 对接收端来说，看解码后的输出数据
- 单个 `ReadAt / WriteAt` 的 raw 数据最大长度固定为 `60KiB`

进一步约束：

- `ReadAt.length` 表示本次希望读取的 raw 数据长度
- `WriteAt.length` 表示本次希望写入的 raw 数据长度
- `payload` 字段只承载这段 raw 数据的编码结果
- 发送端完成压缩判优后，最终放进 `payload` 的字节串无论是 raw 还是压缩结果，都必须 `<= 60KiB`

## SessionDescribe

`SessionDescribe` 返回当前 session 绑定 metadata。

当前最小 metadata 集固定为：

- `disk_size_bytes`
- `max_io_bytes`
- `read_only`
- `backend_id`

当前 `SessionDescribe` body 仍保留 `max_io_bytes` 字段。

固定口径：

- 该字段当前固定写 `60KiB`
- 它不再单独承载另一套长度语义；共享数据面的正式定义统一以上文 `60KiB raw 数据` 为准

它描述的是这个 session 的可见视图，不额外承诺动态 metadata 刷新模型。

其中 `backend_id` 的固定语义为：

- 标识当前 session 看到的 backend 身份
- 只用于等值比较
- 不承诺跨 storer 重启稳定

补充口径：

- `SessionDescribe` 的成功响应 body 在复用该共享数据面的协议边之间保持一致

## ReadAt

### 请求 body

固定长度 `12` 字节：

| 偏移 | 长度 | 字段 | 类型 |
| --- | --- | --- | --- |
| `0` | 8 | `offset` | `u64` |
| `8` | 4 | `length` | `u32` |

### 成功响应 body

| 偏移 | 长度 | 字段 | 类型 |
| --- | --- | --- | --- |
| `0` | 1 | `compress` | `u8` |
| `1` | `N` | `payload` | `bytes[N]` |

约束：

- 请求里的 `length` 表示本次希望读取的 raw 数据长度
- 这个 `raw` 只包含实际数据字节串，不包含通用业务头，也不包含 `ReadAt` 固定 body 头
- `length` 必须在 `1..60KiB`
- `payload` 字段承载对应 raw 数据的编码结果
- `compress=0` 时，`payload` 就是原始数据，且 `N == length`
- `compress=1/2` 时，`payload` 是对应压缩结果，且 `N <= 60KiB`
- `compress=255` 时，`payload` 必须正好是 `1` 字节，且解码结果为重复该字节值的整块数据
- 无论哪种形态，接收端解码后的真实长度都必须等于请求里的 `length`

补充口径：

- 若这条数据面经过中间层桥接，`ReadAt` 的成功响应 body 语义保持不变

## WriteAt

### 请求 body

| 偏移 | 长度 | 字段 | 类型 |
| --- | --- | --- | --- |
| `0` | 8 | `offset` | `u64` |
| `8` | 4 | `length` | `u32` |
| `12` | 1 | `compress` | `u8` |
| `13` | `N` | `payload` | `bytes[N]` |

约束：

- `length` 表示本次希望写入的 raw 数据长度
- 这个 `raw` 只包含实际数据字节串，不包含通用业务头，也不包含 `WriteAt` 固定 body 头
- `length` 必须在 `1..60KiB`
- `payload` 字段承载对应 raw 数据的编码结果
- `compress=0` 时，`payload` 必须就是原始数据，且 `N == length`
- `compress=1/2` 时，`payload` 是对应压缩结果，且 `N <= 60KiB`，解压后长度必须等于 `length`
- `compress=255` 时，`payload` 必须正好是 `1` 字节，且解码结果为重复该字节值的整块数据

### 成功响应 body

- 固定为空

补充口径：

- 若这条数据面经过中间层桥接，`WriteAt` 的请求 body 与响应 `status_code` 语义保持不变

## SessionDataChangedNotice

### notice

- header `session_id` 为目标 session
- header `request_id = 0`
- body 固定为空

### 语义

- `SessionDataChangedNotice` 只表达底层数据内容已经变化
- 它不表示目标 session 失效
- 它不表示 metadata 已变化
- 它不直接等于容量变化或其他 media 事件
- 它是单向 notice，不等待回复
- 具体在哪条边上允许哪个方向，由各边界文档定义
- 若 notice 经中间层桥接，变化的只是目标 `session_id` 视角，不是 notice body 语义

## SessionCloseNotice

### notice

- header `session_id` 为目标 session
- header `request_id = 0`
- body 为 `reason_code`

### 语义

- `SessionCloseNotice` 只表达目标 session 已失效
- 它不直接等于关闭整条 connection
- 它是单向 notice，不等待回复
- 某条协议边的任意一端都可以主动发出它
- 当前项目的最小闭环实现方向由具体边界文档定义
- 若目标 session 已不存在，接收方按幂等忽略
- 若 client 需要等待本地在途 I/O 完成，再决定是否关闭，责任在 client 自己
- 协议层不承诺 graceful close
- 当前最小闭环实现允许：
  - 已经进入同步处理链路的请求自然执行完成
  - 若 session 已关闭，后续回复可以直接丢弃
- 若某条链路存在中间层，它既可以桥接 close，也可以在本地故障路径中合成 close
- `reason_code` 只说明关闭事实来源，不区分该 close 是直接产生、经中间层桥接，还是由中间层合成

## 大块 I/O

共享数据面固定约束：

- 单个 `ReadAt / WriteAt` 的 raw 数据最大长度为 `60KiB`
- 这个长度只计算实际数据字节串，不计算通用业务头，也不计算 `ReadAt / WriteAt` 固定 body 头
- `ReadAt / WriteAt` 之外的消息不承载额外 `60KiB` 语义
- 需要大块 I/O 时，由 client 在协议外主动拆片
- 对端只处理单个 request，不承担跨 request 的业务重组

补充口径：

- `payload` 字段只承载单次 I/O 的一份编码结果，可能是 raw，也可能是压缩结果
- 单次 I/O 不做多段 `payload` 拆分或拼装

## 并发模型

共享数据面固定允许：

- 多个已打开 session 并发读写
- 同一个 connection 上多个 session 并发读写
- 已打开 session 的读写与另一个 auth/open 过程并发

不允许的只是建会话前 lane 冲突：

- auth 对 auth
- open 对 open
- auth 对 open

## Session 失效事实

协议层只定义失效事实，不定义宿主清理策略。

固定事实：

- client-gateway connection 死亡时，该 connection 下 session 全部失效
- route 死亡时，该 route 下 session 全部失效
- session 不定义独立 TTL；它只在 `SessionCloseNotice`、connection 死亡或 route 死亡时收束
- `SessionCloseNotice` 到达时，目标 session 已经失效

## SessionCloseNotice Reason

`SessionCloseNotice.reason_code` 当前定义如下：

| 码值 | 含义 |
| --- | --- |
| `1` | route lost |
| `2` | gateway shutdown |
| `3` | upstream session closed |
| `4` | client connection replaced |
| `5` | normal close |
| `6` | protocol error |

这些 reason code 只说明关闭事实来源，不定义 client 盘对象该如何收束。
