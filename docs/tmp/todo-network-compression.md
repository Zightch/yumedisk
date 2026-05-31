# `ReadAt / WriteAt` 压缩实施清单

## 0. 定位

这份清单不再讨论“要不要压缩”，而只讨论按当前已定口径如何分阶段落地。

当前前置条件已经成立：

- `docs/tmp/todo-reconstruction.md` 已完成
- gateway 已收口为：
  - 控制面保留
  - 数据面 opaque proxy
  - notice bridge

因此后续压缩工作固定建立在下面这个前提上：

- client 构造的 `WriteAt` body 可以经 gateway 原样代理到 storer
- storer 返回的 `ReadAt` body 可以经 gateway 原样代理到 client
- gateway 不参与压缩编解码

## 1. 已定口径

### 1.1 transport 与 `60KiB` 语义

固定事实：

- transport 单帧 `payload` 上限仍然是 `65536` 字节
- transport 的两字节长度头只表示“当前业务帧的实际 payload 长度减一”
- `60KiB` 只属于共享数据面里 `ReadAt / WriteAt` 的 raw 数据上限
- 这个 raw 上限对发送端看编码前 / 压缩前的真实数据字节数
- 对接收端看解码后 / 解压后的真实数据字节数
- 它不计算：
  - `offset`
  - `length`
  - `compress`
  - 协议业务头
  - transport 帧头

也就是说：

- `ReadAt.length <= 60KiB`
- `WriteAt.length <= 60KiB`

其中 `length` 的语义固定为：

- 表示业务 raw 数据长度
- 对发送端来说，就是编码前 / 压缩前的真实数据长度
- 对接收端来说，就是解码后 / 解压后的真实数据长度

剩余约 `4KiB` 明确留给下面这些东西承载：

- 业务固定头
- 压缩封装字段
- 压缩后 payload 的实际线上长度余量
- 后续必要扩展

这部分余量不进入 `max_io_bytes` 语义。

补充口径：

- `ReadAt / WriteAt` 之外的消息不承载额外 `60KiB` 语义
- 所有消息统一只服从 transport 的单帧实际长度约束
- 当前项目不做跨包拼接、多帧重组或跨帧压缩负载拼装

### 1.2 压缩范围

本轮固定只压下面两处数据负载：

- `WriteAt` 请求里的数据负载
- `ReadAt` 成功响应里的数据负载

明确不做：

- transport 整帧压缩
- 通用协议头压缩
- `Auth / SessionOpen / SessionDescribe / Notice` 压缩
- 多帧重组
- 旧版本兼容

### 1.3 明文边界

下面这些字段全部继续保持明文：

- `session_id`
- `request_id`
- `status_code`
- `offset`
- `length`
- `compress`

只有最终数据负载允许进入压缩。

### 1.4 gateway 职责边界

压缩职责固定只落在：

- client
- storer

gateway 固定只做：

1. 校验共享数据面 request 的通用 header 与 session ownership
2. 改写 `request_id / session_id`
3. 原样代理 `WriteAt` request body
4. 原样代理 `ReadAt` response body

固定不做：

- 解析 `compress`
- 解压 `payload`
- 重压 `payload`
- 因压缩形态不同而改写 body

### 1.5 压缩码表目标口径

`compress` 固定为 `u8`，目标定义如下：

| 码值 | 含义 |
| --- | --- |
| `0` | `raw` |
| `1` | `zstd-1` |
| `2` | `zstd-3` |
| `255` | `solid-byte block` |

补充口径：

- `3..254` 暂未分配
- `compress=255` 表示“当前块所有字节都相同”
- `compress=255` 时，`payload` 固定长度必须为 `1`
- `compress=255` 时，`payload[0]` 就是这个重复字节值
- `compress=255` 的解码结果长度由 `length` 决定

### 1.6 目标协议形状

#### `ReadAt` 请求 body

保持不变，固定 `12` 字节：

| 偏移 | 长度 | 字段 | 类型 |
| --- | --- | --- | --- |
| `0` | 8 | `offset` | `u64` |
| `8` | 4 | `length` | `u32` |

#### `ReadAt` 成功响应 body

改为：

| 偏移 | 长度 | 字段 | 类型 |
| --- | --- | --- | --- |
| `0` | 1 | `compress` | `u8` |
| `1` | `N` | `payload` | `bytes[N]` |

