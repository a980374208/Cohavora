param(
    [Parameter(Mandatory=$true)][Alias('Exe')][string]$Executable,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)][string]$ExpectedProductSha256,
    [Parameter(Mandatory=$true)][string]$ExpectedUiaSha256,
    [ValidatePattern('^[0-9a-f]{32}$')][string]$RunId=[guid]::NewGuid().ToString('N'),
    [int]$DesktopSessionId=0,
    [switch]$PrepareOnly,
    [switch]$BootstrapOnly
)
$ErrorActionPreference='Stop'
if($PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1){throw 'WINDOWS_POWERSHELL_51_REQUIRED'}
$workspace=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$source=Join-Path $workspace 'tests/uia/product_desktop.ps1'
$sourceRoot=Split-Path $source
$Executable=(Resolve-Path -LiteralPath $Executable).ProviderPath
if((Split-Path (Split-Path $Executable) -Leaf) -cne 'RelWithDebInfo'){throw 'RELWITHDEBINFO_REQUIRED'}
if((Get-FileHash $Executable -Algorithm SHA256).Hash -ne $ExpectedProductSha256){throw 'PRODUCT_FINGERPRINT_CHANGED'}
if((Get-FileHash $source -Algorithm SHA256).Hash -ne $ExpectedUiaSha256){throw 'UIA_FINGERPRINT_CHANGED'}
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($source,[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'UIA_SOURCE_PARSE_FAILED'}
$functions=@($ast.EndBlock.Statements | Where-Object {$_ -is [Management.Automation.Language.FunctionDefinitionAst]})
$run=@($functions | Where-Object {$_.Name -ceq 'Run-Cycle'})
if($run.Count -ne 1){throw 'RUN_CYCLE_NOT_UNIQUE'}
$firstFunction=$functions[0].Extent.StartOffset
$prefix=$ast.Extent.Text.Substring($ast.ParamBlock.Extent.EndOffset,$firstFunction-$ast.ParamBlock.Extent.EndOffset)
$actions=[ordered]@{}
foreach($name in @('join','page','logging','leave','export')){
    $matches=@($run[0].Body.FindAll({param($node)
        $node -is [Management.Automation.Language.CommandAst] -and
        $node.GetCommandName() -ceq 'Action' -and $node.CommandElements.Count -eq 3 -and
        $node.CommandElements[1] -is [Management.Automation.Language.StringConstantExpressionAst] -and
        $node.CommandElements[1].Value -ceq $name -and
        $node.CommandElements[2] -is [Management.Automation.Language.ScriptBlockExpressionAst]
    },$true))
    if($matches.Count -ne 1){throw ('ACTION_NOT_UNIQUE:'+ $name)}
    $actions[$name]=$matches[0].Extent.Text
}
# Preserve the original post-leave settlement statements, including its process guard.
$settle=@($run[0].Body.EndBlock.Statements | Where-Object {
    ($_.Extent.Text -match '^\$roomUntil\s*=') -or
    ($_ -is [Management.Automation.Language.WhileStatementAst] -and $_.Condition.Extent.Text.Contains('$roomUntil'))
})
if($settle.Count -ne 2){throw 'ROOM_SETTLE_BOUNDARY_NOT_UNIQUE'}
$body=@($actions.join,$actions.page,$actions.logging,$actions.leave,($settle.Extent.Text -join "`r`n"),$actions.export) -join "`r`n"
$definitions=($functions | Where-Object {$_.Name -cne 'Run-Cycle'} | ForEach-Object {$_.Extent.Text}) -join "`r`n"
$main=@'
try {
    if (![Environment]::UserInteractive -or ![ProductDesktop]::Unlocked()) {throw 'UNLOCKED_INTERACTIVE_DEFAULT_DESKTOP_REQUIRED'}
    if (!$env:LIVEKIT_UIA_ACCOUNT -or !$env:LIVEKIT_UIA_PASSWORD -or !$env:LIVEKIT_UIA_MEETING_ID) {throw 'ACCOUNT_AND_MEETING_REQUIRED'}
    if($DedicatedDesktopSessionId -gt 0){
        $script:desktopBaseline=Assert-DedicatedDesktop $DedicatedDesktopSessionId -InputPolicy $DesktopInputPolicy
        $script:desktopBaseline | ConvertTo-Json | Set-Content (Join-Path $OutputDirectory 'desktop-baseline.json') -Encoding UTF8
    }
    $script:cycle=1;$script:cycleId=[guid]::NewGuid().ToString('N')
    $diagnostic.launch_attempts=1
    Start-Product
    Run-Cycle
    $script:completed=1
    Stop-Product
    Save-Result 'DIAGNOSTIC_UIA_COMPLETE' 'independent_media_and_logging_review_required'
    $diagnostic.verdict='DIAGNOSTIC_UIA_COMPLETE'
} catch {
    $diagnostic.verdict='FAIL'
    $diagnostic.error=$_.ToString()+"`n"+$_.ScriptStackTrace
    try {Save-Tree 'cycle-0001-failure'} catch {}
    try {Save-Result 'FAIL' $diagnostic.error} catch {$diagnostic.result_write_error=$_.ToString()}
    throw
} finally {
    try {Cleanup-Product} finally {
        $env:APPDATA=$oldAppData;$env:LOCALAPPDATA=$oldLocalAppData
        $env:QT_QPA_PLATFORM=$oldQtPlatform;$env:QT_ACCESSIBILITY=$oldQtAccessibility
        $env:LIVEKIT_UIA_SETTINGS_ROOT=$oldSettingsRoot
        $diagnostic.finished_utc=[DateTime]::UtcNow.ToString('o')
        $diagnostic | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $OutputDirectory 'first-cycle-diagnostic-result.json') -Encoding UTF8
    }
}
'@
# Execute in the original root without rewriting any extracted source text.
$generated=$prefix+"`r`n"+$definitions+"`r`nfunction Run-Cycle {`r`n"+$body+"`r`n}`r`n"+$main
$null=[Management.Automation.Language.Parser]::ParseInput($generated,[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'DERIVED_SOURCE_PARSE_FAILED'}
function Text-Sha256([string]$value){
    $sha=[Security.Cryptography.SHA256]::Create()
    try {return ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($value)))).Replace('-','').ToLowerInvariant()} finally {$sha.Dispose()}
}
function Bind-SourceRoot([string]$value){
    $bindTokens=$null;$bindErrors=$null
    $bindAst=[Management.Automation.Language.Parser]::ParseInput($value,[ref]$bindTokens,[ref]$bindErrors)
    if($bindErrors.Count){throw 'SOURCE_ROOT_BIND_PARSE_FAILED'}
    $references=@($bindAst.FindAll({param($node)
        $node -is [Management.Automation.Language.VariableExpressionAst] -and
        $node.VariablePath.UserPath -ceq 'PSScriptRoot'
    },$true) | Sort-Object {$_.Extent.StartOffset} -Descending)
    foreach($reference in $references){
        $value=$value.Remove($reference.Extent.StartOffset,$reference.Extent.EndOffset-$reference.Extent.StartOffset).Insert($reference.Extent.StartOffset,'$script:diagnosticSourceRoot')
    }
    return $value
}
$boundPrefix=Bind-SourceRoot $prefix
$boundDefinitions=Bind-SourceRoot $definitions
$manifest=[ordered]@{schema=1;scope='first_cycle_media_log_diagnostic';runtime_qualification_credit=0;
    run_id=$RunId;configuration='RelWithDebInfo';product_sha256=$ExpectedProductSha256;source_sha256=$ExpectedUiaSha256;
    prefix_sha256=(Text-Sha256 $prefix);actions_sha256=(Text-Sha256 $body);derived_sha256=(Text-Sha256 $generated);
    actions=@($actions.Keys);cycles=1;log_pair_seconds=35;room_settle_seconds=10;launch_limit=1;
    parent_watchdog_seconds=600;syntax='PASS';source_root=$sourceRoot;product_started=$false;
    bound_prefix_sha256=(Text-Sha256 $boundPrefix);bound_definitions_sha256=(Text-Sha256 $boundDefinitions)}
