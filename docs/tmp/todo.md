# AppKernel 当前 Todo

当前轮待办只看本文件；已完成事实已归档到 [progress/README.md](../progress/README.md) 和对应日期进度文件。

## 当前总目标

继续按 [开发原则](../development-principles.md) 重建 `windows/AppKernel`，把当前仅可编译的 DLL 骨架推进到真正可用的 `session` 最小闭环。

## 当前边界

- 已完成 `Step 1`：
  - `AppKernel` 已独立成 `DLL` 子项目。
  - 公开 `C ABI`、`AK_*` / `Ak*` 前缀、基础 `state/stats`、`media_ops`、事件结构已经固定。
  - `MSVC` / `MinGW` 双工具链均已完成真实编译验证。
- 当前还没有：
  - 打开 `KMDF control device`
  - `SessionId` 查询
  - heartbeat 线程
  - session break/close drain
  - 真实事件队列
  - 真实协议 transport
- 当前禁止动作：
  - 不把会话逻辑临时塞回 `RWTestApp`
  - 不新增第二套宿主侧直通 `DeviceIoControl` 数据面
  - 不为了后续步骤提前堆 disk runtime、slot worker、写 finalize 逻辑

## 剩余子步骤

1. `Step 2`：实现 session 层最小闭环。
2. `Step 3`：实现 session 级事件队列。
3. `Step 4`：实现协议基础层。
4. `Step 5`：实现 per-disk runtime 骨架。
5. `Step 6`：实现读路径。
6. `Step 7`：实现写路径和最终裁决。
7. `Step 8`：宿主接入与旧路径删除。
8. `Step 9`：编译与验证收口。
9. `Step 10`：补 SDK 文档。

## 当前唯一下一步

实现 `Step 2`：把 `AkOpen / AkClose / AkQuerySessionState / AkQuerySessionStats` 从当前占位实现推进到真实 `KMDF session` 最小闭环，收口 `control device open`、`QueryInfo/SessionId`、heartbeat 线程与同步关闭链路。
