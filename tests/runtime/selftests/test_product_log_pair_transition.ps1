param(
    [string]$OutputDirectory = "$PSScriptRoot/../../../out/memory-release-20261007",
    [switch]$ExpectCurrentUnsafe
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationTypes
$driver = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../uia/product_desktop.ps1'))
$tokens = $null; $parseErrors = $null
$driverAst = [Management.Automation.Language.Parser]::ParseFile($driver, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw 'DRIVER_PARSER_FAILED' }
$append = @($driverAst.EndBlock.Statements | Where-Object {
    $_ -is [Management.Automation.Language.FunctionDefinitionAst] -and $_.Name -eq 'Append-SafeJsonl'
})
if ($append.Count -ne 1) { throw 'JSONL_APPEND_FUNCTION_NOT_UNIQUE' }
. ([scriptblock]::Create($append[0].Extent.Text))
$actions = @($driverAst.FindAll({
    param($node)
    $node -is [Management.Automation.Language.CommandAst] -and
        $node.GetCommandName() -eq 'Action' -and $node.CommandElements.Count -eq 3 -and
        $node.CommandElements[1].Value -eq 'logging'
}, $true))
if ($actions.Count -ne 1 -or
    $actions[0].CommandElements[2] -isnot [Management.Automation.Language.ScriptBlockExpressionAst]) {
    throw 'LOGGING_ACTION_NOT_UNIQUE'
}
$extent = $actions[0].CommandElements[2].ScriptBlock.Extent.Text
$originalBody = $extent.Substring(1, $extent.Length - 2)

function Parse-Body([string]$Text) {
    $bodyTokens = $null; $bodyErrors = $null
    $bodyAst = [Management.Automation.Language.Parser]::ParseInput($Text, [ref]$bodyTokens, [ref]$bodyErrors)
    if ($bodyErrors.Count) { throw ('FIXTURE_BODY_PARSER_FAILED: ' + (($bodyErrors | ForEach-Object { $_.Message + ' at ' + $_.Extent.Text }) -join '; ')) }
    return $bodyAst
}

# Reorder the actual top-level transition statements, retaining both real loops
# and their evidence writer. The deliberately unsafe variant is a negative control.
function Set-TransitionOrder([string]$Text, [bool]$Safe) {
    $bodyAst = Parse-Body $Text
    $edits = [Collections.Generic.List[object]]::new()
    foreach ($state in @('Off', 'On')) {
        $found = @{}
        foreach ($statement in $bodyAst.EndBlock.Statements) {
            foreach ($command in @($statement.FindAll({
                param($node)
                $node -is [Management.Automation.Language.CommandAst] -and
                    $node.GetCommandName() -eq 'Toggle-To'
            }, $true))) {
                if ($command.CommandElements.Count -ne 3 -or
                    $command.CommandElements[2].Extent.Text -notmatch ('::' + $state + '\)\s*$')) { continue }
                $id = $command.CommandElements[1].Value
                if ($id -notin @('consoleSaveLogs', 'consoleCollectDiagnostics')) { continue }
                if ($found.ContainsKey($id)) { throw "TRANSITION_NOT_UNIQUE: $state/$id" }
                $found[$id] = $statement
            }
        }
        if ($found.Count -ne 2) { throw "TRANSITION_PAIR_MISSING: $state" }
        $ordered = @($found.Values | Sort-Object { $_.Extent.StartOffset })
        $firstId = if (($Safe -and $state -eq 'Off') -or (!$Safe -and $state -eq 'On')) {
            'consoleCollectDiagnostics'
        } else { 'consoleSaveLogs' }
        $secondId = if ($firstId -eq 'consoleSaveLogs') { 'consoleCollectDiagnostics' } else { 'consoleSaveLogs' }
        $edits.Add(@{start=$ordered[0].Extent.StartOffset; length=$ordered[0].Extent.EndOffset-$ordered[0].Extent.StartOffset;
            text=$found[$firstId].Extent.Text})
        $edits.Add(@{start=$ordered[1].Extent.StartOffset; length=$ordered[1].Extent.EndOffset-$ordered[1].Extent.StartOffset;
            text=$found[$secondId].Extent.Text})
    }
    foreach ($edit in @($edits | Sort-Object { [int]$_.start } -Descending)) {
        $Text = $Text.Remove($edit.start, $edit.length).Insert($edit.start, $edit.text)
    }
    return $Text
}

function Use-VirtualClock([string]$Text) {
    $bodyAst = Parse-Body $Text
    $clocks = @($bodyAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.MemberExpressionAst] -and $node.Static -and
            $node.Expression -is [Management.Automation.Language.TypeExpressionAst] -and
            $node.Expression.TypeName.FullName -eq 'DateTime' -and $node.Member.Value -eq 'UtcNow'
    }, $true))
    if ($clocks.Count -ne 6) { throw "LOG_WINDOW_CLOCK_SHAPE_CHANGED: $($clocks.Count)" }
    foreach ($clock in @($clocks | Sort-Object { $_.Extent.StartOffset } -Descending)) {
        $Text = $Text.Remove($clock.Extent.StartOffset, $clock.Extent.EndOffset - $clock.Extent.StartOffset).
            Insert($clock.Extent.StartOffset, '(Get-TestUtcNow)')
    }
    return [scriptblock]::Create($Text)
}

