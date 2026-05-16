# 网络盘 Go Server 草案

当前草案按职责拆分，避免把传输层、认证层、路由层、数据面约束全部挤在一份文档里。

## 文档索引

- [总览](overview.md)
- [Client-and-Gateway 业务层协议 SDK](client-and-gateway.md)
- [传输层协议](transport.md)
- [认证与路由](auth-routing.md)
- [数据面最小闭环](data-plane.md)

## 当前目标

当前阶段先只做网络盘最小闭环，不直接做真正的分布式存储系统。

当前第一版实现目标先收敛为：

- `storer` 自带 `embedded gateway`
- 单网盘
- 单部署点
- `client` 只连这一处对外服务入口

`gateway-and-storer` 独立业务协议暂时放后，不作为当前第一版实现前提。

目标路径：

1. `storer` 持有一块真实远端盘
2. `embedded gateway` 负责认证、路由、转发
3. `client` 输入服务器地址 + 领盘码
4. `client` 只连接这一处对外入口
5. `embedded gateway` 认证成功后为 `client` 打开远端盘会话
6. `client` 通过 `NetworkMedia` 使用该会话
7. `BackendRust` 将该远端盘作为 `Media` 使用

唯一主路径：

```text
client <-> storer(embedded gateway)
```

当前不保留：

```text
client <-> gateway
client <-> storer
```

## 当前已定口径

1. `server` 使用 Go
2. `gateway` 与 `storer` 拆成两个子项目
3. `storer` 支持 embedded gateway 形态
4. `client-and-gateway.md` 是当前第一版唯一正式业务协议 SDK
5. `gateway-and-storer` 独立业务协议暂不进入当前实现
6. 认证流程为 `disk_id -> challenge -> proof`
7. `gateway` 预缓存 `storer` 路由与 `auth_verifier`
8. `gateway` 本地完成 `proof` 校验
9. `client` 只连接对外 gateway 入口，不直接理解 `storer` 内部结构
10. 认证成功后由 `embedded gateway` 打开并持有本地数据面会话
11. 假盘不分配完整 pending 表，不进入真实数据路径
12. 同一 `disk_id` 的多连接认证互不覆盖
13. 多登录后的权限策略属于 `storer`，不属于认证层
14. 所有 `AuthFinish` 失败统一随机延迟 `2-5s`
15. 数据面第一版允许多 `DiskSession` 并发复用同一条 `client -> gateway` TCP 连接
16. 第一版 `NetworkMedia` 不做断线重连、不做本地写缓存、不做自动重试

## 当前明确不做

- 多副本
- 分片
- 元数据协调
- 自动恢复
- rebalance
- 断线自动重连
- `client -> storer` 直连
- 独立 `gateway-and-storer` 业务协议实现
- 多条 `client -> gateway` 连接池
- locator
- 隐藏预认证
- gateway 与 storer 间的强节点认证