约束：

- `length` 取自请求
- `compress=0` 时，`payload` 就是原始数据
- `compress=1/2` 时，`payload` 是对应压缩结果
- `compress=255` 时，`payload` 必须正好是 `1` 字节
- 无论哪种形态，接收端解码后的真实长度都必须等于请求里的 `length`

#### `WriteAt` 请求 body

改为：

| 偏移 | 长度 | 字段 | 类型 |
| --- | --- | --- | --- |
| `0` | 8 | `offset` | `u64` |
| `8` | 4 | `length` | `u32` |
| `12` | 1 | `compress` | `u8` |
| `13` | `N` | `payload` | `bytes[N]` |

约束：

- `length` 表示业务 raw 数据长度
- 对发送端来说，就是编码前 / 压缩前的真实数据长度
- `compress=0` 时，`payload` 必须就是原始数据，且 `N == length`
- `compress=1/2` 时，`payload` 解压后长度必须等于 `length`
- `compress=255` 时，`payload` 必须正好是 `1` 字节
- 无论哪种形态，最终实际请求包仍必须能装进单个 transport frame

#### `WriteAt` 成功响应 body

保持为空。

### 1.7 错误口径目标

以下情况统一按 `BadBody` 或等价协议错误处理：

- `compress` 码值未知
- `compress=255` 但 `payload` 长度不等于 `1`
- `zstd` 解压失败
- `WriteAt` 解压后长度不等于 `length`
- `ReadAt` 解压后长度不等于请求 `length`
- `compress=0` 但 `payload` 长度不等于 `length`

下面这些业务语义保持不变：

- 越界仍是 `IOOutOfRange`
- 超过 `max_io_bytes` 仍是 `IOLarge`
- 只读写入仍是 `IOReadOnly`
- 存储后端真实失败仍是 `IOFailed`

## 2. 实施总原则

这轮落地按下面四阶段推进：

1. 先同步 network 正式文档，明确 `60KiB` 与新 body 语义
2. 再改 `network-core` 和 `server` 的协议面，只把 `compress` 字段接入 `ReadAt / WriteAt` body
3. 第三阶段才进入真实压缩编解码、收益判定与回退
4. 第四阶段做首尾收口

这四阶段的关键要求是：

- 第二阶段不提前引入真实压缩算法
- 第二阶段不提前接 `zstd`
- 第二阶段不提前做压缩收益判定
- 第二阶段先把 wire 形状一次改对

## 3. Phase 1: 同步正式网络文档

### 3.1 目标

先把正式协议文档的口径收死。

### 3.2 本阶段需要完成

- 改 `docs/network/define/data-plane.md`
- 如有必要，补 `docs/network/server/README.md` 里的实现口径说明
- 明确 `60KiB` 只计算逻辑数据负载
- 明确 `ReadAt / WriteAt` 新 body 形状
- 明确 `compress=255` 的 `solid-byte block` 语义

### 3.3 本阶段完成标准

- 正式协议文档已经明确：
  - transport 上限仍是 `65536`
  - `max_io_bytes` 约束的是业务 raw 数据长度
  - `ReadAt response = compress:u8 + payload`
  - `WriteAt request = offset:u64 + length:u32 + compress:u8 + payload`
  - `compress=255` 表示整块同字节值

## 4. Phase 2: 协议面重构

### 4.1 目标

只改协议面，不做真实压缩实现。

这里的“协议面”固定指：

- body 编码
- body 解析
- 长度校验
- 错误分支
- 测试

### 4.2 本阶段需要完成

- 改 `network-core`
- 改 `server`
- 在 `ReadAt` 成功响应里加 `compress`
- 在 `WriteAt` 请求里加 `compress`
- 把 `length` 统一解释成“解码后的真实长度”
- 先把 raw 路径按新 body 形状走通

### 4.3 本阶段明确不做

- 不接真实 `zstd`
- 不实现 `compress=255` 的真实解码逻辑
- 不实现 `1/2/255` 的完整算法分支
- 不做压缩收益判定
- 不做“尝试压缩，不合适回退 raw”

### 4.4 推荐临时行为

为了避免半成品协议状态，本阶段建议固定为：

- 编码端只发送 `compress=0`
- 解码端先按新 body 形状解析
- 非 `0` 码值先保留给后续阶段