if($PrepareOnly){$manifest | ConvertTo-Json -Depth 6;exit 0}
if(Test-Path -LiteralPath $OutputDirectory){throw 'NEW_EVIDENCE_DIRECTORY_REQUIRED'}
if(@(Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($Executable)) -ErrorAction SilentlyContinue).Count){throw 'EXISTING_PRODUCT_PROCESS_PRESENT'}
# The controller owns the exclusive lock, collectors, remote load and watchdog.
$Cycles=1;$MinimumSeconds=0;$LogPairSeconds=35;$RoomSettleSeconds=10
$ShareSeconds=60;$StopSettleSeconds=10;$DesktopInputPolicy='diagnostic'
$DedicatedDesktopSessionId=$DesktopSessionId
$ProbeOnly=$false;$Pilot=$false;$Retest=$false;$RetestSmoke=$false
$HeapDiagnostic=$false;$HeapSnapshotDiagnostic=$false;$HeapDiagnosticPersistentUia=$false
$HeapDiagnosticNoShare=$false;$HeapCheckOnly=$false;$HeapDiagnosticNoExport=$false
$HeapPageCheck=$false;$CrashDiagnostic=$false;$IsolateUiaCycles=$false
$diagnostic=[ordered]@{scope='first_cycle_media_log_diagnostic';runtime_qualification_credit=0;formal_started=$false;
    run_id=$RunId;launch_attempts=0;verdict='NOT_RUN';error=$null;result_write_error=$null;finished_utc=$null}
