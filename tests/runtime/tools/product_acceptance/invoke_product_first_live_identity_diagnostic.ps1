param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)][string]$ExpectedProductSha256,
    [Parameter(Mandatory=$true)][string]$ExpectedUiaSha256,
    [switch]$PrepareOnly
)
$ErrorActionPreference='Stop'
if($PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1){throw 'WINDOWS_POWERSHELL_51_REQUIRED'}
$workspace=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$source=Join-Path $workspace 'tests/uia/product_desktop.ps1'
$Executable=(Resolve-Path -LiteralPath $Executable).ProviderPath
if((Split-Path (Split-Path $Executable) -Leaf) -cne 'RelWithDebInfo'){throw 'RELWITHDEBINFO_REQUIRED'}
if((Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash -ne $ExpectedProductSha256){throw 'PRODUCT_FINGERPRINT_CHANGED'}
if((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $ExpectedUiaSha256){throw 'UIA_FINGERPRINT_CHANGED'}
$tokens=$null;$parseErrors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($source,[ref]$tokens,[ref]$parseErrors)
if($parseErrors.Count){throw 'UIA_SOURCE_PARSE_FAILED'}
$functions=[ordered]@{}
foreach($name in @('Save-ProductFirstLiveIdentityFailure','Assert-ProductFirstLiveIdentity','Start-Product',
    'Initialize-ProductFirstLive','Get-ProductLiveElapsedSeconds')){
    $nodes=@($ast.EndBlock.Statements | Where-Object {$_ -is [Management.Automation.Language.FunctionDefinitionAst] -and $_.Name -eq $name})
    if($nodes.Count -ne 1){throw ('SOURCE_FUNCTION_NOT_UNIQUE:'+ $name)}
    $functions[$name]=$nodes[0]
}
# Execute the exact source prefix through the original gate once. Everything
# after that boundary (window discovery, login and meetings) is excluded.
$launchStatements=@();$gateFound=$false
foreach($statement in $functions['Start-Product'].Body.EndBlock.Statements){
    $launchStatements+=$statement.Extent.Text
    if($statement.Extent.Text.Trim() -eq 'Assert-ProductFirstLiveIdentity'){$gateFound=$true;break}
}
if(!$gateFound){throw 'STARTUP_IDENTITY_BOUNDARY_NOT_FOUND'}
$ownedAssignments=@($functions['Start-Product'].Body.EndBlock.Statements | Where-Object {
    $_ -is [Management.Automation.Language.AssignmentStatementAst] -and
    $_.Left -is [Management.Automation.Language.VariableExpressionAst] -and
    $_.Left.VariablePath.UserPath -eq 'script:productOwnedIdentity'
})
if($ownedAssignments.Count -ne 1){throw 'OWNED_IDENTITY_ASSIGNMENT_NOT_UNIQUE'}
$launchBody=$launchStatements -join "`r`n"
$nativeStatements=@($ast.EndBlock.Statements | Where-Object {
    $_ -is [Management.Automation.Language.PipelineAst] -and
    $_.PipelineElements.Count -eq 1 -and
    $_.PipelineElements[0] -is [Management.Automation.Language.CommandAst] -and
    $_.PipelineElements[0].GetCommandName() -eq 'Add-Type' -and
    $_.Extent.Text.Contains('public static class ProductDesktop')
})
if($nativeStatements.Count -ne 1){throw 'ORIGINAL_NATIVE_EXIT_READER_NOT_UNIQUE'}
if($PrepareOnly){
    [pscustomobject]@{status='PREPARED';product_started=$false;launch_statements=$launchStatements.Count;
        source_sha256=$ExpectedUiaSha256;product_sha256=$ExpectedProductSha256;attempts=1} | ConvertTo-Json
    exit 0
}
if(Test-Path -LiteralPath $OutputDirectory){throw 'NEW_EVIDENCE_DIRECTORY_REQUIRED'}
. ([scriptblock]::Create($nativeStatements[0].Extent.Text))
if(![Environment]::UserInteractive -or ![ProductDesktop]::Unlocked()){throw 'UNLOCKED_INTERACTIVE_DESKTOP_REQUIRED'}
if(@(Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($Executable)) -ErrorAction SilentlyContinue).Count){throw 'EXISTING_PRODUCT_PROCESS_PRESENT'}
Add-Type @'
using System;
using System.Text;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class ProductIdentityNativeImage {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool QueryFullProcessImageName(IntPtr handle, uint flags, StringBuilder path, ref uint length);
    public static string Read(IntPtr handle) {
        var path = new StringBuilder(32768); uint length = 32768;
        if (!QueryFullProcessImageName(handle, 0, path, ref length))
            throw new Win32Exception(Marshal.GetLastWin32Error());
        return path.ToString();
    }
}
'@
. ([scriptblock]::Create($functions['Save-ProductFirstLiveIdentityFailure'].Extent.Text))
. ([scriptblock]::Create($functions['Assert-ProductFirstLiveIdentity'].Extent.Text))
. ([scriptblock]::Create($functions['Initialize-ProductFirstLive'].Extent.Text))
. ([scriptblock]::Create($functions['Get-ProductLiveElapsedSeconds'].Extent.Text))
. ([scriptblock]::Create("function Invoke-OriginalStartupPrefix {`r`n"+$launchBody+"`r`n}"))
$script:runId=[guid]::NewGuid().ToString('N');$script:cycle=1
$script:child=$null;$script:childHandle=[IntPtr]::Zero
$script:productOwnedIdentity=$null;$script:productFirstLiveIdentityFailure=$null
$script:productFirstLive=$null;$script:runClock=[Diagnostics.Stopwatch]::StartNew()
$HeapDiagnostic=$false
$savedEnvironment=@{}
foreach($name in @('APPDATA','LOCALAPPDATA','LIVEKIT_UIA_SETTINGS_ROOT','QT_QPA_PLATFORM','QT_ACCESSIBILITY',
    'LIVEKIT_UIA_RUN_ID','LIVEKIT_UIA_PILOT_PROBE','LIVEKIT_UIA_GPU_BUDGET_PROBE','LIVEKIT_UIA_LOG_PAIR')){
    $savedEnvironment[$name]=[Environment]::GetEnvironmentVariable($name,'Process')
}
$record=[ordered]@{schema=1;run_id=$script:runId;status='PREPARED';configuration='RelWithDebInfo';
    source=$source;source_sha256=$ExpectedUiaSha256;product=$Executable;product_sha256=$ExpectedProductSha256;
    host_pid=$PID;host_session=[Diagnostics.Process]::GetCurrentProcess().SessionId;
    powershell_version=$PSVersionTable.PSVersion.ToString();host_is_64bit=[Environment]::Is64BitProcess;
    started_utc=[DateTime]::UtcNow.ToString('o');launch_attempts=0;gate_error=$null;
    validation_phase='first_identity_gate';first_identity_gate='NOT_RUN';first_live_origin='NOT_RUN';
    lifetime_identity_sample='NOT_RUN';lifetime_sample=$null;product_first_live=$null;
    identity_failure=$null;post_gate_observations=@();cleanup=[ordered]@{method=$null;exited=$null;error=$null};
    runtime_qualification_credit=0;remote_started=$false;meeting_started=$false;formal_started=$false}
function Observe-ProductAfterGate([string]$Phase){
    $sample=[ordered]@{phase=$Phase;utc=[DateTime]::UtcNow.ToString('o');pid=$script:child.Id;
        native_image_path=$null;native_image_error=$null;main_module_path=$null;main_module_error=$null;
        has_exited=$null;exit_code_raw=$null;observation_error=$null}
    try{$sample.native_image_path=[ProductIdentityNativeImage]::Read($script:childHandle)}catch{$sample.native_image_error=$_.Exception.ToString()}
    try{$sample.exit_code_raw=[long][ProductDesktop]::ExitCode($script:childHandle)}catch{$sample.observation_error=$_.Exception.ToString()}
    try{$script:child.Refresh();$sample.has_exited=$script:child.HasExited}catch{$sample.observation_error=$_.Exception.ToString()}
    try{$sample.main_module_path=$script:child.get_MainModule().FileName}catch{$sample.main_module_error=$_.Exception.ToString()}
    return $sample
}
$lock=$null;$evidenceCreated=$false
try{
    $lock=[IO.File]::Open((Join-Path $workspace 'out/product-acceptance.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    $null=New-Item -ItemType Directory -Path $OutputDirectory -ErrorAction Stop
    $evidenceCreated=$true
    $OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).ProviderPath
    $env:LIVEKIT_UIA_RUN_ID=$script:runId
    $env:LIVEKIT_UIA_PILOT_PROBE=Join-Path $OutputDirectory 'process-probe.jsonl'
    $env:LIVEKIT_UIA_GPU_BUDGET_PROBE='1';$env:LIVEKIT_UIA_LOG_PAIR='1'
    [IO.File]::WriteAllText((Join-Path $OutputDirectory 'original-startup-prefix.ps1'),$launchBody,[Text.UTF8Encoding]::new($false))
    $record.launch_attempts=1
    try{
        Invoke-OriginalStartupPrefix
        $record.first_identity_gate='PASS'
        $record.validation_phase='first_live_origin'
        . ([scriptblock]::Create($ownedAssignments[0].Extent.Text))
        Initialize-ProductFirstLive $script:runClock.Elapsed.TotalSeconds ([DateTime]::UtcNow.ToString('o')) 'supervisor_owned_live_diagnostic'
        $record.first_live_origin='PASS'
        $record.product_first_live=$script:productFirstLive
        $record.validation_phase='lifetime_identity_sample'
        $record.lifetime_sample=Get-ProductLiveElapsedSeconds -AsSample
        $record.lifetime_identity_sample='PASS'
        $record.status='STARTUP_IDENTITY_VERIFIED'
    }
    catch{$record.status='STARTUP_FAILED';$record.gate_error=$_.Exception.ToString()}
    $record.identity_failure=$script:productFirstLiveIdentityFailure
    if($script:child -and $script:childHandle -ne [IntPtr]::Zero){
        $record.post_gate_observations+=Observe-ProductAfterGate 'immediate_after_original_gate'
        # This is a second observation of the same process, never a gate retry.
        if($record.identity_failure -and !$script:child.HasExited){
            $null=$script:child.WaitForExit(250)
            $record.post_gate_observations+=Observe-ProductAfterGate '250ms_after_failed_gate'
        }
    }
}finally{
    if($script:child){
        try{
            if(!$script:child.HasExited){
                $record.cleanup.method='OWNED_PROCESS_CLOSE_THEN_BOUNDED_KILL'
                $null=$script:child.CloseMainWindow()
                if(!$script:child.WaitForExit(3000)){$script:child.Kill();$null=$script:child.WaitForExit(3000)}
            }else{$record.cleanup.method='ALREADY_EXITED'}
            $record.cleanup.exited=$script:child.HasExited
        }catch{$record.cleanup.error=$_.Exception.ToString()}
        $script:child.Dispose()
    }
    foreach($name in $savedEnvironment.Keys){[Environment]::SetEnvironmentVariable($name,$savedEnvironment[$name],'Process')}
    if($lock){$lock.Dispose()}
    $record.finished_utc=[DateTime]::UtcNow.ToString('o')
    if($evidenceCreated){
        [IO.File]::WriteAllText((Join-Path $OutputDirectory 'startup-identity-diagnostic.json'),($record|ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
    }
}
$record|ConvertTo-Json -Depth 8
if($record.cleanup.error -or $record.cleanup.exited -ne $true){exit 2}
if($record.status -eq 'STARTUP_FAILED'){exit 1}
exit 0
