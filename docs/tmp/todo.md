# 当前待办总表

## 1. 本轮目标

本轮不直接改代码，先把网络盘文档体系收成两层：

- `network` 正式文档只定义协议模型、消息边界、字段、方向、时序和外部可见事实。
- `client` 实现文档只定义当前 `rust-cli` 的对象结构、宿主策略和当前实现限制。
- `server` 实现文档只定义当前 `server` 的角色结构、内部状态真源、当前实现限制和当前项目策略。

这一步的目标不是补说明，而是把当前 `network` 文档中越界承接了实现细节的内容全部收出来，后续分别改写到正确层级。

## 2. 文档分层固定口径

### 2.1 `network` 文档只允许定义

- wire 协议头、body、字节序、frame 结构
- `request_id / auth_id / session_id` 的协议语义
- `AuthStart / AuthFinish / SessionOpen / SessionDescribe / ReadAt / WriteAt / Close`
- `ConnHeartbeat / LinkHeartbeat / SessionCloseNotice` 的消息方向与基础语义
- auth/open 的 in-flight lane 互斥
- 已打开 session 可以并发复用 connection
- `SessionOpen` 不包含 metadata，metadata 需会话打开后再查询
- connection / session / route 的失效事实

### 2.2 `network` 文档不再定义

- 当前项目的内部对象结构
- 当前项目的角色部署方式
- 当前项目的 registry / snapshot / adapter / watchdog / cleanup 实现
- 当前项目的 host 清理策略
- 当前项目的 `whole` 内嵌结构
- 当前项目的 `SessionOpen` 失败类型和失败 body
- 当前项目的 `HelloResponse.server_capabilities` 当前负载内容
- 当前项目的 metadata 真源与复制路径

### 2.3 `client` 实现文档负责承接

- `GatewayConnection / AuthGrant / DiskSession / NetworkMedia`
- connection endpoint 复用边界
- `NetworkMedia` 持有 `disk_id + session + metadata`
- `SessionOpen` 成功后再 `SessionDescribe`
- 读写拆片如何在 client 落地
- session 关闭后 connection 是否回收的策略
- `SessionCloseNotice / disconnect / heartbeat timeout` 后宿主如何清理盘对象

### 2.4 `server` 实现文档负责承接

- `role = gateway / storer / whole`
- route / auth grant / session 三张真源表
- route metadata 的来源和复制策略
- `HelloResponse.server_capabilities` 当前内容
- `SessionOpen` 当前 reject 策略
- challenge token 的具体编码与校验
- `whole` 的 fixed route 和本地内嵌结构
- heartbeat watchdog、route 丢失清理顺序和退出策略

### 2.5 边界留白与实现补实

这一轮收文档时要特别注意：

- `network` 文档可以保留“协议层只定义事实，不定义宿主策略”的边界。
- 但凡 `network` 文档把某个策略明确留给 client/server 自己决定，后续对应的实现文档必须把“当前项目实际采用的方案”补写出来。

不能出现的状态是：

- `network` 文档删掉了实现细节
- `client/server` 实现文档却没有把当前方案补回来
- 最终导致读者只知道“协议没规定”，却不知道“这个项目现在怎么做”

当前必须按这个原则补实的典型项包括：

- `NetworkMedia` 在 session 关闭后的收束策略
- `NetworkMedia` 在 `SessionCloseNotice / disconnect / heartbeat timeout` 后的收束策略
- session 关闭后 connection 是否回收、何时回收
- `SessionOpen` 失败后的当前项目处理策略
- `HelloResponse.server_capabilities` 当前项目返回内容
- route 丢失后的当前项目清理路径
- storer heartbeat 超时后的当前项目退出策略
- whole 本地 fixed route 的当前项目行为

## 3. 当前需要从 `network` 文档收掉的内容总表

以下内容目前散落在 `docs/network-disk-go-server/` 中，但应移出 `network` 正式文档：

