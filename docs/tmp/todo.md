# Shared Storer 重建执行清单

## 1. 目标

本清单只服务于 `docs/tmp/shared-storer-draft.md` 的落地。

本轮目标不是在现有“单导出 storer”上打补丁，而是按重建口径把 server 侧直接重构到 shared storer 正式模型：

- `claim_code_rw` 是唯一权威读写入口
- `claim_code_ro` 是可选共享只读入口
- `rw route` 与 `ro route` 是两个独立导出
- 同一个 storer 概念上只存在一个 writer
- `rw session` 全局独占
- `ro session` 多开共享
- `rw session` 与多个 `ro session` 会话层可并存
- 数据面固定为：
  - 读读并行
  - 写写互斥
  - 读写互斥

## 2. 重建约束

### 2.1 必须保持

- 保持 `auth -> session open -> data plane` 主模型不变
- 保持正式心跳边界不变：
  - `client(connection) -> gateway : ConnHeartbeat`
  - `gateway -> storer : LinkHeartbeat`
- 不引入 session-scoped heartbeat
- 不引入 session TTL
- 不引入新的 `SessionOpen` 模式位
- 关闭语义以前置 `docs/tmp/todo-close.md` 为准
- 不做历史兼容桥
- 不保留单 `claim_code` 和双 `claim_code` 并存的长期过渡结构
- 不保留单导出 `Core` 和双导出 `Core` 并存的长期过渡结构

### 2.2 明确不做

- 不做多 writer
- 不做 snapshot read
- 不做多版本只读视图
- 不做指数退避、supervisor、actor 化的复杂重连框架
- 不做 locator / 分片 / 多副本
- 不把一个 storer 进程扩成多盘进程

### 2.3 代码组织要求

按开发原则执行：

- 相同前缀的模块优先改成目录，而不是继续平铺 `xxx_*.go`
- 不新增“旧结构 + 新结构并存”的补丁层
- 能直接替换的单导出接口就直接替换
- 重构后的公开接口只保留 shared storer 所需最小面

## 3. 当前代码现状

### 3.1 已经可复用的部分

这些能力本身不需要推翻，只需要补测试防回退：

- `server/internal/route/Registry`
  - 已经按 `disk_id` 建索引
  - 可以容纳同一 storer 进程注册两个不同 `disk_id`
- `server/internal/proto/StorerRegister`
  - 已经承载 `disk_id / auth_verifier / disk_size_bytes / read_only / max_io_bytes`
- gateway 的 session 映射与 route 映射
  - 已经按 `route_connection_id` 和 `gateway_session_id` 维护
- gateway 的 client 断线清理
  - 当前旧实现已经是按 client connection 枚举 session，再逐个向上游发 `Close`
  - 这条旧关闭路径的协议重建由 `todo-close.md` 负责
- gateway 的 route 断线清理
  - 当前已经是按 route connection 清理，不是按整个进程清理

### 3.2 当前明确阻塞点

#### close 前置重构尚未完成

shared storer 默认依赖 `docs/tmp/todo-close.md` 先落地。

也就是说：

- `docs/network/define` 的关闭语义
- `windows/network-core` 的关闭语义
- server 当前关闭协议与 handler 路径

都应先按 `todo-close.md` 完成统一重建，再进入 shared storer 主线。

#### 配置层仍是单 claim code

当前 `server/internal/config/config.go` 只存在：

- `ClaimCode string`

这意味着：

- 不能表达 `claim_code_rw`
- 不能表达 `claim_code_ro`
- 不能校验 `disk_id_rw != disk_id_ro`
- `cmd/storer` 启动日志和配置初始化也都默认只有一个 `disk_id`

#### storer core 仍是单导出结构

当前 `server/internal/storer/local_disk.go` 和 `server/internal/storer/core.go` 是：

- 一个 `localDisk`
- 一个 `auth.Material`
- 一个 `session.Metadata`
- 一个 `session.Service`

这意味着：

- 无法表达 `rwExport + roExport`
- 无法让两条导出共用一个 backend
- 无法让两条导出拥有不同的 `ReadOnly` metadata

#### session manager 只有独占实现

当前 `server/internal/session/manager.go` 的行为是：

- 只要已有任意 live session
- 后续 open 一律 reject

这意味着：

