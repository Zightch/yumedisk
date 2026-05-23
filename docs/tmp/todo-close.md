# SessionCloseNotice 关闭语义重建执行清单

## 1. 目标

本清单只服务于网络链路里的关闭语义重建。

本轮目标不是在现有 `Close + SessionCloseNotice` 分叉上继续打补丁，而是按重建口径直接统一为：

- 删除独立 `Close` op
- 统一只保留 `SessionCloseNotice`
- `SessionCloseNotice` 是单向 close 单播
- 不等待回复
- 不定义 close ack
- 不定义 graceful close

同时固定以下边界：

- 协议层允许任意链路两端主动发出 `SessionCloseNotice`
- 当前最小闭环只实现：
  - `client -> gateway`
  - `gateway -> client`
  - `gateway -> storer`
- 不改正式心跳边界：
  - `client(connection) -> gateway : ConnHeartbeat`
  - `gateway -> storer : LinkHeartbeat`
- 不引入 session-scoped heartbeat
- 不引入 session TTL

## 2. 重建约束

### 2.1 必须保持

- 保持 `auth -> session open -> data plane` 主模型不变
- 保持 `SessionOpen`、`ReadAt`、`WriteAt` 的正式协议边界不变
- 不引入新的 `SessionOpen` 模式位
- 不引入“旧 Close + 新 SessionCloseNotice”并存的兼容桥
- `docs/network/define`、`windows/network-core`、server 实现必须同步改到同一口径

### 2.2 当前最小闭环

对外关闭语义固定为：

- 一旦发送 `SessionCloseNotice(session_id, reason_code)`
  - 发送方立即做本地清理
- 一旦收到 `SessionCloseNotice(session_id, reason_code)`
  - 接收方立即把目标 session 视为失效
  - 接收方立即做本地清理
- 若目标 session 已不存在
  - 幂等忽略
  - 不升级成协议错误

对内最小实现固定为：

- 不做额外请求取消框架
- 不做独立请求队列管理
- 已经进入同步处理链路的请求允许自然执行完成
- 若该 session 已经关闭
  - 后续回复可以直接丢弃

### 2.3 必守底线

虽然协议对外是“立即关闭”，但 server 内部不能因此打穿单 writer 约束。

因此必须保持：

- session 至少区分：
  - `live`
  - `closing`
- `SessionCloseNotice` 到达时：
  - 立即把目标 session 置为 `closing`
  - 立即拒绝新的 `ReadAt / WriteAt`
- 已在途请求允许自然跑完
- 对 `rw session` 来说：
  - 独占位不能在收到 close notice 的瞬间释放
  - 必须等在途请求计数清零后，才能真正 drain 完成并释放 writer 独占位

### 2.4 client 责任边界

协议层不替 client 做 graceful close。

固定口径：

- 如果 client 需要“本次写一定完成再关”
  - 责任在 client 自己
  - 必须先等待自己的在途任务完成，再主动发 `SessionCloseNotice`
- 如果 client 在还有在途 I/O 时直接关闭
  - client 不能再依赖这些在途任务的返回结果

## 3. 当前阻塞点

### 3.1 正式协议仍保留旧分叉

当前正式协议文档仍保留旧关闭分叉：

- `docs/network/define/client-gateway.md`
- `docs/network/define/gateway-storer.md`
- `docs/network/define/data-plane.md`

当前问题包括：

- 仍把 `Close` 写成独立 op
- `SessionCloseNotice` 仍只按部分固定方向描述
- 还没有把“任意链路两端都可主动发起，具体实现可收窄”为正式口径写清楚

### 3.2 network-core 仍按旧 Close 模型建模

当前 `windows/network-core` 仍按旧模型实现：

- `windows/network-core/src/protocol_client.rs`
  - 仍存在 `CloseRequest`
- `windows/network-core/src/disk_session.rs`
  - `close()` 仍走独立 `Close` 请求路径
- `windows/network-core` 相关测试
  - 仍假设存在独立 `Close` 语义

### 3.3 server 当前也仍保留独立 Close 路径

当前 server 侧仍保留独立 `Close` 协议实现：

