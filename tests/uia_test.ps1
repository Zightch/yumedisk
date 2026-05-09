# YumeDisk Client UIA 单步测试脚本
# 用途:
# - 定位 Qt / Widgets 控件
# - 读取控件文本和值
# - 点击 / 写入 / 切换控件
# - 枚举主窗口下控件树
# - 做最小窗口级调试（等待窗口、读取窗口信息、关闭窗口）
#
# 约定:
# - 优先通过 `-ProcessId` 定位目标窗口；最稳定。
# - 其次通过 `-Window` 定位主窗口标题。
# - 再次回退到 `-ProcessName`。
#
# 示例:
#   ./uia_test.ps1 -Action windows
#   ./uia_test.ps1 -ProcessId 12345 -Action waitwindow
#   ./uia_test.ps1 -ProcessId 12345 -Action windowinfo
#   ./uia_test.ps1 -ProcessId 12345 -Action list -Scope children
#   ./uia_test.ps1 -ProcessId 12345 -Name yumedisk.session.state_value -Action read
#   ./uia_test.ps1 -ProcessId 12345 -Name yumedisk.disk.create_button -Action click
#   ./uia_test.ps1 -ProcessId 12345 -Name yumedisk.create.media_mode_combo -Action select -Value rawFile

param(
    [string]$Window = "Client",
    [int]$ProcessId = 0,
    [string]$ProcessName = "client",
    [string]$Name = "",
    [string]$Action = "",
    [string]$Value = "",
    [bool]$Check = $false,
    [int]$Timeout = 10000,
    [string]$Condition = "",
    [string]$Scope = "descendants",
    [switch]$Help
)

Set-StrictMode -Version 3.0

try {
    [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
} catch {
}

Add-Type -AssemblyName UIAutomationClient
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class UiaNativeMouse {
    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int x, int y);

    [DllImport("user32.dll")]
    public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extraInfo);

    public const uint LeftDown = 0x0002;
    public const uint LeftUp = 0x0004;
}
"@

if ($Help) {
    $helpText = @'

YumeDisk Client UIA 单步测试脚本

用法:
  ./uia_test.ps1 -ProcessId <pid> -Action <action> [options]
  ./uia_test.ps1 -Window <window_title> -Action <action> [options]
  ./uia_test.ps1 -ProcessName <name> -Action <action> [options]

参数:
  -Window       : 主窗口标题，默认 `Client`
  -ProcessId    : 目标进程 PID，优先级最高
  -ProcessName  : 目标进程名，默认 `client`
  -Name         : 目标控件名，推荐绑定到 Qt `accessibleName`
  -Action       : 操作类型
  -Value        : `write` 操作写入值
  -Check        : `toggle` 操作目标状态
  -Timeout      : `wait` / `waitwindow` 超时，默认 10000ms
  -Condition    : `wait` 条件，支持 `contains:` / `regex:` / `equals:`
  -Scope        : `list` 搜索范围，支持 `descendants` / `children`

支持操作:
  windows       : 列出桌面顶层窗口
  waitwindow    : 等待主窗口出现
  windowinfo    : 读取主窗口信息
  closewindow   : 关闭主窗口
  list          : 列出主窗口下控件
  exists        : 检查控件是否存在
  read          : 读取控件文本 / 值
  write         : 写入文本控件
  click         : 点击控件
  select        : 选择下拉框 / 列表项
  toggle        : 切换复选框
  enabled       : 读取控件可用状态
  wait          : 等待控件满足条件

wait 条件:
  contains:文本
  regex:正则
  equals:文本
  notempty
  enabled
  disabled
  exists
  notexists

推荐调试顺序:
  1. 先跑 `windows`
  2. 再跑 `waitwindow`
  3. 再跑 `windowinfo`
  4. 最后用 `list` 看控件树

'@
    Write-Host $helpText
    exit 0
}