- `rw` 独占可以保留
- `ro` 多开根本无处承载

#### session.Service 依赖具体 manager 实现

当前 `server/internal/session/service.go` 直接依赖单一 `*Manager`。

这意味着：

- 不能自然接 `exclusive manager`
- 也不能自然接 `shared manager`

#### 文件 backend 仍是全串行 mutex

当前 `server/internal/storage/file/backend.go` 是：

- 单 `sync.Mutex`
- `ReadAt / WriteAt / Close` 全部串行

这意味着：

- 现状是“所有读写全串行”
- 不符合 shared storer 目标里的“读读并行”

#### role=storer 运行时只有一条 link

当前 `server/internal/storer/role_runtime.go` 只有：

- 一个 `LinkRuntime`
- 一份 `RegisterInfo`
- 一份 `SessionService`
- link 结束后直接结束该次 runtime 运行

这意味着：

- 不能表达 `rwLinkRuntime`
- 不能表达 `roLinkRuntime`
- 不能为两个导出分别喂狗、分别注册、分别承接数据面
- 不能在 route 断开后继续存活并重连

#### role=whole 只有一个内嵌 route

当前 `server/internal/storer/gateway/local_adapter.go` 和 `server/internal/storer/whole_runtime.go` 只有：

- 一个本地 route registry entry
- 一份 session service
- 一个固定 `WholeRouteConnectionID`

这意味着：

- whole 还停留在“本地唯一盘”
- 无法承载 `disk_id_rw + disk_id_ro`
- 无法把本地 `rw` / `ro` 请求路由到不同 session service

### 3.3 当前需要重点防回退的地方

shared storer 落地时，最容易被误改坏的是 gateway 故障路径。

当前必须保持的正式作用域是：

- client 断线：
  - 只关闭该 client 自己名下的 session
- route 断线：
  - 只关闭该 route 自己名下的 session
- 单 session 失败：
  - 只关闭该 session

shared storer 下必须继续保持：

- `rw route` 死，只影响 `disk_id_rw`
- `ro route` 死，只影响 `disk_id_ro`
- 一个 `ro` client 死亡，不能关闭整个 `ro route`
- 不能引入“按整个 storer 进程整体清理”的 gateway 路径

## 4. 目标结构

## 4.1 配置目标

`StorerConfig` 直接重建为：

- `ClaimCodeRW string`
- `ClaimCodeRO string`

规则固定为：

- `ClaimCodeRW` 必填
- `ClaimCodeRO` 可空
- `ClaimCodeRO` 非空时，必须保证派生出的 `disk_id_ro != disk_id_rw`

## 4.2 session 目标

`session.Service` 必须改成依赖 session manager 抽象，而不是当前单 concrete type。

最终固定存在两种 manager：

- `exclusive`
  - 仅允许一个 live session
- `shared`
  - 允许多个 live session

session 生命周期与 `closing/drain` 语义以前置 `docs/tmp/todo-close.md` 为准。

shared storer 在这个前提上只额外要求：

- `rw session` 的独占位只能在 drain 完成后释放
- `ro session` 共享多开不改变既有 close 生命周期边界

## 4.3 storer core 目标

storer core 直接重建为：

- `sharedStorage`
  - 单 `Backend`
  - 单 `*os.File`
  - 单 `sync.RWMutex`
- `rwExport`
  - `claim_code_rw`
  - `disk_id_rw`
  - `ReadOnly=false`
  - `exclusive manager`
- `roExport`
  - 可选
  - `claim_code_ro`
  - `disk_id_ro`
  - `ReadOnly=true`
  - `shared manager`

## 4.4 runtime 目标

### role=storer

最终固定为：

- `rwLinkRuntime`
- `roLinkRuntime`
- 每条 link 各自带一个简单重连状态机

其中：

- `rwLinkRuntime` 总是存在
- `roLinkRuntime` 仅在 `ClaimCodeRO` 非空时存在
- 每条 link 断开后都按固定 `5s` 节拍重连

重连节拍固定为：

```text
连接开始 -> 连接失败 -> 等待 5s -> 再次连接开始 -> 连接失败 -> 等待 5s -> ...
```

也就是说：

- 不是“失败后立刻重试”
- 而是“每次失败后静默等待完整 5 秒，再发起下一次连接”

重连日志固定为防刷屏口径：

