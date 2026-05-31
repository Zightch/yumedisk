# `ReadAt / WriteAt` 数据压缩草案

## 0. 当前范围

本草案只讨论网络数据面里的 `ReadAt / WriteAt` 数据压缩。

这里的“数据压缩”固定只指：

- `WriteAt` 请求里的数据负载
- `ReadAt` 成功响应里的数据负载

这里明确不做：

- transport 整帧压缩
- 通用业务头压缩
- `Auth / SessionOpen / SessionDescribe / Notice` 压缩
- 旧版本兼容
- 多帧重组
- 字典压缩

## 1. 当前总目标

按最小核心闭环，把压缩方案先收成下面几件事：

1. 只在真正有数据负载的 `ReadAt / WriteAt` 上引入压缩
2. 除 `payload` 外，其余字段继续保持明文固定格式
3. 不兼容旧版本，直接切到新协议形状
4. 首版只引入一套主压缩器，不同时维护多套通用压缩算法
5. 压缩编解码职责只落在 client 与 storer
6. gateway 只依赖前置的 opaque data proxy 重构结果，不再参与压缩解码或重编码

## 2. 固定边界

### 2.1 明文边界

下列字段不进入压缩：

- `session_id`
- `request_id`
- `status_code`
- `offset`
- `length`
- `compress`

换句话说，只有最终数据字节会被压缩，协议控制字段全部继续走正常明文。

### 2.2 不做旧版兼容

本轮固定前提：

- `docs/tmp/todo-reconstruction.md` 已完成
- `client`
- `gateway`
- `storer`

协议版本仍按三边一起升级处理。

但压缩职责固定收口为：

- `client` 负责 client-facing 压缩编解码
- `storer` 负责 storer-facing 压缩编解码
- `gateway` 只负责共享数据面的 opaque proxy，不参与压缩编解码

因此本轮不做：

- 能力协商
- 新旧协议双栈并存
- 按 peer 版本动态切换 body 形状

### 2.3 首版不做复杂压缩矩阵

首版不引入：

- 哈夫曼单独编码
- 纯 LZ 自定义实现
- `gzip / deflate`
- `brotli`
- `lz4`

首版只保留：

- `raw`
- `zstd`

原因很直接：

- `512B ~ 60KiB` 的范围里，单独哈夫曼很容易被码表成本吃掉
- 纯 LZ 或 `deflate` 的工程复杂度并不比 `zstd` 更低
- `zstd` 已经覆盖了“LZ + 熵编码”的主流收益区间
- 先把一套压缩器走通，更符合当前项目的极简核心原则

### 2.4 依赖 gateway 数据面先代理化

本草案固定依赖 `todo-reconstruction.md` 先完成。

也就是说在进入压缩实现前，gateway 已经满足：

- `SessionDescribe / ReadAt / WriteAt` 走 opaque data proxy
- `SessionCloseNotice / SessionDataChangedNotice` 走 notice bridge
- gateway 不再解析 `ReadAt / WriteAt` 业务 body
- gateway 不再重建 `ReadAt / WriteAt` 业务 body

因此本草案的正式前提不是“gateway 解压后再压”，而是：

- client 构造的 `WriteAt` 压缩 body 可以经 gateway 原样代理到 storer
- storer 返回的 `ReadAt` 压缩 body 可以经 gateway 原样代理到 client

## 3. 压缩码表

`compress` 固定为 `u8`，当前定义如下：

| 码值 | 含义 |
| --- | --- |
| `0` | `raw` |
| `1` | `zstd-1` |
| `2` | `zstd-3` |

当前固定事实：

- `3..255` 暂不使用
- 遇到未知 `compress` 码值，按非法 body 处理

## 4. 协议形状

### 4.1 `ReadAt` 请求 body

`ReadAt` 请求不带压缩字段，仍然保持固定 `12` 字节：

| 偏移 | 长度 | 字段 | 类型 |
| --- | --- | --- | --- |
| `0` | 8 | `offset` | `u64` |
| `8` | 4 | `length` | `u32` |

### 4.2 `ReadAt` 成功响应 body

`ReadAt` 成功响应改为：

| 偏移 | 长度 | 字段 | 类型 |
| --- | --- | --- | --- |
| `0` | 1 | `compress` | `u8` |
| `1` | `N` | `payload` | `bytes[N]` |

