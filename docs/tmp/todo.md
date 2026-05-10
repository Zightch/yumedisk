# 当前总目标

重建 `BackendCore` 最小抽离闭环的运行验收口：确认当前 `client` 已稳定收口为 `UI + backendHost + media 实现 + BackendCore`，并把最小闭环运行链正式验收清楚。

## 子步骤

1. 重建最小运行验收口
   - 验收当前 `client` 运行目录：
     - `client.exe`
     - `BackendCore.dll`
     - `AppKernel.dll`
   - 验收当前最小闭环下：
     - `client.exe` 可启动
     - session 状态可刷新
     - 建盘可走通
     - 删盘可走通
     - 磁盘列表可刷新
     - 日志可刷新
   - 当前阶段继续复用现有：
     - `tests/uia_test.ps1`
     - `tests/uia_scenario.ps1`

## 当前轮边界

- 当前路线只做“本进程内最小闭环验收”，不做：
  - 多进程
  - IPC
  - 服务化
  - 插件化
  - 远程协议预埋
- `BackendCore` 继续只做运行时核心，不做：
  - Qt UI
  - Qt DTO
  - `FileMedia`
  - 具体 `Media` 子类实现
- `client` 当前正式保留：
  - `Widget`
  - `CreateDiskDialog`
  - `backendHost`
  - `media/FileMedia`
  - `media/MemoryMedia`
  - `media/RawFileMedia`
- 当前阶段不引入：
  - C API
  - callback table
  - 跨语言 ABI 包装
- 当前阶段默认接受：
  - `client` 与 `BackendCore` 同仓
  - 同编译器
  - 同 CRT
  - 允许先用 `std::unique_ptr<Media>` 跨 DLL 移交
- `AppKernel` 的内部 worker/slot 模型仍由 `AppKernel` 自己负责；当前验收只体现：
  - `N` 盘模型
  - 上层配置可控
  - runtime 真状态归 core
- 当前 todo 只是未来几轮执行清单；执行时仍严格遵守：
  - 每轮只推进一个子步骤
  - 完成即归档
  - 完成即提交
  - 更新 todo 后立即停下

## 当前唯一下一步

重建最小运行验收口：基于 `tests/uia_test.ps1` 与 `tests/uia_scenario.ps1`，验证当前 `client.exe + BackendCore.dll + AppKernel.dll` 的最小闭环运行链。

历史完成事实与验收结果请查看 [../progress/README.md](../progress/README.md) 对应日期文件。