function Write-UiaResult {
    param(
        [string]$Status,
        [string]$Message,
        [object]$Data = $null
    )

    $output = [ordered]@{
        status = $Status
        message = $Message
        data = $Data
        timestamp = (Get-Date -Format "yyyy-MM-dd HH:mm:ss")
    }

    Write-Output ($output | ConvertTo-Json -Compress -Depth 8)
}

function Write-UiaSuccess {
    param(
        [string]$Message,
        [object]$Data = $null
    )

    Write-UiaResult -Status "success" -Message $Message -Data $Data
}

function Write-UiaInfo {
    param(
        [string]$Message,
        [object]$Data = $null
    )

    Write-UiaResult -Status "info" -Message $Message -Data $Data
}

function Write-UiaError {
    param([string]$Message)
    Write-UiaResult -Status "error" -Message $Message
}

function Get-TreeScope {
    if ($Scope -eq "children") {
        return [System.Windows.Automation.TreeScope]::Children
    }

    return [System.Windows.Automation.TreeScope]::Descendants
}

function Get-PropertyValueOrNull {
    param(
        [System.Windows.Automation.AutomationElement]$Element,
        [System.Windows.Automation.AutomationProperty]$Property
    )

    if ($null -eq $Element) {
        return $null
    }

    try {
        return $Element.GetCurrentPropertyValue($Property, $true)
    } catch {
        return $null
    }
}

function Get-ElementValueText {
    param([System.Windows.Automation.AutomationElement]$Element)

    if ($null -eq $Element) {
        return $null
    }

    try {
        $valuePattern = $Element.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
        if ($null -ne $valuePattern -and
            -not [string]::IsNullOrWhiteSpace($valuePattern.Current.Value)) {
            return $valuePattern.Current.Value
        }
    } catch {
    }

    try {
        $legacyPattern = $Element.GetCurrentPattern([System.Windows.Automation.LegacyIAccessiblePattern]::Pattern)
        if ($null -ne $legacyPattern -and
            -not [string]::IsNullOrWhiteSpace($legacyPattern.Current.Value)) {
            return $legacyPattern.Current.Value
        }
    } catch {
    }

    return $null
}

function Get-ElementText {
    param([System.Windows.Automation.AutomationElement]$Element)

    $valueText = Get-ElementValueText -Element $Element
    if (-not [string]::IsNullOrWhiteSpace($valueText)) {
        return $valueText
    }

    $nameText = Get-PropertyValueOrNull -Element $Element -Property ([System.Windows.Automation.AutomationElement]::NameProperty)
    if (-not [string]::IsNullOrWhiteSpace([string]$nameText)) {
        return [string]$nameText
    }

    return ""
}

function Convert-BoundingRectangle {
    param([object]$Rect)

    if ($null -eq $Rect) {
        return $null
    }

    try {
        return [ordered]@{
            left = [int][Math]::Round($Rect.Left)
            top = [int][Math]::Round($Rect.Top)
            width = [int][Math]::Round($Rect.Width)
            height = [int][Math]::Round($Rect.Height)
        }
    } catch {
        return $null
    }
}

