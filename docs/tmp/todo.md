# 当前总目标

重建 `BackendCore` 最小抽离闭环的剩余宿主侧收口：让 `client` 最终稳定为 `UI + backendHost + media 实现`，并把具体介质实现从旧 `backend` 过渡目录中收出来。

## 子步骤

1. 重建 `client` 介质实现归属
   - 把当前具体介质实现留在 `client` 宿主侧
   - 固定归属：
     - `client/media/FileMedia/`
     - `client/media/MemoryMedia/`
     - `client/media/RawFileMedia/`
   - 固定边界：
     - `BackendCore` 只保留 `Media` 抽象接口
     - `FileMedia` 不进入 `BackendCore`
     - 具体文件介质家族不进入 `BackendCore`
   - 目标是：
     - 让 `BackendCore` 只关心“怎么驱动盘”
     - 不关心“宿主用什么本地文件介质实现”

2. 重建最小运行验收口
   - 验收当前 `client` 运行目录：
     - `BackendCore.dll`
     - `AppKernel.dll`
   - 验收当前最小闭环下：
     - `client.exe` 可启动
     - session 状态可刷新
     - 建盘 / 删盘可走通
     - 磁盘列表可刷新
     - 日志可刷新
   - 这一阶段继续复用现有：
     - `tests/uia_test.ps1`
     - `tests/uia_scenario.ps1`

## 当前轮边界

- 当前路线只做“本进程内结构收口”，不做：
  - 多进程
  - IPC
  - 服务化
  - 插件化
  - 远程协议预埋
- `BackendCore` 只做运行时核心，不做：
  - Qt UI
  - Qt DTO
  - `FileMedia`
  - 具体 `Media` 子类实现
- `client` 继续保留：
  - `Widget`
  - `CreateDiskDialog`
  - `backendHost`
  - `FileMedia/MemoryMedia/RawFileMedia`
- 当前阶段不引入：
  - C API
  - callback table
  - 跨语言 ABI 包装
- 当前阶段默认接受：
  - `client` 与 `BackendCore` 同仓
  - 同编译器
  - 同 CRT
  - 允许先用 `std::unique_ptr<Media>` 跨 DLL 移交
- `AppKernel` 的内部 worker/slot 模型仍由 `AppKernel` 自己负责，当前结构只体现：
  - `N` 盘模型
  - 上层配置可控
  - runtime 真状态归 core
- 当前 todo 只是未来几轮执行清单；执行时仍严格遵守：
  - 每轮只推进一个子步骤
  - 完成即归档
  - 完成即提交
  - 更新 todo 后立即停下

## 当前唯一下一步

重建 `client` 介质实现归属：把 `windows/client/backend/media/` 下仍保留的 `FileMedia/MemoryMedia/RawFileMedia` 收到正式宿主侧 `windows/client/media/`，同时保持 `BackendCore` 只依赖 `Media` 抽象接口。

历史完成事实与验收结果请查看 [../progress/README.md](../progress/README.md) 对应日期文件。
