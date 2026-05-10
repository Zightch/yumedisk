# 当前总目标

重建 `client` 的建盘完整配置控制面：在不引入额外复杂度的前提下，把 `BackendCore` 已暴露的 `AppKernel` 配置项继续打通到宿主与 UI，让上层真正可控。

## 子步骤

1. 重建建盘参数输入面
   - 把当前已预留在 `BackendHostCreateDiskRequest` 中的配置项正式放进 `CreateDiskDialog`
   - 至少承接：
     - `sectorSize`
     - `queueDepth`
     - `writeSlotBytes`
     - `readWorkerCount`
     - `writeWorkerCount`
     - `ackBatchMaxRanges`
   - 保持 `denseMem / sparseMem / rawFile` 三种模式下的最小联动

2. 重建宿主参数整形面
   - 继续由 `backendHost` 负责：
     - 文本解析
     - 默认值落位
     - 非法值报错
     - `rawFile` 额外限制校验
   - 保持 `UI` 不直接接触 `BackendCore` 运行时类型

3. 重建最小配置验收口
   - 验收默认参数建盘
   - 验收自定义参数建盘
   - 验收非法参数提示
   - 验收 `denseMem / sparseMem / rawFile` 三模式

## 当前轮边界

- 当前路线继续按“从中间向两边”推进，不做：
  - 多进程
  - IPC
  - 服务化
  - 插件化
  - 远程协议预埋
- 当前阶段的重点是“把上层可控配置链打通”，不做：
  - UI 美化
  - 托盘体验补全
  - 日志筛选与复杂交互
- `BackendCore` 继续只做运行时核心，不做：
  - Qt UI
  - Qt DTO
  - 具体 `Media` 子类实现
- `client` 继续保留：
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
  - 额外 `viewmodel / store / facade` 壳
- `AppKernel` 的内部 worker/slot 模型仍由 `AppKernel` 自己负责；当前阶段只要求：
  - `N` 盘模型保持不变
  - 上层配置继续可控
  - runtime 真状态继续归 `BackendCore`
- 当前 todo 只是未来几轮执行清单；执行时仍严格遵守：
  - 每轮只推进一个子步骤
  - 完成即归档
  - 完成即提交
  - 更新 todo 后立即停下

## 当前唯一下一步

重建建盘参数输入面：把 `BackendHostCreateDiskRequest` 中已预留的磁盘配置项正式放进 `CreateDiskDialog`，同时保持 `denseMem / sparseMem / rawFile` 三模式的最小联动。

历史完成事实与验收结果请查看 [../progress/README.md](../progress/README.md) 对应日期文件。