function Click-BoundingRectangleCenter {
    param([object]$Rect)

    if ($null -eq $Rect) {
        return $false
    }

    try {
        $x = [int][Math]::Round($Rect.Left + ($Rect.Width / 2.0))
        $y = [int][Math]::Round($Rect.Top + ($Rect.Height / 2.0))
        [UiaNativeMouse]::SetCursorPos($x, $y) | Out-Null
        Start-Sleep -Milliseconds 60
        [UiaNativeMouse]::mouse_event([UiaNativeMouse]::LeftDown, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 30
        [UiaNativeMouse]::mouse_event([UiaNativeMouse]::LeftUp, 0, 0, 0, [UIntPtr]::Zero)
        return $true
    } catch {
        return $false
    }
}

function Get-ElementSnapshot {
    param([System.Windows.Automation.AutomationElement]$Element)

    if ($null -eq $Element) {
        return $null
    }

    $controlType = Get-PropertyValueOrNull -Element $Element -Property ([System.Windows.Automation.AutomationElement]::ControlTypeProperty)

    return [ordered]@{
        name = [string](Get-PropertyValueOrNull -Element $Element -Property ([System.Windows.Automation.AutomationElement]::NameProperty))
        value = [string](Get-ElementValueText -Element $Element)
        automationId = [string](Get-PropertyValueOrNull -Element $Element -Property ([System.Windows.Automation.AutomationElement]::AutomationIdProperty))
        className = [string](Get-PropertyValueOrNull -Element $Element -Property ([System.Windows.Automation.AutomationElement]::ClassNameProperty))
        controlType = if ($null -ne $controlType) { $controlType.ProgrammaticName } else { "" }
        processId = [int](Get-PropertyValueOrNull -Element $Element -Property ([System.Windows.Automation.AutomationElement]::ProcessIdProperty))
        isEnabled = [bool](Get-PropertyValueOrNull -Element $Element -Property ([System.Windows.Automation.AutomationElement]::IsEnabledProperty))
        isOffscreen = [bool](Get-PropertyValueOrNull -Element $Element -Property ([System.Windows.Automation.AutomationElement]::IsOffscreenProperty))
        bounds = Convert-BoundingRectangle -Rect (Get-PropertyValueOrNull -Element $Element -Property ([System.Windows.Automation.AutomationElement]::BoundingRectangleProperty))
    }
}

function Get-DesktopWindows {
    $root = [System.Windows.Automation.AutomationElement]::RootElement
    $elements = $root.FindAll(
        [System.Windows.Automation.TreeScope]::Children,
        [System.Windows.Automation.Condition]::TrueCondition
    )

    $result = @()
    foreach ($element in $elements) {
        $snapshot = Get-ElementSnapshot -Element $element
        if ($null -eq $snapshot) {
            continue
        }
        if ([string]::IsNullOrWhiteSpace([string]$snapshot.name) -and
            [string]::IsNullOrWhiteSpace([string]$snapshot.className)) {
            continue
        }
        $result += $snapshot
    }

    return $result
}

function Find-TopLevelWindowByProcessId {
    param([int]$TargetProcessId)

    if ($TargetProcessId -le 0) {
        return $null
    }

    try {
        $process = Get-Process -Id $TargetProcessId -ErrorAction Stop
        if ($process.MainWindowHandle -ne 0) {
            try {
                return [System.Windows.Automation.AutomationElement]::FromHandle([IntPtr]$process.MainWindowHandle)
            } catch {
            }
        }
    } catch {
    }

    $root = [System.Windows.Automation.AutomationElement]::RootElement
    $pidCondition = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ProcessIdProperty,
        $TargetProcessId
    )

    return $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $pidCondition)
}

function Resolve-ProcessIdByName {
    param([string]$TargetProcessName)

    if ([string]::IsNullOrWhiteSpace($TargetProcessName)) {
        return 0
    }

    $processes = Get-Process -Name $TargetProcessName -ErrorAction SilentlyContinue |
        Sort-Object StartTime -Descending

    foreach ($process in $processes) {
        if ($process.MainWindowHandle -ne 0) {
            return $process.Id
        }
    }

    if ($processes.Count -gt 0) {
        return $processes[0].Id
    }

    return 0
}