1. `HelloRequest` 当前负载为空、`HelloResponse.server_capabilities` 当前负载为空、`Hello` 后直接进入 transport。
2. `client disk object`、`opened session`、`connection runtime`、`NetworkMedia` 这类 client 内部对象结构。
3. `gateway runtime / route_registry / auth_grant_registry / session_registry` 这类 server 内部结构。
4. `whole` 的监听口、fixed route、本地 storer core、内嵌 gateway core。
5. 一个 storer 进程只承载一个 `disk_id`、一个 route 只绑定一个 `disk_id`、同一时刻一个 `disk_id` 只允许一个活跃 route。
6. route metadata 真源在 gateway、`SessionOpen` 时复制 metadata、`SessionDescribe` 从 gateway 本地 session 快照回答。
7. `SessionOpen` 失败时的 `busy` 语义、`busy` 的定义、`busy` 时 `auth_id` 仍可重试。
8. challenge token 建议封装结构、随机延迟 `2..5s`、challenge 上下文清理这类认证实现策略。
9. gateway 改写 `request_id / session_id`、不缓存盘数据、只做映射转发这类 server 转发细节。
10. storer watchdog 超时主动退出、whole 本地 fixed route 不走外部 heartbeat 这类部署与运行策略。
11. route / connection / session 失效后的内部清理顺序，例如先撤销 grant、再关 session、再清映射。
12. “第一版不做什么”“第一版最小验收”“当前最小闭环”这类项目推进口径。

## 3.1 协议留白但实现文档必须补实的当前方案

以下内容后续可以继续在 `network` 文档中保留为“协议层不定义具体策略”，但必须在实现文档里写清当前项目做法：

1. `NetworkMedia` 在 session 被主动 `Close`、收到 `SessionCloseNotice`、connection 死亡、heartbeat timeout 后如何收束。
2. 客户端在 session 关闭后是否保留盘对象、是否立即卸载、是否允许挂起。
3. session 关闭后 connection 是否尝试复用、是否主动关闭、触发关闭的唯一入口是什么。
4. `SessionOpen` 失败后当前项目如何处理 `auth_id`、如何展示错误、是否允许重试。
5. `HelloResponse.server_capabilities` 当前字段内容是否为空、是否保留占位扩展。
6. storer route 丢失后 gateway 当前如何撤销 grant、关闭 session、下发 notice。
7. storer heartbeat 超时后当前进程是立即退出、报错停机还是进入其他状态。
8. `whole` 的 fixed route 与本地 storer core 失效后，当前项目如何映射为 client-facing 故障结果。

## 4. `SessionOpen` 专项收口

这是当前最需要单独处理的收口点。

### 4.1 `network` 文档后续只保留

- `SessionOpen` 请求使用 `auth_id`
- 成功时固定返回 `OK + session_id`
- 成功响应可带额外负载，但 `network` 文档不解释其业务语义
- 失败时 `network` 文档不定义失败类型
- 失败时 `network` 文档不定义失败 body
- `SessionOpen` 明确不返回 metadata
- metadata 由会话打开后再查 `SessionDescribe(session_id)`

### 4.2 必须从 `network` 文档删除

- `ERR_SESSION_BUSY` 对 `SessionOpen` 的正式语义
- “目标盘当前已有活跃 session” 这种 `busy` 定义
- “失败且返回 busy 时，auth_id 仍可重试”
- “storer 拒绝新的 open” 这种当前项目行为解释

### 4.3 必须转移到实现文档

- 当前项目实际如何 reject open
- 当前项目 reject open 时使用什么状态码
- 当前项目 reject open 时 response body 是否为空
- 当前项目是否透传 storer 的 open 失败负载

当前统一口径先固定为：

- `network` 文档不再定义 `SessionOpen` 失败类型
- 当前项目实现文档单独说明：若目标 storer 已有打开会话，则当前版本直接 `open reject`

## 5. 按文件改写任务

### 5.1 `docs/network-disk-go-server/README.md`

改写目标：

- 收成索引页 + 协议层总述
- 不再兼任项目实现总表

需要删出或下沉：

- `whole` 的具体部署表现
- 当前项目第一版不做项
- 客户端盘对象显式持有 `disk_id + session + metadata`
- `HelloResponse(server_capabilities = empty)` 这种当前实现负载
- 任何“当前项目最小闭环就是这么做”的推进口径

