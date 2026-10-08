param([string]$EvidenceDirectory = "$PSScriptRoot/../../../out/b14-first-live-identity-20261007")
$ErrorActionPreference='Stop'
if($PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1){throw 'WINDOWS_POWERSHELL_51_REQUIRED'}
$source=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../uia/product_desktop.ps1'))
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($source,[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'REAL_SOURCE_PARSE_FAILED'}
$definitions=[ordered]@{}
foreach($name in @('Save-ProductFirstLiveIdentityFailure','Assert-ProductFirstLiveIdentity','Save-Result','Initialize-ProductFirstLive','Get-ProductLiveElapsedSeconds')){
    $nodes=@($ast.EndBlock.Statements | Where-Object {$_ -is [Management.Automation.Language.FunctionDefinitionAst] -and $_.Name -eq $name})
    if($nodes.Count -ne 1){throw ('REAL_FUNCTION_NOT_UNIQUE:'+ $name)}
    $definitions[$name]=$nodes[0].Extent.Text
    . ([scriptblock]::Create($definitions[$name]))
}
# No product, desktop, native process API or remote service is invoked. These
# doubles expose getter order and independently control the retained-handle code.
Add-Type @'
using System;
public sealed class FirstLiveIdentityTestProcess {
    public int Id = 40001;
    public bool ExitState, RefreshFailure, HasExitedFailure, PathFailure;
    public string ImagePath;
    public int RefreshReads, HasExitedReads, PathReads;
    public DateTime StartTime = new DateTime(2026, 10, 7, 0, 0, 0, DateTimeKind.Utc);
    public int ExitAtRefresh;
    public void Refresh() { ++RefreshReads; if (ExitAtRefresh > 0 && RefreshReads >= ExitAtRefresh) ExitState = true; if (RefreshFailure) throw new InvalidOperationException("stub_refresh_failed"); }
    public bool HasExited { get { ++HasExitedReads; if (HasExitedFailure) throw new InvalidOperationException("stub_has_exited_failed"); return ExitState; } }
    public string Path { get { ++PathReads; if (PathFailure) throw new InvalidOperationException("stub_path_failed"); return ImagePath; } }
}
public static class ProductDesktop {
    public static uint Code;
    public static bool ReadFailure;
    public static int Reads, PathReads;
    public static string NativePath;
    public static bool PathFailure, ExitDuringQuery;
    public static FirstLiveIdentityTestProcess Process;
    public static string ImagePath(IntPtr handle) {
        ++PathReads;
        if (PathFailure) throw new System.ComponentModel.Win32Exception(5);
        if (ExitDuringQuery) Process.ExitState = true;
        return NativePath;
    }
    public static uint ExitCode(IntPtr handle) {
        ++Reads;
        if (ReadFailure) throw new InvalidOperationException("stub_exit_code_failed");
        return Code;
    }
}
'@
$root=Join-Path ([IO.Path]::GetFullPath($EvidenceDirectory)) ('offline-'+[guid]::NewGuid().ToString('N'))
$null=[IO.Directory]::CreateDirectory($root)
$Executable='C:\identity-fixture\Cohavora.exe'
$script:runId='a'*32;$script:cycle=1;$script:completed=0
$script:childHandle=[IntPtr]1
$started=[DateTime]::UtcNow;$Cycles=2;$MinimumSeconds=480
$script:productFirstLive=$null;$script:productLiveElapsedBeforeClose=$null
$DesktopInputPolicy='diagnostic'
$cases=[Collections.Generic.List[object]]::new()
function Assert-True([bool]$Value,[string]$Label){if(!$Value){throw ('ASSERT_FAILED:'+ $Label)}}
function Rejected([string]$Message){
    $caught=$null
    try{Assert-ProductFirstLiveIdentity}catch{$caught=$_.Exception.Message}
    Assert-True ($null -ne $caught -and $caught.Contains($Message)) ('primary-error:'+ $Message+' observed:'+ $caught)
}
function Case([string]$Name,[scriptblock]$Body){
    $script:child=[FirstLiveIdentityTestProcess]::new()
    $script:child.ImagePath=$Executable
    $script:productFirstLiveIdentityFailure=$null
    [ProductDesktop]::Code=259;[ProductDesktop]::ReadFailure=$false;[ProductDesktop]::Reads=0
    [ProductDesktop]::NativePath=$Executable;[ProductDesktop]::PathFailure=$false;[ProductDesktop]::ExitDuringQuery=$false;[ProductDesktop]::PathReads=0;[ProductDesktop]::Process=$script:child
    $script:OutputDirectory=Join-Path $root $Name
    $null=[IO.Directory]::CreateDirectory($script:OutputDirectory)
    & $Body
    $cases.Add([ordered]@{name=$Name;status='PASS'})
}
Case 'valid_identity_has_no_failure_observation' {
    [ProductDesktop]::NativePath=$Executable.ToLowerInvariant()
    Assert-ProductFirstLiveIdentity
    Assert-True ($script:child.RefreshReads -eq 2 -and $script:child.HasExitedReads -eq 2 -and $script:child.PathReads -eq 0) 'single-original-check'
    Assert-True ([ProductDesktop]::Reads -eq 0 -and $null -eq $script:productFirstLiveIdentityFailure) 'pass-does-not-read-code'
    Assert-True (!(Test-Path (Join-Path $OutputDirectory 'product-first-live-identity-failure.json'))) 'pass-does-not-write'
}
Case 'exited_short_circuits_path_and_records_unsigned_code' {
    $script:child.ExitState=$true;$script:child.PathFailure=$true
    [ProductDesktop]::Code=[uint32]3221225794
    Rejected 'PRODUCT_FIRST_LIVE_IDENTITY_INVALID'
    $r=$script:productFirstLiveIdentityFailure
    Assert-True ($script:child.PathReads -eq 0 -and $r.branch -eq 'PROCESS_EXITED' -and $r.path_read_status -eq 'SKIPPED_SHORT_CIRCUIT') 'path-short-circuit'
    Assert-True ($null -eq $r.actual_path -and $r.pid -eq 40001 -and $r.expected_path -eq $Executable) 'identity-fields'
    Assert-True ($r.exit_code -eq 3221225794 -and $r.exit_code_hex -eq '0xC0000142' -and $r.phase -eq 'first_live_identity_pre_cleanup') 'pre-cleanup-code'
}
Case 'path_mismatch_does_not_treat_259_as_exit' {
    [ProductDesktop]::NativePath='C:\identity-fixture\foreign.exe'
    Rejected 'PRODUCT_FIRST_LIVE_IDENTITY_INVALID'
    $r=$script:productFirstLiveIdentityFailure
    Assert-True ($r.branch -eq 'EXECUTABLE_PATH_MISMATCH' -and $r.actual_path -eq [ProductDesktop]::NativePath -and !$r.has_exited_at_gate) 'mismatch-observed-once'
    Assert-True ($null -eq $r.exit_code -and $r.exit_code_raw -eq 259 -and $r.exit_code_status -eq 'STILL_ACTIVE_259') '259-is-not-exit'
}
Case 'process_path_null_empty_and_getter_failure_are_unused' {
    foreach($value in @($null,'')) {
        $script:child.ImagePath=$value
        Assert-ProductFirstLiveIdentity
    }
    $script:child.PathFailure=$true
    Assert-ProductFirstLiveIdentity
    Assert-True ($script:child.PathReads -eq 0 -and [ProductDesktop]::PathReads -eq 3) 'native-path-only'
}
Case 'native_query_failure_is_recorded' {
    [ProductDesktop]::PathFailure=$true
    Rejected 'Exception calling'
    $r=$script:productFirstLiveIdentityFailure
    Assert-True ($r.branch -eq 'PATH_READ_FAILED' -and $r.path_read_status -eq 'READ_FAILED' -and $r.path_source -eq 'retained_handle_QueryFullProcessImageName') 'native-read-failure'
}
Case 'unavailable_has_exited_fails_closed' {
    $script:child.HasExitedFailure=$true
    Rejected 'PRODUCT_FIRST_LIVE_IDENTITY_INVALID'
    Assert-True ($null -eq $script:productFirstLiveIdentityFailure.has_exited_at_gate -and [ProductDesktop]::PathReads -eq 0 -and $script:productFirstLiveIdentityFailure.branch -eq 'HAS_EXITED_READ_FAILED') 'unknown-state-rejected'
}
Case 'refresh_exception_retains_primary_error' {
    $script:child.RefreshFailure=$true
    Rejected 'stub_refresh_failed'
    Assert-True ($script:child.HasExitedReads -eq 0 -and $script:productFirstLiveIdentityFailure.branch -eq 'REFRESH_FAILED') 'refresh-failure'
}
Case 'exit_code_read_failure_preserves_identity_fail' {
    [ProductDesktop]::NativePath='foreign';[ProductDesktop]::ReadFailure=$true
    Rejected 'PRODUCT_FIRST_LIVE_IDENTITY_INVALID'
    $r=$script:productFirstLiveIdentityFailure
    Assert-True ($null -eq $r.exit_code_raw -and $r.exit_code_status -eq 'READ_FAILED' -and $r.exit_code_read_error_type) 'code-read-failure'
}
Case 'evidence_write_failure_preserves_fail_and_result' {
    $path=Join-Path $OutputDirectory 'product-first-live-identity-failure.json'
    [IO.File]::WriteAllText($path,'preserved-existing-evidence')
    [ProductDesktop]::NativePath='foreign'
    Rejected 'PRODUCT_FIRST_LIVE_IDENTITY_INVALID'
    Assert-True ([IO.File]::ReadAllText($path) -eq 'preserved-existing-evidence') 'no-overwrite'
    Assert-True ([bool]$script:productFirstLiveIdentityFailure.evidence_write_error_type) 'write-error-recorded'
    Save-Result 'FAIL' 'PRODUCT_FIRST_LIVE_IDENTITY_INVALID'
    $result=Get-Content (Join-Path $OutputDirectory 'uia-result.json') -Raw | ConvertFrom-Json
    Assert-True ($result.verdict -eq 'FAIL' -and $result.reason -eq 'PRODUCT_FIRST_LIVE_IDENTITY_INVALID' -and $result.product_first_live_identity_failure.evidence_write_error_type) 'memory-evidence-in-original-fail'
}
Case 'first_gate_rejects_exit_during_native_query' {
    [ProductDesktop]::ExitDuringQuery=$true
    Rejected 'PRODUCT_FIRST_LIVE_IDENTITY_INVALID'
    Assert-True ($script:productFirstLiveIdentityFailure.branch -eq 'PROCESS_EXITED_AFTER_PATH' -and $script:child.PathReads -eq 0) 'post-query-exit-rejected'
}
Case 'lifetime_guards_preserve_pid_ticks_native_path_and_alive' {
    $script:child.PathFailure=$true
    $script:runClock=[Diagnostics.Stopwatch]::StartNew()
    $script:productOwnedIdentity=@{pid=$script:child.Id;start_ticks=$script:child.StartTime.ToUniversalTime().Ticks;executable=$Executable}
    Initialize-ProductFirstLive 0 ([DateTime]::UtcNow.ToString('o')) 'offline'
    foreach($field in @('pid','start_ticks','executable')) {
        $saved=$script:productFirstLive[$field]
        if($field -eq 'executable'){$script:productFirstLive[$field]='foreign'}else{$script:productFirstLive[$field]++}
        $caught=$null;try{Get-ProductLiveElapsedSeconds}catch{$caught=$_.Exception.Message}
        Assert-True ($caught -eq 'PRODUCT_FIRST_LIVE_IDENTITY_CHANGED_OR_EXITED') ('sample-'+$field)
        $script:productFirstLive[$field]=$saved
        $saved=$script:productOwnedIdentity[$field]
        if($field -eq 'executable'){$script:productOwnedIdentity[$field]='foreign'}else{$script:productOwnedIdentity[$field]++}
        $script:productFirstLive=$null
        $caught=$null;try{Initialize-ProductFirstLive 0 'offline' 'offline'}catch{$caught=$_.Exception.Message}
        Assert-True ($caught -eq 'PRODUCT_FIRST_LIVE_IDENTITY_CHANGED_OR_EXITED') ('origin-'+$field)
        $script:productOwnedIdentity[$field]=$saved
        Initialize-ProductFirstLive 0 'offline' 'offline'
    }
    foreach($mode in @('exited','unknown','native_failure')) {
        $script:child.ExitState=($mode -eq 'exited');$script:child.HasExitedFailure=($mode -eq 'unknown');[ProductDesktop]::PathFailure=($mode -eq 'native_failure')
        $caught=$null;try{Get-ProductLiveElapsedSeconds}catch{$caught=$_.Exception.Message}
        Assert-True ($null -ne $caught) ('sample-reject-'+$mode)
        $script:productFirstLive=$null
        $caught=$null;try{Initialize-ProductFirstLive 0 'offline' 'offline'}catch{$caught=$_.Exception.Message}
        Assert-True ($null -ne $caught -and $null -eq $script:productFirstLive) ('origin-reject-'+$mode)
        $script:child.ExitState=$false;$script:child.HasExitedFailure=$false;[ProductDesktop]::PathFailure=$false
        Initialize-ProductFirstLive 0 'offline' 'offline'
    }
    [ProductDesktop]::ExitDuringQuery=$true
    $caught=$null;try{Get-ProductLiveElapsedSeconds}catch{$caught=$_.Exception.Message}
    Assert-True ($caught -eq 'PRODUCT_EXIT_DURING_LIFETIME_SAMPLE') 'exit-during-native-query'
    [ProductDesktop]::ExitDuringQuery=$false;$script:child.ExitState=$false
    $script:child.ExitAtRefresh=$script:child.RefreshReads+2
    $caught=$null;try{Get-ProductLiveElapsedSeconds}catch{$caught=$_.Exception.Message}
    Assert-True ($caught -eq 'PRODUCT_EXIT_DURING_LIFETIME_SAMPLE') 'exit-after-clock-sampling'
    Assert-True ($script:child.PathReads -eq 0) 'lifetime-never-reads-process-path'
}
$proof=[ordered]@{schema=1;verdict='PASS';utc=[DateTime]::UtcNow.ToString('o');cases=@($cases);
    source=$source;source_sha256=(Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant();
    powershell_version=$PSVersionTable.PSVersion.ToString();product_started=$false;remote_commands=$false;
    runtime_qualification_credit=0;historical_startup_root_cause='UNKNOWN'}
$proof | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $root 'result.json') -Encoding UTF8
@{verdict='PASS';cases=$cases.Count;evidence=(Join-Path $root 'result.json')} | ConvertTo-Json -Compress
