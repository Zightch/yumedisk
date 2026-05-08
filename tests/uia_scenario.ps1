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
  stop_process : 停止目标进程
  full_shell   : smoke -> inspect -> close_to_tray

说明:
  - 当前 `windows/client` 还是壳体阶段，因此场景主要验证窗口和托盘关闭语义。
  - 真正的 session / 磁盘 / 日志控件测试，会在 UI 落地后复用同一套脚本继续扩展。

"@
    exit 0
}

if ($List) {
    Write-Host "可用场景:"
    Write-Host "  smoke"
    Write-Host "  inspect"
    Write-Host "  close_to_tray"
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
        [int]$LocalTimeout = $Timeout
    )

    $args = @(
        "-NoLogo",
        "-NoProfile",
        "-File", (Join-Path $ScriptDir "uia_test.ps1"),
        "-ProcessId", $script:TargetProcessId,
        "-Window", $Window,
        "-ProcessName", $ProcessName,
        "-Action", $Action,
        "-Scope", $LocalScope,
        "-Timeout", $LocalTimeout
    )

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
