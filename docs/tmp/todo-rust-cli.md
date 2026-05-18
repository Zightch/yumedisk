# windows/rust-cli 重构待办

## 1. 目标

`windows/rust-cli` 当前要按正式网络链路收成以下唯一口径：

- `GatewayConnection` 只是到某个 gateway endpoint 的可复用业务连接
- `auth_id` 是显式授权对象，不是 connection 上的单一“当前已认证盘”
- `session_id` 是显式已打开会话，不是 auth 过程的附属状态
- 同一 connection 上只限制 auth/open 的 in-flight 过程互斥，不限制多个已签发 `auth_id` 和多个已打开 session 并存
- `NetworkMedia` 是宿主盘对象，显式持有 `disk_id + DiskSession + metadata`
- 当前宿主策略固定为故障即立即卸载并清理，不保留假死挂起态

## 2. 当前问题与原因

当前已暴露的问题是：

- 前后两次 `auth` 指令访问同一个 gateway 下的两个 storer
- 第二次 `auth` 成功后，会把第一次 `auth` 对应的盘挤掉
- 结果表现为同一 gateway 下两个盘无法共存

这说明当前 rust-cli 仍把“认证结果”错误地收敛成了 connection 级单一状态，例如：

- connection 上只保留一个“当前盘”
- connection 上只保留一个“当前 auth 结果”
- 后一次 `auth` 直接覆盖前一次 `auth`

这与正式网络链路要求冲突。正式要求是：

- auth 过程期间不能有另一个 auth 或 `SessionOpen` 过程
- `SessionOpen` 过程期间也不能有另一个 auth 或 `SessionOpen` 过程
- 但这只约束建会话前的 in-flight lane
- 已签发 `auth_id` 可以并存
- 已打开 session 可以并存
- 已打开 session 的读写允许与后续新的 auth/open 串行过程并发存在

因此 rust-cli 不能再把 connection 设计成“当前选中的盘”，而必须改成“多个 grant 和多个 session 的宿主”。

## 3. 必须落地的对象边界

### 3.1 `GatewayConnection`

`GatewayConnection` 只负责：

- `Hello -> transport` 建链
- `request_id` 分配与 pending request 管理
- 接收循环与 notice 分发
- `ConnHeartbeat`
- 连接死亡广播
- auth/open in-flight lane 的互斥收口

`GatewayConnection` 不负责：

- 保存单一“当前盘”
- 保存单一“当前 auth 结果”
- 持有 `NetworkMedia` 生命周期
- 把 metadata 当作 connection 级全局状态

它必须满足：

- 以 gateway endpoint 为 key 复用
- 同一条 connection 可承载多个 `auth_id`
- 同一条 connection 可承载多个 opened session

### 3.2 `AuthGrant`

`AuthGrant` 是 rust-cli 对 `auth_id` 的本地对象表达，至少要绑定：

- `auth_id`
- `disk_id`
- `gateway endpoint`
- `issued_at / expire_at`
- `grant_state`

约束：

- 每次 `AuthFinish` 成功都创建新的 `AuthGrant`
- 多个 `AuthGrant` 可以同时挂在同一 `GatewayConnection`
- `SessionOpen` 只消费目标 `AuthGrant`
- 一个 `AuthGrant` 被消费、过期或撤销后，只影响自己，不覆盖其他 grant

### 3.3 `DiskSession`

`DiskSession` 是 rust-cli 对已打开 session 的本地对象表达，至少要绑定：

- `session_id`
- `disk_id`
- 所属 `GatewayConnection`

约束：

- 一个 `DiskSession` 只代表一个已打开 session
- 多个 `DiskSession` 可以同时复用同一条 `GatewayConnection`
- 已有 `DiskSession` 的存在，不阻止后续新的 auth/open 串行发生

### 3.4 `NetworkMedia`

`NetworkMedia` 是宿主盘对象，至少显式保存：

- `disk_id`
- 已打开的 `DiskSession`
- `disk_size_bytes`
- `read_only`
- `max_io_bytes`

约束：

- `NetworkMedia` 必须在 `SessionDescribe` 完成后构造
- `NetworkMedia` 不从 ambient connection 状态推导盘身份
- `NetworkMedia` 不拥有 auth lane
- `NetworkMedia` 不拥有 heartbeat loop

## 4. Phase R1: connection 复用边界

需要完成：

- `auth` / `open` 先按 gateway endpoint 查找现有 `GatewayConnection`
- 没有连接时，先完整走 `Hello -> transport -> ConnHeartbeat`
- 已有连接时，直接复用同一条 `GatewayConnection`
- 严禁为了避免状态冲突而“每个盘单开一条 connection”
- 也严禁在 connection 上继续保留“当前磁盘”或“当前 auth”这类单值字段

为什么必须这样做：

- 正式协议的复用边界是 connection，不是 disk
- 同一 gateway 下多个盘本来就应该共享同一条 client-facing 连接
- 问题根因不是“连接复用错了”，而是“连接内部状态建模错了”

验收标准：

- 同一 gateway endpoint 的两次 `auth` 可以复用同一条 connection
- 第二次 `auth` 不会把第一次 `auth` 对应的盘挤掉

## 5. Phase R2: auth lane 与 grant table

需要完成：

- 在每条 `GatewayConnection` 上明确 auth/open 共用一个前置过程 lane
- `AuthStart -> AuthFinish` 成功后，向 grant table 追加一个新的 `AuthGrant`
- grant table 不能被简化成单个“当前 auth_id”
- auth 失败只清理本次 auth attempt，不影响已有 grant 和 opened session
- grant lookup 以 `auth_id` 为主键，必要时再加 `disk_id` 辅助索引

