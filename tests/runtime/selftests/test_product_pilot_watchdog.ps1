param([string]$OutputDirectory="$PSScriptRoot/../../../out/b14-source63-20261006",[string[]]$CaseFilter=@())
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../tools/product_acceptance/product_pilot_watchdog.ps1')
$Run='a'*32
$NowMs=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
$Utc=[DateTimeOffset]::FromUnixTimeMilliseconds($NowMs).ToString('o')
$OldUtc=[DateTimeOffset]::FromUnixTimeMilliseconds($NowMs-401000).ToString('o')
$owned=Get-Process -Id $PID
$Identity=[pscustomobject]@{run_id=$Run;pid=$PID;start_ticks=$owned.StartTime.ToUniversalTime().Ticks;executable=$owned.Path}
$Root=Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) ('watchdog-fixture-'+[guid]::NewGuid().ToString('N'))
$null=[IO.Directory]::CreateDirectory($Root)
$results=[Collections.Generic.List[object]]::new()
function Check([bool]$Value,[string]$Name){if(!$Value){throw "ASSERT_FAILED: $Name"}}
function Reject([scriptblock]$Body,[string]$Pattern){
    $caught=$null;try{& $Body}catch{$caught=$_}
    Check ($null -ne $caught -and $caught.Exception.Message -match $Pattern) ('reject/'+$Pattern)
}
function Case([string]$Name,[scriptblock]$Body){if($CaseFilter.Count -and $Name -notin $CaseFilter){return};$null=& $Body;$results.Add(@{name=$Name;status='PASS'})}
function Copy-Object($Value){return ($Value|ConvertTo-Json -Depth 8|ConvertFrom-Json)}
function Action([string]$Time){return [pscustomobject]@{run_id=$Run;pid=$PID;utc=$Time;cycle=100;action='export';elapsed_seconds=100.0}}
function Progress {
    return [pscustomobject]@{schema=1;run_id=$Run;pid=$PID;start_ticks=$Identity.start_ticks;executable=$Identity.executable;
        cycle=100;cycles=100;required_seconds=28800;live_elapsed_seconds=28799.0;run_clock_elapsed_seconds=28900.0;utc=$Utc;phase='waiting'}
}
function Health {
    return [pscustomobject]@{schema=1;run_id=$Run;process_run_id=('c'*32);collector='in_process';sequence=1;
        gpu_budget=[pscustomobject]@{pid=$PID};utc_ms=$NowMs;
        history=[pscustomobject]@{queue_drops=0;pending_records_dropped=0;write_failures=0};
        diagnostic=[pscustomobject]@{dropped_ordinary=0;dropped_critical=0;sink_failures=0}}
}
function Child([int]$Id,[bool]$FailWait=$false){
    $value=[pscustomobject]@{Id=$Id;HasExited=$false;ExitCode=0;FailWait=$FailWait;waits=0;kills=0;refreshes=0}
    $value|Add-Member ScriptMethod WaitForExit {param($Milliseconds);$this.waits++;if($this.FailWait -and $this.waits -eq 1){throw 'injected wait failure'};$this.HasExited=$true;return $true}
    $value|Add-Member ScriptMethod Refresh {$this.refreshes++}
    $value|Add-Member ScriptMethod Kill {$this.kills++;$this.HasExited=$true}
    return $value
}
function Invoke-ActualObserverIteration([bool]$LifetimeWait,[bool]$ResourceStale=$false) {
    $invoker=Join-Path $PSScriptRoot '../tools/product_acceptance/invoke_product_external.ps1'
    $tokens=$null;$errors=$null;$ast=[Management.Automation.Language.Parser]::ParseFile($invoker,[ref]$tokens,[ref]$errors)
    Check (!$errors.Count) 'invoker parse'
    $loop=$ast.Find({param($node);$node -is [Management.Automation.Language.WhileStatementAst] -and
        $node.Condition.Extent.Text -eq '!$uia.WaitForExit(1000)'},$true)
    Check ($null -ne $loop) 'actual observer loop'
    $statements=@($loop.Body.Statements);$start=-1
    for($i=0;$i -lt $statements.Count;$i++){if($statements[$i].Extent.Text -eq '$last=$null'){$start=$i;break}}
    Check ($start -ge 0) 'action-first observer start'
    $before=@($statements[0..($start-1)]|Where-Object {$_.Extent.Text -match 'product-identity.json'})
    Check ($before.Count -eq 0) 'identity is not cached before action'
    $body=($statements[$start..($statements.Count-1)]|ForEach-Object {$_.Extent.Text}) -join "`n"
    $fixtureIdentity=$Identity
    $identity=$null;$probeMissingClock=$null;$resourceMissingClock=$null
    $resource=[pscustomobject]@{HasExited=$false};$Root='observer-fixture';$run=$Run;$cycles=100;$seconds=28800
    $uiaLaunchClock=[Diagnostics.Stopwatch]::StartNew();$script:fixtureIdentityPublished=$false
    $script:fixtureReads=[Collections.Generic.List[string]]::new()
    function Test-Path([string]$Path){
        if($Path.EndsWith('product-identity.json')){return $script:fixtureIdentityPublished}
        if($Path.EndsWith('product-lifetime-progress.json')){return $LifetimeWait}
        return $true
    }
    function Read-LatestCompleteUiaAction([string]$Path){
        # Publish both during the read, after any premature identity lookup.
        $script:fixtureIdentityPublished=$true;$script:fixtureReads.Add('action')
        $time=[DateTimeOffset]::UtcNow;if($LifetimeWait){$time=$time.AddSeconds(-401)}
        return Action ($time.ToString('o'))
    }
    function Read-ProductObserverSnapshot([string]$Path){
        if($Path.EndsWith('product-identity.json')){$script:fixtureReads.Add('identity');return $fixtureIdentity}
        Start-Sleep -Milliseconds 30
        $value=Progress;$value.utc=[DateTimeOffset]::UtcNow.ToString('o')
        $script:fixtureReads.Add('lifetime');return $value
    }
    function Read-ProductPilotCompleteJsonlTail {
        Start-Sleep -Milliseconds 30
        $time=[DateTimeOffset]::UtcNow
        if($ResourceStale){$time=$time.AddMilliseconds(-10001)}
        $value=Copy-Object $fixtureIdentity;$value|Add-Member NoteProperty utc ($time.ToString('o'))
        $value|Add-Member NoteProperty process_alive $true
        $script:fixtureReads.Add('resource');return ($value|ConvertTo-Json -Compress)
    }
    function Read-ProductPilotProbeTail {
        Start-Sleep -Milliseconds 30
        $value=Health;$value.utc_ms=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
        $script:fixtureReads.Add('native');return ($value|ConvertTo-Json -Depth 4 -Compress)
    }
    . ([scriptblock]::Create($body))
    Check ($null -ne $identity -and $null -ne $probeMissingClock -and $null -ne $resourceMissingClock) 'published identity and both live discovery clocks'
    Check (($script:fixtureReads[0..1] -join ',') -eq 'action,identity') 'action precedes identity'
    Check ($script:fixtureReads -contains 'resource' -and $script:fixtureReads -contains 'native') 'new frames passed real live checks'
    if($LifetimeWait){Check ($script:fixtureReads -contains 'lifetime') 'new wait frame passed real progress check'}
}
try{
    Case 'shared_qpc_marker_real_roundtrip_and_rejection' {
        $path=Join-Path $Root 'run-clock.json';$budget=Write-ProductRunBudget $path $Run 30000
        $loaded=Read-ProductRunBudget $path $Run 30000
        Check ((Get-ProductRunBudgetElapsed $loaded) -ge 0) 'real elapsed'
        Reject {Read-ProductRunBudget $path ('b'*32) 30000} 'RUN_BUDGET_INVALID'
        Reject {Read-ProductRunBudget $path $Run 30001} 'RUN_BUDGET_INVALID'
        foreach($pair in @(@('schema',$true),@('frequency_hz',1),@('start_qpc_ticks',[long]::MaxValue))){
            $bad=Copy-Object $loaded;$bad.($pair[0])=$pair[1]
            Reject {Assert-ProductRunBudget $bad $Run 30000} 'RUN_BUDGET_INVALID'
        }
        $huge=Join-Path $Root 'huge.json';[IO.File]::WriteAllText($huge,(' '*8193))
        Reject {Read-ProductObserverSnapshot $huge} 'OBSERVER_SNAPSHOT_TOO_LARGE'
    }
    Case 'startup_400_boundary_without_first_action' {
        Assert-ProductActionProgress $null $Run 400 $NowMs $null $Identity 100 28800 $null
        Reject {Assert-ProductActionProgress $null $Run 400.001 $NowMs $null $Identity 100 28800 $null} 'UIA_STARTUP_WATCHDOG_TIMEOUT'
        $bad=Action $Utc;$bad.run_id='b'*32
        Reject {Assert-ProductActionProgress $bad $Run 1 $NowMs $null $Identity 100 28800 $null} 'UIA_ACTION_IDENTITY_INVALID'
    }
    Case 'old_action_valid_wait_owned_real_process_continues' {
        Assert-ProductActionProgress (Action $OldUtc) $Run 500 $NowMs (Progress) $Identity 100 28800 $owned
        Reject {Assert-ProductActionProgress (Action $OldUtc) $Run 500 $NowMs $null $Identity 100 28800 $owned} 'UIA_OPERATION_WATCHDOG_TIMEOUT'
    }
    Case 'invalid_wait_frame_and_old_shutdown_frame_rejected' {
        foreach($pair in @(@('run_id',('b'*32)),@('pid',($PID+1)),@('start_ticks',($Identity.start_ticks+1)),
            @('executable',($Identity.executable+'.other')),@('cycle',99),@('cycles',99),@('required_seconds',28799),
            @('live_elapsed_seconds',[double]::NaN),@('live_elapsed_seconds',28800.0),@('run_clock_elapsed_seconds',1.0),
            @('utc',$OldUtc),@('phase','complete'),@('schema',$true))){
            $bad=Progress;$bad.($pair[0])=$pair[1]
            Reject {Assert-ProductActionProgress (Action $OldUtc) $Run 500 $NowMs $bad $Identity 100 28800 $owned} 'IDENTITY_INVALID|WATCHDOG_TIMEOUT'
        }
        $exitAction=Action $OldUtc;$exitAction.action='process_exit'
        Reject {Assert-ProductActionProgress $exitAction $Run 500 $NowMs (Progress) $Identity 100 28800 $owned} 'UIA_OPERATION_WATCHDOG_TIMEOUT'
        Reject {Assert-ProductActionProgress (Action $OldUtc) $Run 500 $NowMs (Progress) $Identity 100 28800 $null} 'PRODUCT_NOT_LIVE'
        $badAction=Action $OldUtc;$badAction.cycle='100'
        Reject {Assert-ProductActionProgress $badAction $Run 500 $NowMs (Progress) $Identity 100 28800 $owned} 'UIA_OPERATION_WATCHDOG_TIMEOUT'
    }
    Case 'actual_observer_concurrent_first_action_identity_and_new_health_frames' {Invoke-ActualObserverIteration $false}
    Case 'actual_observer_reads_lifetime_resource_native_before_clock_sample' {Invoke-ActualObserverIteration $true}
    Case 'actual_observer_alive_resource_with_stalled_sample_fails_10s_gate' {
        Reject {Invoke-ActualObserverIteration $false $true} 'RESOURCE_SAMPLE_STALE'
    }
    Case 'native_missing_15s_and_stale_boundary' {
        Assert-ProductNativeHealth $null $Run $Identity 15 $NowMs $false
        Reject {Assert-ProductNativeHealth $null $Run $Identity 15.001 $NowMs $false} 'PROCESS_PROBE_MISSING_OR_INVALID'
        $value=Health;$value.utc_ms=$NowMs-15000;Assert-ProductNativeHealth $value $Run $Identity 100 $NowMs $false
        $value.utc_ms--
        Reject {Assert-ProductNativeHealth $value $Run $Identity 100 $NowMs $false} 'PROCESS_PROBE_STALE'
        Assert-ProductNativeHealth $value $Run $Identity 100 $NowMs $true
    }
    Case 'native_wrong_identity_missing_counter_and_all_six_loss_fields_fail' {
        $value=Health;$value.run_id='b'*32;Reject {Assert-ProductNativeHealth $value $Run $Identity 0 $NowMs $false} 'IDENTITY_INVALID'
        $value=Health;$value.gpu_budget.pid++;Reject {Assert-ProductNativeHealth $value $Run $Identity 0 $NowMs $false} 'IDENTITY_INVALID'
        $value=Health;$value.history.queue_drops=$null;Reject {Assert-ProductNativeHealth $value $Run $Identity 0 $NowMs $false} 'HEALTH_INVALID'
        foreach($field in @('history.queue_drops','history.pending_records_dropped','history.write_failures',
            'diagnostic.dropped_ordinary','diagnostic.dropped_critical','diagnostic.sink_failures')){
            $value=Health;$parts=$field.Split('.');$value.($parts[0]).($parts[1])=1
            Reject {Assert-ProductNativeHealth $value $Run $Identity 0 $NowMs $true} 'LIVE_ZERO_LOSS_GATE_FAILED'
        }
    }
    Case 'native_raw_nested_gpu_pid_contract_and_no_flattened_fallback' {
        # The native writer emits gpu_budget.pid; the invoker passes this raw
        # JSON object directly, without creating a gpu_target_pid projection.
        $value=Copy-Object (Health)
        Check ($value.collector -eq 'in_process' -and $null -eq $value.gpu_target_pid) 'raw native fixture shape'
        Assert-ProductNativeHealth $value $Run $Identity 0 $NowMs $false
        $value.gpu_budget.pid++
        $value|Add-Member NoteProperty gpu_target_pid $PID
        Reject {Assert-ProductNativeHealth $value $Run $Identity 0 $NowMs $false} 'PROCESS_PROBE_IDENTITY_INVALID'
        $value=Copy-Object (Health);$value.gpu_budget.pid=[string]$PID
        Reject {Assert-ProductNativeHealth $value $Run $Identity 0 $NowMs $false} 'PROCESS_PROBE_IDENTITY_INVALID'
        $value=Copy-Object (Health);$value.PSObject.Properties.Remove('gpu_budget')
        $value|Add-Member NoteProperty gpu_target_pid $PID
        Reject {Assert-ProductNativeHealth $value $Run $Identity 0 $NowMs $false} 'PROCESS_PROBE_IDENTITY_INVALID'
    }
    Case 'resource_10s_gap_alive_collector_cannot_hide_stale_samples' {
        $sample=Copy-Object $Identity;$sample|Add-Member NoteProperty utc $Utc;$sample|Add-Member NoteProperty process_alive $true
        Assert-ProductResourceHealth $sample $Run $Identity 100 $NowMs $false
        $sample.utc=[DateTimeOffset]::FromUnixTimeMilliseconds($NowMs-10001).ToString('o')
        Reject {Assert-ProductResourceHealth $sample $Run $Identity 100 $NowMs $false} 'RESOURCE_SAMPLE_STALE'
        Reject {Assert-ProductResourceHealth $null $Run $Identity 10.001 $NowMs $false} 'RESOURCE_SAMPLE_MISSING'
        $sample.utc=$Utc;$sample.pid++;Reject {Assert-ProductResourceHealth $sample $Run $Identity 0 $NowMs $false} 'IDENTITY_INVALID'
    }
    Case 'cleanup_fault_does_not_skip_other_owned_children' {
        $first=Child 1001 $true;$second=Child 1002
        $result=Invoke-ProductCollectorCleanup @($first,$second) $null '' '' ''
        Check (!$result.passed -and $result.collectors.Count -eq 2 -and $result.errors.Count -eq 1) 'failed with complete receipts'
        Check ($first.HasExited -and $first.kills -eq 1 -and $second.waits -eq 1 -and $second.HasExited) 'both cleaned'
    }
    Case 'gpu_stop_file_fault_still_cleans_all_children' {
        $gpu=Child 1003;$other=Child 1004
        $result=Invoke-ProductCollectorCleanup @($gpu,$other) $gpu '' '' (Join-Path $Root 'nonexistent')
        Check (!$result.passed -and $result.errors -contains 'GPU_STOP_FILE_FAILED' -and $result.collectors.Count -eq 2) 'write fault retained'
        Check ($gpu.HasExited -and $other.HasExited) 'children still cleaned'
    }
    Case 'actual_invoker_lost_launch_reply_runs_shutdown_and_retains_failure' {
        $invoker=Join-Path $PSScriptRoot '../tools/product_acceptance/invoke_product_external.ps1'
        $tokens=$null;$errors=$null;$ast=[Management.Automation.Language.Parser]::ParseFile($invoker,[ref]$tokens,[ref]$errors)
        Check (!$errors.Count) 'invoker parse'
        $tries=@($ast.EndBlock.Statements|Where-Object {$_ -is [Management.Automation.Language.TryStatementAst]})
        $main=$tries[0]
        $selected=@($main.Body.Statements|Where-Object {
            $_.Extent.Text -match '^\$runBudget=Write-ProductRunBudget|^\$remoteLaunchAttempted=\$true|^Invoke-TestRemote "nohup'
        })
        Check ($selected.Count -eq 3) 'actual three launch statements'
        $Root=Join-Path $Root 'launch';$null=[IO.Directory]::CreateDirectory($Root)
        $run=$Run;$maximum=30000;$remote='/fixture';$remoteRun='/fixture/pilot';$prefix='aaaaaaaa';$target=@{livekit_config='/fixture/config'}
        $schedulerArg='';$formalArg='';$meetingId='000000001';$remoteLaunchAttempted=$false;$runExit=1
        $children=@();$gpuTrace=$null;$GpuTraceTool='';$gpuSession='';$gpuDirectory='';$AudioTimingDiagnostic=$false
        $cleanupErrors=[Collections.Generic.List[string]]::new();$script:shutdownCalls=[Collections.Generic.List[object]]::new()
        function Invoke-TestRemote {throw 'injected committed launch lost receipt'}
        function python {$script:shutdownCalls.Add(@($args));$global:LASTEXITCODE=0}
        $caught=$false
        try{. ([scriptblock]::Create(($selected|ForEach-Object {$_.Extent.Text}) -join "`n"))}catch{$caught=$true}
        Check ($caught -and $remoteLaunchAttempted) 'uncertain launch marked before failure'
        . ([scriptblock]::Create($main.Finally.Extent.Text.TrimStart('{').TrimEnd('}')))
        Check ($script:shutdownCalls.Count -eq 1 -and $script:shutdownCalls[0] -contains '--launch-attempted') 'actual finally calls same run cleanup'
        Check ($script:shutdownCalls[0] -contains $run) 'full run id'
        # Run the actual terminal finally, which must preserve the failed launch.
        $terminal=$tries[-1]
        . ([scriptblock]::Create($terminal.Finally.Extent.Text.TrimStart('{').TrimEnd('}')))
        $receipt=Get-Content -LiteralPath (Join-Path $Root 'runner-exit.json') -Raw|ConvertFrom-Json
        Check ($receipt.exit_code -eq 1 -and $receipt.verdict -eq 'FAIL') 'failure remains failure'
    }
    $proof=@{schema=1;verdict='PASS';cases_passed=$results.Count;cases=@($results.ToArray());product_started=$false;remote_called=$false;
        source_sha256=(Get-FileHash (Join-Path $PSScriptRoot '../tools/product_acceptance/product_pilot_watchdog.ps1')).Hash.ToLowerInvariant();
        invoker_sha256=(Get-FileHash (Join-Path $PSScriptRoot '../tools/product_acceptance/invoke_product_external.ps1')).Hash.ToLowerInvariant();case_filter=@($CaseFilter)}
    $proof|ConvertTo-Json -Depth 6|Set-Content (Join-Path $OutputDirectory ('watchdog-'+[guid]::NewGuid().ToString('N')+'.json')) -Encoding UTF8
    $proof|ConvertTo-Json -Depth 6 -Compress
}finally{$owned.Dispose()}
