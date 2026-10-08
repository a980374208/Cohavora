param([string]$EvidenceDirectory = "$PSScriptRoot/../../../out/b14-repair-20261007")
$ErrorActionPreference='Stop'
if ($PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {throw 'WINDOWS_POWERSHELL_51_REQUIRED'}
$EvidenceDirectory=[IO.Path]::GetFullPath($EvidenceDirectory)
$OutputDirectory=Join-Path $EvidenceDirectory ('lifetime-handoff-'+[guid]::NewGuid().ToString('N'))
$null=[IO.Directory]::CreateDirectory($OutputDirectory)
$driver=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../uia/product_desktop.ps1'))
$watchdog=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../tools/product_acceptance/product_pilot_watchdog.ps1'))
$definitions=[ordered]@{}
$loaded=[ordered]@{}
foreach($entry in @(
    @{path=$driver;names=@('Append-SafeJsonl','Write-ProductLifetimeProgress','Wait-ProductMinimumLifetime','Record','Action')},
    @{path=$watchdog;names=@('Read-ProductObserverSnapshot','Test-ProductFiniteNumber','Assert-ProductObserverIdentity','Get-ProductObserverAgeMilliseconds','Assert-ProductActionProgress')})) {
    $tokens=$null;$errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile($entry.path,[ref]$tokens,[ref]$errors)
    if($errors.Count){throw 'REAL_SOURCE_PARSE_FAILED'}
    foreach($name in $entry.names){
        $matches=@($ast.EndBlock.Statements | Where-Object {$_ -is [Management.Automation.Language.FunctionDefinitionAst] -and $_.Name -eq $name})
        if($matches.Count -ne 1){throw ('REAL_FUNCTION_NOT_UNIQUE: '+$name)}
        $definitions[$name]=$matches[0].Extent.Text
        $loaded[$name]=[ordered]@{path=$entry.path;line=$matches[0].Extent.StartLineNumber}
        . ([scriptblock]::Create($definitions[$name]))
    }
}
function Assert-True([bool]$Value,[string]$Label){if(!$Value){throw ('ASSERT_FAILED: '+$Label)}}
function Assert-Rejected([scriptblock]$Body,[string]$Reason){
    $caught=$null
    try{$null=& $Body}catch{$caught=$_.Exception.Message}
    Assert-True ($caught -eq $Reason) ('expected '+$Reason+' got '+$caught)
}
function Copy-Frame($Frame){return ($Frame | ConvertTo-Json -Depth 6 -Compress | ConvertFrom-Json)}
$cases=[Collections.Generic.List[object]]::new()
function Case([string]$Name,[scriptblock]$Body){
    $null=& $Body
    $cases.Add([ordered]@{name=$Name;status='PASS'})
}
$script:child=Get-Process -Id $PID
$script:runId=[guid]::NewGuid().ToString('N');$script:cycle=100;$Cycles=100;$MinimumSeconds=28800
$script:productFirstLive=@{pid=$PID;start_ticks=$script:child.StartTime.ToUniversalTime().Ticks;executable=$script:child.Path;run_clock_elapsed_origin_seconds=[double]0}
$identity=[pscustomobject]@{run_id=$script:runId;pid=$PID;start_ticks=$script:productFirstLive.start_ticks;executable=$script:productFirstLive.executable}
$script:runClock=[Diagnostics.Stopwatch]::StartNew()
$script:oldAction=[pscustomobject]@{run_id=$script:runId;pid=$PID;cycle=100;action='export';phase='uia_observed';utc=[DateTime]::UtcNow.AddSeconds(-401).ToString('o');elapsed_seconds=[double]28399}
$script:sample=[pscustomobject]@{pid=$PID;start_ticks=$script:productFirstLive.start_ticks;executable=$script:productFirstLive.executable;
    live_elapsed_seconds=[double]28800;run_clock_elapsed_seconds=[double]28800;utc=[DateTime]::UtcNow.ToString('o')}
# Supply a floor-boundary clock sample. Real writer, wait, Record, Action and
# watchdog definitions are used; no product, UIA or remote operation is started.
function Get-ProductLiveElapsedSeconds {param([switch]$AsSample) return $script:sample}
function Sample-Resource {param([string]$Phase) throw 'UNEXPECTED_WAIT'}
$script:fences=[Collections.Generic.List[string]]::new()
$script:exitBodyReached=$false
function Sync-ObserverContext([string]$Action,[string]$Phase){
    Assert-True ($Action -eq 'process_exit') 'real-action-is-process-exit'
    $script:fences.Add($Phase)
    if($Phase -eq 'requested'){
        Assert-True (!(Test-Path (Join-Path $OutputDirectory 'uia-actions.jsonl'))) 'fence-precedes-requested-append'
        $progress=Read-ProductObserverSnapshot (Join-Path $OutputDirectory 'product-lifetime-progress.json')
        Assert-True ($progress.phase -ceq 'complete') 'real-wait-published-complete'
        Assert-ProductActionProgress $script:oldAction $script:runId 1 ([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()) $progress $identity $Cycles $MinimumSeconds $script:child
    }
}
$oldRemote=$env:LIVEKIT_UIA_REMOTE_CONTEXT
$failure=$null;$stub=$null;$cleanupErrors=[Collections.Generic.List[string]]::new()
try {
    Case 'real_complete_to_exit_preappend_remote_fence_is_accepted' {
        $env:LIVEKIT_UIA_REMOTE_CONTEXT='1'
        Wait-ProductMinimumLifetime
        Action 'process_exit' {$script:exitBodyReached=$true}
        Assert-True $script:exitBodyReached 'real-action-body-reached'
        Assert-True (($script:fences -join ',') -eq 'requested,uia_observed') 'both-real-record-fences-observed'
        $rows=@(Get-Content (Join-Path $OutputDirectory 'uia-actions.jsonl') | ForEach-Object {$_ | ConvertFrom-Json})
        Assert-True ($rows.Count -eq 2 -and $rows[0].phase -eq 'requested' -and $rows[1].phase -eq 'uia_observed') 'real-action-appended-two-complete-rows'
    }
    $progress=Read-ProductObserverSnapshot (Join-Path $OutputDirectory 'product-lifetime-progress.json')
    $now=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    Case 'complete_freshness_400_second_boundary_is_bounded' {
        $timestamp=[DateTimeOffset]::Parse($progress.utc).ToUnixTimeMilliseconds()
        Assert-ProductActionProgress $script:oldAction $script:runId 1 ($timestamp+400000) $progress $identity $Cycles $MinimumSeconds $script:child
        Assert-Rejected {Assert-ProductActionProgress $script:oldAction $script:runId 1 ($timestamp+400001) $progress $identity $Cycles $MinimumSeconds $script:child} 'UIA_OPERATION_WATCHDOG_TIMEOUT'
    }
    Case 'complete_wrong_run_pid_start_ticks_or_executable_is_rejected' {
        foreach($field in @('run_id','pid','start_ticks','executable')){
            $bad=Copy-Frame $progress
            switch($field){
                run_id {$bad.run_id='b'*32}
                pid {$bad.pid=$PID+1}
                start_ticks {$bad.start_ticks=[long]($bad.start_ticks+1)}
                executable {$bad.executable=$bad.executable+'.foreign'}
            }
            Assert-Rejected {Assert-ProductActionProgress $script:oldAction $script:runId 1 $now $bad $identity $Cycles $MinimumSeconds $script:child} 'OBSERVER_PRODUCT_IDENTITY_INVALID'
        }
    }
    Case 'complete_wrong_action_pid_cannot_bridge_old_action' {
        $bad=Copy-Frame $script:oldAction;$bad.pid=$PID+1
        Assert-Rejected {Assert-ProductActionProgress $bad $script:runId 1 $now $progress $identity $Cycles $MinimumSeconds $script:child} 'UIA_OPERATION_WATCHDOG_TIMEOUT'
    }
    Case 'complete_requires_floor_and_final_cycle' {
        $bad=Copy-Frame $progress;$bad.live_elapsed_seconds=[double]28799.99
        Assert-Rejected {Assert-ProductActionProgress $script:oldAction $script:runId 1 $now $bad $identity $Cycles $MinimumSeconds $script:child} 'UIA_OPERATION_WATCHDOG_TIMEOUT'
        $bad=Copy-Frame $progress;$bad.cycle=99
        Assert-Rejected {Assert-ProductActionProgress $script:oldAction $script:runId 1 $now $bad $identity $Cycles $MinimumSeconds $script:child} 'UIA_OPERATION_WATCHDOG_TIMEOUT'
    }
    Case 'waiting_at_or_above_floor_and_missing_live_process_are_rejected' {
        $bad=Copy-Frame $progress;$bad.phase='waiting'
        Assert-Rejected {Assert-ProductActionProgress $script:oldAction $script:runId 1 $now $bad $identity $Cycles $MinimumSeconds $script:child} 'UIA_OPERATION_WATCHDOG_TIMEOUT'
        Assert-Rejected {Assert-ProductActionProgress $script:oldAction $script:runId 1 $now $progress $identity $Cycles $MinimumSeconds $null} 'LIFETIME_PROGRESS_PRODUCT_NOT_LIVE'
    }
    Case 'old_process_exit_cannot_be_refreshed_by_complete' {
        $bad=Copy-Frame $script:oldAction;$bad.action='process_exit'
        Assert-Rejected {Assert-ProductActionProgress $bad $script:runId 1 $now $progress $identity $Cycles $MinimumSeconds $script:child} 'UIA_OPERATION_WATCHDOG_TIMEOUT'
    }
    Case 'actual_exited_owned_process_cannot_bridge_exit_handoff' {
        $info=[Diagnostics.ProcessStartInfo]::new()
        $info.FileName=Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
        $info.Arguments='-NoProfile -NonInteractive -Command "Start-Sleep -Seconds 2; exit 0"'
        $info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
        $script:stub=[Diagnostics.Process]::Start($info)
        $null=$script:stub.Handle
        $exitedIdentity=Copy-Frame $identity
        $exitedIdentity.pid=$script:stub.Id;$exitedIdentity.start_ticks=$script:stub.StartTime.ToUniversalTime().Ticks;$exitedIdentity.executable=$info.FileName
        Assert-True ($script:stub.WaitForExit(5000)) 'owned-stub-exited'
        $bad=Copy-Frame $progress;$bad.pid=$exitedIdentity.pid;$bad.start_ticks=$exitedIdentity.start_ticks;$bad.executable=$exitedIdentity.executable
        $action=Copy-Frame $script:oldAction;$action.pid=$exitedIdentity.pid
        Assert-Rejected {Assert-ProductActionProgress $action $script:runId 1 ([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()) $bad $exitedIdentity $Cycles $MinimumSeconds $script:stub} 'LIFETIME_PROGRESS_PRODUCT_IDENTITY_CHANGED'
    }
    # A concurrent edit outside these functions need not invalidate this gate;
    # every function actually executed must still have exactly the same body.
    foreach($name in $definitions.Keys){
        $tokens=$null;$errors=$null
        $ast=[Management.Automation.Language.Parser]::ParseFile($loaded[$name].path,[ref]$tokens,[ref]$errors)
        $current=@($ast.EndBlock.Statements | Where-Object {$_ -is [Management.Automation.Language.FunctionDefinitionAst] -and $_.Name -eq $name})
        Assert-True (!$errors.Count -and $current.Count -eq 1 -and $current[0].Extent.Text -ceq $definitions[$name]) ('executed-function-stable: '+$name)
    }
} catch {$failure=$_.Exception.Message}
finally {
    $env:LIVEKIT_UIA_REMOTE_CONTEXT=$oldRemote
    if($script:stub){
        try{if(!$script:stub.HasExited){$script:stub.Kill();$null=$script:stub.WaitForExit(5000)}}catch{$cleanupErrors.Add($_.Exception.Message)}
        $script:stub.Dispose()
    }
    $script:child.Dispose()
}
$functionHashes=[ordered]@{}
foreach($name in $definitions.Keys){
    $sha=[Security.Cryptography.SHA256]::Create()
    try{$functionHashes[$name]=([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($definitions[$name])))).Replace('-','').ToLowerInvariant()}
    finally{$sha.Dispose()}
}
$verdict=if($failure -or $cleanupErrors.Count){'FAIL'}else{'PASS'}
$proof=[ordered]@{schema=1;verdict=$verdict;cases_passed=$cases.Count;cases=@($cases);failure=$failure;cleanup_errors=@($cleanupErrors);
    utc=[DateTime]::UtcNow.ToString('o');powershell_version=$PSVersionTable.PSVersion.ToString();function_sha256=$functionHashes;
    driver_sha256=(Get-FileHash $driver).Hash.ToLowerInvariant();watchdog_sha256=(Get-FileHash $watchdog).Hash.ToLowerInvariant();
    scope='actual writer/wait/Record/Action/watchdog functions; supplied floor clock sample and hidden owned exit stub';
    boundaries=@{product_started=$false;network_access=$false;uia_run=$false;formal_credit=0;qualification_credit=0}}
$proofPath=Join-Path $OutputDirectory 'verification.json'
[IO.File]::WriteAllText($proofPath,($proof | ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
[ordered]@{verdict=$verdict;cases_passed=$cases.Count;evidence=$proofPath;failure=$failure} | ConvertTo-Json -Compress
if($verdict -ne 'PASS'){exit 1}