为什么必须这样做：

- 协议要求限制的是 in-flight 过程，不是 granted 对象数量
- 如果 grant table 退化成单个字段，后一次 `auth` 必然覆盖前一次

验收标准：

- 同一 connection 上连续两次成功 `auth` 后，本地同时持有两个独立 `AuthGrant`
- 两个 `AuthGrant` 分别保留各自的 `disk_id`

## 6. Phase R3: `SessionOpen` 与 session table

需要完成：

- `SessionOpen` 明确只消费一个指定 `AuthGrant`
- `SessionOpen` 成功后创建新的 `DiskSession`
- `SessionDescribe` 单独执行，并把 metadata 绑定到目标 session 对应的盘对象
- session table 不能被简化成单个“当前 mounted disk”
- 已有 session 的读写在新的 auth/open 发生时仍允许继续

为什么必须这样做：

- 正式协议区分“`SessionOpen` 过程”和“已打开 session”
- `SessionOpen` 互斥不代表整个 connection 只能有一个 opened session

验收标准：

- 先打开盘 A，再 `auth/open` 盘 B 后，盘 A 的 session 仍保持存活
- 盘 A 与盘 B 可同时作为两个独立的 opened session 存在

## 7. Phase R4: `NetworkMedia` 绑定口径

需要完成：

- `NetworkMedia` 新增并固定显式 `disk_id`
- `NetworkMedia` 构造时使用目标 `DiskSession` 和该 session 的 metadata
- 禁止再从 connection 上的单一“当前盘”反推盘身份
- 一个 `GatewayConnection` 可以同时支撑多个 `NetworkMedia`

为什么必须这样做：

- 同一 connection 下可以同时存在多个 `AuthGrant` 和多个 `DiskSession`
- 如果 `NetworkMedia` 不显式持有 `disk_id`，后续 auth/open 很容易把对象语义冲掉

验收标准：

- 每个宿主盘对象都稳定绑定自己的 `disk_id + session + metadata`
- 后续新的 `auth` 不会改写已存在 `NetworkMedia` 的盘身份

## 8. Phase R5: 故障收束策略

当前阶段 rust-cli 宿主策略固定为：立即卸载并清理。

需要汇入同一 cleanup 入口的事件：

- `SessionCloseNotice`
- TCP 断开
- transport fatal error
- `ConnHeartbeat` timeout
- terminal session error

收束要求：

- connection 死亡时，清掉该 connection 下全部 pending request
- connection 死亡时，撤销该 connection 下全部本地 `AuthGrant`
- connection 死亡时，标记该 connection 下全部 `DiskSession` 已失效
- 对应的 `NetworkMedia` 立即卸载并清理
- `SessionCloseNotice` 只清理目标 session 对应的盘对象，不波及其他仍然存活的 session

另外补充一条 session 关闭后的 connection 收束规则：

- 当一个 session 进入 closed 后，rust-cli 才允许检查该 connection 是否应主动关闭
- 检查时必须同时确认该 connection 下已经没有：
  - 已打开 session
  - open 过程
  - auth 过程
  - 已签发但未失效的 `AuthGrant`
- 只有上述对象全部为空时，才主动关闭这条 connection
- 这条 connection 清理逻辑只允许发生在 session 关闭路径上
- 不能做成“任意时刻只要观察到 connection 为空就立刻关闭”的通用即时回收

session 关闭事件包括至少：

- client 主动 `Close` 成功
- 收到 `SessionCloseNotice`
- 本地把目标 session 明确收束为 closed 的其他等价路径

说明：

- 这属于 rust-cli 宿主策略
- 不回写到正式 network 文档

## 9. Phase R6: heartbeat 与 `whole` 兼容

需要完成：

- `ConnHeartbeat` 只挂在 `GatewayConnection`
- `DiskSession` 与 `NetworkMedia` 不自行维护 heartbeat
- 对 `role = gateway` 与 `role = whole` 统一复用同一套 client-facing 主链
- `whole` 下仍完整经过 `Hello -> transport -> auth -> session -> metadata -> data plane`

验收标准：

- rust-cli 不再出现 session-scoped heartbeat 分叉
- rust-cli 连 `whole` 与连独立 `gateway` 的 connection 复用与 grant/session 语义一致

## 10. 测试

至少补齐：

- 同一 gateway、同一条 connection、连续两次 `auth` 后两个 `AuthGrant` 并存
- 同一 gateway 下盘 A 已打开后，再打开盘 B，两个 `DiskSession` 并存
- 盘 A 已打开时，盘 B 的 auth/open 不会使盘 A 被卸载
- 已打开 session 的读写可与后续新的 auth/open 串行过程共存
- 第二次 `auth` 失败不影响第一次 `auth` 得到的 grant 和已有 session
- `SessionCloseNotice` 只清理目标盘对象
- session 关闭后若该 connection 下已无 session、无 open 过程、无 auth 过程、无 `AuthGrant`，则主动关闭该 connection
- session 关闭后若该 connection 下仍有其他 session 或 grant，connection 保持存活
- 不能因为某次普通状态扫描观察到 connection 当前为空，就直接触发 connection 关闭
- connection 死亡会清理该 connection 下全部 grant / session / `NetworkMedia`
- `gateway` 与 `whole` 两种 endpoint 都满足上述语义

最终验收：

- `cargo test` 通过
