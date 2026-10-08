param([string]$Fixture='', [string]$OutputDirectory="$PSScriptRoot/../../out/menu-evidence")
$ErrorActionPreference='Stop'
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes
. (Join-Path $PSScriptRoot 'product_desktop_evidence.ps1')
$OutputDirectory=Join-Path $OutputDirectory ([guid]::NewGuid().ToString('N'))
$null=New-Item -ItemType Directory -Path $OutputDirectory
$OutputDirectory=(Resolve-Path $OutputDirectory).Path
$script:runId=[guid]::NewGuid().ToString('N');$script:cycle=1;$script:operation='menu-test'
function Sample-Resource($Phase) {}
if (!$Fixture) {
    $script:desktopState=[pscustomobject]@{session_id=2;interactive=$true;default_input_desktop=$true;same_input_desktop=$true;input_available=$true;last_input_tick=10}
    function Get-DesktopEvidenceState { $script:desktopState }
    foreach ($case in @(@{session=1;baseline=$null;expected='DEDICATED_DESKTOP_SESSION_MISMATCH'},
                       @{session=2;baseline=@{last_input_tick=9};expected='DEDICATED_DESKTOP_INPUT_ACTIVITY'})) {
        $failure=$null
        try {$null=Assert-DedicatedDesktop $case.session $case.baseline} catch {$failure=$_.Exception.Message}
        if ($failure -ne $case.expected) {throw "Unexpected desktop result: $failure"}
    }
    # Input evidence remains diagnostic; session and interactive desktop gates
    # continue to reject environments where real UIA cannot run correctly.
    $state=Assert-DedicatedDesktop 2 @{last_input_tick=9} diagnostic
    if($state.last_input_tick -ne 10){throw 'DIAGNOSTIC_INPUT_EVIDENCE_LOST'}
    $script:desktopState.input_available=$false
    $null=Assert-DedicatedDesktop 2 @{last_input_tick=9} diagnostic
    $failure=$null
    try {$null=Assert-DedicatedDesktop 2} catch {$failure=$_.Exception.Message}
    if($failure -ne 'DEDICATED_DESKTOP_INPUT_EVIDENCE_UNAVAILABLE'){throw "Unexpected strict input result: $failure"}
    foreach($policy in @('strict','diagnostic')) {
        $failure=$null
        try {$null=Assert-DedicatedDesktop 1 -InputPolicy $policy} catch {$failure=$_.Exception.Message}
        if($failure -ne 'DEDICATED_DESKTOP_SESSION_MISMATCH'){throw "Unexpected session result: $failure"}
        foreach($field in @('interactive','default_input_desktop','same_input_desktop')) {
            $script:desktopState.$field=$false
            $failure=$null
            try {$null=Assert-DedicatedDesktop 2 -InputPolicy $policy} catch {$failure=$_.Exception.Message}
            if($failure -ne 'DEDICATED_DESKTOP_NOT_INTERACTIVE'){throw "Unexpected interactive result: $failure"}
            $script:desktopState.$field=$true
        }
    }
    $failure=$null
    try {$null=Assert-DedicatedDesktop 2 -InputPolicy 'ignore'} catch {$failure=$_.Exception.Message}
    if(!$failure){throw 'INVALID_INPUT_POLICY_ACCEPTED'}
    $script:desktopState.input_available=$true
    $script:child=@{Id=123}
    function Write-MenuPhase($AttemptId,$Phase) {}
    function Save-MenuObservation($Observer,$AttemptId) {if(!$Observer.Healthy){throw 'MENU_OBSERVER_EVIDENCE_INCOMPLETE'}}
    function Start-MenuObserver($ProcessId) {return $script:observer}
    function Invoke($Id) {$script:openCount++}
    function Find-Node($Id,$Role,[switch]$Optional) {if($script:present){return @{id=$Id}}}
    function Require-Pattern($Node,$Id) {return $script:pattern}
    foreach($case in @(@{closed=$true;seen=$true;present=$false;healthy=$true;expected='ACCOUNT_MENU_CLOSED_BEFORE_SELECTION'},
                       @{closed=$false;seen=$false;present=$false;healthy=$true;expected='ACCOUNT_MENU_NOT_OBSERVED'},
                       @{closed=$false;seen=$true;present=$false;healthy=$true;expected='ACCOUNT_MENU_ITEM_NOT_AVAILABLE'},
                       @{closed=$false;seen=$true;present=$true;healthy=$true;expected=$null},
                       @{closed=$false;seen=$false;present=$false;healthy=$false;expected='MENU_OBSERVER_EVIDENCE_INCOMPLETE'})) {
        $script:openCount=0;$script:selected=0;$script:present=$case.present
        $script:observer=[pscustomobject]@{PopupClosed=$case.closed;PopupSeen=$case.seen;Healthy=$case.healthy}
        $script:observer | Add-Member ScriptMethod Dispose {}
        $script:pattern=New-Object PSObject
        $script:pattern | Add-Member ScriptMethod Invoke {$script:selected++}
        $failure=$null
        try {Invoke-AccountTelemetry -Seconds 0} catch {$failure=$_.Exception.Message}
        if($failure -ne $case.expected -or $script:openCount -ne 1 -or $script:selected -ne [int]$case.present) {throw "Unexpected menu result: $failure"}
    }
    Write-Output 'PASS: menu close/missing/item missing/selection/observer failure; strict/diagnostic input and unchanged session/interactive gates'
    exit 0
}
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'product_desktop.ps1'),[ref]$tokens,[ref]$errors)
foreach($name in @('New-TreeCacheRequest','Get-ProcessRoots','Get-Nodes','Get-LiveNode','Find-Node','Require-Pattern','Invoke','Wait-For')) {
    $definition=$ast.Find({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name},$true)
    Invoke-Expression $definition.Extent.Text
}
$env:QT_QPA_PLATFORM='windows';$env:QT_ACCESSIBILITY='1';$script:rootCache=@{}
$script:child=Start-Process $Fixture -ArgumentList ('"'+$OutputDirectory+'/fixture.jsonl"') -PassThru
try {
    Start-Sleep -Seconds 2
    Invoke-AccountTelemetry -Seconds 5
    Invoke 'telemetryPostClose'
    Invoke 'armOutsidePress'
    # Let the fixture close the popup before the first locator lookup.
    function Sample-Resource($Phase) {Start-Sleep -Milliseconds 400}
    $watch=[Diagnostics.Stopwatch]::StartNew();$failure=$null
    try {Invoke-AccountTelemetry -Seconds 5} catch {$failure=$_.Exception.Message}
    $watch.Stop()
    @{reason=$failure;elapsed_seconds=$watch.Elapsed.TotalSeconds} | ConvertTo-Json | Set-Content "$OutputDirectory/result.json"
    if ($failure -ne 'ACCOUNT_MENU_CLOSED_BEFORE_SELECTION' -or $watch.Elapsed.TotalSeconds -ge 5) {throw "Real popup close not detected: $failure"}
    Write-Output "PASS: real Qt popup selection and close detected; evidence=$OutputDirectory"
} finally {
    if(!$script:child.HasExited){$script:child.Kill();$script:child.WaitForExit()}
    $script:child.Dispose()
}