function Emit-NativeEvent([string]$Boundary) {
    $script:eventCount++
    if ($script:production) {
        $script:accepted++
        if ($script:persistence) { $script:written++ }
        else { $script:gaps.Add(@{boundary=$Boundary;virtual_utc=$script:testNow.ToString('o')}) }
    }
}
function Get-TestUtcNow { return $script:testNow }
function Invoke([string]$Id) {
    if ($Id -ne 'meetingConsole') { throw "UNEXPECTED_INVOKE: $Id" }
    Emit-NativeEvent "Invoke:$Id"
}
function Wait-Top([string]$Id) {
    if ($Id -ne 'meetingLogConsole') { throw "UNEXPECTED_WAIT: $Id" }
    Emit-NativeEvent "Wait-Top:$Id"
}
function Toggle-To([string]$Id, [Windows.Automation.ToggleState]$State) {
    $enabled = $State -eq [Windows.Automation.ToggleState]::On
    switch ($Id) {
        'consoleCollectDiagnostics' { $script:production = $enabled }
        'consoleSaveLogs' { $script:persistence = $enabled }
        default { throw "UNEXPECTED_TOGGLE: $Id" }
    }
    $script:transitions.Add("${Id}:$State")
    Emit-NativeEvent "Toggle-To:${Id}:$State"
}
function Sample-Resource([string]$Phase) {
    if ($Phase -notin @('log_off', 'log_on')) { throw "UNEXPECTED_SAMPLE: $Phase" }
    $script:samples[$Phase]++
    if ($Phase -eq 'log_off' -and $script:persistence) { throw 'OFF_WINDOW_PERSISTENCE_ENABLED' }
    if ($Phase -eq 'log_off' -and $env:LIVEKIT_UIA_LOG_PAIR -eq '1' -and $script:production) {
        throw 'PAIRED_OFF_WINDOW_PRODUCTION_ENABLED'
    }
    if ($Phase -eq 'log_on' -and (!$script:production -or !$script:persistence)) { throw 'ON_WINDOW_DISABLED' }
    Emit-NativeEvent "Sample-Resource:$Phase"
}
function Start-Sleep([int]$Seconds) {
    if ($Seconds -ne 1) { throw 'LOG_WINDOW_SLEEP_CHANGED' }
    $script:testNow = $script:testNow.AddSeconds($Seconds)
    Emit-NativeEvent 'Start-Sleep'
}