- 某条 link 第一次进入重连态时：
  - 打印一次 `rw重连中...` 或 `ro重连中...`
- 重连态期间：
  - 只静默中间重复的连接失败日志
  - 不额外静默其他正常运行日志
- 某条 link 重连成功时：
  - 打印一次 `rw重连成功` 或 `ro重连成功`
- 成功后：
  - 再恢复普通运行日志输出

注意：

- `rw` 与 `ro` 的重连状态机彼此独立
- 一个 link 进入重连，不应把另一个 link 也拖进重连态

### role=whole

最终固定为：

- 对 client 暴露一个 listener
- 内嵌 gateway core
- 注册本地 `rw route`
- 可选注册本地 `ro route`

注意：

- whole 不对其他 storer 暴露 storer listener
- whole 下的 `rw/ro` 也必须是两个独立导出

## 5. 推荐目录重组

按开发原则，shared storer 落地时建议直接重组到目录化结构，避免继续平铺前缀文件。

建议目标：

```text
server/internal/session/
  manager/
    interface.go
    exclusive.go
    shared.go
  service.go
  metadata.go
  record.go

server/internal/storer/
  core/
    shared_storage.go
    export.go
    core.go
  runtime/
    role.go
    storer.go
    whole.go
  gateway/
    local_adapter.go
    link/
      runtime.go
      register_client.go
      data_plane_handler.go
```

本轮不要继续新增这类平铺文件：

- `session_xxx.go`
- `storer_xxx.go`
- `shared_storer_xxx.go`

## 6. 分阶段任务

### 阶段 A：先重建配置与启动口径

目标：

- 先把 shared storer 的输入模型立起来
- 消灭“单 claim code / 单 disk_id”的配置假设

任务：

- 重建 `server/internal/config/StorerConfig`
  - `ClaimCode` 替换为 `ClaimCodeRW / ClaimCodeRO`
- 重写：
  - `LoadStorer`
  - `SaveStorer`
  - `Validate`
  - `promptAndCreateStorer`
- `Validate` 必须直接保证：
  - `ClaimCodeRW` 合法
  - `ClaimCodeRO` 为空或合法
  - `disk_id_rw != disk_id_ro`
- 更新 `server/cmd/storer/main.go`
  - 日志不再假设只有一个 `disk_id`
  - 明确打印：
    - `disk_id_rw`
    - `disk_id_ro` 或 `ro_disabled`

验收：

- 配置文件不再出现旧 `claim_code`
- `role=whole` / `role=storer` 都能表达 shared storer
- 启动日志不再调用单值 `DiskID()`

### 阶段 B：重建 session manager 抽象

目标：

- 把当前单独占 manager 拆成抽象 + 两个实现

任务：

- 为 `session.Service` 提取 manager 接口
- 将当前 `server/internal/session/manager.go` 的行为保留为：
  - `exclusive manager`
- 新增：
  - `shared manager`
- 复用 `todo-close.md` 已完成的 session lifecycle 抽象
- 保持 `session` 错误语义不变：
  - `ErrSessionOpenRejected`
  - `ErrSessionUnavailable`
  - `ErrReadOnly`

验收：

- `session.Service` 可同时接：
  - `exclusive manager`
  - `shared manager`
- 当前 gateway / whole / storer 调用面不再依赖单 concrete manager
- `rw session` 在 `closing` 且仍有在途写入时，不能被新的 `rw open` 抢占

### 阶段 C：重建 shared storage

目标：

- 把数据面读写语义收口在 backend 末端

任务：

- 重写 `server/internal/storage/file/Backend`
- 将当前单 `Mutex` 改为：
  - `sync.RWMutex`
- 行为固定为：
  - `ReadAt` 用 `RLock`
  - `WriteAt` 用 `Lock`
  - `Close` 用 `Lock`
- 保持：
  - `Path()`
  - `Size()`
  - `ReadOnly()`
  - 越界与 I/O 错误映射

验收：

- 读读可并行
- 写与读写互斥
- 没有在更上层再叠加第二把 I/O 锁

### 阶段 D：重建 storer core

目标：

- 从“单 `localDisk` + 单 `session.Service`”重建为 shared storage + dual export

任务：

- 删除当前单导出 `localDisk` 假设
- 新建 `sharedStorage`
- 新建 export 结构：
  - `rwExport`
  - `roExport`