也就是说第二阶段的目的不是“先做半套压缩”，而是：

- 先把 `ReadAt / WriteAt` 的 wire 形状切到新协议

### 4.5 本阶段完成标准

- `network-core` 与 `server` 已全部使用新 body 形状
- raw 路径在新协议形状下通过测试
- gateway 主流程仍然不重新解析 `ReadAt / WriteAt` 业务 body
- 没有引入任何跨包拼接或多帧重组逻辑

## 5. Phase 3: 正式进入压缩编解码

### 5.1 目标

在协议面已经稳定后，再把真实压缩能力接进来。

### 5.2 本阶段需要完成

- 补统一 `compress` 码表定义
- 实现 `compress=255` 的 `solid-byte block`
- 接入 `zstd-1`
- 接入 `zstd-3`
- 实现分档压缩策略
- 实现收益判定
- 实现回退 raw

### 5.3 当前推荐压缩选择

当前建议保留下面四类发送形态：

- `0 = raw`
- `1 = zstd-1`
- `2 = zstd-3`
- `255 = solid-byte block`

其中：

- `solid-byte block` 可以独立于 `zstd` 优先判断
- 若整块同字节，则可直接使用 `255`

### 5.4 当前推荐分档

- `< 1024`：默认 `raw`
- `1024..4095`：尝试 `zstd-1`
- `4096..61440`：尝试 `zstd-3`

### 5.5 当前推荐收益判定

压缩只有在满足下面条件时才成立：

- 节省字节数 `>= max(64, ceil(raw_len * 5%))`

否则统一回退 `raw`。

### 5.6 本阶段完成标准

- `solid-byte block` 编解码成立
- `zstd-1 / zstd-3` 编解码成立
- 收益判定成立
- 不可压缩数据会自动回退 raw
- 解码长度校验与单帧上限校验都已补齐

## 6. Phase 4: 首尾收口

### 6.1 目标

把前面三阶段的实现收成可交付状态。

### 6.2 本阶段需要完成

- client / gateway / storer 端到端集成测试
- whole 路径集成测试
- bad code / bad length / 解压失败测试
- 文档回填
- 常量与错误码统一
- 清理临时兼容逻辑

### 6.3 本阶段完成标准

- 三条路径都已验证：
  - client -> gateway -> storer
  - storer -> gateway -> client
  - whole
- 压缩与非压缩路径都通过端到端验证
- 当前临时文档可被正式实现文档替代

## 7. 建议改动落点

按这份清单推进，至少会碰到：

- 协议文档
  - `docs/network/define/data-plane.md`
- Rust 协议面
  - `windows/network-core/src/protocol_client.rs`
  - `windows/network-core/src/disk_session.rs`
- Go 协议面
  - `server/internal/proto/session.go`
- Go storer 数据面
  - `server/internal/storer/gateway/link/data_plane_handler.go`

若进入真实压缩编解码阶段，还会继续改：

- client 侧压缩发送与解码逻辑
- storer 侧压缩发送与解码逻辑
- 对应 Go / Rust 单测

## 8. 当前建议结论

这份清单当前建议固定为：

- transport 单帧 `payload` 上限保持 `65536`
- `60KiB` 只约束 `ReadAt / WriteAt` 的 raw 数据长度，只计算解码后的真实数据字节数
- `ReadAt request` 不带 `compress`
- `ReadAt response` 形状为 `compress:u8 | payload`
- `WriteAt request` 形状为 `offset:u64 | length:u32 | compress:u8 | payload`
- `WriteAt response` 仍为空
- `compress=255` 固定表示整块同字节值：
  - `payload` 固定为 `1` 字节
  - `payload[0]` 是当前块重复字节值
  - 解码结果为 `length` 个该字节值
- 落地顺序固定为：
  - 先收正式协议文档
  - 再改协议面
  - 再上真实压缩编解码
  - 最后做首尾收口
- transport 和业务层严格分离：
  - transport 只处理实际帧长与包边界
  - 业务层只处理 `ReadAt / WriteAt` 的 raw 长度与压缩语义

这样推进的好处很直接：

- 协议语义先被写死
- wire 形状先被一次改对
- 不把 `zstd`、收益判定和故障路径混到同一阶段
- gateway 继续保持为数据面 opaque proxy，不成为压缩演进阻塞点
