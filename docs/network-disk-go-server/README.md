# 网络盘 Go Server

当前文档按职责拆分，避免把传输层、认证层、路由层、数据面约束全部挤在一份文档里。

## 文档索引

- [总览](overview.md)
- [Client-and-Gateway 业务层协议 SDK](client-and-gateway.md)
- [Gateway-and-Storer 业务层协议](gateway-and-storer.md)
- [传输层协议](transport.md)
- [认证与路由](auth-routing.md)
- [数据面最小闭环](data-plane.md)

## 当前实现目标

当前阶段只做网络盘最小闭环，不直接做真正的分布式存储系统。

当前第一版已收口为：

- `whole` 角色：`storer` 自带 `embedded gateway`
- `storer` 角色：只持有后端存储并主动注册到独立 `gateway`
- `gateway` 为独立可执行文件，不属于 `storer` 的 `role`
- 单网盘
- `client` 只连 `gateway`

当前 `gateway-and-storer` 已明确不重新发明一整套数据面业务协议，而是拆成：

1. 注册阶段
2. 复用现有 `SessionOpen / ReadAt / WriteAt / Ping / Close`

目标路径：

1. `storer` 持有一块真实远端盘
2. `gateway` 负责认证、路由、转发
3. `client` 输入服务器地址 + 领盘码
4. `client` 只连接这一处对外入口
5. `gateway` 认证成功后为 `client` 打开远端盘会话
6. `client` 通过 `NetworkMedia` 使用该会话
7. `BackendRust` 将该远端盘作为 `Media` 使用

唯一主路径：

```text
client <-> gateway <-> storer
```

部署形态：

```text
whole  = storer(embedded gateway)
storer = external gateway + external storer
```

## 当前启动方式

- `cmd/gateway`
  - 独立可执行文件
  - 读取可执行文件同目录 `config.toml`
  - 启动 client-facing 与 storer-facing 两个监听入口
- `cmd/storer`
  - 独立可执行文件
  - 读取可执行文件同目录 `config.toml`
  - 内部只支持 `role = whole | storer`
  - `role = whole` 时直接对 client 提供完整服务
  - `role = storer` 时主动长连外部 `gateway`

当前已完成真实独立联调：

- `gateway + storer + windows/rust-cli/src/bin/network-auth-open.rs`
- 已验证：
  - auth
  - open
  - busy
  - close
  - reopen

## 当前已定口径

1. `server` 使用 Go
2. `gateway` 与 `storer` 拆成两个可执行文件
3. `storer` 可执行文件内部只支持 `role = whole | storer`
4. `gateway` 是独立程序，不进入 `storer.role`
5. `client-and-gateway.md` 是当前第一版唯一正式业务协议 SDK
6. `gateway-and-storer.md` 只单独定义注册阶段和复用规则
7. 认证流程为 `disk_id -> challenge -> proof`
8. `gateway` 预缓存 `storer` 路由与 `auth_verifier`
9. `gateway` 本地完成 `proof` 校验
10. `client` 只连接对外 gateway 入口，不直接理解 `storer` 内部结构
11. 认证成功后只获得当前连接上的开会话资格，不直接得到 `session_id`
12. 假盘不分配完整 pending 表，不进入真实数据路径
13. 同一 `disk_id` 的多连接认证互不覆盖
14. 多登录后的权限策略属于 `storer`，不属于认证层
15. 所有 `AuthFinish` 失败统一随机延迟 `2-5s`
16. 数据面第一版允许多 `DiskSession` 并发复用同一条 `client -> gateway` TCP 连接
17. 第一版 `NetworkMedia` 不做断线重连、不做本地写缓存、不做自动重试
18. `gateway <-> storer` 注册成功后复用 `SessionOpen / ReadAt / WriteAt / Ping / Close` 语义
19. `gateway` 对 `request_id` 和 `session_id` 负责本地映射，不做裸字节盲转发

当前 `client-and-gateway` 业务协议必须拆成三段：

1. 认证层：`AuthStart / AuthFinish`
2. 会话建立层：`SessionOpen`
3. 数据面层：`ReadAt / WriteAt / Ping / Close`

硬约束：

- 认证成功 != 会话已建立
- 认证成功只授予“申请打开该盘会话”的资格
- 只有 `SessionOpen` 成功后，才能创建 `DiskSession`
- 第一版 `SessionOpen` 采用单盘独占策略；已有活跃会话时返回 busy

## 当前联调工具

为避免 `BackendRust / AppKernel / KMDF` 宿主单例影响协议联调，当前提供一个独立协议联调工具：

- `windows/rust-cli/src/bin/network-auth-open.rs`

用途：

- 保持一条真实 `GatewayConnection`
- 显式执行 `auth`
- 显式执行 `open`
- 在 `disk-busy` 后保持同一连接不退出
- 在远端释放会话后，用同一连接再次 `open`

## 当前明确不做

- 多副本
- 分片
- 元数据协调
- 自动恢复
- rebalance
- 断线自动重连
- `client -> storer` 直连
- 多条 `client -> gateway` 连接池
- locator
- 隐藏预认证
- gateway 与 storer 间的强节点认证