$oldLogPair=$env:LIVEKIT_UIA_LOG_PAIR
try {
    $env:LIVEKIT_UIA_LOG_PAIR='1'
    $script:diagnosticSourceRoot=$sourceRoot
    try {
        # Prefix creates the new output directory before any launch.
        . ([scriptblock]::Create($boundPrefix))
        [IO.File]::WriteAllText((Join-Path $OutputDirectory 'derived-first-cycle-driver.ps1'),$generated,[Text.UTF8Encoding]::new($false))
        $manifest | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $OutputDirectory 'derived-driver-manifest.json') -Encoding UTF8
        . ([scriptblock]::Create($boundDefinitions))
        if($BootstrapOnly){
            [ordered]@{scope='bootstrap_only';product_started=$false;network_started=$false;
                runtime_qualification_credit=0;prefix_executed=$true;definitions_loaded=$true;
                source_root=$script:diagnosticSourceRoot;
                probe_tail_loaded=[bool](Get-Command Read-ProductPilotProbeTail -ErrorAction SilentlyContinue);
                evidence_loaded=[bool](Get-Command Assert-DedicatedDesktop -ErrorAction SilentlyContinue)} |
                ConvertTo-Json | Set-Content (Join-Path $OutputDirectory 'bootstrap-result.json') -Encoding UTF8
        }else{
            . ([scriptblock]::Create("function Run-Cycle {`r`n"+$body+"`r`n}`r`n"+$main))
        }
    } finally {
        if($BootstrapOnly){
            $env:APPDATA=$oldAppData;$env:LOCALAPPDATA=$oldLocalAppData
            $env:QT_QPA_PLATFORM=$oldQtPlatform;$env:QT_ACCESSIBILITY=$oldQtAccessibility
            $env:LIVEKIT_UIA_SETTINGS_ROOT=$oldSettingsRoot
        }
    }
} finally {$env:LIVEKIT_UIA_LOG_PAIR=$oldLogPair}
