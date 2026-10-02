param(
    [Parameter(Mandatory=$true)][string]$Fixture,
    [string]$OutputDirectory = "$PSScriptRoot/../../out/uia-results",
    [switch]$ProbeOnly,
    [switch]$ProbeComboSelection,
    [ValidateSet('console','whiteboard','whiteboard_clear','whiteboard_files','whiteboard_file_edges','join','settings','meeting')][string]$Scenario = 'console',
    [ValidateSet('zh_CN','en_US')][string]$Language = 'zh_CN'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class UiaDesktop {
    [DllImport("user32.dll", SetLastError=true)] static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);
    [DllImport("user32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool GetUserObjectInformation(IntPtr h, int index, StringBuilder value, int length, out int needed);
    [DllImport("user32.dll")] static extern bool CloseDesktop(IntPtr h);
    public static bool Available() {
        var h = OpenInputDesktop(0, false, 1);
        if (h == IntPtr.Zero) return false;
        try {
            var name = new StringBuilder(256); int needed;
            return GetUserObjectInformation(h, 2, name, 512, out needed) && name.ToString() == "Default";
        } finally { CloseDesktop(h); }
    }
}
'@
$Fixture = (Resolve-Path -LiteralPath $Fixture).Path
$OutputDirectory = Join-Path $OutputDirectory ((Get-Date -Format 'yyyyMMdd-HHmmss-fff') + "-$Scenario-$Language-$PID")
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$script:window = $null
$script:child = $null
$script:step = 'launch'
$script:checks = New-Object 'System.Collections.Generic.List[string]'
$script:deferred = @()
$oldPlatform = $env:QT_QPA_PLATFORM
$oldAccessibility = $env:QT_ACCESSIBILITY
function Save-Result([string]$Verdict, [string]$Detail) {
    $fixtureSource = if ($Scenario -in @('join','settings')) { 'entry_fixture.cpp' } elseif ($Scenario -eq 'meeting') { '../meeting/test_participant_snapshot_remediation.cpp' } elseif ($Scenario -eq 'whiteboard_file_edges') { 'whiteboard_files_fixture.cpp' } else { "${Scenario}_fixture.cpp" }
    $productionSource = switch ($Scenario) {
        'console' { 'meeting_log_console.cpp' }
        'join' { 'meeting_main_window.cpp' }
        'settings' { 'settings_dialog.cpp' }
        'meeting' { 'meeting_room_window.cpp' }
        default { 'whiteboard/whiteboard_panel.cpp' }
    }
    $sources = @($Fixture, $PSCommandPath, "$PSScriptRoot/$fixtureSource", "$PSScriptRoot/${Scenario}_steps.ps1", "$PSScriptRoot/test_${Scenario}.ps1",
        "$PSScriptRoot/../cmake/UiaTests.cmake", "$PSScriptRoot/../../src/ui/$productionSource")
    if ($Scenario -in @('whiteboard','meeting')) {
        $sources += "$PSScriptRoot/../../src/ui/whiteboard/accessible_combo_box.cpp"
        $sources += "$PSScriptRoot/../../src/ui/whiteboard/accessible_combo_box.h"
    }
    if ($Scenario -in @('whiteboard_files','whiteboard_file_edges')) {
        $sources += "$PSScriptRoot/whiteboard_file_helpers.ps1"
        $sources += "$PSScriptRoot/../../src/ui/whiteboard/whiteboard_image_loader.cpp"
        $sources += "$PSScriptRoot/../../src/ui/whiteboard/whiteboard_canvas.cpp"
        $sources += "$PSScriptRoot/../../src/core/whiteboard/whiteboard_document.cpp"
    }
    [ordered]@{ verdict=$Verdict; detail=$Detail; step=$script:step;
        utc=[DateTime]::UtcNow.ToString('o'); language=$Language; scenario=$Scenario;
        deferred=@($script:deferred);
        checks=@($script:checks.ToArray()); fingerprints=@(Get-FileHash -LiteralPath $sources -Algorithm SHA256 |
            Select-Object Path, Hash) } | ConvertTo-Json -Depth 6 |
        Set-Content -Encoding UTF8 "$OutputDirectory/result.json"
}
function Assert-That([bool]$Condition, [string]$Description) {
    $script:step = $Description
    if (!$Condition) { throw $Description }
    $script:checks.Add($Description)
}
function Wait-For([string]$Description, [scriptblock]$Check) {
    $script:step = $Description
    $timer = [Diagnostics.Stopwatch]::StartNew()
    do {
        if ($script:child.HasExited) { throw "Fixture exited: $($script:child.ExitCode) at $Description" }
        $value = & $Check
        if ($value) { $script:step = $Description; $script:checks.Add($Description); return $value }
        Start-Sleep -Milliseconds 100
    } while ($timer.Elapsed.TotalSeconds -lt 10)
    $script:step = $Description
    throw "Timed out: $Description"
}
function Save-Tree([string]$Name, $Root = $script:window) {
    if ($null -eq $Root) { return }
    $nodes = $Root.FindAll([Windows.Automation.TreeScope]::Subtree,
        [Windows.Automation.Condition]::TrueCondition)
    $rows = foreach ($node in $nodes) {
        $c = $node.Current
        $parent = [Windows.Automation.TreeWalker]::RawViewWalker.GetParent($node)
        $states = @{}
        foreach ($kind in @('Value','Toggle','Text','RangeValue','SelectionItem')) {
            $patternId = switch ($kind) {
                'Value' { [Windows.Automation.ValuePattern]::Pattern }
                'Toggle' { [Windows.Automation.TogglePattern]::Pattern }
                'Text' { [Windows.Automation.TextPattern]::Pattern }
                'RangeValue' { [Windows.Automation.RangeValuePattern]::Pattern }
                'SelectionItem' { [Windows.Automation.SelectionItemPattern]::Pattern }
            }
            $pattern = $null
            if ($node.TryGetCurrentPattern($patternId, [ref]$pattern)) {
                switch ($kind) {
                    'Value' { $states.value=$pattern.Current.Value; $states.readOnly=$pattern.Current.IsReadOnly }
                    'Toggle' { $states.toggle=$pattern.Current.ToggleState.ToString() }
                    'Text' { $states.text=$pattern.DocumentRange.GetText(4096) }
                    'RangeValue' { $states.rangeValue=$pattern.Current.Value; $states.minimum=$pattern.Current.Minimum; $states.maximum=$pattern.Current.Maximum }
                    'SelectionItem' { $states.selected=$pattern.Current.IsSelected }
                }
            }
        }
        [ordered]@{ id=$c.AutomationId; name=$c.Name; role=$c.ControlType.ProgrammaticName;
            class=$c.ClassName; pid=$c.ProcessId; enabled=$c.IsEnabled; offscreen=$c.IsOffscreen;
            runtimeId=@($node.GetRuntimeId()); parentRuntimeId=$(if ($parent) { @($parent.GetRuntimeId()) });
            state=$states;
            patterns=@($node.GetSupportedPatterns() | ForEach-Object { $_.ProgrammaticName }) }
    }
    ConvertTo-Json -InputObject @($rows) -Depth 6 | Set-Content -Encoding UTF8 "$OutputDirectory/$Name.json"
}
function Find-Control([string]$Id, [Windows.Automation.ControlType]$Role, [bool]$Enabled = $true, $Root = $script:window) {
    $script:step = "discover $Id"
    if ($script:child.HasExited) { throw "Fixture exited: $($script:child.ExitCode) at $script:step" }
    $matches = @($Root.FindAll([Windows.Automation.TreeScope]::Descendants,
        [Windows.Automation.Condition]::TrueCondition) | Where-Object {
        $c = $_.Current
        $c.ProcessId -eq $script:child.Id -and
        ($c.AutomationId -eq $Id -or $c.AutomationId.EndsWith(".$Id", [StringComparison]::Ordinal))
    })
    Assert-That ($matches.Count -eq 1) "$Id has exactly one process-scoped match (got $($matches.Count))"
    $node = $matches[0]
    Assert-That ($node.Current.ControlType -eq $Role) "$Id has expected role $($Role.ProgrammaticName)"
    Assert-That (![string]::IsNullOrWhiteSpace($node.Current.Name)) "$Id has an accessible name"
    Assert-That ($node.Current.IsEnabled -eq $Enabled) "$Id enabled=$Enabled"
    Assert-That (!$node.Current.IsOffscreen) "$Id is onscreen"
    return $node
}
function Require-Pattern($Node, [Windows.Automation.AutomationPattern]$PatternId) {
    $script:step = "require $($Node.Current.AutomationId) $($PatternId.ProgrammaticName)"
    $pattern = $null
    if (!$Node.TryGetCurrentPattern($PatternId, [ref]$pattern)) {
        throw "MISSING_PATTERN: $script:step; coordinate/input fallback is forbidden"
    }
    return $pattern
}
function Get-LatestState {
    $latest = Get-ChildItem -LiteralPath $OutputDirectory -Filter 'model-state.json.*' |
        Where-Object { $_.Name -match '^model-state\.json\.\d{8}$' } |
        Sort-Object Name | Select-Object -Last 1
    if ($null -eq $latest) { return $null }
    return (Get-Content -LiteralPath $latest.FullName -Raw | ConvertFrom-Json)
}
try {
    if (![Environment]::UserInteractive -or ![UiaDesktop]::Available()) {
        Save-Result 'NOT_RUN' 'Unlocked interactive Default desktop required'
        Write-Output "NOT_RUN: unlocked interactive Default desktop required; $OutputDirectory"
        exit 77
    }
    $env:QT_QPA_PLATFORM = 'windows'
    $env:QT_ACCESSIBILITY = '1'
    $fixtureArguments = @("--language=$Language")
    if ($Scenario -in @('whiteboard_clear','whiteboard_files','whiteboard_file_edges','settings','meeting')) {
        $fixtureArguments += @('--state-file', ('"' + "$OutputDirectory/model-state.json" + '"'))
    }
    if ($Scenario -eq 'settings') { $fixtureArguments += '--settings' }
    if ($Scenario -eq 'meeting') { $fixtureArguments += '--uia-meeting-fixture' }
    if ($Scenario -eq 'whiteboard_file_edges') { $fixtureArguments += '--file-edges' }
    $script:child = Start-Process -FilePath $Fixture -ArgumentList $fixtureArguments -PassThru -RedirectStandardError "$OutputDirectory/fixture-stderr.log"
    $pidCondition = New-Object Windows.Automation.PropertyCondition(
        [Windows.Automation.AutomationElement]::ProcessIdProperty, [int]$script:child.Id)
    $script:window = Wait-For 'process-scoped window discovery' {
        $windows = @([Windows.Automation.AutomationElement]::RootElement.FindAll(
            [Windows.Automation.TreeScope]::Children, $pidCondition) | Where-Object {
                $_.Current.ClassName -ne 'ConsoleWindowClass' -and
                $_.Current.ControlType -eq [Windows.Automation.ControlType]::Window
            })
        if ($windows.Count -gt 1) { throw 'Ambiguous fixture top-level windows' }
        if ($windows.Count -eq 1) { return $windows[0] }
    }
    Save-Tree 'initial-tree'
    if ($ProbeOnly) {
        if ($Scenario -in @('whiteboard_clear','whiteboard_files')) { . "$PSScriptRoot/${Scenario}_steps.ps1" }
        Save-Result 'PROBED' 'Tree observation only; no regression verdict'
        Write-Output "PROBED: $OutputDirectory/initial-tree.json"
    } else {
        . "$PSScriptRoot/${Scenario}_steps.ps1"
        if (!$script:child.HasExited) { Save-Tree 'final-tree' }
        Save-Result 'PASS' "$Scenario UIA discovery, control patterns and observable state transitions"
        Write-Output "PASS: $($script:checks.Count) checks; $OutputDirectory"
    }
} catch {
    $failure = $_.ToString()
    try { Save-Tree 'failure-tree' } catch { Write-Warning "Tree capture failed: $_" }
    Save-Result 'FAIL' $failure
    Write-Error "FAIL at $script:step : $failure; $OutputDirectory" -ErrorAction Continue
    exit 1
} finally {
    $env:QT_QPA_PLATFORM = $oldPlatform
    $env:QT_ACCESSIBILITY = $oldAccessibility
    if ($null -ne $script:child) {
        if (!$script:child.HasExited) { $script:child.Kill(); $script:child.WaitForExit() }
        $script:child.Dispose()
    }
}
exit 0