- `Core` 改为持有：
  - `sharedStorage`
  - 必选 `rwExport`
  - 可选 `roExport`
- `Core` 对外不再暴露：
  - `DiskID()`
  - `SessionService()`
  - `RouteEntry()`
  - `GatewayRegisterInfo()`
- `Core` 对外改为暴露 export 级能力：
  - 列出导出
  - 按导出返回 route metadata
  - 按导出返回 register info
  - 按导出返回 session service

验收：

- `rwExport` 和 `roExport` 共用一个 backend
- 两者 metadata 正确分离：
  - `rw.ReadOnly=false`
  - `ro.ReadOnly=true`
- 不再存在任何“单一盘导出”的 core API

### 阶段 E：重建 role=storer 双 link runtime

目标：

- 外部 storer 形态能同时向 gateway 发布 `rw route` 和 `ro route`

任务：

- 重建 `StorerRuntime`
- 将当前单 `linkRuntime` 改成：
  - `rwLinkRuntime`
  - 可选 `roLinkRuntime`
- 每条 link 都必须：
  - 单独注册
  - 单独 heartbeat
  - 单独承接数据面
  - 单独维护上游 session 生命周期
  - 单独维护自己的重连状态机
- `ClaimCodeRO` 为空时：
  - 不创建 `roLinkRuntime`
- 更新 runtime 对外状态描述：
  - 不再假设“一个 runtime 只有一个 disk_id”

重连行为固定为：

- gateway-storer 连接断开后
  - 先按既有约定完成该导出的 route/session 清理
  - 再进入该 link 自己的重连态
- 下一次拨号固定在失败后 `5s` 发起
- 同步拨号失败后继续等待 `5s`
- 中间重复的重连失败日志默认静默，不逐次刷日志
- 其他运行日志不额外静默

建议实现方式：

- 每条 link runtime 各自维护简单状态机：
  - `running`
  - `retry_wait`
  - `reconnecting`
- 状态机只服务于：
  - 控制固定重连节拍
  - 控制防刷屏日志
- 不在这里引入更复杂的 supervisor / actor / backoff 策略

必须删除当前旧行为：

- `role=storer` 下 gateway-storer 连接一旦断开就结束该次 runtime 运行
- `linkRuntime.Run(...)` 返回错误后直接把整个 storer 运行带出

重建后固定改为：

- route 断开只结束该次连接
- 不结束整个 storer 进程运行
- 清理完成后立即进入固定节拍重连逻辑

必须额外明确一条实现决策：

- dual link 的监护策略不能回退成“任意一条 link 出错就直接按整进程单导出思路处理”
- runtime 内部必须按导出边界组织资源与清理

验收：

- `role=storer` 在 `ro` 关闭时只注册一条 route
- `role=storer` 在 `ro` 开启时注册两条 route
- 两条 route 的 `disk_id / read_only / auth_verifier` 正确独立
- `rw` link 断线后按固定 `5s` 节拍重连
- `ro` link 断线后按固定 `5s` 节拍重连
- 重连期间日志不刷屏，只在进入重连态和重连成功时打印一次
- gateway-storer 断线后 storer 进程不退出

### 阶段 F：重建 role=whole 双本地导出

目标：

- whole 模式下也变成 shared storer 的完整双导出模型

任务：

- 重建 `server/internal/storer/gateway/local_adapter.go`
- 本地 route registry 必须支持：
  - `disk_id_rw`
  - 可选 `disk_id_ro`
- 本地 data plane dispatch 必须按导出路由到正确 session service
- whole 模式下本地 route connection id 不应继续只有一个魔法常量
- `rw` 与 `ro` 本地导出应使用不同的本地 route identity

验收：

- whole 下 client 看到的仍是标准 client-gateway 协议
- whole 下可独立打开：
  - 一个 `rw session`
  - 多个 `ro session`
- whole 下 `rw` / `ro` 不会混用同一 session service

### 阶段 G：gateway 侧补强与回归保护

目标：

- gateway 逻辑尽量不重写
- 但要把 shared storer 需要的作用域边界用测试钉死

任务：

- 审视并保持以下代码路径的最小作用域：
  - client connection 死亡
  - route connection 死亡
  - 单 session `SessionUnavailable`
  - open reject