语义固定为：

- `compress=0` 时，`payload` 就是原始数据
- `compress=1/2` 时，`payload` 是对应等级的 `zstd` 压缩结果
- 解码后的真实长度必须等于请求里的 `length`

### 4.3 `WriteAt` 请求 body

`WriteAt` 请求改为：

| 偏移 | 长度 | 字段 | 类型 |
| --- | --- | --- | --- |
| `0` | 8 | `offset` | `u64` |
| `8` | 4 | `length` | `u32` |
| `12` | 1 | `compress` | `u8` |
| `13` | `N` | `payload` | `bytes[N]` |

语义固定为：

- `length` 表示解码后的真实数据长度
- `compress=0` 时，`payload` 必须就是原始数据，且 `N == length`
- `compress=1/2` 时，`payload` 是压缩结果，解压后长度必须等于 `length`

### 4.4 `WriteAt` 成功响应 body

保持为空，不引入压缩字段。

## 5. 编解码规则

### 5.1 `WriteAt`

发送端流程固定为：

1. 先拿到原始数据
2. 按长度分档决定是否尝试压缩，以及尝试哪个等级
3. 若压缩收益不成立，则改发 `raw`
4. 把最终 `compress + payload` 放进请求 body

接收端流程固定为：

1. 先解析 `offset + length + compress`
2. 按 `compress` 解码 `payload`
3. 校验解码后长度必须等于 `length`
4. 再进入真正的 `write_at(offset, data)`

### 5.2 `ReadAt`

服务端流程固定为：

1. 先按请求里的 `offset + length` 读出原始数据
2. 按原始数据长度决定是否尝试压缩，以及尝试哪个等级
3. 若压缩收益不成立，则改发 `raw`
4. 把最终 `compress + payload` 放进成功响应 body

客户端流程固定为：

1. 先解析响应里的 `compress`
2. 按 `compress` 解码 `payload`
3. 校验解码后长度必须等于请求里的 `length`
4. 再把结果拷入目标 buffer

### 5.3 gateway 路径固定口径

在本草案成立的前提下，gateway 对压缩数据面的参与固定收口为：

1. 校验共享数据面 request 的通用 header 与 session ownership
2. 改写 `request_id / session_id`
3. 原样代理 `WriteAt` request body
4. 原样代理 `ReadAt` response body

固定不做：

- 解析 `compress`
- 解压 `payload`
- 重压 `payload`
- 按压缩结果改写上游或下游 body

## 6. 分档策略

### 6.1 当前推荐方案

当前推荐只保留一条简单分界线，不再切 `16KiB`：

- `< 1024`：直接 `raw`
- `1024..4095`：尝试 `zstd-1`
- `4096..61440`：尝试 `zstd-3`

这里的 `61440` 即 `60KiB`。

### 6.2 为什么不再切 `16KiB`

当前范围只有 `512B ~ 60KiB`。

在这个区间里，如果继续切：

- `4KiB`
- `16KiB`
- `32KiB`

之类的更多档位，收益未必显著，但会明显增加：

- 规则复杂度
- 调参与测试成本
- 线上问题定位难度

首版先保留 `1KiB` 和 `4KiB` 两个关键分界，更符合当前项目节奏。

### 6.3 压缩收益回退

本轮不建议“按档位必压”，而建议“按档位尝试压缩”。

固定规则建议如下：

- 若压缩后长度 `>=` 原始长度，则回退 `raw`
- 若压缩后会让最终业务 payload 超出 transport 单帧上限，则回退 `raw`

这样可以自动避免下面这些坏情况：

- 随机数据
- 已压缩图片
- 已压缩归档
- 本来就几乎不可压缩的数据块

## 7. transport 上限与 `max_io_bytes`

transport 单帧 payload 上限固定为 `65536` 字节。

本轮要特别注意一个事实：

- 旧 `WriteAt` 固定头是 `12` 字节
- 新 `WriteAt` 固定头会变成 `13` 字节

这意味着如果仍然保留理论上的绝对 `65500` 字节写入上限，那么：

- `24` 字节业务头
- `13` 字节 `WriteAt` body 固定头
- `65500` 字节原始 payload

