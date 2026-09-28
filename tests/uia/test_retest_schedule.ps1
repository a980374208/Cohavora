# Exercise the actual Run-Cycle scheduling with mocked UI actions. No service.
$ErrorActionPreference='Stop'
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'product_desktop.ps1'),[ref]$tokens,[ref]$errors)
if ($errors.Count) {throw 'DRIVER_PARSE_FAILED'}
$definition=$ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Run-Cycle'},$false)
Invoke-Expression $definition.Extent.Text
function Action([string]$Name,[scriptblock]$Body) {$script:actions.Add($Name)}
function Record([string]$Name,[string]$Phase) {$script:records.Add("${Name}:${Phase}")}
function Sample-Resource([string]$Phase) {}
function Find-Node($Id,$Role,[switch]$Optional) {if ($Id -eq 'meetingLeave') {return $true}; return $null}
$Retest=$true;$SteadySeconds=1;$MixedSeconds=2;$Cycles=3
$HeapDiagnostic=$false;$HeapDiagnosticNoShare=$false;$HeapDiagnosticNoExport=$false
$StopSettleSeconds=0;$RoomSettleSeconds=0
$script:child=@{HasExited=$false}
foreach ($cycle in @(1,2)) {
    $script:cycle=$cycle
    $script:actions=[Collections.Generic.List[string]]::new()
    $script:records=[Collections.Generic.List[string]]::new()
    Run-Cycle
    $expected=if($cycle -eq 1){'join,leave,export'}else{'join,page,share_start,share_stop,logging,leave,export'}
    if (($script:actions -join ',') -ne $expected) {throw "SCHEDULE_MISMATCH: $cycle"}
    $phase=if($cycle -eq 1){'steady'}else{'mixed'}
    if ($script:records[0] -ne "retest_${phase}:started" -or $script:records[-1] -ne "retest_${phase}:completed") {throw 'PHASE_EVIDENCE_MISSING'}
    if ($script:retestDeadline.Elapsed.TotalSeconds -lt 1) {throw 'PHASE_TOO_SHORT'}
}
Write-Output 'PASS: actual driver steady/mixed actions and minimum dwell, offline only'