function Find-MainWindow {
    param(
        [string]$TargetWindow,
        [int]$TargetProcessId,
        [string]$TargetProcessName
    )

    if ($TargetProcessId -gt 0) {
        return Find-TopLevelWindowByProcessId -TargetProcessId $TargetProcessId
    }

    $root = [System.Windows.Automation.AutomationElement]::RootElement

    if (-not [string]::IsNullOrWhiteSpace($TargetWindow)) {
        $nameCondition = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::NameProperty,
            $TargetWindow
        )

        $window = $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $nameCondition)
        if ($null -ne $window) {
            return $window
        }

        $window = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $nameCondition)
        if ($null -ne $window) {
            return $window
        }
    }

    $resolvedProcessId = Resolve-ProcessIdByName -TargetProcessName $TargetProcessName
    if ($resolvedProcessId -gt 0) {
        return Find-TopLevelWindowByProcessId -TargetProcessId $resolvedProcessId
    }

    return $null
}

function Wait-MainWindow {
    param(
        [string]$TargetWindow,
        [int]$TargetProcessId,
        [string]$TargetProcessName,
        [int]$TimeoutMs
    )

    $start = Get-Date
    while (((Get-Date) - $start).TotalMilliseconds -lt $TimeoutMs) {
        $window = Find-MainWindow -TargetWindow $TargetWindow -TargetProcessId $TargetProcessId -TargetProcessName $TargetProcessName
        if ($null -ne $window) {
            return $window
        }

        Start-Sleep -Milliseconds 200
    }

    return $null
}

function Find-Element {
    param(
        [System.Windows.Automation.AutomationElement]$Root,
        [string]$ElementName
    )

    if ($null -eq $Root -or [string]::IsNullOrWhiteSpace($ElementName)) {
        return $null
    }

    $treeScope = Get-TreeScope

    if ($ElementName.Contains("*") -or $ElementName.Contains("?")) {
        $elements = $Root.FindAll($treeScope, [System.Windows.Automation.Condition]::TrueCondition)
        foreach ($element in $elements) {
            $candidateName = [string](Get-PropertyValueOrNull -Element $element -Property ([System.Windows.Automation.AutomationElement]::NameProperty))
            if ($candidateName -like $ElementName) {
                return $element
            }
        }

        return $null
    }

    $condition = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::NameProperty,
        $ElementName
    )

    return $Root.FindFirst($treeScope, $condition)
}

function Get-AllElements {
    param([System.Windows.Automation.AutomationElement]$Root)

    if ($null -eq $Root) {
        return @()
    }

    $treeScope = Get-TreeScope
    $elements = $Root.FindAll($treeScope, [System.Windows.Automation.Condition]::TrueCondition)

    $result = @()
    foreach ($element in $elements) {
        $snapshot = Get-ElementSnapshot -Element $element
        if ($null -eq $snapshot) {
            continue
        }
        if ([string]::IsNullOrWhiteSpace([string]$snapshot.name) -and
            [string]::IsNullOrWhiteSpace([string]$snapshot.className) -and
            [string]::IsNullOrWhiteSpace([string]$snapshot.automationId)) {
            continue
        }
        $result += $snapshot
    }

    return $result
}

function Invoke-ElementClick {
    param([System.Windows.Automation.AutomationElement]$Element)

    if ($null -eq $Element) {
        return $false
    }

    try {
        $Element.SetFocus()
    } catch {
    }

    $controlType = Get-PropertyValueOrNull -Element $Element -Property ([System.Windows.Automation.AutomationElement]::ControlTypeProperty)
    $controlTypeName = if ($null -ne $controlType) { [string]$controlType.ProgrammaticName } else { "" }

    if ($controlTypeName -in @("ControlType.DataItem", "ControlType.ListItem", "ControlType.TreeItem", "ControlType.TabItem")) {
        try {
            $selectionPattern = $Element.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern)
            $selectionPattern.Select()
            return $true
        } catch {
        }
    }

    try {
        $invokePattern = $Element.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
        $invokePattern.Invoke()
        return $true
    } catch {
    }

    try {
        $selectionPattern = $Element.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern)
        $selectionPattern.Select()
        return $true
    } catch {
    }

    try {
        $legacyPattern = $Element.GetCurrentPattern([System.Windows.Automation.LegacyIAccessiblePattern]::Pattern)
        $legacyPattern.DoDefaultAction()
        return $true
    } catch {
    }

    try {
        $rect = $Element.GetCurrentPropertyValue([System.Windows.Automation.AutomationElement]::BoundingRectangleProperty)
        if (Click-BoundingRectangleCenter -Rect $rect) {
            return $true
        }
    } catch {
    }

    return $false
}