$evidenceRoot = [IO.Path]::GetFullPath((Join-Path $OutputDirectory ('log-pair-transition-' + [guid]::NewGuid().ToString('N'))))
$null = [IO.Directory]::CreateDirectory($evidenceRoot)
$cases = [Collections.Generic.List[object]]::new()
function Check-Scenario([string]$Name, [string]$BodyText, [bool]$Paired, [bool]$ExpectGap) {
    $script:production = $true; $script:persistence = $true
    $script:eventCount = 0; $script:accepted = 0; $script:written = 0
    $script:gaps = [Collections.Generic.List[object]]::new()
    $script:transitions = [Collections.Generic.List[string]]::new()
    $script:samples = @{log_off=0;log_on=0}
    $script:testNow = [datetime]::Parse('2026-10-07T00:00:00.0000000Z').ToUniversalTime()
    $script:child = [pscustomobject]@{HasExited=$false;Id=4242}
    $script:runId = '00000000000000000000000000000001'; $script:cycle = 1; $script:operation = 'logging'
    $LogPairSeconds = 35
    $env:LIVEKIT_UIA_LOG_PAIR = if ($Paired) { '1' } else { '0' }
    $OutputDirectory = Join-Path $evidenceRoot $Name
    $null = [IO.Directory]::CreateDirectory($OutputDirectory)
    $body = Use-VirtualClock $BodyText
    & $body
    $windowLines = @(Get-Content -LiteralPath (Join-Path $OutputDirectory 'uia-log-windows.jsonl'))
    if ($windowLines.Count -ne 1) { throw "WINDOW_EVIDENCE_COUNT: $Name" }
    $window = $windowLines[0] | ConvertFrom-Json
    $offSeconds = ([datetime]$window.off_end_utc - [datetime]$window.off_start_utc).TotalSeconds
    $onSeconds = ([datetime]$window.on_end_utc - [datetime]$window.on_start_utc).TotalSeconds
    if ($offSeconds -ne 35 -or $onSeconds -ne 35 -or $script:samples.log_off -ne 35 -or $script:samples.log_on -ne 35 -or
        $window.production_and_persistence -ne $Paired -or !$script:production -or !$script:persistence) {
        throw "LOG_WINDOW_BEHAVIOR_CHANGED: $Name"
    }
    $expectedCalls = if ($Paired) { 4 } else { 2 }
    if ($script:transitions.Count -ne $expectedCalls) { throw "TRANSITION_COUNT: $Name" }
    if (!$Paired -and ($script:transitions -join ',') -ne 'consoleSaveLogs:Off,consoleSaveLogs:On') {
        throw "ORDINARY_MODE_CHANGED: $Name"
    }
    $hasGap = $script:gaps.Count -gt 0
    $status = if ($hasGap -eq $ExpectGap) { 'PASS' } else { 'FAIL' }
    $cases.Add([ordered]@{name=$Name;status=$status;paired=$Paired;expected_gap=$ExpectGap;
        gap_count=$script:gaps.Count;accepted=$script:accepted;written=$script:written;native_event_count=$script:eventCount;
        transitions=@($script:transitions);off_seconds=$offSeconds;on_seconds=$onSeconds;
        transition_gaps=@($script:gaps | Where-Object {$_.boundary -like 'Toggle-To:*'})})
}

$savedPair = $env:LIVEKIT_UIA_LOG_PAIR
try {
    $safeBody = Set-TransitionOrder $originalBody $true
    $unsafeBody = Set-TransitionOrder $originalBody $false
    Check-Scenario 'current-paired' $originalBody $true $ExpectCurrentUnsafe.IsPresent
    Check-Scenario 'candidate-safe-paired' $safeBody $true $false
    Check-Scenario 'negative-control-unsafe-paired' $unsafeBody $true $true
    Check-Scenario 'current-ordinary' $originalBody $false $true
    Check-Scenario 'candidate-safe-ordinary' $safeBody $false $true
} finally {
    $env:LIVEKIT_UIA_LOG_PAIR = $savedPair
}
$failures = @($cases | Where-Object {$_.status -ne 'PASS'})
$record = Join-Path $evidenceRoot 'verification.json'
[ordered]@{
    schema=1;utc=[DateTime]::UtcNow.ToString('o');status=$(if ($failures.Count) {'FAIL'} else {'PASS'});
    expect_current_unsafe=$ExpectCurrentUnsafe.IsPresent;cases=$cases;
    source_hashes=@{driver=(Get-FileHash $driver -Algorithm SHA256).Hash.ToLowerInvariant();
        selftest=(Get-FileHash $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()};
    scope='Real logging Action AST with virtual clock, mocked UIA controls, and one native event at every mocked call boundary';
    diagnostic_only=$true;qualification_credit=0
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $record -Encoding UTF8
Write-Output $record
if ($failures.Count) { throw ('LOG_PAIR_TRANSITION_FAILED: ' + (($failures | ForEach-Object {$_.name}) -join ',')) }