迁移目标：

- `whole`、角色部署、第一版范围 -> `server` 实现文档
- client 盘对象和宿主策略 -> `client` 实现文档

### 5.2 `docs/network-disk-go-server/overview.md`

改写目标：

- 保留高层概念图和协议层总模型
- 删除所有 client/server 内部组织细节

需要删出或下沉：

- `client / gateway / storer / whole` 的内部 runtime 结构
- 三张真源表的具体字段、snapshot 复制、metadata 真源
- `whole runtime` 具体组件
- 失败时 gateway 内部清理顺序
- `open` 失败中的 `busy` 语义
- “宿主如何收束盘对象”之外的具体实现路径

迁移目标：

- client runtime 结构 -> `client` 实现文档
- gateway/storer/whole runtime 结构 -> `server` 实现文档

### 5.3 `docs/network-disk-go-server/transport.md`

改写目标：

- 只保留 bootstrap 与 framed transport 的协议边界

需要删出或下沉：

- `HelloRequest` 当前空负载
- `HelloResponse.server_capabilities` 当前空负载
- 当前无 TLS 因此直接进入 transport 这种实现态结果
- client disk object / opened session / connection runtime 的对象图
- `whole` 的内嵌结构

保留前提：

- `client-facing` 先 `Hello` 后 transport
- `storer-facing` 当前协议没有 `Hello`
- frame 结构、长度头、大端序、并发承载能力

迁移目标：

- 当前 `Hello` 负载内容 -> `server` 实现文档
- client 对象结构 -> `client` 实现文档
- `whole` 部署结构 -> `server` 实现文档

### 5.4 `docs/network-disk-go-server/client-and-gateway.md`

改写目标：

- 保留纯 client-gateway 业务协议
- 删除 rust-cli 宿主与当前项目的实现解释

需要删出或下沉：

- `HelloResponse(server_capabilities = empty)`
- `ERR_SESSION_BUSY` 作为 `SessionOpen` 正式失败语义
- `SessionOpen` 失败 `busy` 后 `auth_id` 可重试
- metadata 来自 `gateway.session_registry` 的实现表述
- client 可用 `disk_id + session + metadata` 构造盘对象
- 当前最小验收章节

需要改写为协议中立表达：

- `SessionOpen` 成功响应 body 从“固定为空”改为“允许带扩展负载，但本协议当前不解释其语义”
- `SessionDescribe` 从“gateway 本地回答”改为“返回该 session 绑定的 metadata”

迁移目标：

- open reject 行为 -> `server` 实现文档
- client 建盘流程 -> `client` 实现文档
- metadata 来源 -> `server` 实现文档

### 5.5 `docs/network-disk-go-server/gateway-and-storer.md`

改写目标：

- 保留 gateway-storer 边的协议与方向
- 删除当前项目角色结构和 route 实现组织

需要删出或下沉：

- `whole` 的完整角色说明
- 一个 storer 进程只注册一个 `disk_id`
- route metadata 真源在 gateway、复制为 session 快照
- 当前 route 只承载一个 `disk_id` 的项目结构解释
- `whole` 的 fixed route 细节
- watchdog 主动退出、whole 不走外部 heartbeat 这些实现策略
- route 故障后的内部清理顺序

需要保留但改成协议表述：

- `LinkHeartbeat` 是 `gateway -> storer` 唯一心跳方向
- storer-facing `SessionOpen / ReadAt / WriteAt / Close` 是独立私有协议边

迁移目标：

- `whole / storer` 角色结构 -> `server` 实现文档
- route 清理和 watchdog -> `server` 实现文档

### 5.6 `docs/network-disk-go-server/auth-routing.md`

改写目标：

- 保留 claim code、challenge、`auth_id`、`SessionOpen(auth_id)` 的协议关系
- 删除 registry 和 token 内部实现说明

需要删出或下沉：

- route 来源是外部注册还是 whole fixed route
- `route_registry` 的字段真源说明
- challenge token 的建议 `Seal({...})` 结构
- `auth_id` 绑定字段与 `grant_state` 命名
- 随机延迟 `2..5s`
- challenge 上下文清理
- `SessionOpen` 的 `busy` 失败语义
- route 失效后的内部清理步骤