function Set-ElementValue {
    param(
        [System.Windows.Automation.AutomationElement]$Element,
        [string]$TargetValue
    )

    if ($null -eq $Element) {
        return $false
    }

    try {
        $valuePattern = $Element.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
        $valuePattern.SetValue($TargetValue)
        return $true
    } catch {
        return $false
    }
}

function Select-ElementOption {
    param(
        [System.Windows.Automation.AutomationElement]$Root,
        [System.Windows.Automation.AutomationElement]$Element,
        [string]$TargetValue
    )

    if (($null -eq $Root) -or ($null -eq $Element) -or [string]::IsNullOrWhiteSpace($TargetValue)) {
        return $false
    }

    try {
        $Element.SetFocus()
    } catch {
    }

    $readCurrentValue = {
        $text = Get-ElementValueText -Element $Element
        if (-not [string]::IsNullOrWhiteSpace($text)) {
            return $text
        }
        return Get-ElementText -Element $Element
    }

    try {
        $valuePattern = $Element.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
        if (($null -ne $valuePattern) -and (-not $valuePattern.Current.IsReadOnly)) {
            $valuePattern.SetValue($TargetValue)
            Start-Sleep -Milliseconds 150
            if ((& $readCurrentValue) -eq $TargetValue) {
                return $true
            }
        }
    } catch {
    }

    try {
        $expandPattern = $Element.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
        if ($expandPattern.Current.ExpandCollapseState -eq [System.Windows.Automation.ExpandCollapseState]::Collapsed) {
            $expandPattern.Expand()
            Start-Sleep -Milliseconds 200
        }
    } catch {
    }

    $option = Find-Element -Root $Element -ElementName $TargetValue
    if ($null -eq $option) {
        $option = Find-Element -Root $Root -ElementName $TargetValue
    }
    if ($null -eq $option) {
        $processId = [int](Get-PropertyValueOrNull -Element $Element -Property ([System.Windows.Automation.AutomationElement]::ProcessIdProperty))
        if ($processId -gt 0) {
            $pidCondition = New-Object System.Windows.Automation.PropertyCondition(
                [System.Windows.Automation.AutomationElement]::ProcessIdProperty,
                $processId
            )
            $nameCondition = New-Object System.Windows.Automation.PropertyCondition(
                [System.Windows.Automation.AutomationElement]::NameProperty,
                $TargetValue
            )
            $condition = New-Object System.Windows.Automation.AndCondition($pidCondition, $nameCondition)
            $option = [System.Windows.Automation.AutomationElement]::RootElement.FindFirst(
                [System.Windows.Automation.TreeScope]::Descendants,
                $condition
            )
        }
    }

    if ($null -eq $option) {
        return $false
    }

    try {
        $option.SetFocus()
    } catch {
    }

    try {
        $invokePattern = $option.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
        $invokePattern.Invoke()
        Start-Sleep -Milliseconds 150
        if ((& $readCurrentValue) -eq $TargetValue) {
            return $true
        }
    } catch {
    }

    try {
        $legacyPattern = $option.GetCurrentPattern([System.Windows.Automation.LegacyIAccessiblePattern]::Pattern)
        $legacyPattern.DoDefaultAction()
        Start-Sleep -Milliseconds 150
        if ((& $readCurrentValue) -eq $TargetValue) {
            return $true
        }
    } catch {
    }

    try {
        $selectionPattern = $option.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern)
        $selectionPattern.Select()
        Start-Sleep -Milliseconds 150
        if ((& $readCurrentValue) -eq $TargetValue) {
            return $true
        }
    } catch {
    }

    try {
        $rect = $option.GetCurrentPropertyValue([System.Windows.Automation.AutomationElement]::BoundingRectangleProperty)
        if (Click-BoundingRectangleCenter -Rect $rect) {
            Start-Sleep -Milliseconds 200
            if ((& $readCurrentValue) -eq $TargetValue) {
                return $true
            }
        }
    } catch {
    }

    return $false
}

