# YumeDisk Client UIA 场景脚本
# 用途:
# - 启动当前 client.exe
# - 做最小壳体 smoke / inspect / close-to-tray 验证
# - 为后续 UI 完整落地提供固定调试入口

param(
    [string]$Scenario = "",
    [string]$Window = "Client",
    [int]$ProcessId = 0,
    [string]$ProcessName = "client",
    [string]$ExePath = "windows/client/cmake-build-debug/client.exe",
    [int]$Timeout = 10000,
    [switch]$Launch,
    [switch]$StopAfter,
    [switch]$List,
    [switch]$Help
)

Set-StrictMode -Version 3.0

try {
    [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
} catch {
}

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ScriptDir
$script:LaunchedProcess = $null
$script:TargetProcessId = 0

if ($Help) {
    Write-Host @"

YumeDisk Client UIA 场景脚本

用法:
  ./uia_scenario.ps1 -Scenario smoke -Launch
  ./uia_scenario.ps1 -Scenario inspect -ProcessId <pid>
  ./uia_scenario.ps1 -Scenario full_shell -Launch -StopAfter

参数:
  -Scenario    : 场景名
  -Window      : 主窗口标题，默认 `Client`
  -ProcessId   : 目标进程 PID
  -ProcessName : 目标进程名，默认 `client`
  -ExePath     : client 可执行文件路径，默认 `windows/client/cmake-build-debug/client.exe`
  -Timeout     : 等待超时，默认 10000ms
  -Launch      : 先启动 client 再执行场景
  -StopAfter   : 场景结束后停止目标进程

可用场景:
  smoke        : 等待主窗口并读取窗口信息 + children 控件列表
  inspect      : 等待主窗口并导出 descendants 控件列表
  close_to_tray: 发送关闭窗口命令，并确认进程仍存活
  minimal_loop : 验证 create -> list -> remove -> quit 最小闭环
  quit_cleanup : 验证保留磁盘直接退出时会清盘并可再次打开 session
  stop_process : 停止目标进程
  full_shell   : smoke -> inspect -> close_to_tray

说明:
  - 当前脚本既覆盖壳体验证，也覆盖 `client` 最小闭环验收。
  - 推荐全量执行：先跑 `full_shell`，再跑 `minimal_loop`。

"@
    exit 0
}

if ($List) {
    Write-Host "可用场景:"
    Write-Host "  smoke"
    Write-Host "  inspect"
    Write-Host "  close_to_tray"
    Write-Host "  minimal_loop"
    Write-Host "  quit_cleanup"
    Write-Host "  stop_process"
    Write-Host "  full_shell"
    exit 0
}

function Write-TestStart {
    param([string]$Text)
    Write-Host "`n[TEST] $Text" -ForegroundColor Cyan
}

function Write-TestPass {
    param([string]$Text)
    Write-Host "  [PASS] $Text" -ForegroundColor Green
}

function Write-TestFail {
    param([string]$Text)
    Write-Host "  [FAIL] $Text" -ForegroundColor Red
}

function Write-TestInfo {
    param([string]$Text)
    Write-Host "  [INFO] $Text" -ForegroundColor Yellow
}

function Resolve-ExePath {
    $candidate = $ExePath
    if (-not [System.IO.Path]::IsPathRooted($candidate)) {
        $candidate = Join-Path $RepoRoot $candidate
    }

    return [System.IO.Path]::GetFullPath($candidate)
}

function Invoke-UiaTest {
    param(
        [string]$Action,
        [string]$Name = "",
        [string]$Value = "",
        [bool]$Check = $false,
        [string]$Condition = "",
        [string]$LocalScope = "descendants",
        [int]$LocalTimeout = $Timeout,
        [string]$LocalWindow = $Window,
        [switch]$WindowOnly
    )

    $args = @(
        "-NoLogo",
        "-NoProfile",
        "-File", (Join-Path $ScriptDir "uia_test.ps1"),
        "-Window", $LocalWindow,
        "-ProcessName", $ProcessName,
        "-Action", $Action,
        "-Scope", $LocalScope,
        "-Timeout", $LocalTimeout
    )

    if (-not $WindowOnly) {
        $args += @("-ProcessId", $script:TargetProcessId)
    }

    if (-not [string]::IsNullOrWhiteSpace($Name)) {
        $args += @("-Name", $Name)
    }
    if (-not [string]::IsNullOrEmpty($Value)) {
        $args += @("-Value", $Value)
    }
    if ($PSBoundParameters.ContainsKey("Check")) {
        $args += @("-Check", $Check)
    }
    if (-not [string]::IsNullOrWhiteSpace($Condition)) {
        $args += @("-Condition", $Condition)
    }

    $raw = & pwsh @args 2>&1
    $jsonLine = $raw | Where-Object { $_ -is [string] -and $_.TrimStart().StartsWith("{") } | Select-Object -Last 1
    if ($null -eq $jsonLine) {
        return [pscustomobject]@{
            status = "error"
            message = "uia_test.ps1 未返回 JSON"
            data = ($raw -join "`n")
        }
    }

    return $jsonLine | ConvertFrom-Json
}

function Read-ElementTextValue {
    param(
        [string]$ElementName,
        [string]$LocalWindow = $Window,
        [switch]$WindowOnly
    )

    $result = Invoke-UiaTest -Action "read" -Name $ElementName -LocalWindow $LocalWindow -WindowOnly:$WindowOnly
    if ($result.status -ne "success") {
        throw "读取控件失败: $ElementName, $($result.message)"
    }

    $valueText = ""
    if ($null -ne $result.data) {
        $valueText = [string]$result.data.value
        if ([string]::IsNullOrWhiteSpace($valueText)) {
            $valueText = [string]$result.data.name
        }
    }

    return $valueText
}

function Wait-TargetProcessExit {
    param([int]$TimeoutMs = 10000)

    $start = Get-Date
    while (((Get-Date) - $start).TotalMilliseconds -lt $TimeoutMs) {
        $process = Get-Process -Id $script:TargetProcessId -ErrorAction SilentlyContinue
        if ($null -eq $process) {
            return $true
        }

        Start-Sleep -Milliseconds 200
    }

    return $false
}

function Get-LatestCreatedTargetId {
    $logText = Read-ElementTextValue -ElementName "yumedisk.log.text"
    $matches = [regex]::Matches($logText, "created target=(\d+)")
    if ($matches.Count -le 0) {
        throw "未在日志中找到 created target="
    }

    return [string]$matches[$matches.Count - 1].Groups[1].Value
}

function Get-VisibleYumeDiskDrives {
    $drives = Get-CimInstance Win32_DiskDrive -ErrorAction SilentlyContinue |
        Where-Object {
            ([string]$_.Model -like "Zightch YumeDisk*") -or
            ([string]$_.Caption -like "Zightch YumeDisk*")
        } |
        Select-Object DeviceID, Model, Caption, Size

    return @($drives)
}

function Wait-VisibleYumeDiskCount {
    param(
        [int]$ExpectedCount,
        [int]$TimeoutMs = 15000
    )

    $start = Get-Date
    while (((Get-Date) - $start).TotalMilliseconds -lt $TimeoutMs) {
        $drives = Get-VisibleYumeDiskDrives
        if (@($drives).Count -eq $ExpectedCount) {
            return [pscustomobject]@{
                count = @($drives).Count
                drives = @($drives)
            }
        }

        Start-Sleep -Milliseconds 300
    }

    return $null
}

function Wait-VisibleYumeDiskCountAtMost {
    param(
        [int]$MaxCount,
        [int]$TimeoutMs = 15000
    )

    $start = Get-Date
    while (((Get-Date) - $start).TotalMilliseconds -lt $TimeoutMs) {
        $drives = Get-VisibleYumeDiskDrives
        if (@($drives).Count -le $MaxCount) {
            return [pscustomobject]@{
                count = @($drives).Count
                drives = @($drives)
            }
        }

        Start-Sleep -Milliseconds 300
    }

    return $null
}

function Relaunch-TargetProcess {
    $resolvedExe = Resolve-ExePath
    $script:LaunchedProcess = Start-Process -FilePath $resolvedExe -WorkingDirectory (Split-Path -Parent $resolvedExe) -PassThru
    $script:TargetProcessId = $script:LaunchedProcess.Id
    return $script:TargetProcessId
}

function Resolve-TargetProcessId {
    if ($script:TargetProcessId -gt 0) {
        return $script:TargetProcessId
    }

    if ($ProcessId -gt 0) {
        $script:TargetProcessId = $ProcessId
        return $script:TargetProcessId
    }

    if ($Launch) {
        $resolvedExe = Resolve-ExePath
        if (-not (Test-Path $resolvedExe)) {
            throw "未找到 client 可执行文件: $resolvedExe"
        }

        Write-TestInfo "启动 client: $resolvedExe"
        $script:LaunchedProcess = Start-Process -FilePath $resolvedExe -WorkingDirectory (Split-Path -Parent $resolvedExe) -PassThru
        $script:TargetProcessId = $script:LaunchedProcess.Id
        return $script:TargetProcessId
    }

    $process = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
        Sort-Object StartTime -Descending |
        Select-Object -First 1

    if ($null -ne $process) {
        $script:TargetProcessId = $process.Id
        return $script:TargetProcessId
    }

    throw "未找到目标进程，请使用 -Launch 或 -ProcessId"
}

function Ensure-WindowReady {
    $result = Invoke-UiaTest -Action "waitwindow" -LocalTimeout $Timeout
    if ($result.status -ne "success") {
        throw "等待主窗口失败: $($result.message)"
    }

    return $result
}

function Test-Smoke {
    Write-TestStart "client 壳体 smoke"

    $result = Ensure-WindowReady
    Write-TestPass "主窗口已出现"
    Write-TestInfo "窗口: $($result.data.name) pid=$($result.data.processId)"

    $result = Invoke-UiaTest -Action "windowinfo"
    if ($result.status -ne "success") {
        Write-TestFail "读取窗口信息失败"
        return $false
    }
    Write-TestPass "读取窗口信息成功"

    $result = Invoke-UiaTest -Action "list" -LocalScope "children"
    if ($result.status -ne "success") {
        Write-TestFail "读取 children 控件失败"
        return $false
    }

    $count = 0
    if ($null -ne $result.data) {
        $count = @($result.data).Count
    }

    Write-TestPass "读取 children 控件成功"
    Write-TestInfo "children 控件数: $count"
    return $true
}

function Test-Inspect {
    Write-TestStart "client 控件树 inspect"

    $null = Ensure-WindowReady
    $result = Invoke-UiaTest -Action "list" -LocalScope "descendants"
    if ($result.status -ne "success") {
        Write-TestFail "导出 descendants 控件失败"
        return $false
    }

    $count = 0
    if ($null -ne $result.data) {
        $count = @($result.data).Count
    }

    Write-TestPass "导出 descendants 控件成功"
    Write-TestInfo "descendants 控件数: $count"
    return $true
}

function Test-CloseToTray {
    Write-TestStart "关闭主窗口保留进程"

    $null = Ensure-WindowReady
    $result = Invoke-UiaTest -Action "closewindow"
    if ($result.status -ne "success") {
        Write-TestFail "发送关闭主窗口命令失败"
        return $false
    }

    Start-Sleep -Milliseconds 800
    $process = Get-Process -Id $script:TargetProcessId -ErrorAction SilentlyContinue
    if ($null -eq $process) {
        Write-TestFail "关闭窗口后进程已退出"
        return $false
    }

    Write-TestPass "关闭窗口后进程仍存活"
    Write-TestInfo "PID=$($process.Id)"
    return $true
}

function Test-MinimalLoop {
    Write-TestStart "client 最小闭环"

    $null = Ensure-WindowReady

    $result = Invoke-UiaTest -Action "click" -Name "yumedisk.disk.create_button"
    if ($result.status -ne "success") {
        Write-TestFail "打开建盘对话框失败"
        return $false
    }
    Write-TestPass "打开建盘对话框成功"

    $result = Invoke-UiaTest -Action "waitwindow" -LocalWindow "yumedisk.create.dialog" -WindowOnly -LocalTimeout 5000
    if ($result.status -ne "success") {
        Write-TestFail "等待建盘对话框失败"
        return $false
    }
    Write-TestPass "建盘对话框已出现"

    $result = Invoke-UiaTest -Action "click" -LocalWindow "yumedisk.create.dialog" -WindowOnly -Name "yumedisk.create.submit_button"
    if ($result.status -ne "success") {
        Write-TestFail "提交建盘失败"
        return $false
    }
    Write-TestPass "提交建盘成功"

    $result = Invoke-UiaTest -Action "wait" -Name "yumedisk.log.text" -Condition "contains:created target=" -LocalTimeout 15000
    if ($result.status -ne "success") {
        Write-TestFail "等待建盘日志失败"
        return $false
    }
    Write-TestPass "建盘日志已出现"

    $targetId = ""
    try {
        $targetId = Get-LatestCreatedTargetId
    } catch {
        Write-TestFail $_.Exception.Message
        return $false
    }
    Write-TestInfo "target id: $targetId"

    $result = Invoke-UiaTest -Action "list" -LocalScope "descendants"
    if ($result.status -ne "success") {
        Write-TestFail "读取磁盘表控件树失败"
        return $false
    }

    $dataItems = @($result.data | Where-Object { $_.controlType -eq "ControlType.DataItem" })
    $hasTargetCell = @($dataItems | Where-Object { [string]$_.name -eq $targetId }).Count -gt 0
    $hasRunningCell = @($dataItems | Where-Object { [string]$_.name -eq "running" }).Count -gt 0
    $hasMediaCell = @($dataItems | Where-Object { [string]$_.name -in @("dense", "sparse", "auto") }).Count -gt 0
    $hasVisiblePathCell = @(
        $dataItems | Where-Object {
            $name = [string]$_.name
            (-not [string]::IsNullOrWhiteSpace($name)) -and
            ($name -ne $targetId) -and
            ($name -ne "running") -and
            ($name -notin @("dense", "sparse", "auto"))
        }
    ).Count -gt 0

    if (($dataItems.Count -lt 4) -or (-not $hasTargetCell) -or (-not $hasRunningCell) -or (-not $hasMediaCell) -or (-not $hasVisiblePathCell)) {
        Write-TestFail "磁盘列表未收口出完整一行数据"
        return $false
    }
    Write-TestPass "磁盘列表已出现完整一行数据"

    $result = Invoke-UiaTest -Action "click" -Name $targetId
    if ($result.status -ne "success") {
        Write-TestFail "选中磁盘行失败"
        return $false
    }
    Write-TestPass "选中磁盘行成功"

    $result = Invoke-UiaTest -Action "wait" -Name "yumedisk.disk.remove_button" -Condition "enabled" -LocalTimeout 5000
    if ($result.status -ne "success") {
        Write-TestFail "删盘按钮未启用"
        return $false
    }
    Write-TestPass "删盘按钮已启用"

    $result = Invoke-UiaTest -Action "click" -Name "yumedisk.disk.remove_button"
    if ($result.status -ne "success") {
        Write-TestFail "点击删盘按钮失败"
        return $false
    }
    Write-TestPass "点击删盘按钮成功"

    $result = Invoke-UiaTest -Action "wait" -Name "yumedisk.log.text" -Condition ("contains:removed target=" + $targetId) -LocalTimeout 15000
    if ($result.status -ne "success") {
        Write-TestFail "等待删盘日志失败"
        return $false
    }
    Write-TestPass "删盘日志已出现"

    $result = Invoke-UiaTest -Action "wait" -Name $targetId -Condition "notexists" -LocalTimeout 10000
    if ($result.status -ne "success") {
        Write-TestFail "磁盘行未从列表移除"
        return $false
    }
    Write-TestPass "磁盘行已从列表移除"

    $result = Invoke-UiaTest -Action "click" -Name "yumedisk.client.quit_button"
    if ($result.status -ne "success") {
        Write-TestFail "点击显式退出按钮失败"
        return $false
    }
    Write-TestPass "点击显式退出按钮成功"

    if (-not (Wait-TargetProcessExit -TimeoutMs 10000)) {
        Write-TestFail "显式退出后进程仍存活"
        return $false
    }
    Write-TestPass "显式退出后进程已退出"
    return $true
}

function Test-QuitCleanup {
    Write-TestStart "保留磁盘直接显式退出"

    $null = Ensure-WindowReady
    $baselineDrives = Get-VisibleYumeDiskDrives
    $baselineCount = @($baselineDrives).Count
    Write-TestInfo "baseline visible disks: $baselineCount"

    $result = Invoke-UiaTest -Action "click" -Name "yumedisk.disk.create_button"
    if ($result.status -ne "success") {
        Write-TestFail "打开建盘对话框失败"
        return $false
    }
    Write-TestPass "打开建盘对话框成功"

    $result = Invoke-UiaTest -Action "waitwindow" -LocalWindow "yumedisk.create.dialog" -WindowOnly -LocalTimeout 5000
    if ($result.status -ne "success") {
        Write-TestFail "等待建盘对话框失败"
        return $false
    }
    Write-TestPass "建盘对话框已出现"

    $result = Invoke-UiaTest -Action "click" -LocalWindow "yumedisk.create.dialog" -WindowOnly -Name "yumedisk.create.submit_button"
    if ($result.status -ne "success") {
        Write-TestFail "提交建盘失败"
        return $false
    }
    Write-TestPass "提交建盘成功"

    $result = Invoke-UiaTest -Action "wait" -Name "yumedisk.log.text" -Condition "contains:created target=" -LocalTimeout 15000
    if ($result.status -ne "success") {
        Write-TestFail "等待建盘日志失败"
        return $false
    }
    Write-TestPass "建盘日志已出现"

    $targetId = ""
    try {
        $targetId = Get-LatestCreatedTargetId
    } catch {
        Write-TestFail $_.Exception.Message
        return $false
    }
    Write-TestInfo "target id: $targetId"

    $drivesAfterCreate = Wait-VisibleYumeDiskCount -ExpectedCount ($baselineCount + 1) -TimeoutMs 15000
    if ($null -eq $drivesAfterCreate) {
        Write-TestFail "系统侧未观察到新增 YumeDisk 可见盘"
        return $false
    }
    Write-TestPass "系统侧已观察到新增 YumeDisk 可见盘"

    $result = Invoke-UiaTest -Action "click" -Name "yumedisk.client.quit_button"
    if ($result.status -ne "success") {
        Write-TestFail "点击显式退出按钮失败"
        return $false
    }
    Write-TestPass "点击显式退出按钮成功"

    if (-not (Wait-TargetProcessExit -TimeoutMs 10000)) {
        Write-TestFail "显式退出后进程仍存活"
        return $false
    }
    Write-TestPass "显式退出后进程已退出"

    $drivesAfterQuit = Wait-VisibleYumeDiskCountAtMost -MaxCount $baselineCount -TimeoutMs 15000
    if ($null -eq $drivesAfterQuit) {
        Write-TestFail "显式退出后可见盘未回落到基线"
        return $false
    }
    Write-TestPass "显式退出后可见盘已回落到基线"

    $null = Relaunch-TargetProcess
    Write-TestInfo "重启后 PID: $script:TargetProcessId"

    try {
        $null = Ensure-WindowReady
    } catch {
        Write-TestFail "重启后主窗口未出现"
        return $false
    }
    Write-TestPass "重启后主窗口已出现"

    $result = Invoke-UiaTest -Action "wait" -Name "yumedisk.log.text" -Condition "contains:session opened" -LocalTimeout 10000
    if ($result.status -ne "success") {
        Write-TestFail "重启后未观察到 session reopened 日志"
        return $false
    }
    Write-TestPass "重启后已观察到 session reopened 日志"

    $logText = Read-ElementTextValue -ElementName "yumedisk.log.text"
    if ($logText.Contains("open session failed") -or $logText.Contains("open-failed(")) {
        Write-TestFail "重启后日志仍出现 session 打开失败"
        return $false
    }
    Write-TestPass "重启后未出现 session 打开失败"
    return $true
}

function Stop-TargetProcess {
    Write-TestStart "停止目标进程"

    $process = Get-Process -Id $script:TargetProcessId -ErrorAction SilentlyContinue
    if ($null -eq $process) {
        Write-TestInfo "目标进程已不存在"
        return $true
    }

    Stop-Process -Id $script:TargetProcessId -Force
    Start-Sleep -Milliseconds 300

    $process = Get-Process -Id $script:TargetProcessId -ErrorAction SilentlyContinue
    if ($null -eq $process) {
        Write-TestPass "目标进程已停止"
        return $true
    }

    Write-TestFail "停止目标进程失败"
    return $false
}

if ([string]::IsNullOrWhiteSpace($Scenario)) {
    Write-Host "请使用 -Scenario 指定场景，或使用 -List 查看所有场景" -ForegroundColor Yellow
    exit 1
}

try {
    $null = Resolve-TargetProcessId
    Write-TestInfo "目标 PID: $script:TargetProcessId"
} catch {
    Write-Host "[ERROR] $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

$overallSuccess = $false

switch ($Scenario.ToLowerInvariant()) {
    "smoke" {
        $overallSuccess = Test-Smoke
    }
    "inspect" {
        $overallSuccess = Test-Inspect
    }
    "close_to_tray" {
        $overallSuccess = Test-CloseToTray
    }
    "minimal_loop" {
        $overallSuccess = Test-MinimalLoop
    }
    "quit_cleanup" {
        $overallSuccess = Test-QuitCleanup
    }
    "stop_process" {
        $overallSuccess = Stop-TargetProcess
    }
    "full_shell" {
        Write-Host "`n========================================" -ForegroundColor Cyan
        Write-Host "         client 壳体验证" -ForegroundColor Cyan
        Write-Host "========================================" -ForegroundColor Cyan

        $results = [ordered]@{
            smoke = Test-Smoke
            inspect = Test-Inspect
            close_to_tray = Test-CloseToTray
        }

        Write-Host "`n========================================" -ForegroundColor Cyan
        Write-Host "         场景结果汇总" -ForegroundColor Cyan
        Write-Host "========================================" -ForegroundColor Cyan

        $passCount = 0
        $failCount = 0
        foreach ($item in $results.GetEnumerator()) {
            if ($item.Value) {
                Write-Host "  [PASS] $($item.Key)" -ForegroundColor Green
                $passCount += 1
            } else {
                Write-Host "  [FAIL] $($item.Key)" -ForegroundColor Red
                $failCount += 1
            }
        }

        Write-Host "`n总计: $passCount 通过, $failCount 失败" -ForegroundColor Cyan
        $overallSuccess = ($failCount -eq 0)
    }
    default {
        Write-Host "[ERROR] 未知场景: $Scenario" -ForegroundColor Red
        exit 1
    }
}

if ($StopAfter) {
    $stopSuccess = Stop-TargetProcess
    if (-not $stopSuccess) {
        $overallSuccess = $false
    }
}

if ($overallSuccess) {
    exit 0
}

exit 1
