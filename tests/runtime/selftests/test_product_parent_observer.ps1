param([string]$OutputDirectory="$PSScriptRoot/../../../out/b14-repair-20261007",[string[]]$CaseFilter=@())
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../tools/product_acceptance/product_pilot_watchdog.ps1')
. (Join-Path $PSScriptRoot '../tools/product_acceptance/product_pilot_probe_tail.ps1')
. (Join-Path $PSScriptRoot '../tools/product_acceptance/product_pilot_observer.ps1')
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
$null=[IO.Directory]::CreateDirectory($OutputDirectory)
$FixtureRoot=Join-Path $OutputDirectory ('parent-observer-fixture-'+[guid]::NewGuid().ToString('N'))
$null=[IO.Directory]::CreateDirectory($FixtureRoot)
$Run='a'*32
$NowMs=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
$MaximumBytes=[long]419430400
$results=[Collections.Generic.List[object]]::new()
$failure=$null
function Check([bool]$Condition,[string]$Name){if(!$Condition){throw ('ASSERT_FAILED: '+$Name)}}
function Reject([scriptblock]$Body,[string]$Pattern){
    $caught=$null;try{$null=& $Body}catch{$caught=$_}
    Check ($null -ne $caught -and $caught.Exception.Message -match $Pattern) ('reject/'+$Pattern)
}
function Case([string]$Name,[scriptblock]$Body){
    if($CaseFilter.Count -and $Name -notin $CaseFilter){return}
    $null=& $Body;$results.Add(@{name=$Name;status='PASS'})
}
function Copy-Value($Value){return ($Value|ConvertTo-Json -Depth 10|ConvertFrom-Json)}
function Write-Json([string]$Path,$Value){
    [IO.File]::WriteAllText($Path,($Value|ConvertTo-Json -Depth 10),[Text.UTF8Encoding]::new($false))
}
function Parse-Source([string]$Path){
    $tokens=$null;$errors=$null
    $value=[Management.Automation.Language.Parser]::ParseFile($Path,[ref]$tokens,[ref]$errors)
    Check (!$errors.Count) ('parse/'+$Path);return $value
}
$invoker=Join-Path $PSScriptRoot '../tools/product_acceptance/invoke_product_external.ps1'
$driver=Join-Path $PSScriptRoot '../../uia/product_desktop.ps1'
$invokerAst=Parse-Source $invoker
$driverAst=Parse-Source $driver
$readerAst=$invokerAst.Find({param($n);$n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Read-GpuTraceHeartbeat'},$true)
$saveAst=$driverAst.Find({param($n);$n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Save-Result'},$true)
$calls=@($invokerAst.FindAll({param($n);$n -is [Management.Automation.Language.CommandAst] -and $n.GetCommandName() -eq 'Test-ProductEarlyObservers'},$true))
Check ($null -ne $readerAst -and $null -ne $saveAst -and $calls.Count -eq 1) 'real definitions and sole production call'
Invoke-Expression $readerAst.Extent.Text
Invoke-Expression $saveAst.Extent.Text
$script:ActualGpuReader=(Get-Item Function:Read-GpuTraceHeartbeat).ScriptBlock
$script:ActualObserverCall=[scriptblock]::Create($calls[0].Extent.Text)
$script:GpuReadHook=$null
function Read-GpuTraceHeartbeat([string]$Path){
    $value=& $script:ActualGpuReader $Path
    if($script:GpuReadHook){& $script:GpuReadHook}
    return $value
}
function New-Clock([double]$Elapsed){
    $clock=[pscustomobject]@{Elapsed=[TimeSpan]::FromSeconds($Elapsed);restarts=0}
    $clock|Add-Member ScriptMethod Restart {$this.restarts++;$this.Elapsed=[TimeSpan]::Zero}
    return $clock
}
function New-Child([string]$Name){
    $child=[pscustomobject]@{name=$Name;HasExited=$false;ExitCode=0;refreshes=0;PublishOnRefresh=$false}
    $child|Add-Member ScriptMethod Refresh {
        $this.refreshes++
        if($this.PublishOnRefresh){Publish-Completion;$this.HasExited=$true;$this.PublishOnRefresh=$false}
    }
    return $child
}
function New-Health {
    return [pscustomobject]@{utc_ms=$NowMs;target_pid=0;maximum_bytes=$MaximumBytes;
        events_lost=0;realtime_buffers_lost=0;query_error=0}
}
function New-Fixture {
    $script:CaseRoot=Join-Path $FixtureRoot ([guid]::NewGuid().ToString('N'))
    foreach($directory in @('uia','gpu','checkpoint-archive')){$null=[IO.Directory]::CreateDirectory((Join-Path $script:CaseRoot $directory))}
    $script:FixtureIdentity=[pscustomobject]@{run_id=$Run;pid=4321;
        start_ticks=[DateTimeOffset]::FromUnixTimeMilliseconds($NowMs-20000).UtcDateTime.Ticks;
        executable='C:\fixture\RelWithDebInfo\Cohavora.exe'}
    $script:Archive=New-Child 'archive';$script:Diagnostic=New-Child 'diagnostic';$script:Gpu=New-Child 'gpu'
    $script:GpuClock=New-Clock 0;$script:GpuReadHook=$null;$script:FixtureMode='Formal'
    Write-Json (Join-Path $script:CaseRoot 'gpu/heartbeat.json') (New-Health)
}
function Publish-Completion([string]$Verdict='',[string]$Reason='fixture_only') {
    $OutputDirectory=Join-Path $script:CaseRoot 'uia'
    $Cycles=100;$MinimumSeconds=28800;$DesktopInputPolicy='ObserveOnly';$Retest=$false;$RetestSmoke=$false
    $started=[DateTime]::UtcNow.AddHours(-8)
    $script:runId=$Run;$script:completed=100;$script:productFirstLive=Copy-Value $script:FixtureIdentity
    $script:productLiveElapsedBeforeClose=28800.0
    Write-Json (Join-Path $OutputDirectory 'product-identity.json') $script:FixtureIdentity
    Write-Json (Join-Path $OutputDirectory 'process-exit.json') ([ordered]@{run_id=$Run;pid=4321;exit_code=0})
    Write-Json (Join-Path $script:CaseRoot 'diagnostic-watcher-result.json') ([ordered]@{run_id=$Run;status='COMPLETE'})
    [IO.File]::WriteAllText((Join-Path $script:CaseRoot 'checkpoint-archive/collector.jsonl'),
        ((@{run_id=$Run;status='COMPLETE';event='collector.stopped'}|ConvertTo-Json -Compress)+"`n"),[Text.UTF8Encoding]::new($false))
    if(!$Verdict){$Verdict=if($script:FixtureMode -eq 'Formal'){'UIA_COMPLETE'}else{'PILOT_COMPLETE'}}
    Save-Result $Verdict $Reason
}
function Invoke-ActualObserver {
    $Root=$script:CaseRoot;$run=$Run;$Mode=$script:FixtureMode;$cycles=100;$seconds=28800
    $archive=$script:Archive;$diagnostic=$script:Diagnostic;$gpuTrace=$script:Gpu
    $gpuDirectory=Join-Path $Root 'gpu';$gpuHeartbeatClock=$script:GpuClock;$identity=$null
    $plan=[pscustomobject]@{gpu_etw_observer=[pscustomobject]@{maximum_bytes=$MaximumBytes}}
    return (& $script:ActualObserverCall)
}
try {
    Case 'actual_atomic_save_result_create_replace_and_lazy_terminal_identity' {
        New-Fixture
        Check (!(Test-ProductUiaCompletion $script:CaseRoot $Run 'Formal' 100 28800)) 'no result does not require not-yet-published identity'
        Publish-Completion 'FAIL'
        Reject {Invoke-ActualObserver} 'UIA_TERMINAL_RESULT_INVALID'
        Publish-Completion
        Check (Invoke-ActualObserver) 'actual Save-Result replacement accepted'
        Check (!(Test-Path -LiteralPath (Join-Path $script:CaseRoot 'uia/uia-result.json.tmp'))) 'atomic publication leaves no temporary'
        $saved=Read-ProductObserverSnapshot (Join-Path $script:CaseRoot 'uia/uia-result.json')
        Check ($saved.verdict -eq 'UIA_COMPLETE' -and $saved.product_first_live.pid -eq 4321) 'actual driver payload'
        New-Fixture;$script:FixtureMode='Pilot';Publish-Completion
        Check (Invoke-ActualObserver) 'Pilot exact completion supported'
    }
    Case 'actual_atomic_save_result_unicode_roundtrips_legacy_powershell_reader' {
        New-Fixture
        # Keep this PS5.1 source ASCII while exercising non-ASCII result data.
        $han=[string][char]0x5171+[char]0x4eab+[char]0x5931+[char]0x8d25
        $reason=$han+"`r`nScriptStackTrace:`r`n"+$han+' C:\tests\'+$han+'\product_desktop.ps1: 123'
        Publish-Completion 'FAIL' $reason
        $path=Join-Path $script:CaseRoot 'uia/uia-result.json'
        $bytes=[IO.File]::ReadAllBytes($path)
        Check ($bytes.Length -gt 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) 'UTF8 BOM matches legacy Set-Content UTF8 format'
        $saved=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json -ErrorAction Stop
        Check ($saved.reason -ceq $reason -and $saved.verdict -ceq 'FAIL') 'legacy default reader preserves Chinese and CRLF'
        $replacement=$reason+"`r`n"+$han
        Publish-Completion 'UIA_COMPLETE' $replacement
        $saved=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json -ErrorAction Stop
        Check ($saved.reason -ceq $replacement -and $saved.verdict -ceq 'UIA_COMPLETE') 'atomic replacement preserves Unicode result'
        Check (Invoke-ActualObserver) 'strict UTF8 observer accepts BOM result'
        Check (!(Test-Path -LiteralPath ($path+'.tmp'))) 'replacement leaves no temporary'
    }
    Case 'result_published_during_real_gpu_read_and_diagnostic_exit' {
        New-Fixture
        $script:GpuReadHook={Publish-Completion;$script:Diagnostic.HasExited=$true}
        Check (Invoke-ActualObserver) 'exit is rechecked after GPU reader publishes result'
        Check ($script:GpuClock.restarts -eq 1 -and $script:Diagnostic.refreshes -eq 1) 'real reader and child observation executed'
    }
    Case 'result_published_inside_diagnostic_refresh' {
        New-Fixture;$script:Diagnostic.PublishOnRefresh=$true
        Check (Invoke-ActualObserver) 'Refresh exit publication accepted in same iteration'
    }
    Case 'early_exit_without_result_stays_rejected' {
        New-Fixture;$script:Diagnostic.HasExited=$true
        Reject {Invoke-ActualObserver} 'COLLECTOR_STOPPED_BEFORE_UIA_COMPLETION'
        New-Fixture;$script:Archive.HasExited=$true
        Reject {Invoke-ActualObserver} 'COLLECTOR_STOPPED_BEFORE_UIA_COMPLETION'
        New-Fixture;$script:Gpu.HasExited=$true
        Reject {Invoke-ActualObserver} 'GPU_ETW_COLLECTOR_STOPPED_EARLY'
    }
    Case 'wrong_run_failed_partial_and_wrong_counts_are_not_completion' {
        foreach($pair in @(@('run_id',('b'*32)),@('verdict','FAIL'),@('verdict','PILOT_COMPLETE'),
            @('cycles_requested',99),@('cycles_completed',99),@('minimum_seconds',28799),@('cycles_completed','100'))){
            New-Fixture;Publish-Completion
            $path=Join-Path $script:CaseRoot 'uia/uia-result.json';$bad=Read-ProductObserverSnapshot $path;$bad.($pair[0])=$pair[1];Write-Json $path $bad
            Reject {Invoke-ActualObserver} 'UIA_TERMINAL_RESULT_INVALID'
        }
        New-Fixture;Publish-Completion
        [IO.File]::WriteAllText((Join-Path $script:CaseRoot 'uia/uia-result.json'),'{"run_id":')
        Reject {Invoke-ActualObserver} '.'
        New-Fixture;Publish-Completion
        $path=Join-Path $script:CaseRoot 'uia/uia-result.json';$bad=Read-ProductObserverSnapshot $path;$bad.PSObject.Properties.Remove('product_first_live');Write-Json $path $bad
        Reject {Invoke-ActualObserver} 'UIA_TERMINAL_EXIT_INVALID'
    }
    Case 'terminal_identity_or_nonzero_product_exit_stays_rejected' {
        foreach($pair in @(@('run_id',('b'*32)),@('pid',4322),@('exit_code',1),@('exit_code','0'))){
            New-Fixture;Publish-Completion
            $path=Join-Path $script:CaseRoot 'uia/process-exit.json';$bad=Read-ProductObserverSnapshot $path;$bad.($pair[0])=$pair[1];Write-Json $path $bad
            Reject {Invoke-ActualObserver} 'UIA_TERMINAL_EXIT_INVALID'
        }
        foreach($field in @('start_ticks','executable')){
            New-Fixture;Publish-Completion
            $path=Join-Path $script:CaseRoot 'uia/product-identity.json';$bad=Read-ProductObserverSnapshot $path
            if($field -eq 'start_ticks'){$bad.start_ticks++}else{$bad.executable+='different'}
            Write-Json $path $bad;Reject {Invoke-ActualObserver} 'UIA_TERMINAL_EXIT_INVALID'
        }
    }
    Case 'collector_nonzero_exit_and_missing_or_failed_receipts_rejected' {
        foreach($name in @('Archive','Diagnostic')){
            New-Fixture;Publish-Completion;$child=Get-Variable -Name $name -Scope Script -ValueOnly;$child.HasExited=$true;$child.ExitCode=1
            Reject {Invoke-ActualObserver} 'COLLECTOR_TERMINAL_EXIT_FAILED'
        }
        foreach($receipt in @('diagnostic-watcher-result.json','checkpoint-archive/collector.jsonl')){
            New-Fixture;Publish-Completion;$script:Archive.HasExited=$true;$script:Diagnostic.HasExited=$true
            [IO.File]::Delete((Join-Path $script:CaseRoot $receipt));Reject {Invoke-ActualObserver} '.'
        }
        foreach($status in @('INTERRUPTED','FAILED')){
            New-Fixture;Publish-Completion;$script:Diagnostic.HasExited=$true
            Write-Json (Join-Path $script:CaseRoot 'diagnostic-watcher-result.json') @{run_id=$Run;status=$status}
            Reject {Invoke-ActualObserver} 'DIAGNOSTIC_TERMINAL_RECEIPT_INVALID'
        }
        New-Fixture;Publish-Completion;$script:Archive.HasExited=$true
        [IO.File]::WriteAllText((Join-Path $script:CaseRoot 'checkpoint-archive/collector.jsonl'),
            ((@{run_id=$Run;status='COMPLETE';event='collector.started'}|ConvertTo-Json -Compress)+"`n"))
        Reject {Invoke-ActualObserver} 'ARCHIVE_TERMINAL_RECEIPT_INVALID'
        New-Fixture;Publish-Completion;$script:Archive.HasExited=$true;$script:Diagnostic.HasExited=$true
        Check (Invoke-ActualObserver) 'both zero exits and exact COMPLETE receipts accepted'
    }
    Case 'first_gpu_heartbeat_missing_15_second_boundary_via_actual_call' {
        New-Fixture;[IO.File]::Delete((Join-Path $script:CaseRoot 'gpu/heartbeat.json'))
        $script:GpuClock=New-Clock 15
        Check (!(Invoke-ActualObserver)) 'first missing heartbeat at bound continues'
        Check ($script:GpuClock.restarts -eq 0) 'absence never resets discovery clock'
        $script:GpuClock=New-Clock 15.001
        Reject {Invoke-ActualObserver} 'GPU_ETW_HEARTBEAT_MISSING'
    }
    Case 'fixed_clock_gpu_freshness_future_loss_and_numeric_schema' {
        $health=New-Health;Assert-ProductGpuHeartbeat $health 0 $NowMs $null $MaximumBytes
        $health.utc_ms=$NowMs-15000;Assert-ProductGpuHeartbeat $health 0 $NowMs $null $MaximumBytes
        foreach($utc in @(($NowMs-15001),($NowMs+1))){$bad=New-Health;$bad.utc_ms=$utc;Reject {Assert-ProductGpuHeartbeat $bad 0 $NowMs $null $MaximumBytes} 'GPU_ETW_STALE_OR_LOSSY'}
        foreach($field in @('events_lost','realtime_buffers_lost','query_error')){
            $bad=New-Health;$bad.$field=1;Reject {Assert-ProductGpuHeartbeat $bad 0 $NowMs $null $MaximumBytes} 'GPU_ETW_STALE_OR_LOSSY'
        }
        foreach($field in @('utc_ms','target_pid','maximum_bytes','events_lost','realtime_buffers_lost','query_error')){
            foreach($value in @($null,$true,-1,'0')){
                $bad=New-Health;if($null -eq $value){$bad.PSObject.Properties.Remove($field)}else{$bad.$field=$value}
                Reject {Assert-ProductGpuHeartbeat $bad 0 $NowMs $null $MaximumBytes} 'GPU_ETW_HEARTBEAT_INVALID'
            }
        }
    }
    Case 'fixed_clock_gpu_target_binding_and_maximum_bytes' {
        New-Fixture
        $health=New-Health;$health.target_pid=$script:FixtureIdentity.pid
        Assert-ProductGpuHeartbeat $health 0 $NowMs $script:FixtureIdentity $MaximumBytes
        $health.target_pid++;Reject {Assert-ProductGpuHeartbeat $health 0 $NowMs $script:FixtureIdentity $MaximumBytes} 'GPU_ETW_HEARTBEAT_IDENTITY_INVALID'
        $health=New-Health;$health.maximum_bytes++;Reject {Assert-ProductGpuHeartbeat $health 0 $NowMs $null $MaximumBytes} 'GPU_ETW_HEARTBEAT_IDENTITY_INVALID'
        $health=New-Health;$identity=Copy-Value $script:FixtureIdentity
        $identity.start_ticks=[DateTimeOffset]::FromUnixTimeMilliseconds($NowMs-15000).UtcDateTime.Ticks
        Assert-ProductGpuHeartbeat $health 0 $NowMs $identity $MaximumBytes
        $identity.start_ticks=[DateTimeOffset]::FromUnixTimeMilliseconds($NowMs-15001).UtcDateTime.Ticks
        Reject {Assert-ProductGpuHeartbeat $health 0 $NowMs $identity $MaximumBytes} 'GPU_ETW_HEARTBEAT_IDENTITY_INVALID'
    }
    Case 'real_gpu_reader_preserves_schema_and_rejects_partial_json' {
        New-Fixture;$path=Join-Path $script:CaseRoot 'gpu/heartbeat.json'
        $health=& $script:ActualGpuReader $path
        Check ($health.maximum_bytes -eq $MaximumBytes -and $health.events_lost -eq 0) 'actual reader payload'
        [IO.File]::WriteAllText($path,'{"utc_ms":')
        Reject {Invoke-ActualObserver} '.'
        Write-Json $path @{utc_ms=$NowMs}
        Reject {Invoke-ActualObserver} 'GPU_ETW_HEARTBEAT_INVALID'
    }
}catch{$failure=$_.Exception.Message}
$sources=@('../tools/product_acceptance/product_pilot_observer.ps1','../tools/product_acceptance/product_pilot_watchdog.ps1',
    '../tools/product_acceptance/product_pilot_probe_tail.ps1','../tools/product_acceptance/invoke_product_external.ps1','../../uia/product_desktop.ps1')
$proof=[ordered]@{schema=1;utc=[DateTimeOffset]::UtcNow.ToString('o');verdict=$(if($failure){'FAIL'}else{'PASS'});
    cases_passed=$results.Count;cases=@($results.ToArray());failure=$failure;fixed_now_ms=$NowMs;clock_fixture='explicit milliseconds and elapsed Timespan; no sleeps';
    actual_invoker_call=$calls[0].Extent.Text;actual_save_result=$true;actual_gpu_reader=$true;product_started=$false;remote_called=$false;runtime_acceptance_credit=0;
    sources=@($sources|ForEach-Object {$path=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot $_));@{path=$path;sha256=(Get-FileHash $path).Hash.ToLowerInvariant()}});
    selftest_sha256=(Get-FileHash $PSCommandPath).Hash.ToLowerInvariant();case_filter=@($CaseFilter);fixture_root=$FixtureRoot}
$report=Join-Path $OutputDirectory ('parent-observer-'+[guid]::NewGuid().ToString('N')+'.json')
$proof|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $report -Encoding UTF8
$proof|ConvertTo-Json -Depth 8 -Compress
Write-Output ('EVIDENCE: '+$report)
if($failure){throw $failure}
