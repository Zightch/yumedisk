# YumeDisk

**远程块设备挂载系统** — 为 Windows 提供本地磁盘语义的远程存储挂载方案。

## 为什么需要 YumeDisk？

现有远程传输方案各有痛点：

### SMB/NFS

- 依赖固定协议端口 (SMB: 445, NFS: 2049)，需要额外防火墙/NAT 配置
- 在公网和共享网络环境下部署困难
- 穿透 NAT 往往需要 VPN 或复杂的端口转发方案
- 对多租户、共享 IP 环境不够友好

### iSCSI

- 自带发现模型和主机 IP 语义，绑定过死
- 在共享 IP 的机房环境里部署和运维都不够轻量
- 多租户隔离复杂，容易产生冲突
- 配置 IQN、Portal、Target 映射链路较长

### FTP / Web 网盘 / 专有客户端

- 不能直接给工作软件提供"本地磁盘"语义
- 需要额外的同步软件或手动上传下载
- 无法像本地磁盘一样直接运行程序、编辑文件、挂载为工作目录
- 工作软件需要适配特定的存储接口，无法透明使用

### YumeDisk 的解决方案

YumeDisk 填补了这些空隙：

| 痛点 | YumeDisk 方案 |
|------|---------------|
| 端口固定、NAT 穿透困难 | 客户端主动连出，单端口打通，端口可改 |
| 无本地磁盘语义 | 挂载为系统块设备，工作软件直接按本地磁盘使用 |
| 后端绑定死 | 服务端后端抽象可替换，支持内存盘、文件盘等 |
| 共享 IP 环境部署难 | 客户端主动建立连接，无需服务端主动入站 |

## 核心能力

- 支持多客户端并发访问 (1 RW + N RO)
- Journal + Finalize 写语义保证一致性
- Commit Epoch 提供版本校验基准
- 断线冻结与自动重连机制

## 架构概览

```
Client Host                     Server Host
┌──────────────────┐            ┌──────────────────┐
│ YumeCtl ──▶ YumeAgent         │    YumeServer    │
│              │                 │        │         │
│         AppKernel ◀──Network──▶ VolumeRuntime    │
│              │                 │        │         │
│     Kernel Drivers             │   VolumeBackend │
└──────────────────┘            └──────────────────┘
```

组件职责简述：
- **Kernel Drivers** — SCSI 适配与 KMDF 框架
- **AppKernel** — 用户态数据面内核 (DLL)
- **YumeAgent** — 本地常驻服务，管理挂载与连接
- **YumeServer** — 服务端进程，管理卷与租约
- **VolumeBackend** — 存储后端适配

## 技术文档

详细设计与 SDK 文档请参阅：

- [应用层设计草案](docs/tmp/yumedisk-application-layer-design-draft.md) — 产品定位、一致性模型、协议设计
- [AppKernel 设计文档](windows/AppKernel/AppKernel设计文档.md) — 架构分层、接口定义、写路径语义
- [AppKernel SDK 文档](windows/AppKernel/AppKernel-SDK文档.md) — DLL 接入指南
- [开发原则](docs/development-principles.md) — 极简核心、单一真实来源、边界闸口等原则

## 开发状态

核心架构已落地，服务端与控制端正在开发中。

## License

MIT License

---

**YumeDisk** — 不是文件同步器，不是 SMB 套壳，不是 Web 网盘客户端。

是一个真正意义上的**远程块设备挂载系统**。