- `server/internal/proto`
- `server/internal/gateway/client/session/opener.go`
- `server/internal/storer/gateway/link/data_plane_handler.go`
- 相关 `integration_test / data_plane_handler_test / session_mapping_test`

### 3.4 session lifecycle 还没有完全收口

当前 server 内部还没有把以下语义正式钉死：

- `closing`
- 在途请求计数
- drain 完成后的最终释放
- writer 独占位延迟释放

## 4. 目标结构

## 4.1 协议目标

正式协议统一改为：

- 删除独立 `Close`
- `SessionCloseNotice` 成为唯一关闭语义
- `SessionCloseNotice` 永远不是 response
- `request_id = 0`
- 不进入 request/response pending 队列

## 4.2 主动发起矩阵

协议层允许：

- client -> gateway
- gateway -> client
- gateway -> storer
- storer -> gateway

但当前最小闭环只实现：

- client -> gateway
- gateway -> client
- gateway -> storer

也就是说：

- 协议文档保留主动发起留白
- 具体项目实现当前只落三条方向
- storer 当前不具备主动 close 能力

## 4.3 session lifecycle 目标

server 内部固定补齐：

- `live`
- `closing`
- `drained`

行为固定为：

- `live`
  - 可接受正常读写
- `closing`
  - 不再接受新读写
  - 允许已入链路请求自然结束
- `drained`
  - 完成最终释放

## 4.4 gateway 行为目标

gateway 固定按以下模式处理：

- 收到 client 主动 `SessionCloseNotice`
  - 立即本地清理该 gateway session
  - best-effort 向所属 storer 发 `SessionCloseNotice`
- client connection 死亡
  - 立即枚举并清理该 connection 名下 session
  - 对各自所属 storer 逐个 best-effort 发 `SessionCloseNotice`
- route connection 死亡
  - 立即清理该 route 名下 session 映射
  - 对仍在线 client 逐个发 `SessionCloseNotice`

这里的关键是：

- cleanup 只按
  - session
  - client connection
  - route connection
  三个作用域收束
- 不因为单个 close 或单条 route 故障扩大成整个 storer 进程清理

## 5. 分阶段任务

### 阶段 A：先改正式协议文档

目标：

- 先把 `docs/network/define` 收成单一 `SessionCloseNotice` 模型

任务：

- 改写 `docs/network/define/client-gateway.md`
  - 删除 op 表中的独立 `Close`
  - 明确 `SessionCloseNotice` 是该边上的唯一 close 语义
  - 明确协议层允许 connection 两端都可主动发 `SessionCloseNotice`
  - 明确当前最小闭环只实现：
    - `client -> gateway`
    - `gateway -> client`
- 改写 `docs/network/define/gateway-storer.md`
  - 删除 op 表中的独立 `Close`
  - 明确 `SessionCloseNotice` 是该边上的唯一 close 语义
  - 明确协议层允许 route 两端都可主动发 `SessionCloseNotice`
  - 明确当前最小闭环只实现：
    - `gateway -> storer`
- 改写 `docs/network/define/data-plane.md`
  - 删除独立 `Close` 小节
  - 明确 `SessionCloseNotice` 只表达“目标 session 立即失效”
  - 明确协议层不承诺 graceful close
  - 明确 server 不对“在途请求未完成就 close”导致的上层一致性负责

验收：

- `docs/network/define` 中不再存在独立 `Close` 分叉
- 三份正式文档对 `SessionCloseNotice` 的定义一致

### 阶段 B：重建 windows/network-core

目标：

- 删除客户端公共协议库中的旧 `Close` 模型

任务：

- 改写 `windows/network-core/src/protocol_client.rs`
  - 删除独立 `CloseRequest`
  - 统一 `SessionCloseNotice` 的主动发送与被动接收模型
- 改写 `windows/network-core/src/gateway_connection.rs`
  - 新增不占用 pending-response 槽位的单向 notice 发送路径
- 改写 `windows/network-core/src/disk_session.rs`
  - `close()` 改成发送 `SessionCloseNotice` 后立即本地标记关闭
