param(
    [Parameter(Mandatory=$true)][string]$Root,
    [Parameter(Mandatory=$true)][string]$PreparedDirectory,
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$AudioCollector,
    [Parameter(Mandatory=$true)][string]$ExpectedProductSha256,
    [Parameter(Mandatory=$true)][string]$ExpectedUiaSha256,
    [Parameter(Mandatory=$true)][int]$DesktopSessionId
)
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path (Join-Path $PSScriptRoot '../../../..')).Path
Set-Location $workspace
. (Join-Path $PSScriptRoot 'product_pilot_probe_tail.ps1')
. (Join-Path $PSScriptRoot 'product_pilot_watchdog.ps1')
. (Join-Path $workspace 'tests/uia/product_desktop_evidence.ps1')
$python='C:\python3\python.exe'
$Executable=(Resolve-Path -LiteralPath $Executable).Path
$AudioCollector=(Resolve-Path -LiteralPath $AudioCollector).Path
$PreparedDirectory=(Resolve-Path -LiteralPath $PreparedDirectory).Path
foreach($binary in @($Executable,$AudioCollector)) {
    if((Split-Path (Split-Path $binary) -Leaf) -cne 'RelWithDebInfo'){throw 'RELWITHDEBINFO_REQUIRED'}
}
if((Get-FileHash $Executable -Algorithm SHA256).Hash -ne $ExpectedProductSha256){throw 'PRODUCT_FINGERPRINT_CHANGED'}
if((Get-FileHash "$workspace/tests/uia/product_desktop.ps1" -Algorithm SHA256).Hash -ne $ExpectedUiaSha256){throw 'UIA_FINGERPRINT_CHANGED'}
if(Test-Path -LiteralPath $Root){throw 'NEW_EVIDENCE_DIRECTORY_REQUIRED'}
if(@(Get-Process -Name Cohavora -ErrorAction SilentlyContinue).Count){throw 'EXISTING_PRODUCT_PROCESS_PRESENT'}
$setup=Get-Content "$PreparedDirectory/setup.json" -Raw | ConvertFrom-Json
$target=Get-Content "$PSScriptRoot/product_aliyun_target.json" -Raw | ConvertFrom-Json
if($setup.status -cne 'PREPARED' -or $setup.meeting_id -cnotmatch '^[0-9]{9}$' -or
    $setup.service_url.TrimEnd('/') -cne $target.service_url){throw 'PREPARED_FIXTURE_INVALID'}
$baseline=Assert-DedicatedDesktop $DesktopSessionId -InputPolicy diagnostic
$run=[guid]::NewGuid().ToString('N');$maximum=600;$remote=$target.remote_root
$prefix=$run.Substring(0,8);$remoteRun="$remote/pilot-$prefix"
$lock=$null;$uia=$null;$children=@();$remoteLaunchAttempted=$false
$failure=$null;$code=1;$identity=$null;$probeClock=$null;$resourceClock=$null;$uiaForced=$false
$errors=[Collections.Generic.List[string]]::new()
$savedEnvironment=@{}
$environmentNames=@('LIVEKIT_UIA_ACCOUNT','LIVEKIT_UIA_PASSWORD','LIVEKIT_UIA_MEETING_ID','LIVEKIT_UIA_SERVICE_URL',
    'LIVEKIT_UIA_RUN_ID','LIVEKIT_UIA_REMOTE_CONTEXT','LIVEKIT_UIA_LOG_PAIR','LIVEKIT_UIA_PILOT_PROBE',
    'LIVEKIT_UIA_GPU_BUDGET_PROBE','LIVEKIT_UIA_GPU_ETW_DIRECTORY')