function Set-ElementToggle {
    param(
        [System.Windows.Automation.AutomationElement]$Element,
        [bool]$CheckState
    )

    if ($null -eq $Element) {
        return $false
    }

    try {
        $togglePattern = $Element.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
        $currentState = $Element.GetCurrentPropertyValue([System.Windows.Automation.TogglePattern]::ToggleStateProperty)
        $targetState = if ($CheckState) {
            [System.Windows.Automation.ToggleState]::On
        } else {
            [System.Windows.Automation.ToggleState]::Off
        }

        if ($currentState -ne $targetState) {
            $togglePattern.Toggle()
        }

        return $true
    } catch {
        return $false
    }
}

function Test-ElementEnabled {
    param([System.Windows.Automation.AutomationElement]$Element)

    if ($null -eq $Element) {
        return $false
    }

    try {
        return [bool]$Element.GetCurrentPropertyValue([System.Windows.Automation.AutomationElement]::IsEnabledProperty)
    } catch {
        return $false
    }
}

function Wait-ElementCondition {
    param(
        [System.Windows.Automation.AutomationElement]$Root,
        [string]$ElementName,
        [string]$ConditionStr,
        [int]$TimeoutMs
    )

    $conditionType = ""
    $conditionValue = ""

    if ($ConditionStr.StartsWith("contains:")) {
        $conditionType = "contains"
        $conditionValue = $ConditionStr.Substring(9)
    } elseif ($ConditionStr.StartsWith("regex:")) {
        $conditionType = "regex"
        $conditionValue = $ConditionStr.Substring(6)
    } elseif ($ConditionStr.StartsWith("equals:")) {
        $conditionType = "equals"
        $conditionValue = $ConditionStr.Substring(7)
    } elseif ($ConditionStr -in @("notempty", "enabled", "disabled", "exists", "notexists")) {
        $conditionType = $ConditionStr
    } else {
        $conditionType = "contains"
        $conditionValue = $ConditionStr
    }

    $start = Get-Date
    $lastText = ""
    $lastEnabled = $false

    while (((Get-Date) - $start).TotalMilliseconds -lt $TimeoutMs) {
        $element = Find-Element -Root $Root -ElementName $ElementName

        if ($conditionType -eq "exists") {
            if ($null -ne $element) {
                return @{ success = $true; text = Get-ElementText -Element $element; enabled = Test-ElementEnabled -Element $element }
            }
            Start-Sleep -Milliseconds 200
            continue
        }

        if ($conditionType -eq "notexists") {
            if ($null -eq $element) {
                return @{ success = $true; text = ""; enabled = $false }
            }
            Start-Sleep -Milliseconds 200
            continue
        }

        if ($null -eq $element) {
            Start-Sleep -Milliseconds 200
            continue
        }

        $lastText = Get-ElementText -Element $element
        $lastEnabled = Test-ElementEnabled -Element $element
        $matched = $false

        switch ($conditionType) {
            "contains" {
                if (-not [string]::IsNullOrEmpty($lastText) -and $lastText.Contains($conditionValue)) {
                    $matched = $true
                }
            }
            "regex" {
                if (-not [string]::IsNullOrEmpty($lastText) -and $lastText -match $conditionValue) {
                    $matched = $true
                }
            }
            "equals" {
                if ($lastText -eq $conditionValue) {
                    $matched = $true
                }
            }
            "notempty" {
                if (-not [string]::IsNullOrWhiteSpace($lastText)) {
                    $matched = $true
                }
            }
            "enabled" {
                if ($lastEnabled) {
                    $matched = $true
                }
            }
            "disabled" {
                if (-not $lastEnabled) {
                    $matched = $true
                }
            }
        }

        if ($matched) {
            return @{ success = $true; text = $lastText; enabled = $lastEnabled }
        }

        Start-Sleep -Milliseconds 200
    }

    return @{ success = $false; text = $lastText; enabled = $lastEnabled }
}