- 保持 `SessionCloseNotice` / disconnect 的现有异步事件模型

验收：

- `windows/network-core` 不再保留独立 `CloseRequest`
- `disk_session.close()` 不再等待 server 响应
- 主动发送 `SessionCloseNotice` 不进入 request/response pending 队列

### 阶段 C：重建 server 协议实现

目标：

- 删除 server 内部独立 `Close` 路径

任务：

- 改写 `server/internal/proto`
  - 删除独立 `Close` op
- 改写 gateway client handler
  - 收到 client `SessionCloseNotice` 后：
    - 立即做本地 session 清理
    - best-effort 向上游发 `SessionCloseNotice`
    - 不再回任何 close 响应
- 改写 storer data plane handler
  - 收到 `SessionCloseNotice` 后：
    - 立即把 session 置为 `closing`
    - 不再回任何 close 响应
- 删除所有以独立 `Close` 为前提的 helper 和测试

验收：

- server 协议实现里不再保留独立 `Close`
- gateway 与 storer 都不等待 close ack

### 阶段 D：补齐 session lifecycle

目标：

- 把内部 `closing/drain` 语义正式补齐

任务：

- 为 session record/service 增加：
  - `closing` 状态
  - 在途请求计数
  - drain 完成后的最终释放
- 读写入口必须固定为：
  - 进入 handler 后先校验 session 是否 `live`
  - 已是 `closing` 则直接拒绝
- writer 独占位释放必须延迟到：
  - session 在途请求计数清零
  - drain 完成

验收：

- `rw session` 在 `closing` 且仍有在途写入时，不能被新的 `rw open` 抢占
- 已关闭 session 的在途回复允许直接丢弃

### 阶段 E：补测试与故障矩阵

目标：

- 用测试把关闭语义彻底钉死

任务：

- 补 formal docs 对应的 wire 级测试
- 补 `windows/network-core` 主动发送 `SessionCloseNotice` 测试
- 补 server 协议 handler 测试
- 补 gateway 故障矩阵测试
- 补在途 I/O + close 的 writer 独占保护测试

验收：

- close 路径不会回退成 request/response
- close 路径不会打穿单 writer 约束

## 6. 测试清单

### 6.1 正式协议

- 独立 `Close` op 已删除
- `SessionCloseNotice` 是唯一关闭语义
- 协议层允许任意链路两端主动发 `SessionCloseNotice`
- 当前最小闭环只实现：
  - `client -> gateway`
  - `gateway -> client`
  - `gateway -> storer`

### 6.2 windows/network-core

- 不再保留独立 `CloseRequest`
- `disk_session.close()` 不再等待 server 响应
- 主动发送 `SessionCloseNotice` 不进入 pending response
- 被动接收 `SessionCloseNotice` 继续走异步事件模型

### 6.3 server 协议实现

- 收到 `SessionCloseNotice` 后不回响应
- gateway 向上游发 `SessionCloseNotice` 超时或发送失败，不阻塞本地 cleanup
- gateway 向 client 发 `SessionCloseNotice` 失败，不回退已完成的本地 cleanup

### 6.4 session lifecycle

- session 进入 `closing` 后拒绝新 I/O
- 已在途 I/O 可自然完成
- 已关闭 session 的在途回复允许丢弃
- `rw session` 必须 drain 后才释放独占位

### 6.5 故障矩阵

- client 主动 `SessionCloseNotice` 只关闭目标 session
- client connection 死亡只关闭该 connection 名下 session
- route connection 死亡只关闭该 route 名下 session
- 单 session 失效不升级成整 route 故障

## 7. 最终验收标准

满足以下条件才算 close 重建完成：

- `docs/network/define` 不再保留独立 `Close`
- `windows/network-core` 不再保留独立 `CloseRequest`
- server 不再保留独立 `Close` 协议路径
- `SessionCloseNotice` 在 formal docs、`windows/network-core`、server 实现三处口径一致
- 当前最小闭环只实现：
  - `client -> gateway`
  - `gateway -> client`
  - `gateway -> storer`
- close 路径不回退成 request/response
- close 路径不打穿单 writer 约束
