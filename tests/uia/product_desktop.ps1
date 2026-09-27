param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [int]$Cycles = 100,
    [int]$MinimumSeconds = 28800,
    [int]$ShareSeconds = 60,
    [int]$LogPairSeconds = 30,
    [int]$StopSettleSeconds = 10,
    [int]$RoomSettleSeconds = 10,
    [switch]$ProbeOnly,
    [switch]$Pilot
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class ProductDesktop {
    [DllImport("user32.dll")] static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern bool GetUserObjectInformation(IntPtr h, int index, StringBuilder value, int length, out int needed);
    [DllImport("user32.dll")] static extern bool CloseDesktop(IntPtr h);
    public static bool Unlocked() {
        var h = OpenInputDesktop(0, false, 1);
        if (h == IntPtr.Zero) return false;
        try { var name = new StringBuilder(256); int needed;
            return GetUserObjectInformation(h, 2, name, 512, out needed) && name.ToString() == "Default";
        } finally { CloseDesktop(h); }
    }
}
'@
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Output directory must be new' }
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$null = New-Item -ItemType Directory -Path $OutputDirectory
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$script:runId = [Guid]::NewGuid().ToString('N')
$script:cycle = 0
$script:completed = 0
$script:child = $null
$script:step = 'prerequisites'
$script:operation = ''
$script:lastResourceSample = [DateTime]::MinValue
$script:layout = 'unknown'
$started = [DateTime]::UtcNow
$oldAppData = $env:APPDATA
$oldLocalAppData = $env:LOCALAPPDATA
$oldQtPlatform = $env:QT_QPA_PLATFORM
$oldQtAccessibility = $env:QT_ACCESSIBILITY
function Record([string]$Action, [string]$Phase) {
    $row = [ordered]@{schema=1; run_id=$script:runId; cycle=$script:cycle;
        operation_id=$script:operation; action=$Action; phase=$Phase;
        pid=$(if ($script:child) { $script:child.Id } else { $null }); utc=[DateTime]::UtcNow.ToString('o')}
    [IO.File]::AppendAllText((Join-Path $OutputDirectory 'uia-actions.jsonl'),
        (($row | ConvertTo-Json -Compress) + "`n"), [Text.UTF8Encoding]::new($false))
}
function Save-Result([string]$Verdict, [string]$Reason) {
    [ordered]@{schema=1; run_id=$script:runId; verdict=$Verdict; reason=$Reason;
        cycles_requested=$Cycles; cycles_completed=$script:completed;
        minimum_seconds=$MinimumSeconds; started_utc=$started.ToString('o');
        finished_utc=[DateTime]::UtcNow.ToString('o'); ui_only=$true} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'uia-result.json') -Encoding UTF8
}
function Sample-Resource([string]$Phase) {
    if (!$script:child) { return }
    $now = [DateTime]::UtcNow
    if ($Phase -eq 'active' -and ($now - $script:lastResourceSample).TotalSeconds -lt 1) { return }
    $script:lastResourceSample = $now
    $process = Get-Process -Id $script:child.Id -ErrorAction SilentlyContinue
    $row = [ordered]@{schema=1; run_id=$script:runId; cycle=$script:cycle;
        pid=$script:child.Id; operation_id=$script:operation; utc=$now.ToString('o');
        phase=$Phase; layout=$script:layout; process_alive=($null -ne $process);
        private_bytes=$(if ($process) { $process.PrivateMemorySize64 } else { 0 });
        handles=$(if ($process) { $process.HandleCount } else { 0 });
        threads=$(if ($process) { $process.Threads.Count } else { 0 });
        gpu_local_bytes=$null; gpu_nonlocal_bytes=$null; queue_depth=$null}
    [IO.File]::AppendAllText((Join-Path $OutputDirectory 'uia-resources.jsonl'),
        (($row | ConvertTo-Json -Compress) + "`n"), [Text.UTF8Encoding]::new($false))
}
function Get-Nodes {
    if (!$script:child -or $script:child.HasExited) { throw "Product exited during $script:step" }
    $condition = New-Object Windows.Automation.PropertyCondition(
        [Windows.Automation.AutomationElement]::ProcessIdProperty, [int]$script:child.Id)
    return @([Windows.Automation.AutomationElement]::RootElement.FindAll(
        [Windows.Automation.TreeScope]::Descendants, $condition))
}
function Save-Tree([string]$Name) {
    if (!$script:child -or $script:child.HasExited) { return }
    $rows = foreach ($node in Get-Nodes) {
        try {
            $c = $node.Current
            [ordered]@{id=$c.AutomationId; role=$c.ControlType.ProgrammaticName;
                name_present=![string]::IsNullOrWhiteSpace($c.Name); pid=$c.ProcessId;
                enabled=$c.IsEnabled; offscreen=$c.IsOffscreen;
                patterns=@($node.GetSupportedPatterns() | ForEach-Object { $_.ProgrammaticName })}
        } catch [Windows.Automation.ElementNotAvailableException] { }
    }
    ConvertTo-Json -InputObject @($rows) -Depth 4 |
        Set-Content -LiteralPath (Join-Path $OutputDirectory "$Name.json") -Encoding UTF8
}
function Find-Node([string]$Id, [Windows.Automation.ControlType]$Role, [switch]$Optional) {
    $script:step = "discover $Id"
    $matches = @(Get-Nodes | Where-Object {
        try { $c = $_.Current
            $c.AutomationId -eq $Id -or $c.AutomationId.EndsWith(".$Id", [StringComparison]::Ordinal)
        } catch [Windows.Automation.ElementNotAvailableException] { $false }
    })
    if ($Optional -and $matches.Count -eq 0) { return $null }
    if ($matches.Count -ne 1) { throw "CONTROL_COUNT: $Id=$($matches.Count) pid=$($script:child.Id)" }
    $c = $matches[0].Current
    if ($Optional -and $c.IsOffscreen) { return $null }
    if ($c.ControlType -ne $Role -or [string]::IsNullOrWhiteSpace($c.Name) -or $c.IsOffscreen) {
        throw "CONTROL_CONTRACT: $Id role=$($c.ControlType.ProgrammaticName) enabled=$($c.IsEnabled) offscreen=$($c.IsOffscreen)"
    }
    return $matches[0]
}
function Wait-For([string]$Description, [scriptblock]$Condition, [int]$Seconds = 45) {
    $script:step = $Description
    $deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    do {
        if ($script:child.HasExited) { throw "PROCESS_EXIT: $Description code=$($script:child.ExitCode)" }
        Sample-Resource 'active'
        try { $value = & $Condition; if ($value) { return $value } }
        catch [Windows.Automation.ElementNotAvailableException] { }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "TIMEOUT: $Description"
}
function Require-Pattern($Node, [Windows.Automation.AutomationPattern]$Id) {
    $pattern = $null
    if (!$Node.TryGetCurrentPattern($Id, [ref]$pattern)) {
        throw "MISSING_PATTERN: $($Node.Current.AutomationId) $($Id.ProgrammaticName)"
    }
    return $pattern
}
function Invoke([string]$Id, [Windows.Automation.ControlType]$Role = [Windows.Automation.ControlType]::Button) {
    $node = Find-Node $Id $Role
    if (!$node.Current.IsEnabled) { throw "DISABLED_CONTROL: $Id" }
    (Require-Pattern $node ([Windows.Automation.InvokePattern]::Pattern)).Invoke()
}
function Set-Value([string]$Id, [string]$Value) {
    $pattern = Require-Pattern (Find-Node $Id ([Windows.Automation.ControlType]::Edit)) ([Windows.Automation.ValuePattern]::Pattern)
    if ($pattern.Current.IsReadOnly) { throw "READ_ONLY: $Id" }
    $pattern.SetValue($Value)
}
function Toggle-To([string]$Id, [Windows.Automation.ToggleState]$Expected) {
    $node = Find-Node $Id ([Windows.Automation.ControlType]::CheckBox)
    $pattern = Require-Pattern $node ([Windows.Automation.TogglePattern]::Pattern)
    if ($pattern.Current.ToggleState -ne $Expected) { $pattern.Toggle() }
    $null = Wait-For "$Id toggle=$Expected" {
        $current = Require-Pattern (Find-Node $Id ([Windows.Automation.ControlType]::CheckBox)) `
            ([Windows.Automation.TogglePattern]::Pattern)
        $current.Current.ToggleState -eq $Expected
    }
}
function Action([string]$Name, [scriptblock]$Body) {
    $script:operation = [Guid]::NewGuid().ToString('N')
    Record $Name 'requested'
    & $Body
    Record $Name 'uia_observed'
}
function Wait-Top([string]$Id) {
    return Wait-For "$Id window" {
        $windows = @(Get-Nodes | Where-Object {
                $_.Current.AutomationId -eq $Id -or
                $_.Current.AutomationId.EndsWith(".$Id", [StringComparison]::Ordinal) })
        if ($windows.Count -gt 1) { throw "AMBIGUOUS_WINDOW: $Id" }
        if ($windows.Count -eq 1) {
            $c = $windows[0].Current
            if ($c.ControlType -ne [Windows.Automation.ControlType]::Window -or
                [string]::IsNullOrWhiteSpace($c.Name) -or $c.IsOffscreen) {
                throw "WINDOW_CONTRACT: $Id role=$($c.ControlType.ProgrammaticName)"
            }
            $null = Require-Pattern $windows[0] ([Windows.Automation.WindowPattern]::Pattern)
            $windows[0]
        }
    }
}
function Select-FirstShareSource {
    $combo = Find-Node 'screenShareSource' ([Windows.Automation.ControlType]::ComboBox)
    $expand = $null
    if ($combo.TryGetCurrentPattern([Windows.Automation.ExpandCollapsePattern]::Pattern, [ref]$expand)) {
        $expand.Expand()
    } else { (Require-Pattern $combo ([Windows.Automation.InvokePattern]::Pattern)).Invoke() }
    $list = Wait-For 'share source list' {
        $lists = @($combo.FindAll([Windows.Automation.TreeScope]::Descendants,
            [Windows.Automation.Condition]::TrueCondition) | Where-Object {
                $_.Current.ControlType -eq [Windows.Automation.ControlType]::List })
        if ($lists.Count -eq 1) { $lists[0] }
    }
    $items = @($list.FindAll([Windows.Automation.TreeScope]::Children,
        [Windows.Automation.Condition]::TrueCondition) | Where-Object {
            $_.Current.ControlType -eq [Windows.Automation.ControlType]::ListItem })
    if ($items.Count -lt 1) { throw 'NO_SHARE_SOURCE' }
    (Require-Pattern $items[0] ([Windows.Automation.SelectionItemPattern]::Pattern)).Select()
    $null = Wait-For 'share source committed' {
        $value = Require-Pattern (Find-Node 'screenShareSource' ([Windows.Automation.ControlType]::ComboBox)) `
            ([Windows.Automation.ValuePattern]::Pattern)
        $value.Current.Value -eq $items[0].Current.Name
    }
    Invoke 'screenShareAccept'
}
function Share-State([Windows.Automation.ToggleState]$Expected) {
    $node = Find-Node 'meetingShareScreen' ([Windows.Automation.ControlType]::CheckBox)
    return (Require-Pattern $node ([Windows.Automation.TogglePattern]::Pattern)).Current.ToggleState -eq $Expected
}
function Start-Product {
    $profile = Join-Path $OutputDirectory 'profile'
    $env:APPDATA = Join-Path $profile 'Roaming'
    $env:LOCALAPPDATA = Join-Path $profile 'Local'
    $null = New-Item -ItemType Directory -Force -Path $env:APPDATA,$env:LOCALAPPDATA
    $env:QT_QPA_PLATFORM = 'windows'
    $env:QT_ACCESSIBILITY = '1'
    $script:child = Start-Process -FilePath $Executable -WorkingDirectory (Split-Path $Executable) -PassThru
    $null = Wait-For 'product login or main window' {
            (Find-Node 'mainJoinMeeting' ([Windows.Automation.ControlType]::Button) -Optional) -or
            (Find-Node 'loginAccount' ([Windows.Automation.ControlType]::Edit) -Optional)
        }
        Save-Tree ("cycle-{0:d4}-initial" -f $script:cycle)
        if ($ProbeOnly) {
            $tabs = @(Get-Nodes | Where-Object {
                $_.Current.ControlType -eq [Windows.Automation.ControlType]::TabItem })
            if ($tabs.Count -ne 3) { throw "GUEST_TAB_COUNT: $($tabs.Count)" }
            (Require-Pattern $tabs[2] ([Windows.Automation.InvokePattern]::Pattern)).Invoke()
            Invoke 'guestBtn'
            $null = Wait-For 'guest main window' {
                Find-Node 'mainJoinMeeting' ([Windows.Automation.ControlType]::Button) -Optional
            }
            Save-Tree 'probe-main'
            Invoke 'mainAccountMenu'
            $null = Wait-For 'probe account menu action' {
                Find-Node 'mainPostMeetingTelemetry' ([Windows.Automation.ControlType]::MenuItem) -Optional
            } 5
            Save-Tree 'probe-account-menu'
            Invoke 'mainPostMeetingTelemetry' ([Windows.Automation.ControlType]::MenuItem)
            $null = Wait-Top 'telemetryPostMeetingDialog'
            Save-Tree 'probe-telemetry-dialog'
            Invoke 'telemetryPostClose'
            return
        }
        if (!(Find-Node 'mainJoinMeeting' ([Windows.Automation.ControlType]::Button) -Optional)) {
            Action 'login' {
                Set-Value 'loginAccount' $env:LIVEKIT_UIA_ACCOUNT
                Set-Value 'loginPassword' $env:LIVEKIT_UIA_PASSWORD
                if ($env:LIVEKIT_UIA_SERVICE_URL) {
                    Invoke 'serverSettingsToggle'
                    Set-Value 'serverBaseUrl' $env:LIVEKIT_UIA_SERVICE_URL
                }
                Invoke 'primaryBtn'
                $null = Wait-For 'main window after account login' {
                    Find-Node 'mainJoinMeeting' ([Windows.Automation.ControlType]::Button) -Optional
                } 90
            }
        }
}
function Run-Cycle {
    Action 'join' {
            Invoke 'mainJoinMeeting'
            $null = Wait-Top 'joinMeetingDialog'
            Set-Value 'joinMeetingId' $env:LIVEKIT_UIA_MEETING_ID
            Set-Value 'joinDisplayName' ("U-{0}-{1:d4}" -f $script:runId.Substring(0,8),$script:cycle)
            Toggle-To 'joinMicrophone' ([Windows.Automation.ToggleState]::On)
            Toggle-To 'joinCamera' ([Windows.Automation.ToggleState]::On)
            Invoke 'joinBtn'
            $null = Wait-Top 'MeetingRoomWindow'
            $layoutPattern = Require-Pattern `
                (Find-Node 'videoPageSize' ([Windows.Automation.ControlType]::ComboBox)) `
                ([Windows.Automation.ValuePattern]::Pattern)
            $script:layout = $layoutPattern.Current.Value
            Sample-Resource 'joined'
        }
        Action 'page' {
            $null = Find-Node 'videoPageIndicator' ([Windows.Automation.ControlType]::Text)
            if ((Find-Node 'previousVideoPage' ([Windows.Automation.ControlType]::Button)).Current.IsEnabled -or
                !(Find-Node 'nextVideoPage' ([Windows.Automation.ControlType]::Button)).Current.IsEnabled) {
                throw 'REMOTE_PAGE_UNAVAILABLE'
            }
            Invoke 'nextVideoPage'
            $null = Wait-For 'second video page' {
                (Find-Node 'previousVideoPage' ([Windows.Automation.ControlType]::Button)).Current.IsEnabled
            }
            Invoke 'previousVideoPage'
            $null = Wait-For 'first video page restored' {
                !(Find-Node 'previousVideoPage' ([Windows.Automation.ControlType]::Button)).Current.IsEnabled
            }
        }
        Action 'share_start' {
            Invoke 'meetingShareScreen' ([Windows.Automation.ControlType]::CheckBox)
            $null = Wait-Top 'screen-share-picker'
            Select-FirstShareSource
            $null = Wait-For 'share control active' { Share-State ([Windows.Automation.ToggleState]::On) } 90
            $activeUntil = [DateTime]::UtcNow.AddSeconds($ShareSeconds)
            while ([DateTime]::UtcNow -lt $activeUntil) {
                if ($script:child.HasExited) { throw 'PROCESS_EXIT_DURING_SHARE' }
                Sample-Resource 'share_active'
                Start-Sleep -Seconds 1
            }
        }
        Action 'share_stop' {
            Invoke 'meetingShareScreen' ([Windows.Automation.ControlType]::CheckBox)
            $null = Wait-For 'share control idle' { Share-State ([Windows.Automation.ToggleState]::Off) } 90
            Sample-Resource 'share_stopped'
        }
        $stopUntil = [DateTime]::UtcNow.AddSeconds($StopSettleSeconds)
        while ([DateTime]::UtcNow -lt $stopUntil) {
            if ($script:child.HasExited) { throw 'PROCESS_EXIT_AFTER_SHARE_STOP' }
            Sample-Resource 'share_settled'
            Start-Sleep -Seconds 1
        }
        Action 'logging' {
            Invoke 'meetingConsole'
            $null = Wait-Top 'meetingLogConsole'
            Toggle-To 'consoleSaveLogs' ([Windows.Automation.ToggleState]::Off)
            $offStart = [DateTime]::UtcNow
            while (([DateTime]::UtcNow - $offStart).TotalSeconds -lt $LogPairSeconds) {
                if ($script:child.HasExited) { throw 'PROCESS_EXIT_DURING_LOG_PAIR' }
                Sample-Resource 'log_off'
                Start-Sleep -Seconds 1
            }
            $offEnd = [DateTime]::UtcNow
            Toggle-To 'consoleSaveLogs' ([Windows.Automation.ToggleState]::On)
            $onStart = [DateTime]::UtcNow
            while (([DateTime]::UtcNow - $onStart).TotalSeconds -lt $LogPairSeconds) {
                if ($script:child.HasExited) { throw 'PROCESS_EXIT_DURING_LOG_PAIR' }
                Sample-Resource 'log_on'
                Start-Sleep -Seconds 1
            }
            $window = [ordered]@{run_id=$script:runId; cycle=$script:cycle;
                operation_id=$script:operation; pid=$script:child.Id;
                off_start_utc=$offStart.ToString('o'); off_end_utc=$offEnd.ToString('o');
                on_start_utc=$onStart.ToString('o'); on_end_utc=[DateTime]::UtcNow.ToString('o')}
            [IO.File]::AppendAllText((Join-Path $OutputDirectory 'uia-log-windows.jsonl'),
                (($window | ConvertTo-Json -Compress) + "`n"), [Text.UTF8Encoding]::new($false))
        }
        $releaseDue = $started.AddSeconds($script:cycle * $MinimumSeconds / $Cycles)
        while ([DateTime]::UtcNow -lt $releaseDue) {
            if ($script:child.HasExited) { throw 'PROCESS_EXIT_BEFORE_LEAVE' }
            Sample-Resource 'settled'
            Start-Sleep -Seconds 1
        }
        Action 'leave' {
            Invoke 'meetingLeave'
            $null = Wait-Top 'meetingLeaveConfirmation'
            Invoke 'meetingLeaveConfirm'
            $null = Wait-For 'meeting window closed' {
                !(Find-Node 'meetingLeave' ([Windows.Automation.ControlType]::Button) -Optional)
            } 90
            Sample-Resource 'room_released'
        }
        $roomUntil = [DateTime]::UtcNow.AddSeconds($RoomSettleSeconds)
        while ([DateTime]::UtcNow -lt $roomUntil) {
            if ($script:child.HasExited) { throw 'PROCESS_EXIT_AFTER_LEAVE' }
            Sample-Resource 'room_settled'
            Start-Sleep -Seconds 1
        }
        Action 'export' {
            $reportDeadline = [DateTime]::UtcNow.AddSeconds(90)
            do {
                Invoke 'mainAccountMenu'
                $null = Wait-For 'account menu export action' {
                    Find-Node 'mainPostMeetingTelemetry' ([Windows.Automation.ControlType]::MenuItem) -Optional
                }
                Invoke 'mainPostMeetingTelemetry' ([Windows.Automation.ControlType]::MenuItem)
                $null = Wait-Top 'telemetryPostMeetingDialog'
                if ((Find-Node 'telemetryExport' ([Windows.Automation.ControlType]::Button)).Current.IsEnabled) {
                    break
                }
                Invoke 'telemetryPostClose'
                Start-Sleep -Seconds 1
            } while ([DateTime]::UtcNow -lt $reportDeadline)
            if ([DateTime]::UtcNow -ge $reportDeadline) { throw 'TIMEOUT: completed telemetry report unavailable' }
            Invoke 'telemetryExport'
            $null = Wait-Top 'telemetryExportDirectory'
            $destination = Join-Path $OutputDirectory ("export-{0:d4}" -f $script:cycle)
            $null = New-Item -ItemType Directory -Path $destination
            Set-Value 'fileNameEdit' $destination
            Invoke 'telemetryExportAccept'
            $null = Wait-For 'diagnostic support bundle artifact' {
                @(Get-ChildItem -LiteralPath $destination -Filter 'cohavora-diagnostic-bundle-*' -Directory).Count -eq 1
            } 90
            $null = Wait-Top 'meetingTelemetryExportResult'
            Invoke 'meetingTelemetryExportDismiss'
            Invoke 'telemetryPostClose'
        }
}
function Stop-Product {
        Action 'process_exit' {
            $main = Wait-Top 'MeetingMainWindow'
            (Require-Pattern $main ([Windows.Automation.WindowPattern]::Pattern)).Close()
            if (!$script:child.WaitForExit(60000)) { throw 'PRODUCT_SHUTDOWN_TIMEOUT' }
            if ($script:child.ExitCode -ne 0) { throw "PRODUCT_EXIT_CODE: $($script:child.ExitCode)" }
        }
        Sample-Resource 'final_exit'
}
function Cleanup-Product {
        if ($script:child -and !$script:child.HasExited) {
            $script:child.Kill()
            $script:child.WaitForExit()
        }
        if ($script:child) { $script:child.Dispose(); $script:child = $null }
}
try {
    if (![Environment]::UserInteractive -or ![ProductDesktop]::Unlocked()) {
        Save-Result 'NOT_RUN' 'unlocked_interactive_default_desktop_required'; exit 77
    }
    if ($ProbeOnly -and $Pilot) { throw 'Select either ProbeOnly or Pilot' }
    if (!$ProbeOnly -and (!$env:LIVEKIT_UIA_ACCOUNT -or !$env:LIVEKIT_UIA_PASSWORD -or
        !$env:LIVEKIT_UIA_MEETING_ID -or (!$Pilot -and $env:LIVEKIT_UIA_DEDICATED_DESKTOP -ne '1'))) {
        Save-Result 'NOT_RUN' 'dedicated_desktop_account_and_meeting_required'; exit 77
    }
    if ($Pilot -and $Cycles -ne 1) { throw 'PILOT_PROFILE_REQUIRED: one cycle' }
    if (!$ProbeOnly -and !$Pilot -and ($Cycles -ne 100 -or $MinimumSeconds -lt 28800 -or
        $ShareSeconds -lt 60 -or $LogPairSeconds -lt 30 -or
        $StopSettleSeconds -lt 10 -or $RoomSettleSeconds -lt 10)) {
        throw 'FORMAL_PROFILE_REQUIRED: 100 complete cycles and at least 8 hours'
    }
    $script:cycle = 1
    Start-Product
    for ($index=1; $index -le $(if ($ProbeOnly) { 1 } else { $Cycles }); ++$index) {
        $script:cycle = $index
        if (!$ProbeOnly) {
            $due = $started.AddSeconds(($index - 1) * $MinimumSeconds / $Cycles)
            while ([DateTime]::UtcNow -lt $due) { Start-Sleep -Seconds 1 }
        }
        if (!$ProbeOnly) { Run-Cycle }
        if (!$ProbeOnly) { ++$script:completed }
    }
    if (!$ProbeOnly) { Stop-Product }
    if ($ProbeOnly) { Save-Result 'PROBED' 'tree_observation_only' }
    elseif ($Pilot) { Save-Result 'PILOT_COMPLETE' 'not_formal_acceptance' }
    elseif (([DateTime]::UtcNow - $started).TotalSeconds -lt $MinimumSeconds) {
        Save-Result 'INCONCLUSIVE' 'duration_shorter_than_eight_hours'; exit 2
    } else { Save-Result 'UIA_COMPLETE' 'independent_witnesses_required' }
} catch {
    $failure = $_.ToString()
    try { Save-Tree ("cycle-{0:d4}-failure" -f $script:cycle) } catch { }
    Save-Result 'FAIL' $failure
    Write-Error "FAIL at $script:step : $failure" -ErrorAction Continue
    exit 1
} finally {
    Cleanup-Product
    $env:APPDATA = $oldAppData
    $env:LOCALAPPDATA = $oldLocalAppData
    $env:QT_QPA_PLATFORM = $oldQtPlatform
    $env:QT_ACCESSIBILITY = $oldQtAccessibility
}
exit 0