foreach($name in $environmentNames){$savedEnvironment[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
function Remote([string]$Command) {
    $response=& $python "$PSScriptRoot/product_aliyun_transport.py" $Command
    if($LASTEXITCODE){throw 'ALIYUN_REMOTE_COMMAND_FAILED'}
    return ($response -join "`n")
}
function Latest([string]$Path) {
    $rows=@(Read-ProductPilotCompleteJsonlTail -Path $Path -Count 1 -MaximumBytes 1048576)
    if($rows.Count){return ($rows[0] | ConvertFrom-Json -ErrorAction Stop)}
    return $null
}
function Terminal {
    if(!(Test-Path "$Root/uia/uia-result.json")){return $false}
    $result=Read-ProductObserverSnapshot "$Root/uia/uia-result.json"
    $owned=Read-ProductObserverSnapshot "$Root/uia/product-identity.json"
    $exit=Read-ProductObserverSnapshot "$Root/uia/process-exit.json"
    if($result.run_id -cne $run -or $result.verdict -cne 'DIAGNOSTIC_UIA_COMPLETE' -or
        $result.cycles_requested -ne 1 -or $result.cycles_completed -ne 1 -or $result.minimum_seconds -ne 0 -or
        $owned.run_id -cne $run -or $owned.executable -cne $Executable -or
        $exit.run_id -cne $run -or $exit.pid -ne $owned.pid -or $exit.exit_code -isnot [int] -or $exit.exit_code -ne 0 -or
        $result.product_first_live.pid -ne $owned.pid -or $result.product_first_live.start_ticks -ne $owned.start_ticks -or
        $result.product_first_live.executable -cne $Executable){throw 'DIAGNOSTIC_TERMINAL_IDENTITY_OR_EXIT_INVALID'}
    return $true
}
try {
    $lock=[IO.File]::Open("$workspace/out/product-acceptance.lock",[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    $null=New-Item -ItemType Directory -Path $Root
    $Root=(Resolve-Path -LiteralPath $Root).Path
    $baseline | ConvertTo-Json | Set-Content "$Root/desktop-baseline.json" -Encoding UTF8
    $policyPath="$PSScriptRoot/product_pilot_scheduler_policy.json"
    $policy=@{sha256=(Get-FileHash $policyPath -Algorithm SHA256).Hash.ToLowerInvariant();policy=(Get-Content $policyPath -Raw|ConvertFrom-Json)}
    $plan=[ordered]@{schema=2;run_id=$run;root=$Root;mode='FirstCycleMediaLogDiagnostic';scope='remote_exit_and_first_cycle_media_log';
        seconds=0;maximum_seconds=$maximum;cycles=1;meeting_id=$setup.meeting_id;configuration='RelWithDebInfo';
        diagnostic_only=$true;release_eligible=$false;qualification_credit=0;runtime_qualification_credit=0;full_media_gpu=$false;formal_started=$false;
        launch_limit=1;desktop_input_policy='diagnostic';share_seconds=0;log_pair_seconds=35;
        requires_context=$true;explicit_microphone_unmute=$true;collector_scheduler_policy=$policy;
        gpu_etw_observer=@{required=$false;status='OUT_OF_SCOPE'};
        server_provider='aliyun';server_instance_id=$target.instance_id;server_cpu=$target.server_cpu;
        server_memory_gib=$target.server_memory_gib;server_bandwidth_mbps=$target.server_bandwidth_mbps;
        collector_media_route=$target.collector_media_route;
        load=@{video_publishers=10;width=160;height=90;fps=5;video_bps_each=40000;video_codec='VP8';audio_bps=24000;simulcast=$false;encryption_mode='off'}}
    $plan|ConvertTo-Json -Depth 8|Set-Content "$Root/plan.json" -Encoding UTF8
    @{kind='first_cycle_media_log';diagnostic_only=$true;release_eligible=$false;qualification_credit=0;run_id=$run}|ConvertTo-Json|Set-Content "$Root/diagnostic-debugger.json" -Encoding UTF8
    Copy-Item "$PSScriptRoot/product_external_limits.json" "$Root/limits.json"
    Copy-Item $policyPath "$Root/collector-scheduler-policy.json"
    $hashes=[ordered]@{}
    $inputs=@($Executable,$AudioCollector,"$workspace/tests/uia/product_desktop.ps1","$workspace/tests/uia/product_desktop_evidence.ps1",$PSCommandPath,
        "$PSScriptRoot/invoke_product_first_cycle_media_log_diagnostic.ps1","$PSScriptRoot/verify_product_first_cycle_media_log.py")+
        @(Get-ChildItem $PSScriptRoot -File | Where-Object {$_.Name -match '^product_(pilot|aliyun|external|meeting|audio)'} | Select-Object -ExpandProperty FullName)
    foreach($inputPath in $inputs){$hashes[$inputPath]=(Get-FileHash $inputPath -Algorithm SHA256).Hash.ToLowerInvariant()}
    $hashes|ConvertTo-Json|Set-Content "$Root/executed-inputs.json" -Encoding UTF8
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot/product_meeting_fixture.ps1" -PreparedDirectory $PreparedDirectory -OutputDirectory $Root -MinimumRemainingSeconds 900
    if($LASTEXITCODE){throw 'MEETING_PREFLIGHT_FAILED'}
    $remoteNames=@('product_pilot_remote.py','product_pilot_load.py','product_pilot_context.py','product_pilot_local_route.py','product_pilot_scheduler.py','product_pilot_scheduler_policy.json','product_aliyun_target.json','product_pilot_timing.py','product_pilot_video_counter.py','product_pilot_audio_reference.py')
    $remoteHashes=Remote ('sha256sum '+(($remoteNames | ForEach-Object {"$remote/$_"}) -join ' '))
    $remoteHashes | Set-Content "$Root/remote-input-hashes.txt" -Encoding UTF8
    foreach($name in $remoteNames){
        $expected=(Get-FileHash "$PSScriptRoot/$name" -Algorithm SHA256).Hash.ToLowerInvariant()
        if(!($remoteHashes -split "`n" | Where-Object {$_ -ceq "$expected  $remote/$name"})){throw 'REMOTE_COLLECTOR_INPUT_MISMATCH'}
    }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot/product_audio_devices.ps1" -Output "$Root/audio-devices.json" -RunId $run
    if($LASTEXITCODE){throw 'AUDIO_DEVICE_OBSERVER_FAILED'}
    $budget=Write-ProductRunBudget "$Root/run-clock.json" $run $maximum
    $remoteLaunchAttempted=$true
    $null=Remote "nohup $remote/venv/bin/python $remote/product_pilot_local_route.py --target-config $remote/product_aliyun_target.json --run-id $run --result $remote/pilot-$prefix-route.json --scheduler-policy $remote/product_pilot_scheduler_policy.json -- $remote/venv/bin/python $remote/product_pilot_remote.py --dependencies $remote/collector-python --config $($target.livekit_config) --output $remoteRun --run-id $run --room $($setup.meeting_id) --seconds $maximum --scheduler-policy $remote/product_pilot_scheduler_policy.json > $remote/pilot-$prefix.stderr 2>&1 < /dev/null &"
    $ready=$false
    for($attempt=0;$attempt -lt 30;$attempt++){
        if((Get-ProductRunBudgetElapsed $budget) -gt $maximum){throw 'RUN_WATCHDOG_TIMEOUT'}
        $response=Remote "if test -f $remoteRun/ready.json; then cat $remoteRun/ready.json; else echo null; fi"
        if(($response|ConvertFrom-Json).run_id -ceq $run){$ready=$true;break}
        Start-Sleep -Seconds 1
    }
    if(!$ready){throw 'REMOTE_READY_TIMEOUT'}
    $secret=(Get-Content "$PreparedDirectory/password.dpapi" -Raw).Trim() | ConvertTo-SecureString
    $env:LIVEKIT_UIA_ACCOUNT=$setup.account;$env:LIVEKIT_UIA_PASSWORD=[Net.NetworkCredential]::new('', $secret).Password
    $secret=$null
    $env:LIVEKIT_UIA_MEETING_ID=$setup.meeting_id;$env:LIVEKIT_UIA_SERVICE_URL=$setup.service_url
    $env:LIVEKIT_UIA_RUN_ID=$run;$env:LIVEKIT_UIA_REMOTE_CONTEXT='1';$env:LIVEKIT_UIA_LOG_PAIR='1'
    $env:LIVEKIT_UIA_PILOT_PROBE="$Root/process-probe.jsonl";$env:LIVEKIT_UIA_GPU_BUDGET_PROBE='1'
    Remove-Item Env:LIVEKIT_UIA_GPU_ETW_DIRECTORY -ErrorAction SilentlyContinue
    $resource=Start-Process powershell.exe -WindowStyle Hidden -PassThru -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',"$PSScriptRoot/product_pilot_resources.ps1",'-UiaDirectory',"$Root/uia",'-Destination',"$Root/external-resources.jsonl",'-RunId',$run,'-MaximumSeconds',$maximum,'-RunBudgetPath',"$Root/run-clock.json",'-AudioCollector',$AudioCollector) -RedirectStandardOutput "$Root/resources.stdout" -RedirectStandardError "$Root/resources.stderr"
    $children+=$resource;$null=$resource.Handle
    $archive=Start-Process $python -WindowStyle Hidden -PassThru -ArgumentList @("$PSScriptRoot/product_pilot_checkpoints.py",'--probe',"$Root/process-probe.jsonl",'--result',"$Root/uia/uia-result.json",'--output',"$Root/checkpoint-archive",'--run-id',$run,'--seconds',$maximum,'--run-budget',"$Root/run-clock.json",'--maximum-bytes',536870912) -RedirectStandardOutput "$Root/archive.stdout" -RedirectStandardError "$Root/archive.stderr"
    $children+=$archive;$null=$archive.Handle
    $diagnostic=Start-Process $python -WindowStyle Hidden -PassThru -ArgumentList @("$PSScriptRoot/product_pilot_diagnostics.py",'--root',$Root,'--watch','--seconds',$maximum,'--run-budget',"$Root/run-clock.json") -RedirectStandardOutput "$Root/diagnostic.stdout" -RedirectStandardError "$Root/diagnostic.stderr"
    $children+=$diagnostic;$null=$diagnostic.Handle
    $launchClock=[Diagnostics.Stopwatch]::StartNew()
    $uia=Start-Process powershell.exe -WindowStyle Hidden -PassThru -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',"$PSScriptRoot/invoke_product_first_cycle_media_log_diagnostic.ps1",'-Executable',$Executable,'-OutputDirectory',"$Root/uia",'-ExpectedProductSha256',$ExpectedProductSha256,'-ExpectedUiaSha256',$ExpectedUiaSha256,'-RunId',$run,'-DesktopSessionId',$DesktopSessionId) -RedirectStandardOutput "$Root/uia.stdout" -RedirectStandardError "$Root/uia.stderr"
    $null=$uia.Handle
    Remove-Item Env:LIVEKIT_UIA_PASSWORD -ErrorAction SilentlyContinue
    @{run_id=$run;uia_pid=$uia.Id;resource_pid=$resource.Id;archive_pid=$archive.Id;diagnostic_pid=$diagnostic.Id}|ConvertTo-Json|Set-Content "$Root/collectors.json" -Encoding UTF8
    while(!$uia.WaitForExit(1000)){
        Get-DesktopEvidenceState|ConvertTo-Json -Compress|Add-Content "$Root/desktop-observations.jsonl" -Encoding UTF8
        $null=Assert-DedicatedDesktop $DesktopSessionId $baseline diagnostic
        if((Get-ProductRunBudgetElapsed $budget) -gt $maximum){throw 'RUN_WATCHDOG_TIMEOUT'}
        if((Get-PSDrive -Name ([IO.Path]::GetPathRoot($Root).Substring(0,1))).Free -lt 5GB){throw 'EVIDENCE_DISK_RESERVE_EXHAUSTED'}
        if(Terminal){continue}
        $last=$null;if(Test-Path "$Root/uia/uia-actions.jsonl"){$last=Latest "$Root/uia/uia-actions.jsonl"}
        if(!$identity -and (Test-Path "$Root/uia/product-identity.json")){$identity=Read-ProductObserverSnapshot "$Root/uia/product-identity.json";$probeClock=[Diagnostics.Stopwatch]::StartNew()}
        $now=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
        Assert-ProductActionProgress $last $run $launchClock.Elapsed.TotalSeconds $now $null $identity 1 0 $null
        $exiting=($last -and $last.action -ceq 'process_exit')
        foreach($observer in @($archive,$diagnostic)){
            $observer.Refresh()
            if($observer.HasExited -and !(Terminal)){throw 'EVIDENCE_COLLECTOR_STOPPED_BEFORE_UIA_COMPLETION'}
            if($observer.HasExited -and $observer.ExitCode -ne 0){throw 'EVIDENCE_COLLECTOR_EXIT_FAILED'}
        }
        if($last -and !$resourceClock){$resourceClock=[Diagnostics.Stopwatch]::StartNew()}
        if($resource.HasExited -and $last -and !$exiting){throw 'RESOURCE_COLLECTOR_STOPPED_EARLY'}
        if($resourceClock){
            $health=$null;if(Test-Path "$Root/external-resources.jsonl"){$health=Latest "$Root/external-resources.jsonl"}
            Assert-ProductResourceHealth $health $run $identity $resourceClock.Elapsed.TotalSeconds ([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()) $exiting
            if($health){$resourceClock.Restart()}
        }
        if($probeClock){
            $health=$null
            if(Test-Path "$Root/process-probe.jsonl"){$health=Read-ProductPilotProbeTail -Path "$Root/process-probe.jsonl" -Count 2 -MaximumBytes 1048576|ForEach-Object {try{$_|ConvertFrom-Json}catch{}}|Select-Object -Last 1}
            Assert-ProductNativeHealth $health $run $identity $probeClock.Elapsed.TotalSeconds ([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()) $exiting
            if($health){$probeClock.Restart()}
        }
    }
    if((Get-ProductRunBudgetElapsed $budget) -gt $maximum){throw 'RUN_WATCHDOG_TIMEOUT'}
    $uia.Refresh()
    if($uia.ExitCode -ne 0 -or !(Terminal)){throw 'DIAGNOSTIC_UIA_FAILED'}
    $code=0
} catch {
    $failure=$_.Exception.Message
    if(Test-Path -LiteralPath $Root){
        @{run_id=$run;reason=$failure}|ConvertTo-Json|Set-Content "$Root/collector-stop.json" -Encoding UTF8
        $product=$null
        try {
            if(Test-Path "$Root/uia/product-identity.json"){
                $owned=Read-ProductObserverSnapshot "$Root/uia/product-identity.json"
                $product=Get-Process -Id $owned.pid -ErrorAction SilentlyContinue
                if($product -and $owned.run_id -ceq $run -and $owned.executable -ceq $Executable -and
                    $product.Path -eq $Executable -and $product.StartTime.ToUniversalTime().Ticks -eq $owned.start_ticks){$product.Kill();$null=$product.WaitForExit(5000)}
            }
        }catch{$errors.Add('PRODUCT_FAILURE_CLEANUP_FAILED')}finally{if($product){$product.Dispose()}}
        if($uia){try{if(!$uia.HasExited){$uiaForced=$true;$uia.Kill();$null=$uia.WaitForExit(5000)}}catch{$errors.Add('UIA_FAILURE_CLEANUP_FAILED')}}
    }
} finally {
    foreach($name in $environmentNames){[Environment]::SetEnvironmentVariable($name,$savedEnvironment[$name],'Process')}
    if($remoteLaunchAttempted){
        try{& $python "$PSScriptRoot/product_pilot_shutdown.py" --root $Root --run-id $run --launch-attempted;if($LASTEXITCODE){throw 'REMOTE_SHUTDOWN_OR_EVIDENCE_FAILED'}}
        catch{$code=1;$errors.Add('REMOTE_SHUTDOWN_OR_EVIDENCE_FAILED')}
    }
    $cleanup=Invoke-ProductCollectorCleanup $children $null '' '' ''
    if(!$cleanup.passed){$code=1}
    foreach($item in $cleanup.errors){$errors.Add($item)}
    if(Test-Path -LiteralPath $Root){
        @{run_id=$run;collectors=$cleanup.collectors;cleanup_errors=@($errors.ToArray())}|ConvertTo-Json -Depth 5|Set-Content "$Root/collector-exits.json" -Encoding UTF8
        @{run_id=$run;exit_code=$code;failure=$failure;diagnostic_only=$true;qualification_credit=0;formal_started=$false;
            uia_pid=$(if($uia){$uia.Id}else{$null});uia_exit_code=$(if($uia -and $uia.HasExited){$uia.ExitCode}else{$null});uia_forced_stop=$uiaForced}|ConvertTo-Json|Set-Content "$Root/controller-result.json" -Encoding UTF8
    }
    if($lock){$lock.Dispose()}
}
if(Test-Path -LiteralPath $Root){
    try{& $python "$PSScriptRoot/verify_product_first_cycle_media_log.py" --root $Root --review-output "$Root/scoped-review";if($LASTEXITCODE){$code=1}}
    catch{$code=1;$errors.Add('SCOPED_REVIEW_FAILED')}
    @{run_id=$run;exit_code=$code;verdict=$(if($code -eq 0){'SCOPED_DIAGNOSTIC_PASS'}else{'FAIL'});diagnostic_only=$true;qualification_credit=0;formal_started=$false;cleanup_errors=@($errors.ToArray())}|ConvertTo-Json -Depth 5|Set-Content "$Root/runner-exit.json" -Encoding UTF8
}
Write-Output "RUN_ROOT=$Root"
exit $code