function Close-MainWindow {
    param([System.Windows.Automation.AutomationElement]$MainWindow)

    if ($null -eq $MainWindow) {
        return $false
    }

    try {
        $windowPattern = $MainWindow.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern)
        $windowPattern.Close()
        return $true
    } catch {
    }

    $windowProcessId = Get-PropertyValueOrNull -Element $MainWindow -Property ([System.Windows.Automation.AutomationElement]::ProcessIdProperty)
    if ([int]$windowProcessId -gt 0) {
        try {
            $process = Get-Process -Id ([int]$windowProcessId) -ErrorAction Stop
            return $process.CloseMainWindow()
        } catch {
        }
    }

    return $false
}

$actionName = $Action.ToLowerInvariant()

if ($actionName -eq "windows") {
    Write-UiaSuccess -Message "获取顶层窗口成功" -Data (Get-DesktopWindows)
    exit 0
}

$mainWindow = $null

if ($actionName -eq "waitwindow") {
    $mainWindow = Wait-MainWindow -TargetWindow $Window -TargetProcessId $ProcessId -TargetProcessName $ProcessName -TimeoutMs $Timeout
    if ($null -eq $mainWindow) {
        Write-UiaError "等待主窗口超时"
        exit 1
    }

    Write-UiaSuccess -Message "主窗口已出现" -Data (Get-ElementSnapshot -Element $mainWindow)
    exit 0
}

$mainWindow = Find-MainWindow -TargetWindow $Window -TargetProcessId $ProcessId -TargetProcessName $ProcessName
if ($null -eq $mainWindow) {
    Write-UiaError "未找到目标主窗口"
    exit 1
}

