# YumeDisk Client UIA 测试调试说明

## 1. 目标

这套脚本只服务当前仓库的 `windows/client` 调试：

- 定位 Qt 控件；
- 读取控件文本和值；
- 触发点击、写入、切换；
- 验证窗口壳体与“关闭主窗口仅隐藏”语义；
- 为后续最小闭环 UI 落地保留固定测试入口。

当前不要再临时手写一堆零散 UIA 命令；优先直接复用这里的脚本。

## 2. 文件说明

- `tests/uia_test.ps1`
  - 单步操作脚本。
  - 适合查窗口、列控件、读值、点按钮、等状态。
- `tests/uia_scenario.ps1`
  - 场景脚本。
  - 适合做当前 `client` 壳体 smoke / inspect / close-to-tray 验证。
- `tests/uia_test_gbk.ps1`
  - 仅是 `uia_test.ps1` 的 GBK 控制台包装。
  - 默认优先用 UTF-8 版 `uia_test.ps1`。

## 3. 管理员边界

`client.exe` 运行时可能需要管理员权限。

固定口径：

- 构建、改代码、改文档时，继续使用普通终端。
- 只有在“启动 `client.exe` 做 UIA 调试”这件事上，才使用已经管理员运行的终端 / MCP。
- 不要在管理员终端里做仓库文件编辑，避免文件权限和属主被污染。

## 4. 当前可直接使用的场景

当前现成场景分两类：壳体验证 + 最小闭环验收。

- `smoke`
  - 等待主窗口出现；
  - 读取主窗口信息；
  - 读取主窗口 children 控件列表。
- `inspect`
  - 导出主窗口 descendants 控件列表；
  - 主要用于定位 Qt 控件。
- `close_to_tray`
  - 发送关闭主窗口命令；
  - 验证进程仍然存活。
- `minimal_loop`
  - 启动后走一轮 `create -> list -> remove -> quit`；
  - 直接验证当前客户端最小闭环。
- `quit_cleanup`
  - 创建磁盘后不手动删盘；
  - 直接显式退出；
  - 验证可见盘回落和再次启动 session 正常打开。
- `stop_process`
  - 强制停止目标进程；
  - 只用于调试收尾。
- `full_shell`
  - 顺序执行 `smoke -> inspect -> close_to_tray`；
  - 可配合 `-StopAfter` 清理进程。

## 5. 推荐用法

### 5.1 直接跑壳体验证

在管理员 PowerShell 中运行：

```powershell
pwsh -File tests/uia_scenario.ps1 -Scenario full_shell -Launch -StopAfter
```

### 5.2 跑最小闭环验收

在管理员 PowerShell 中运行：

```powershell
pwsh -File tests/uia_scenario.ps1 -Scenario minimal_loop -Launch
```

如果要跑当前全量回归，固定口径是：

```powershell
pwsh -File tests/uia_scenario.ps1 -Scenario full_shell -Launch -StopAfter
pwsh -File tests/uia_scenario.ps1 -Scenario minimal_loop -Launch
pwsh -File tests/uia_scenario.ps1 -Scenario quit_cleanup -Launch -StopAfter
```

默认可执行文件路径：

- `windows/client/cmake-build-debug/client.exe`

如果你换了构建目录，直接显式传：

```powershell
pwsh -File tests/uia_scenario.ps1 `
  -Scenario smoke `
  -Launch `
  -ExePath windows/client/cmake-build-minsizerel/client.exe
```

### 5.3 只做控件树定位

先启动 `client`，再跑：

```powershell
pwsh -File tests/uia_scenario.ps1 -Scenario inspect -ProcessId <pid>
```

### 5.4 单步读控件 / 点控件

```powershell
pwsh -File tests/uia_test.ps1 -ProcessId <pid> -Action list -Scope descendants
pwsh -File tests/uia_test.ps1 -ProcessId <pid> -Name yumedisk.session.state_value -Action read
pwsh -File tests/uia_test.ps1 -ProcessId <pid> -Name yumedisk.disk.create_button -Action click
pwsh -File tests/uia_test.ps1 -ProcessId <pid> -Name yumedisk.create.capacity_input -Action write -Value 1024
```

表格行选择也继续用 `click`，脚本现在会优先按可选项语义处理 `DataItem / ListItem / TreeItem / TabItem`，避免 Qt 表格只触发默认动作却不更新选择状态。

### 5.5 先看桌面窗口，再决定目标

```powershell
pwsh -File tests/uia_test.ps1 -Action windows
```

### 5.6 等待主窗口出现

```powershell
pwsh -File tests/uia_test.ps1 -ProcessId <pid> -Action waitwindow -Timeout 10000
```

## 6. 当前脚本能力

`tests/uia_test.ps1` 当前支持：

- `windows`
- `waitwindow`
- `windowinfo`
- `closewindow`
- `list`
- `exists`
- `read`
- `write`
- `click`
- `toggle`
- `enabled`
- `wait`

`wait` 当前支持条件：

- `contains:文本`
- `regex:正则`
- `equals:文本`
- `notempty`
- `enabled`
- `disabled`
- `exists`
- `notexists`

## 7. 对 `windows/client` 的控件命名约束

后续 UI 落地时，必须给关键 Qt 控件设置稳定的 `accessibleName`。  
脚本默认按这个值查控件，不建议依赖可变按钮文字。

推荐命名如下：

- 主窗口：`yumedisk.client.main_window`
- session 状态值：`yumedisk.session.state_value`
- 受管磁盘表：`yumedisk.disk.table`
- 创建磁盘按钮：`yumedisk.disk.create_button`
- 删除磁盘按钮：`yumedisk.disk.remove_button`
- 显式退出按钮：`yumedisk.client.quit_button`
- 日志面板：`yumedisk.log.text`
- 建盘对话框：`yumedisk.create.dialog`
- 容量输入：`yumedisk.create.capacity_input`
- 介质模式选择：`yumedisk.create.media_mode_combo`
- 只读开关：`yumedisk.create.read_only_check`
- target id 输入：`yumedisk.create.target_id_input`
- 建盘确认按钮：`yumedisk.create.submit_button`
- 建盘取消按钮：`yumedisk.create.cancel_button`
- 托盘打开动作：`yumedisk.tray.open_action`
- 托盘退出动作：`yumedisk.tray.quit_action`

这份命名约束从现在开始就是测试契约。  
后续 `client` UI 改造时，直接按这个契约挂控件名，脚本就能立即复用。

## 8. 当前阶段的验收口径

截至 `2026-05-09`，这套脚本当前要验证的不是业务闭环，而是：

- `client.exe` 能被启动；
- 主窗口能被 UIA 找到；
- 能导出控件树；
- 关闭主窗口后进程仍然存活；
- 最小闭环 `create -> list -> remove -> quit` 可被直接回归。

当前全量测试固定拆成两段：

- `full_shell`
- `minimal_loop`
- `quit_cleanup`

## 9. 常见问题

- 找不到窗口：
  - 先跑 `windows` 看当前桌面窗口；
  - 再优先改用 `-ProcessId`。
- 列不出预期控件：
  - 先确认 Qt 控件是否真的设置了 `accessibleName`；
  - 再确认控件是否在当前窗口树下。
- elevated `client` 看不到：
  - 用管理员终端运行脚本。
- 中文显示异常：
  - 优先用 `pwsh` + `tests/uia_test.ps1`；
  - 不行再退回 `tests/uia_test_gbk.ps1`。
