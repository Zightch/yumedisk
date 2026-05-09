# 当前总目标

重建 `BackendCore` 最小抽离闭环：把当前 `windows/client/backend/` 中真正属于运行时核心的部分抽成独立 `BackendCore.dll`，并让 `client` 最终收口为 `UI + backendHost + media 实现`。

## 子步骤

1. 重建 `BackendCore` 建盘与删盘接口
   - `BackendCore` 只接收已经收口后的运行参数
   - `Media` 由宿主侧创建后移交给 `BackendCore`
   - `BackendCore` 负责：
     - 参数合法性校验
     - 建盘
     - 删盘
     - 全删盘
     - `shutdown / close`
   - 这里要明确调整一条现有行为：
     - `readWorkerCount`
     - `writeWorkerCount`
     - `ackBatchMaxRanges`
     - `queueDepth`
     不再由 core 隐式替上层决定，改为上层可传、core 校验后落地

2. 重建 `client` 宿主适配层 `backendHost`
   - 新建 `windows/client/backendHost/`
   - 它只负责：
     - `QString` / 表单输入解析
     - `QFileInfo` / 路径存在性判断
     - `rawFile` 大小与 4096 对齐校验
     - `denseMem / sparseMem / rawFile` 选择
     - 创建 `Media`
     - 调 `BackendCore`
     - 把 core 快照转换成 UI DTO
   - 它不负责：
     - 自己维护 session 真状态
     - 自己维护 disk runtime
     - 自己维护 staging 生命周期
     - 自己接 `AppKernel`

3. 重建 `client` 介质实现归属
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

4. 重建 `client` 调用链
   - 把当前 `Widget/CreateDiskDialog -> Backend` 的直连路径
     重建为：
     - `Widget/CreateDiskDialog`
     - `-> backendHost`
     - `-> BackendCore`
   - `Widget` 只读 UI 快照，只发 UI 命令
   - `CreateDiskDialog` 只收集输入，不承接运行时规则
   - 保持当前“UI 与运行时分离”原则，不回退成 UI 直接解释 runtime 细节

5. 重建构建与部署链路
   - `client` 改为链接 `BackendCore`
   - `BackendCore` 再链接：
     - `AppKernel`
     - `scan`
   - `client` 运行目录需要同时具备：
     - `BackendCore.dll`
     - `AppKernel.dll`
   - 保证当前最小闭环下：
     - IDE 构建可运行
     - `cmake-build-debug` 产物可直接启动

6. 重建验收口径
   - 先做构建验收：
     - `BackendCore` 独立可编译
     - `client` 链接新链路后可编译
   - 再做最小运行验收：
     - `client.exe` 可启动
     - session 状态可刷新
     - 建盘 / 删盘可走通
     - 磁盘列表可刷新
     - 日志可刷新
   - 这一阶段不新增新的测试体系，继续复用现有：
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

重建 `BackendCore` 建盘与删盘接口：让 core 正式接收宿主移交的 `Media` 与显式 `SessionConfig / DiskConfig`，清掉当前建盘路径里“media 实例尚未正式进入 core 接口”的最后过渡痕迹。

历史完成事实与验收结果请查看 [../progress/README.md](../progress/README.md) 对应日期文件。