- 基于 `todo-close.md` 已固定的关闭语义，确保 cleanup 作用域仍严格落在：
  - session
  - client connection
  - route connection
- 若发现实现里仍有“按整个 storer 整体清理”的暗含假设，直接删掉
- 不新增“storer 进程级 cleanup facade”

验收：

- 一个 `ro` client 断线，只关闭它自己的 session
- `ro route` 死亡，不影响 `rw route`
- `rw route` 死亡，不影响 `ro route`
- 单 session 请求失败，不升级成整 route 故障

### 阶段 H：清理单导出残留 API 与日志

目标：

- 删除 shared storer 完成后不再成立的单导出公开面

任务：

- 删除或替换：
  - `RoleRuntime.DiskID()`
  - 所有假设“只有一个 disk_id”的日志
  - 所有假设“只有一个 SessionService”的 helper
- 清理旧命名和旧注释
- 保证代码阅读时只看到 shared storer 模型

验收：

- 代码中不再有“单导出 storer”对外 API
- 日志、注释、测试命名都与 shared storer 一致

## 7. 测试清单

### 7.1 配置层

- `ClaimCodeRW` 必填
- `ClaimCodeRO` 可空
- `ClaimCodeRO` 非空时，`disk_id_ro != disk_id_rw`
- `SaveStorer/LoadStorer` 对双 claim code 正确往返

### 7.2 session manager

- exclusive manager：
  - 第一个 open 成功
  - 第二个 open reject
- shared manager：
  - 多个 open success
  - `Close` 只关闭目标 session
  - `CloseConnection` 只关闭该 connection 名下 session
  - `rw session` 必须 drain 后才释放独占位

### 7.3 backend

- 读读可并行
- 写与读互斥
- 写与写互斥
- `Close` 与在途访问不产生悬空 file handle

### 7.4 storer core

- 仅 `rw` 时：
  - 只有一个 export
- `rw + ro` 时：
  - 有两个 export
  - 两个 export 共用一个 backend
  - 两个 export metadata 正确分离

### 7.5 role=storer

- `ClaimCodeRO` 为空时只注册一条 route
- `ClaimCodeRO` 非空时注册两条 route
- `rw route` metadata 正确
- `ro route` metadata 正确
- `ro` 写入返回 `IOReadOnly`
- `rw` link 断线后进入固定 `5s` 重连节拍
- `ro` link 断线后进入固定 `5s` 重连节拍
- `rw` 重连期间只打印一次 `rw重连中...`
- `ro` 重连期间只打印一次 `ro重连中...`
- `rw` 重连成功后只打印一次 `rw重连成功`
- `ro` 重连成功后只打印一次 `ro重连成功`
- `rw` 重连失败过程中不逐次刷失败日志
- `ro` 重连失败过程中不逐次刷失败日志
- 断线后 runtime 不退出，而是完成清理后继续常驻重连

### 7.6 role=whole

- 仅 `rw` 时可正常 auth/open/read/write
- 开启 `ro` 后：
  - `rw` 可独占 open
  - 多个 `ro` 可并存 open
  - `rw` 与 `ro` 可会话层共存

### 7.7 gateway 故障矩阵

- 一个 client 断线，只关闭它自己的 `ro session`
- 一个 client 持有 writer 断线后，writer 被释放，其他 `ro session` 继续存在
- `ro route` 死亡，不关闭 `rw session`
- `rw route` 死亡，不关闭 `ro session`
- 单 session `SessionUnavailable` 不升级成 route 故障
- open reject 不清理已有 `ro session`

### 7.8 端到端集成

- 两个 client 同时打开同一个 `ro` 导出成功
- 一个 client 打开 `rw` 导出成功，同时多个 client 打开 `ro` 成功
- writer 写入后，reader 后续重读能看到新数据
- writer 二次 open reject
- `ro` client 写入失败且不影响其他 `ro` 会话

## 8. 最终验收标准

满足以下条件才算 shared storer 重建完成：

- server 代码中不再保留单导出 storer 的核心结构
- `rw` / `ro` 双导出在 `role=storer` 和 `role=whole` 下都成立
- gateway 故障路径严格按：
  - session
  - client connection
  - route connection
  三个作用域收束
- 不出现“多 writer”入口
- 不引入新的协议分支或兼容桥
- `go test ./server/...` 全通过