需要保留但收薄：

- `auth_id` 是 connection-scoped 授权对象
- `auth_id` 不等于长期 authorized connection
- `SessionOpen(auth_id)` 成功后只得到 `session_id`

迁移目标：

- token 编码与 grant registry -> `server` 实现文档
- open reject 策略 -> `server` 实现文档

### 5.7 `docs/network-disk-go-server/data-plane.md`

改写目标：

- 保留 `SessionDescribe / ReadAt / WriteAt / Close / heartbeat / notice` 的协议边界
- 删除 client/server 当前实现组织

需要删出或下沉：

- client 盘对象显式保存哪些字段
- client 盘对象不负责认证/心跳/重连
- metadata 来自注册 -> route -> session 快照的具体路径
- `max_io_bytes` 由客户端盘对象主动拆片
- gateway 改写 `request_id / session_id` 的具体转发步骤
- `ERR_SESSION_BUSY`
- route / connection 失效后的内部 grant/session 撤销顺序
- 当前最小验收章节

需要保留但改成协议表述：

- 大块 I/O 需要上层主动拆片
- `ConnHeartbeat` 和 `LinkHeartbeat` 的方向
- `SessionCloseNotice` 表示目标 session 已失效

迁移目标：

- I/O 拆片落地 -> `client` 实现文档
- gateway 转发表达与故障清理 -> `server` 实现文档

## 6. 新增 `client` 实现文档任务

后续需要新增或重写一份 `client` 项目实现文档，至少承接以下内容：

1. `GatewayConnection / AuthGrant / DiskSession / NetworkMedia` 的对象边界。
2. connection 以 gateway endpoint 为 key 复用。
3. auth/open 只有单 in-flight lane，但多个已签发 `auth_id` 与多个已打开 session 可以并存。
4. `SessionOpen` 成功后必须再 `SessionDescribe`，然后才构造盘对象。
5. `NetworkMedia` 显式持有 `disk_id + session + metadata`。
6. 大块 I/O 在 client 侧按 `max_io_bytes` 拆片。
7. 当前宿主在 `SessionCloseNotice / disconnect / ConnHeartbeat timeout` 后立即卸载并清理。
8. connection 空闲回收只允许发生在 session 关闭路径。
9. 当前项目如何消费 `SessionOpen` 的 reject 结果。

## 7. 新增 `server` 实现文档任务

后续需要新增或重写一份 `server` 项目实现文档，至少承接以下内容：

1. `role = gateway / storer / whole` 的正式部署形态。
2. `whole` 的内嵌 gateway、本地 storer core 与 fixed route。
3. route / auth grant / session 三张真源表及其字段归属。
4. `HelloResponse.server_capabilities` 当前返回内容。
5. route metadata 的来源、复制策略和 session snapshot 语义。
6. challenge token 的具体编码和 connection 绑定方式。
7. `SessionOpen` 当前 reject 策略：
   - 当前最小闭环下，若目标 storer 已有打开会话，则直接 `open reject`
   - 不再使用 `busy` 作为 network 正式文档语义
8. 当前项目 reject open 时实际使用的状态码与 response body 形态。
9. `gateway -> storer` heartbeat watchdog 和 storer 超时退出策略。
10. route 丢失时 grant/session/notice 的当前清理路径。

## 8. 本轮文档改写验收

本轮文档重写完成后，至少满足：

1. `docs/network-disk-go-server/` 中不再出现当前项目内部对象结构、registry 实现、whole 部署细节、watchdog 策略、host 清理策略。
2. `network` 文档中的 `SessionOpen` 不再定义 `busy` 语义，不再定义失败类型和失败 body。
3. `network` 文档仍完整保留 wire 结构、消息方向、协议顺序和失效事实。
4. `client` 实现文档能够独立解释 rust-cli 当前对象模型和宿主策略。
5. `server` 实现文档能够独立解释当前角色结构、open reject 策略、metadata 来源和故障清理路径。