三者相加会变成 `65537`，超过单帧上限 `1` 字节。

因此本轮有两种收口方式：

1. 明确把全链路 `max_io_bytes` 收成 `<= 60KiB`
2. 或者把理论绝对上限从 `65500` 下调到 `65499`

结合当前业务目标，本草案推荐第一种：

- 直接以 `60KiB` 作为当前正式目标上限

这样无论 `raw` 还是压缩形态，都有足够余量。

## 8. 错误处理

以下情况统一按 `BadBody` 或等价协议错误处理：

- `compress` 码值未知
- `zstd` 解压失败
- `WriteAt` 解压后长度不等于 `length`
- `ReadAt` 解压后长度不等于请求 `length`
- `compress=0` 但 `payload` 长度不等于 `length`

下面这些语义保持不变：

- 越界仍是 `IOOutOfRange`
- 超过 `max_io_bytes` 仍是 `IOLarge`
- 只读写入仍是 `IOReadOnly`
- 存储后端真实失败仍是 `IOFailed`

## 9. 为什么首版不选别的压缩算法

### 9.1 哈夫曼

不建议单独引入哈夫曼。

原因：

- 小块下码表成本很显著
- 单独哈夫曼对重复块、零块并不一定优于 `zstd`
- 需要再维护一套独立编码器和测试矩阵

### 9.2 纯 LZ

不建议单独引入自定义纯 LZ。

原因：

- 工程复杂度并不会更低
- 压缩率通常不如 `zstd`
- 没有明显理由在首版维护一套自研压缩协议

### 9.3 `lz4`

`lz4` 是唯一值得后续再看的备选。

它的优势是：

- 编解码速度通常更快

但当前先不引入，原因也很直接：

- 首版优先减少协议和依赖复杂度
- 当前数据块上限只有 `60KiB`
- 先观察 `zstd-1 / zstd-3` 的 CPU 与压缩率表现，再决定是否需要 `lz4`

## 10. 当前代码改动落点

如果按本草案实现，至少会改到下面这些位置：

- 协议文档
  - `docs/network/define/data-plane.md`
- Go 协议解析
  - `server/internal/proto/session.go`
- Go storer 数据面
  - `server/internal/storer/gateway/link/data_plane_handler.go`
- Rust client 协议解析
  - `windows/network-core/src/protocol_client.rs`
  - `windows/network-core/src/disk_session.rs`

此外还需要补：

- Go 侧单测
- Rust 侧单测
- gateway / storer 集成测试

固定要求：

- gateway 主流程代码不应因为本草案重新引入 `ReadAt / WriteAt` body 解析
- gateway 主流程代码不应因为本草案重新引入压缩编解码逻辑

## 11. 首版实施顺序

建议按下面顺序落地：

1. 先完成 `todo-reconstruction.md` 里的 gateway 数据面代理化
2. 把协议文档改成新 body 形状
3. 在 Go / Rust 两侧补统一的 `compress` 码表定义
4. 改 `ReadAt / WriteAt` 的 body 解析与编码
5. 接入 `zstd` 编解码
6. 实现“尝试压缩，不合适则回退 raw”
7. 补齐 raw / compressed / bad code / bad length / 不可压缩回退 测试

## 12. 当前建议结论

本草案当前建议固定为：

- 只压 `ReadAt` 响应与 `WriteAt` 请求里的数据负载
- `ReadAt request` 不带 `compress`
- `ReadAt response` 形状为 `compress:u8 | payload`
- `WriteAt request` 形状为 `offset:u64 | length:u32 | compress:u8 | payload`
- `WriteAt response` 仍为空
- 压缩码表先只保留 `raw / zstd-1 / zstd-3`
- 分档先只保留：
  - `< 1024 => raw`
  - `1024..4095 => 尝试 zstd-1`
  - `4096..61440 => 尝试 zstd-3`
- 若压缩无收益，则自动回退 `raw`
- gateway 不参与压缩编解码，只依赖共享数据面 opaque proxy

这版方案的核心特征是：

- 协议简单
- 实现面清晰
- gateway 不再成为压缩协议演进阻塞点
- 不为了少量潜在收益引入多套压缩器
- 先解决主要流量问题，再决定是否要做更深的优化
