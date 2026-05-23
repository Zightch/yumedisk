# Data Plane

## 定位

本文档只定义会话打开后的 metadata 查询与共享数据面，不定义 client 盘对象或 server 内部转发表。

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

## SessionDescribe

`SessionDescribe` 返回当前 session 绑定 metadata。

当前最小 metadata 集固定为：

- `disk_size_bytes`
- `max_io_bytes`
- `read_only`

它描述的是这个 session 的可见视图，不额外承诺动态 metadata 刷新模型。

## ReadAt

### 请求 body

固定长度 `12` 字节：

| 偏移 | 长度 | 字段 | 类型 |
| --- | --- | --- | --- |
| `0` | 8 | `offset` | `u64` |
| `8` | 4 | `length` | `u32` |

### 成功响应 body

- 返回完整数据段

## WriteAt

### 请求 body

| 偏移 | 长度 | 字段 | 类型 |
| --- | --- | --- | --- |
| `0` | 8 | `offset` | `u64` |
| `8` | 4 | `length` | `u32` |
| `12` | `N` | `payload` | `bytes[N]` |

约束：

- `N` 必须等于 `length`

### 成功响应 body

- 当前不要求固定负载

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

## 大块 I/O

transport 单帧 payload 上限为 `65536` 字节。

因此协议层固定约束：

- 单次 `ReadAt / WriteAt` 必须服从 `max_io_bytes`
- 需要大块 I/O 时，由 client 在协议外主动拆片
- 对端只处理单个 request，不承担跨 request 业务重组

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