switch ($actionName) {
    "windowinfo" {
        Write-UiaSuccess -Message "读取主窗口成功" -Data (Get-ElementSnapshot -Element $mainWindow)
    }

    "closewindow" {
        if (Close-MainWindow -MainWindow $mainWindow) {
            Write-UiaSuccess -Message "关闭主窗口命令已发送" -Data (Get-ElementSnapshot -Element $mainWindow)
        } else {
            Write-UiaError "关闭主窗口失败"
            exit 1
        }
    }

    "list" {
        Write-UiaSuccess -Message "获取控件列表成功" -Data (Get-AllElements -Root $mainWindow)
    }

    "exists" {
        if ([string]::IsNullOrWhiteSpace($Name)) {
            Write-UiaError "需要指定 -Name"
            exit 1
        }

        $element = Find-Element -Root $mainWindow -ElementName $Name
        if ($null -ne $element) {
            Write-UiaSuccess -Message "控件存在" -Data (Get-ElementSnapshot -Element $element)
        } else {
            Write-UiaInfo -Message "控件不存在" -Data @{ name = $Name }
        }
    }

    "read" {
        if ([string]::IsNullOrWhiteSpace($Name)) {
            Write-UiaError "需要指定 -Name"
            exit 1
        }

        $element = Find-Element -Root $mainWindow -ElementName $Name
        if ($null -eq $element) {
            Write-UiaError "未找到控件: $Name"
            exit 1
        }

        Write-UiaSuccess -Message "读取成功" -Data (Get-ElementSnapshot -Element $element)
    }

    "click" {
        if ([string]::IsNullOrWhiteSpace($Name)) {
            Write-UiaError "需要指定 -Name"
            exit 1
        }

        $element = Find-Element -Root $mainWindow -ElementName $Name
        if ($null -eq $element) {
            Write-UiaError "未找到控件: $Name"
            exit 1
        }

        if (Invoke-ElementClick -Element $element) {
            Write-UiaSuccess -Message "点击成功" -Data (Get-ElementSnapshot -Element $element)
        } else {
            Write-UiaError "点击失败: $Name"
            exit 1
        }
    }

    "select" {
        if ([string]::IsNullOrWhiteSpace($Name)) {
            Write-UiaError "需要指定 -Name"
            exit 1
        }
        if ([string]::IsNullOrWhiteSpace($Value)) {
            Write-UiaError "需要指定 -Value"
            exit 1
        }

        $element = Find-Element -Root $mainWindow -ElementName $Name
        if ($null -eq $element) {
            Write-UiaError "未找到控件: $Name"
            exit 1
        }

        if (Select-ElementOption -Root $mainWindow -Element $element -TargetValue $Value) {
            Write-UiaSuccess -Message "选择成功" -Data (Get-ElementSnapshot -Element $element)
        } else {
            Write-UiaError "选择失败: $Name -> $Value"
            exit 1
        }
    }

    "write" {
        if ([string]::IsNullOrWhiteSpace($Name)) {
            Write-UiaError "需要指定 -Name"
            exit 1
        }
        if ([string]::IsNullOrEmpty($Value)) {
            Write-UiaError "需要指定 -Value"
            exit 1
        }

        $element = Find-Element -Root $mainWindow -ElementName $Name
        if ($null -eq $element) {
            Write-UiaError "未找到控件: $Name"
            exit 1
        }

        if (Set-ElementValue -Element $element -TargetValue $Value) {
            Write-UiaSuccess -Message "写入成功" -Data (Get-ElementSnapshot -Element $element)
        } else {
            Write-UiaError "写入失败: $Name"
            exit 1
        }
    }

    "toggle" {
        if ([string]::IsNullOrWhiteSpace($Name)) {
            Write-UiaError "需要指定 -Name"
            exit 1
        }

        $element = Find-Element -Root $mainWindow -ElementName $Name
        if ($null -eq $element) {
            Write-UiaError "未找到控件: $Name"
            exit 1
        }

        if (Set-ElementToggle -Element $element -CheckState $Check) {
            Write-UiaSuccess -Message "切换成功" -Data (Get-ElementSnapshot -Element $element)
        } else {
            Write-UiaError "切换失败: $Name"
            exit 1
        }
    }

    "enabled" {
        if ([string]::IsNullOrWhiteSpace($Name)) {
            Write-UiaError "需要指定 -Name"
            exit 1
        }

        $element = Find-Element -Root $mainWindow -ElementName $Name
        if ($null -eq $element) {
            Write-UiaError "未找到控件: $Name"
            exit 1
        }

        Write-UiaSuccess -Message "读取可用状态成功" -Data @{
            name = $Name
            isEnabled = (Test-ElementEnabled -Element $element)
        }
    }

    "wait" {
        if ([string]::IsNullOrWhiteSpace($Name)) {
            Write-UiaError "需要指定 -Name"
            exit 1
        }
        if ([string]::IsNullOrWhiteSpace($Condition)) {
            Write-UiaError "需要指定 -Condition"
            exit 1
        }

        $result = Wait-ElementCondition -Root $mainWindow -ElementName $Name -ConditionStr $Condition -TimeoutMs $Timeout
        if ($result.success) {
            Write-UiaSuccess -Message "条件满足" -Data $result
        } else {
            Write-UiaError "等待超时"
            exit 1
        }
    }

    default {
        if ([string]::IsNullOrWhiteSpace($Action)) {
            Write-UiaError "需要指定 -Action。使用 -Help 查看帮助"
        } else {
            Write-UiaError "未知操作: $Action"
        }
        exit 1
    }
}
