param([string]$OutputDirectory="$PSScriptRoot/../../../out/b14-repair-20261007", [string[]]$CaseFilter=@())
$ErrorActionPreference='Stop'
$module = Join-Path $PSScriptRoot '../tools/product_acceptance/product_heap_snapshot.ps1'
$tokens=$null; $parseErrors=$null
$null=[Management.Automation.Language.Parser]::ParseFile($module,[ref]$tokens,[ref]$parseErrors)
if($parseErrors.Count){throw 'HEAP_SNAPSHOT_MODULE_PARSE_FAILED'}
. $module
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
$root=Join-Path $OutputDirectory ('heap-snapshot-selftest-'+[guid]::NewGuid().ToString('N'))
$null=[IO.Directory]::CreateDirectory($root)
$results=[Collections.Generic.List[object]]::new()
$failure=$null
$script:ActualLength=(Get-Item Function:Get-ProductHeapSnapshotLength).ScriptBlock
$script:ActualHash=(Get-Item Function:Get-ProductHeapSnapshotHash).ScriptBlock
function Check([bool]$Condition,[string]$Message){if(!$Condition){throw ('ASSERT_FAILED: '+$Message)}}
function Case([string]$Name,[scriptblock]$Body){
    if($CaseFilter.Count -and $Name -notin $CaseFilter){return}
    & $Body; $results.Add(@{name=$Name;status='PASS'})
}
function New-FakeProcess([int]$ProcessId,[string]$Path){
    $process=[pscustomobject]@{Id=$ProcessId;Path=$Path;StartTime=[DateTime]::UtcNow.AddMinutes(-1);
        Handle=[IntPtr]1;HasExited=$false;ExitCode=0;Killed=$false;Disposed=$false;RefreshCount=0;KillAttempts=0}
    $process|Add-Member ScriptMethod Refresh {
        $this.RefreshCount++
        if($this.Id -eq 4321 -and $script:Mode -eq 'identity_after_capture' -and $script:Started){$this.StartTime=$this.StartTime.AddTicks(1)}
        if($this.Id -eq 4321 -and $script:Mode -eq 'exit_during_capture' -and $script:Started){$this.HasExited=$true}
        if($this.Id -eq 9876 -and $script:Mode -eq 'helper_identity_changes'){$this.StartTime=$this.StartTime.AddTicks(1)}
        if($this.Id -eq 9876 -and $script:Mode -eq 'delayed_helper_path' -and $this.RefreshCount -ge 3){$this.Path=$script:Cdb}
    }
    $process|Add-Member ScriptMethod WaitForExit {
        param($Milliseconds)
        if($this.Killed){return $true}
        if($Milliseconds -eq 0){return $this.HasExited}
        if($script:Mode -eq 'delayed_helper_path' -and !$this.Path){return $false}
        if($script:Mode -eq 'timeout' -or $script:Mode -eq 'helper_identity_changes' -or $script:Mode -eq 'never_ready_helper_path'){
            $script:Clock.Elapsed=[TimeSpan]::FromSeconds(10);return $false
        }
        if($script:Mode -eq 'late_completion'){$script:Clock.Elapsed=[TimeSpan]::FromSeconds(10)}
        $this.HasExited=$true;return $true
    }
    $process|Add-Member ScriptMethod Kill {
        $this.KillAttempts++
        if($script:Mode -eq 'cleanup_failure_after_primary'){throw 'MOCK_HELPER_KILL_FAILED'}
        $this.Killed=$true;$this.HasExited=$true
    }
    $process|Add-Member ScriptMethod Dispose {$this.Disposed=$true}
    return $process
}
function New-Fixture([string]$Mode='success'){
    $script:Mode=$Mode;$script:Started=$false;$script:StartCalls=0;$script:PrivateInitialized=$false
    $script:Prefix=Join-Path $root ([guid]::NewGuid().ToString('N'))
    $script:Cdb=(Get-Process -Id $PID).Path
    $script:Target=New-FakeProcess 4321 $script:Cdb
    $script:Identity=[pscustomobject]@{pid=4321;start_ticks=$script:Target.StartTime.Ticks;executable=$script:Cdb;run_id=('a'*32)}
    $script:Helper=New-FakeProcess 9876 $script:Cdb
    $script:Clock=[pscustomobject]@{Elapsed=[TimeSpan]::FromSeconds(0.25)}
}
function New-ProductHeapSnapshotClock { return $script:Clock }
function Get-ProductHeapSnapshotProcess([int]$TargetPid){
    Check ($TargetPid -eq 4321) 'only requested product identity is inspected'
    return $script:Target
}
function Initialize-ProductHeapSnapshotPrivateDirectory([string]$Path){
    $null=[IO.Directory]::CreateDirectory($Path);$script:PrivateInitialized=$true
}
function Get-ProductHeapSnapshotLength([string]$Path){
    if($script:Mode -eq 'oversized' -or $script:Mode -eq 'cleanup_failure_after_primary'){return [long]2147483649}
    return (& $script:ActualLength $Path)
}
function Get-ProductHeapSnapshotHash([string]$Path,$Clock){
    $hash=& $script:ActualHash $Path $Clock
    if($script:Mode -eq 'hash_timeout'){$Clock.Elapsed=[TimeSpan]::FromSeconds(10)}
    return $hash
}
function Start-Process {
    param($FilePath,$WindowStyle,[switch]$PassThru,$ArgumentList,$RedirectStandardOutput,$RedirectStandardError,$ErrorAction)
    $script:StartCalls++;$script:Started=$true
    Check $script:PrivateInitialized 'private directory initialized before capture'
    Check ($FilePath -eq $script:Cdb -and $WindowStyle -eq 'Hidden' -and $PassThru) 'only explicit hidden helper starts'
    Check ($ArgumentList -contains '-pv' -and $ArgumentList -contains '-sins' -and $ArgumentList -contains '-y' -and
        !($ArgumentList -contains '-netsym:no')) 'noninvasive local-symbol capture uses installed CDB-compatible arguments'
    Check (!($ArgumentList -contains '-g') -and !($ArgumentList -contains '-G')) 'no target launch profile'
    $commandPath=$script:Prefix+'.capture.commands'
    $commands=Get-Content -LiteralPath $commandPath -Raw
    Check ($commands -notmatch '\+ust|\+hpa|\+hfc|\+htc|\+hpc|\.call' -and $commands -match '(?m)^qd\r?$') 'capture never mutates allocator and explicitly detaches'
    $dump=Join-Path ($script:Prefix+'.capture-private') 'snapshot.full.dmp'
    if($script:Mode -ne 'missing_dump'){
        $header=New-Object byte[] 64
        [Array]::Copy([BitConverter]::GetBytes([uint32]0x504d444d),0,$header,0,4)
        if($script:Mode -ne 'not_full_memory'){[Array]::Copy([BitConverter]::GetBytes([uint64]2),0,$header,24,8)}
        [IO.File]::WriteAllBytes($dump,$header)
    }
    $log="Dump successfully written`r`nB14_HEAP_SNAPSHOT_COMPLETE`r`n"
    if($script:Mode -eq 'missing_marker'){$log="0:000> .echo B14_HEAP_SNAPSHOT_COMPLETE`r`nDump successfully written`r`n"}
    [IO.File]::WriteAllText(($script:Prefix+'.capture.log'),$log)
    [IO.File]::WriteAllText($RedirectStandardOutput,'mock helper output')
    [IO.File]::WriteAllText($RedirectStandardError,'preserved mock stderr')
    if($script:Mode -eq 'nonzero_exit'){$script:Helper.ExitCode=7}
    if($script:Mode -in @('delayed_helper_path','never_ready_helper_path','early_exit_no_path')){$script:Helper.Path=$null}
    if($script:Mode -eq 'early_exit_no_path'){
        $script:Helper.HasExited=$true;$script:Helper.ExitCode=-2147467262
        [IO.File]::WriteAllText($RedirectStandardOutput,'Unable to examine process id 4321, HRESULT 0x80004002')
    }
    return $script:Helper
}
function Invoke-Snapshot {return (Invoke-ProductNormalHeapSnapshot -Identity $script:Identity -Prefix $script:Prefix -CdbExecutable $script:Cdb)}
function Reject-Snapshot([string]$Pattern,[bool]$ExpectReceipt=$true){
    $caught=$null;$value=$null
    try{$value=Invoke-Snapshot}catch{$caught=$_}
    Check ($null -ne $caught -and $caught.Exception.Message -match $Pattern) ('reject '+$Pattern)
    Check ($null -eq $value) 'failure returns no success receipt'
    if($ExpectReceipt){
        $receipt=Get-Content -LiteralPath ($script:Prefix+'.capture.json') -Raw|ConvertFrom-Json
        Check ($receipt.status -eq 'FAILED' -and !$receipt.release_eligible -and $receipt.formal_credit -eq 0) 'failure receipt cannot earn release credit'
        Check ((Get-Content -LiteralPath ($script:Prefix+'.capture.stderr') -Raw) -eq 'preserved mock stderr') 'stderr preserved'
    }
}
try{
    Case 'success_is_one_typed_diagnostic_receipt_with_real_hash' {
        New-Fixture
        $values=@(Invoke-Snapshot);Check ($values.Count -eq 1) 'only one result'
        $r=$values[0]
        Check ($r.PSObject.TypeNames[0] -eq 'Product.NormalHeapSnapshotReceipt' -and $r.status -eq 'CAPTURED_DIAGNOSTIC_ONLY') 'typed diagnostic receipt'
        Check ($r.dump_sha256 -eq (Get-FileHash -LiteralPath $r.dump_path).Hash.ToLowerInvariant() -and $r.dump_size_bytes -eq 64) 'real minidump-header hash'
        Check ($r.same_process_alive_after_detach -and $r.capture_exit_code -eq 0 -and $r.capture_helper_cleaned) 'detach/exit receipt'
        Check (!$r.normal_heap_flags_verified -and $r.formal_credit -eq 0 -and $r.qualification_credit -eq 0 -and $r.instrumented_diagnostic) 'no inferred heap flags or release credit'
        Check (!$script:Target.Killed -and $script:Target.Disposed -and $script:Helper.Disposed) 'product never killed and handles disposed'
    }
    Case 'invalid_identity_rejected_before_helper_start' {
        foreach($field in @('pid','start_ticks','run_id')){
            New-Fixture
            if($field -eq 'pid'){$script:Identity.pid='4321'}elseif($field -eq 'start_ticks'){$script:Identity.start_ticks='12'}else{$script:Identity.run_id='bad'}
            Reject-Snapshot 'IDENTITY_INVALID' $false;Check ($script:StartCalls -eq 0) 'invalid identity has no helper'
        }
        New-Fixture;$script:Identity.start_ticks++
        Reject-Snapshot 'IDENTITY_CHANGED_OR_EXITED' $false;Check ($script:StartCalls -eq 0) 'stale identity has no helper'
    }
    Case 'existing_dump_and_prefix_cannot_be_overwritten' {
        New-Fixture;$path=$script:Prefix+'.full.dmp';[IO.File]::WriteAllText($path,'preserve')
        Reject-Snapshot 'ALREADY_EXISTS' $false
        Check ((Get-Content -LiteralPath $path -Raw) -eq 'preserve' -and $script:StartCalls -eq 0) 'existing evidence unchanged'
        New-Fixture;$null=Invoke-Snapshot;$before=(Get-FileHash ($script:Prefix+'.capture.json')).Hash
        Reject-Snapshot 'ALREADY_EXISTS' $false
        Check ((Get-FileHash ($script:Prefix+'.capture.json')).Hash -eq $before -and $script:StartCalls -eq 1) 'same prefix is one-shot'
    }
    Case 'outside_out_prefix_rejected' {
        New-Fixture;$script:Prefix=Join-Path ([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))) 'invalid-heap-snapshot'
        Reject-Snapshot 'PREFIX_NOT_LOCAL_OUT' $false;Check ($script:StartCalls -eq 0) 'outside output root never starts helper'
    }
    Case 'nonzero_exit_preserves_failure' {New-Fixture 'nonzero_exit';Reject-Snapshot 'CDB_FAILED'}
    Case 'missing_dump_rejected' {New-Fixture 'missing_dump';Reject-Snapshot 'DUMP_SIZE_INVALID'}
    Case 'echoed_command_is_not_completion_marker' {New-Fixture 'missing_marker';Reject-Snapshot 'DUMP_NOT_CONFIRMED'}
    Case 'non_full_memory_dump_rejected' {New-Fixture 'not_full_memory';Reject-Snapshot 'NOT_FULL_MEMORY_DUMP'}
    Case 'oversized_dump_stops_exact_helper' {
        New-Fixture 'oversized';Reject-Snapshot 'DUMP_SIZE_INVALID'
        Check ($script:Helper.Killed -and !$script:Target.Killed) 'only owned helper killed'
    }
    Case 'target_identity_change_never_returns_success' {New-Fixture 'identity_after_capture';Reject-Snapshot 'IDENTITY_CHANGED_OR_EXITED'}
    Case 'target_exit_stops_capture' {
        New-Fixture 'exit_during_capture';Reject-Snapshot 'TARGET_EXITED_DURING_CAPTURE'
        Check ($script:Helper.Killed) 'owned helper stopped on target exit'
    }
    Case 'timeout_kills_only_exact_owned_helper' {
        New-Fixture 'timeout';Reject-Snapshot 'TIMEOUT'
        Check ($script:Helper.Killed -and !$script:Target.Killed) 'timeout cleanup exact helper'
    }
    Case 'reused_helper_identity_is_never_killed' {
        New-Fixture 'helper_identity_changes';Reject-Snapshot 'HELPER_START_IDENTITY_CHANGED'
        Check (!$script:Helper.Killed) 'different helper identity not killed'
    }
    Case 'delayed_helper_path_waits_without_killing_live_target' {
        New-Fixture 'delayed_helper_path';$receipt=Invoke-Snapshot
        Check ($receipt.status -eq 'CAPTURED_DIAGNOSTIC_ONLY' -and $receipt.capture_identity.path_verified) 'Path becomes ready within same budget'
        Check ($script:Helper.RefreshCount -ge 3 -and !$script:Helper.Killed -and !$script:Target.Killed) 'startup race is not a failure or kill'
    }
    Case 'early_helper_exit_without_path_preserves_real_cdb_failure' {
        New-Fixture 'early_exit_no_path';Reject-Snapshot 'CDB_FAILED: exit=-2147467262'
        $receipt=Get-Content -LiteralPath ($script:Prefix+'.capture.json') -Raw|ConvertFrom-Json
        Check ($receipt.capture_exit_code -eq -2147467262 -and !$receipt.capture_identity.path_verified -and $receipt.capture_helper_cleaned) 'real exit preserved without unavailable Path'
        Check ($receipt.error -match 'CDB_FAILED' -and !$receipt.PSObject.Properties['cleanup_error'] -and $script:Helper.KillAttempts -eq 0) 'exited helper is not killed and cleanup cannot replace primary failure'
        Check ((Get-Content -LiteralPath ($script:Prefix+'.capture.stdout') -Raw) -match '0x80004002') 'original attach error remains in stdout'
    }
    Case 'unavailable_path_budget_kills_retained_exact_owner' {
        New-Fixture 'never_ready_helper_path';Reject-Snapshot 'TIMEOUT'
        Check ($script:Helper.Killed -and $script:Helper.KillAttempts -eq 1 -and !$script:Target.Killed) 'stable retained handle/PID/start time allows exact cleanup with absent Path'
    }
    Case 'cleanup_failure_is_separate_from_first_failure' {
        New-Fixture 'cleanup_failure_after_primary';Reject-Snapshot 'DUMP_SIZE_INVALID'
        $receipt=Get-Content -LiteralPath ($script:Prefix+'.capture.json') -Raw|ConvertFrom-Json
        Check ($receipt.error -eq 'NORMAL_HEAP_SNAPSHOT_DUMP_SIZE_INVALID' -and $receipt.cleanup_error -match 'MOCK_HELPER_KILL_FAILED') 'receipt preserves primary error and separate cleanup error'
    }
    Case 'late_exit_and_hash_exceeding_budget_cannot_pass' {
        New-Fixture 'late_completion';Reject-Snapshot 'TIMEOUT'
        New-Fixture 'hash_timeout';Reject-Snapshot 'TIMEOUT'
    }
}catch{$failure=$_}
$summary=[ordered]@{schema=1;kind='offline_mock_helper_contract';status=$(if($failure){'FAIL'}else{'PASS'});
    product_started=$false;debugger_started=$false;fixture_started=$false;runtime_credit=0;cases=@($results.ToArray());
    module_sha256=(Get-FileHash -LiteralPath $module).Hash.ToLowerInvariant();evidence_directory=$root}
if($failure){$summary.error=$failure.Exception.Message;$summary.stack=$failure.ScriptStackTrace}
$summary|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $root 'result.json') -Encoding UTF8
$summary|ConvertTo-Json -Depth 8 -Compress
if($failure){throw $failure}
