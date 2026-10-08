param(
    [Parameter(Mandatory=$true)][string]$Root,
    [Parameter(Mandatory=$true)][string]$PreparedDirectory,
    [string]$Executable='out/build/windows-vs2026-dev/src/app/Debug/Cohavora.exe',
    [string]$AudioCollector='out/build/windows-vs2026-dev/Debug/product_audio_loopback.exe',
    [string]$GpuTraceTool='out/build/product-gpu-budget/RelWithDebInfo/product_gpu_trace.exe',
    [ValidateSet('Pilot','Formal')][string]$Mode='Pilot',
    [string]$ReleaseGate='',
    [int]$DedicatedDesktopSessionId=0,
    [ValidateSet('strict','diagnostic')][string]$DesktopInputPolicy='strict',
    [switch]$HeapDiagnostic,
    [switch]$HeapSnapshotDiagnostic,
    [switch]$HeapDiagnosticPersistentUia,
    [switch]$HeapDiagnosticNoShare,
    [switch]$HeapCheckOnly,
    [switch]$HeapDiagnosticNoExport,
    [switch]$HeapPageCheck,
    [switch]$CrashDiagnostic,
    [ValidateRange(2,10)][int]$DiagnosticCycles=2,
    [switch]$IsolateUiaCycles,
    [switch]$AudioTimingDiagnostic,
    [switch]$GpuBudgetDiagnostic,
    [ValidateRange(-10,0)][int]$DiagnosticReceiverNice=0,
    [switch]$DiagnosticNoRealtime
)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'product_pilot_probe_tail.ps1')
. (Join-Path $PSScriptRoot 'product_pilot_watchdog.ps1')
. (Join-Path $PSScriptRoot 'product_pilot_admission.ps1')
. (Join-Path $PSScriptRoot 'product_pilot_observer.ps1')
function Read-GpuTraceHeartbeat([string]$Path) {
    # The collector atomically replaces this file. Allow its DELETE handle as
    # well as readers/writers, and retry only transient sharing/lock conflicts.
    $deadline=[Diagnostics.Stopwatch]::StartNew()
    while($true) {
        $stream=$null;$reader=$null
        try {
            $share=[IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete
            $stream=[IO.FileStream]::new($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,$share)
            $reader=[IO.StreamReader]::new($stream,[Text.UTF8Encoding]::new($false),$true)
            return ($reader.ReadToEnd() | ConvertFrom-Json)
        } catch {
            $cause=$_.Exception.GetBaseException()
            $code=$cause.HResult -band 65535
            if($cause -isnot [IO.IOException] -or $code -notin @(32,33) -or
                $deadline.ElapsedMilliseconds -ge 500) {throw}
        } finally {
            if($reader){$reader.Dispose()}elseif($stream){$stream.Dispose()}
        }
        Start-Sleep -Milliseconds 10
    }
}
function Read-LatestCompleteUiaAction([string]$Path) {
    $lines=@(Read-ProductPilotCompleteJsonlTail -Path $Path -Count 1 -MaximumBytes 1048576)
    if(!$lines.Count){return $null}
    $record=ConvertFrom-Json -InputObject $lines[0] -ErrorAction Stop
    if(!$record -or $record -is [Array] -or $record.utc -isnot [string]){throw 'UIA_ACTION_RECORD_INVALID'}
    return $record
}
if($DiagnosticReceiverNice -ne 0 -and !$AudioTimingDiagnostic){throw 'RECEIVER_PRIORITY_REQUIRES_TIMING_DIAGNOSTIC'}
if($DiagnosticNoRealtime -and (!$AudioTimingDiagnostic -or $DiagnosticReceiverNice -ne 0)){throw 'NO_REALTIME_REQUIRES_TIMING_DIAGNOSTIC_NICE_ZERO'}
$workspace=(Resolve-Path (Join-Path $PSScriptRoot '../../../..')).Path
Set-Location $workspace
$Executable=(Resolve-Path -LiteralPath $Executable).Path
$AudioCollector=(Resolve-Path -LiteralPath $AudioCollector).Path
$GpuTraceTool=(Resolve-Path -LiteralPath $GpuTraceTool).Path
if((Split-Path (Split-Path $GpuTraceTool) -Leaf) -ne 'RelWithDebInfo'){throw 'GPU_TRACE_RELWITHDEBINFO_REQUIRED'}
$evidenceDrive=[IO.Path]::GetPathRoot([IO.Path]::GetFullPath($Root)).Substring(0,1)
$PreparedDirectory=(Resolve-Path -LiteralPath $PreparedDirectory).Path
$setup=Get-Content "$PreparedDirectory/setup.json" -Raw | ConvertFrom-Json
if ($setup.status -ne 'PREPARED' -or $setup.meeting_id -cnotmatch '^[0-9]{9}$') {throw 'PREPARED_MEETING_INVALID'}
$target=Get-Content (Join-Path $PSScriptRoot 'product_aliyun_target.json') -Raw | ConvertFrom-Json
if ($setup.service_url.TrimEnd('/') -ne $target.service_url) {throw 'TEST_SERVICE_UNEXPECTED'}
. (Join-Path $workspace 'tests/uia/product_desktop_evidence.ps1')
if ($Mode -eq 'Formal' -or $DedicatedDesktopSessionId -gt 0) {
    $desktopBaseline=Assert-DedicatedDesktop $DedicatedDesktopSessionId -InputPolicy $DesktopInputPolicy
}
function Invoke-TestRemote([string]$Command) {
    $output=& python "$PSScriptRoot/product_aliyun_transport.py" $Command
    if ($LASTEXITCODE) {throw 'ALIYUN_REMOTE_COMMAND_FAILED'}
    return $output
}
$meetingId=$setup.meeting_id
if(Test-Path -LiteralPath $Root){throw 'Run directory must be new'}
if($HeapDiagnostic -and $Mode -ne 'Pilot'){throw 'HEAP_DIAGNOSTIC_REQUIRES_PILOT'}
if($HeapSnapshotDiagnostic -and (!$HeapDiagnostic -or $HeapCheckOnly -or $HeapPageCheck -or $CrashDiagnostic)){throw 'HEAP_SNAPSHOT_REQUIRES_EXCLUSIVE_HEAP_DIAGNOSTIC'}
if($HeapDiagnosticNoShare -and !$HeapDiagnostic){throw 'NO_SHARE_REQUIRES_HEAP_DIAGNOSTIC'}
if($HeapCheckOnly -and !$HeapDiagnostic){throw 'HEAP_CHECK_REQUIRES_DIAGNOSTIC'}
if($HeapDiagnosticNoExport -and !$HeapDiagnostic){throw 'NO_EXPORT_REQUIRES_HEAP_DIAGNOSTIC'}
if($HeapPageCheck -and (!$HeapDiagnostic -or $HeapCheckOnly)){throw 'PAGE_CHECK_REQUIRES_EXCLUSIVE_HEAP_DIAGNOSTIC'}
if($CrashDiagnostic -and (!$HeapDiagnostic -or $HeapCheckOnly -or $HeapPageCheck)){throw 'CRASH_DIAGNOSTIC_REQUIRES_EXCLUSIVE_MODE'}
if($DiagnosticCycles -ne 2 -and !$HeapDiagnostic){throw 'EXTENDED_CYCLES_REQUIRE_DIAGNOSTIC'}
if($IsolateUiaCycles -and !$HeapDiagnostic){throw 'ISOLATED_CLIENT_EXPERIMENT_REQUIRES_DIAGNOSTIC'}
if($HeapDiagnosticPersistentUia -and !$HeapDiagnostic){throw 'PERSISTENT_CLIENT_REQUIRES_HEAP_DIAGNOSTIC'}
if($HeapDiagnosticPersistentUia -and $IsolateUiaCycles){throw 'HEAP_UIA_PROFILES_CONFLICT'}
if($AudioTimingDiagnostic -and ($Mode -eq 'Formal' -or $HeapDiagnostic)){throw 'AUDIO_TIMING_REQUIRES_SEPARATE_NONFORMAL_RUN'}
if($GpuBudgetDiagnostic -and ($Mode -eq 'Formal' -or $HeapDiagnostic)){throw 'GPU_BUDGET_REQUIRES_SEPARATE_NONFORMAL_RUN'}
$limits=Get-Content (Join-Path $PSScriptRoot 'product_external_limits.json') -Raw | ConvertFrom-Json
$gpuLimitsPath=Join-Path $PSScriptRoot 'product_gpu_queue_limits.json'
$gpuLimits=Get-Content -LiteralPath $gpuLimitsPath -Raw | ConvertFrom-Json
$frozenGpu=!($AudioTimingDiagnostic -or $GpuBudgetDiagnostic -or $HeapDiagnostic)
$schedulerPolicyPath=Join-Path $PSScriptRoot 'product_pilot_scheduler_policy.json'
$schedulerPolicy=$null
if($frozenGpu){
    # The Python consumers strictly validate all policy fields before SDK import.
    $schedulerPolicy=@{sha256=(Get-FileHash -LiteralPath $schedulerPolicyPath -Algorithm SHA256).Hash.ToLowerInvariant();
        policy=(Get-Content -LiteralPath $schedulerPolicyPath -Raw | ConvertFrom-Json)}
}
if($frozenGpu -and ($gpuLimits.status -ne 'FROZEN_B14_TEST' -or $gpuLimits.diagnostic_only -ne $false)){throw 'GPU_TEST_LIMITS_NOT_FROZEN'}
if($frozenGpu -and (Get-PSDrive -Name $evidenceDrive).Free -lt $gpuLimits.minimum_free_evidence_disk_bytes){throw 'GPU_EVIDENCE_DISK_BUDGET_UNAVAILABLE'}
if($Mode -eq 'Formal') {
    if(!$ReleaseGate){throw 'FORMAL_RELEASE_GATE_REQUIRED'}
    $gate=Get-Content -LiteralPath $ReleaseGate -Raw | ConvertFrom-Json
    if($gate.verdict -ne 'READY' -or !$gate.historical_crash_regression_closed){throw 'FORMAL_GATE_NOT_READY'}
    if($gate.full_media_gpu_ready -ne $true){throw 'FORMAL_FULL_MEDIA_GPU_NOT_READY'}
    $gateInputPolicy=if($gate.desktop_input_policy){$gate.desktop_input_policy}else{'strict'}
    if($gateInputPolicy -cne $DesktopInputPolicy){throw 'FORMAL_DESKTOP_INPUT_POLICY_MISMATCH'}
    if(!$schedulerPolicy -or $gate.collector_scheduler_policy.sha256 -ne $schedulerPolicy.sha256){throw 'FORMAL_COLLECTOR_SCHEDULER_POLICY_NOT_READY'}
    Assert-ProductQualifiedTools $gate $Executable $AudioCollector $GpuTraceTool
    foreach($entry in $gate.inputs.PSObject.Properties) {
        if((Get-FileHash -LiteralPath $entry.Name -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.Value){throw 'RELEASE_INPUT_CHANGED'}
    }
    if((Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant() -ne $gate.product_sha256){throw 'RELEASE_BINARY_CHANGED'}
    if((Get-PSDrive -Name $evidenceDrive).Free -lt $limits.minimum_free_disk_bytes){throw 'EVIDENCE_DISK_BUDGET_UNAVAILABLE'}
}
$null=New-Item -ItemType Directory -Path $Root
$Root=(Resolve-Path -LiteralPath $Root).Path
if ($desktopBaseline) {$desktopBaseline | ConvertTo-Json | Set-Content "$Root/desktop-baseline.json" -Encoding UTF8}
$run=[guid]::NewGuid().ToString('N')
$cycles=if($Mode -eq 'Formal'){100}else{$DiagnosticCycles}
$seconds=if($Mode -eq 'Formal'){28800}else{240*$cycles}
$maximum=if($Mode -eq 'Formal'){[int]$limits.observer_timeout_seconds}else{$seconds+420}
$archiveBudget=if($Mode -eq 'Formal'){[long]$limits.archive_maximum_bytes}else{[long]536870912*$cycles}
$prefix=$run.Substring(0,8)
$remote=$target.remote_root
$remoteRun="$remote/pilot-$prefix"
$plan=[ordered]@{schema=2;run_id=$run;root=$Root;mode=$Mode;seconds=$seconds;cycles=$cycles;meeting_id=$meetingId;
    desktop_input_policy=$DesktopInputPolicy;
    share_seconds=60;log_pair_seconds=35;requires_context=$true;explicit_microphone_unmute=$true;
    gpu_budget_observer=@{required=$true;scope='calling_process_all_enumerated_hardware_adapters_all_nodes';maximum_gap_ms=2000};
    gpu_queue_observer=@{required=$true;scope='process_all_enumerated_hardware_adapters_all_scheduler_nodes';maximum_gap_ms=2000;queue_bounds=$(if($frozenGpu){'FROZEN_B14_TEST'}else{'NOT_FROZEN'})};
    gpu_etw_observer=@{required=$true;scope='process_owned_device_context_scheduler_packet_lifecycle';
        mode='lossless realtime owner-filtered JSONL';maximum_bytes=$(if($frozenGpu){[long]$gpuLimits.etw_maximum_bytes}else{8589934592});storage_budget_status=$(if($frozenGpu){'FROZEN_B14_TEST'}else{'PROVISIONAL_DIAGNOSTIC'})};
    server_provider='aliyun';server_instance_id=$target.instance_id;
    server_cpu=$target.server_cpu;server_memory_gib=$target.server_memory_gib;server_bandwidth_mbps=$target.server_bandwidth_mbps;
    collector_media_route=$target.collector_media_route;
    load=@{video_publishers=10;width=160;height=90;fps=5;video_bps_each=40000;video_codec='VP8';audio_bps=24000;simulcast=$false;encryption_mode='off'}}
$plan | ConvertTo-Json -Depth 6 | Set-Content "$Root/plan.json" -Encoding UTF8
if($frozenGpu){
    Copy-Item -LiteralPath $gpuLimitsPath -Destination "$Root/gpu-queue-limits.json"
    $plan.gpu_queue_limits_sha256=(Get-FileHash -LiteralPath $gpuLimitsPath -Algorithm SHA256).Hash.ToLowerInvariant()
    Copy-Item -LiteralPath $schedulerPolicyPath -Destination "$Root/collector-scheduler-policy.json"
    $plan.collector_scheduler_policy=$schedulerPolicy
    $plan | ConvertTo-Json -Depth 6 | Set-Content "$Root/plan.json" -Encoding UTF8
}
if($AudioTimingDiagnostic -or $GpuBudgetDiagnostic){
    if($AudioTimingDiagnostic){
        $plan.diagnostic_receiver_scheduling=@{nice=$DiagnosticReceiverNice;policy='SCHED_OTHER';publisher_nice=0}
        $plan.diagnostic_audio_reference=@{required=$true;scope='existing fixed tone in separate PeerConnection, same receiver process';qualification_credit=0}
        if($DiagnosticNoRealtime){$plan.diagnostic_realtime_restriction=@{required=$true;scope='collector child and publisher descendants only';cap_sys_nice=$false;cap_sys_resource=$false;no_new_privs=$true;rtprio_limit=@(0,0);qualification_credit=0}}
    }
    $plan.diagnostic_only=$true
    $plan.release_eligible=$false
    if($GpuBudgetDiagnostic){
        $gpuQueuePolicy=Join-Path $PSScriptRoot 'product_gpu_queue_diagnostic_policy.json'
        Copy-Item -LiteralPath $gpuQueuePolicy -Destination "$Root/gpu-queue-diagnostic-policy.json"
        $plan.gpu_queue_diagnostic_policy_sha256=(Get-FileHash -LiteralPath $gpuQueuePolicy -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $plan | ConvertTo-Json -Depth 6 | Set-Content "$Root/plan.json" -Encoding UTF8
    @{kind=$(if($GpuBudgetDiagnostic){'gpu_budget'}else{'audio_timing'});audio_timing=$AudioTimingDiagnostic.IsPresent;gpu_budget=$GpuBudgetDiagnostic.IsPresent;diagnostic_only=$true;release_eligible=$false;run_id=$run} | ConvertTo-Json | Set-Content "$Root/diagnostic-debugger.json" -Encoding UTF8
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'product_external_limits.json') -Destination "$Root/limits.json"
$hashes=[ordered]@{}
$paths=@($Executable,$AudioCollector,$GpuTraceTool,[IO.Path]::ChangeExtension($GpuTraceTool,'.pdb'),
    (Join-Path $PSScriptRoot '../diagnostics/gpu_budget/CMakeLists.txt'),
    (Join-Path $PSScriptRoot '../diagnostics/gpu_budget/invoke_gpu_live_control.ps1'),
    (Join-Path $workspace 'tests/uia/product_desktop.ps1'),(Join-Path $workspace 'tests/uia/product_desktop_cycle.ps1'),(Join-Path $workspace 'tests/uia/product_desktop_evidence.ps1')) +
    @(Get-ChildItem -LiteralPath @($PSScriptRoot, (Join-Path $PSScriptRoot '../../selftests'), (Join-Path $PSScriptRoot '../../probes')) -File | Where-Object {$_.Name -match 'product_(pilot|audio|external|heap|meeting|aliyun|gpu_budget|gpu_queue|gpu_etw|gpu_trace)|invoke_product_external|verify_product|release_product_acceptance'} | Select-Object -ExpandProperty FullName)
foreach($path in $paths){$hashes[$path]=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()}
$hashes | ConvertTo-Json | Set-Content "$Root/executed-inputs.json" -Encoding UTF8
$children=@();$uia=$null;$gpuTrace=$null;$remoteLaunchAttempted=$false;$failure=$null;$runExit=1
$runBudget=$null;$uiaLaunchClock=$null;$identity=$null;$last=$null
$probeMissingClock=$null;$resourceMissingClock=$null
$cleanupErrors=[Collections.Generic.List[string]]::new()
try {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot/product_meeting_fixture.ps1" -PreparedDirectory $PreparedDirectory -OutputDirectory $Root -MinimumRemainingSeconds ($maximum+300)
    if($LASTEXITCODE){throw 'MEETING_PREFLIGHT_FAILED'}
    foreach($name in @('product_pilot_remote.py','product_pilot_load.py','product_pilot_context.py','product_pilot_local_route.py','product_pilot_scheduler.py','product_pilot_scheduler_policy.json','product_aliyun_target.json','product_pilot_timing.py','product_pilot_video_counter.py','product_pilot_audio_reference.py')) {
        $remoteHash=Invoke-TestRemote "sha256sum $remote/$name"
        if($LASTEXITCODE -or !$remoteHash -or ($remoteHash -split ' ')[0] -ne (Get-FileHash "$PSScriptRoot/$name" -Algorithm SHA256).Hash.ToLowerInvariant()) {
            throw 'REMOTE_COLLECTOR_INPUT_MISMATCH'
        }
    }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/runtime/tools/product_acceptance/product_audio_devices.ps1 -Output "$Root/audio-devices.json" -RunId $run
    if($LASTEXITCODE){throw 'AUDIO_DEVICE_OBSERVER_FAILED'}
    $formalArg=if($Mode -eq 'Formal'){'--formal'}elseif($AudioTimingDiagnostic){"--diagnostic --timing-diagnostic --diagnostic-receiver-nice $DiagnosticReceiverNice"}elseif($HeapDiagnostic -or $GpuBudgetDiagnostic){'--diagnostic'}else{''}
    $schedulerArg=if($AudioTimingDiagnostic){"--diagnostic-receiver-nice $DiagnosticReceiverNice"}else{''}
    if($DiagnosticNoRealtime){$formalArg+=' --diagnostic-no-realtime';$schedulerArg+=' --diagnostic-no-realtime'}
    if($schedulerPolicy){$formalArg+=" --scheduler-policy $remote/product_pilot_scheduler_policy.json";$schedulerArg+=" --scheduler-policy $remote/product_pilot_scheduler_policy.json"}
    Invoke-TestRemote "$remote/venv/bin/python --version; $remote/bootstrap/bin/uv pip freeze --python $remote/venv/bin/python" | Set-Content "$Root/remote-environment.txt" -Encoding UTF8
    if ($desktopBaseline) {$null=Assert-DedicatedDesktop $DedicatedDesktopSessionId $desktopBaseline $DesktopInputPolicy}
    # Start the common capture budget before every remote/local collector.
    $runBudget=Write-ProductRunBudget "$Root/run-clock.json" $run $maximum
    # A transport timeout cannot prove that the submitted launch did not happen.
    $remoteLaunchAttempted=$true
    Invoke-TestRemote "nohup $remote/venv/bin/python $remote/product_pilot_local_route.py --target-config $remote/product_aliyun_target.json --run-id $run --result $remote/pilot-$prefix-route.json $schedulerArg -- $remote/venv/bin/python $remote/product_pilot_remote.py --dependencies $remote/collector-python --config $($target.livekit_config) --output $remoteRun --run-id $run --room $meetingId --seconds $maximum $formalArg > $remote/pilot-$prefix.stderr 2>&1 < /dev/null &"
    if($LASTEXITCODE){throw 'REMOTE_START_FAILED'}
    $ready=$false
    for($i=0;$i -lt 30;++$i){
        $response=Invoke-TestRemote "if test -f $remoteRun/ready.json; then cat $remoteRun/ready.json; else echo null; fi" 2>$null
        if($LASTEXITCODE -eq 0 -and ($response | ConvertFrom-Json).run_id -eq $run){$ready=$true;break}
        Start-Sleep -Seconds 1
    }
    if(!$ready){throw 'REMOTE_READY_TIMEOUT'}
    $secret=(Get-Content "$PreparedDirectory/password.dpapi" -Raw).Trim() | ConvertTo-SecureString
    $env:LIVEKIT_UIA_ACCOUNT=$setup.account
    $env:LIVEKIT_UIA_PASSWORD=[Net.NetworkCredential]::new('', $secret).Password
    $env:LIVEKIT_UIA_MEETING_ID=$setup.meeting_id
    $env:LIVEKIT_UIA_SERVICE_URL=$setup.service_url
    $env:LIVEKIT_UIA_RUN_ID=$run
    $env:LIVEKIT_UIA_REMOTE_CONTEXT='1'
    $env:LIVEKIT_UIA_LOG_PAIR='1'
    $env:LIVEKIT_UIA_PILOT_PROBE="$Root/process-probe.jsonl"
    $env:LIVEKIT_UIA_GPU_BUDGET_PROBE='1'
    if ($desktopBaseline) {$null=Assert-DedicatedDesktop $DedicatedDesktopSessionId $desktopBaseline $DesktopInputPolicy}
    $resource=Start-Process powershell.exe -WindowStyle Hidden -PassThru -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',"$PSScriptRoot/product_pilot_resources.ps1",'-UiaDirectory',"$Root/uia",'-Destination',"$Root/external-resources.jsonl",'-RunId',$run,'-MaximumSeconds',$maximum,'-RunBudgetPath',"$Root/run-clock.json",'-AudioCollector',$AudioCollector) -RedirectStandardOutput "$Root/resources.stdout" -RedirectStandardError "$Root/resources.stderr"
    $children+=$resource
    $null=$resource.Handle
    $archive=Start-Process python.exe -WindowStyle Hidden -PassThru -ArgumentList @("$PSScriptRoot/product_pilot_checkpoints.py",'--probe',"$Root/process-probe.jsonl",'--result',"$Root/uia/uia-result.json",'--output',"$Root/checkpoint-archive",'--run-id',$run,'--seconds',$maximum,'--run-budget',"$Root/run-clock.json",'--maximum-bytes',$archiveBudget) -RedirectStandardOutput "$Root/archive.stdout" -RedirectStandardError "$Root/archive.stderr"
    $children+=$archive
    $null=$archive.Handle
    $diagnostic=Start-Process python.exe -WindowStyle Hidden -PassThru -ArgumentList @("$PSScriptRoot/product_pilot_diagnostics.py",'--root',$Root,'--watch','--seconds',$maximum,'--run-budget',"$Root/run-clock.json") -RedirectStandardOutput "$Root/diagnostic.stdout" -RedirectStandardError "$Root/diagnostic.stderr"
    $children+=$diagnostic
    $null=$diagnostic.Handle
    $gpuDirectory=Join-Path $Root 'gpu-etw'
    $null=New-Item -ItemType Directory -Path $gpuDirectory
    $gpuSession='B14-Gpu-Release-'+$run
    $gpuTrace=Start-Process $GpuTraceTool -WindowStyle Hidden -PassThru -ArgumentList @('--live',$gpuSession,('"'+$gpuDirectory+'"'),$maximum,$plan.gpu_etw_observer.maximum_bytes) -RedirectStandardOutput "$Root/gpu-trace.stdout" -RedirectStandardError "$Root/gpu-trace.stderr"
    $children+=$gpuTrace;$null=$gpuTrace.Handle
    $gpuDeadline=[DateTime]::UtcNow.AddSeconds(10)
    while(!(Test-Path "$gpuDirectory/trace-ready.json")){
        if($gpuTrace.HasExited -or [DateTime]::UtcNow -gt $gpuDeadline){throw 'GPU_ETW_COLLECTOR_NOT_READY'}
        Start-Sleep -Milliseconds 100
    }
    $gpuHeartbeatClock=[Diagnostics.Stopwatch]::StartNew()
    $env:LIVEKIT_UIA_GPU_ETW_DIRECTORY=$gpuDirectory
    $uiaArgs=@('-NoProfile','-ExecutionPolicy','Bypass','-File',"$workspace/tests/uia/product_desktop.ps1",'-Executable',$Executable,'-OutputDirectory',"$Root/uia",'-RunId',$run,'-Cycles',$cycles,'-MinimumSeconds',$seconds,'-ShareSeconds',60,'-LogPairSeconds',35,'-StopSettleSeconds',10,'-RoomSettleSeconds',10)
    if ($DedicatedDesktopSessionId -gt 0) {$uiaArgs+=@('-DedicatedDesktopSessionId',$DedicatedDesktopSessionId)}
    $uiaArgs+=@('-DesktopInputPolicy',$DesktopInputPolicy)
    if($Mode -eq 'Pilot'){$uiaArgs+='-Pilot'}
    if($HeapDiagnostic){$uiaArgs+='-HeapDiagnostic'}
    if($HeapSnapshotDiagnostic){$uiaArgs+='-HeapSnapshotDiagnostic'}
    if($HeapDiagnosticPersistentUia){$uiaArgs+='-HeapDiagnosticPersistentUia'}
    if($HeapDiagnosticNoShare){$uiaArgs+='-HeapDiagnosticNoShare'}
    if($HeapCheckOnly){$uiaArgs+='-HeapCheckOnly'}
    if($HeapDiagnosticNoExport){$uiaArgs+='-HeapDiagnosticNoExport'}
    if($HeapPageCheck){$uiaArgs+='-HeapPageCheck'}
    if($CrashDiagnostic){$uiaArgs+='-CrashDiagnostic'}
    if($IsolateUiaCycles){$uiaArgs+='-IsolateUiaCycles'}
    $uiaLaunchClock=[Diagnostics.Stopwatch]::StartNew()
    $uia=Start-Process powershell.exe -WindowStyle Hidden -PassThru -ArgumentList $uiaArgs -RedirectStandardOutput "$Root/uia.stdout" -RedirectStandardError "$Root/uia.stderr"
    $uiaHandle=$uia.Handle
    Remove-Item Env:LIVEKIT_UIA_GPU_BUDGET_PROBE -ErrorAction SilentlyContinue
    Remove-Item Env:LIVEKIT_UIA_GPU_ETW_DIRECTORY -ErrorAction SilentlyContinue
    Remove-Item Env:LIVEKIT_UIA_PASSWORD
    @{uia_pid=$uia.Id;resource_pid=$resource.Id;archive_pid=$archive.Id;diagnostic_pid=$diagnostic.Id;gpu_trace_pid=$gpuTrace.Id;gpu_trace_session=$gpuSession;run_id=$run} | ConvertTo-Json | Set-Content "$Root/collectors.json" -Encoding UTF8
    while(!$uia.WaitForExit(1000)) {
        if ($desktopBaseline) {
            Get-DesktopEvidenceState | ConvertTo-Json -Compress | Add-Content "$Root/desktop-observations.jsonl" -Encoding UTF8
            $null=Assert-DedicatedDesktop $DedicatedDesktopSessionId $desktopBaseline $DesktopInputPolicy
        }
        if((Get-ProductRunBudgetElapsed $runBudget) -gt $maximum){throw 'RUN_WATCHDOG_TIMEOUT'}
        $diskReserve=if($frozenGpu){[long]$gpuLimits.evidence_disk_reserve_bytes}else{5GB}
        if((Get-PSDrive -Name $evidenceDrive).Free -lt $diskReserve){throw 'EVIDENCE_DISK_RESERVE_EXHAUSTED'}
        if(Test-ProductEarlyObservers $Root $run $Mode $cycles $seconds $archive $diagnostic $gpuTrace $gpuDirectory $gpuHeartbeatClock $identity $plan.gpu_etw_observer.maximum_bytes){continue}
        $last=$null
        if(Test-Path "$Root/uia/uia-actions.jsonl"){$last=Read-LatestCompleteUiaAction "$Root/uia/uia-actions.jsonl"}
        # The driver atomically publishes identity before its first action.
        # Read action first so a concurrently published action cannot outrun a
        # null identity cached earlier in this observer iteration.
        if(!$identity -and (Test-Path "$Root/uia/product-identity.json")){
            $identity=Read-ProductObserverSnapshot "$Root/uia/product-identity.json"
            $probeMissingClock=[Diagnostics.Stopwatch]::StartNew()
        }
        $nowMs=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
        $progress=$null;$liveProduct=$null
        try{
            if($last -and (Get-ProductObserverAgeMilliseconds $last.utc $nowMs) -gt 400000 -and
               (Test-Path "$Root/uia/product-lifetime-progress.json")){
                $progress=Read-ProductObserverSnapshot "$Root/uia/product-lifetime-progress.json"
                if($identity){$liveProduct=Get-Process -Id $identity.pid -ErrorAction SilentlyContinue}
            }
            $nowMs=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
            Assert-ProductActionProgress $last $run $uiaLaunchClock.Elapsed.TotalSeconds $nowMs $progress $identity $cycles $seconds $liveProduct
        }finally{if($liveProduct){$liveProduct.Dispose()}}
        $exiting=($last -and $last.action -eq 'process_exit')
        if($last -and !$resourceMissingClock){$resourceMissingClock=[Diagnostics.Stopwatch]::StartNew()}
        if($resource.HasExited -and $last -and !$exiting){throw 'RESOURCE_COLLECTOR_STOPPED_EARLY'}
        if($resourceMissingClock){
            $resourceHealth=$null
            if(Test-Path "$Root/external-resources.jsonl"){
                $resourceRows=@(Read-ProductPilotCompleteJsonlTail -Path "$Root/external-resources.jsonl" -Count 1)
                if($resourceRows.Count){$resourceHealth=$resourceRows[0]|ConvertFrom-Json -ErrorAction Stop}
            }
            $nowMs=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
            Assert-ProductResourceHealth $resourceHealth $run $identity $resourceMissingClock.Elapsed.TotalSeconds $nowMs $exiting
            if($resourceHealth){$resourceMissingClock.Restart()}
        }
        $health=$null
        if(Test-Path "$Root/process-probe.jsonl"){
            $health=Read-ProductPilotProbeTail -Path "$Root/process-probe.jsonl" -Count 2 -MaximumBytes 1048576 | ForEach-Object {try{$_ | ConvertFrom-Json}catch{}} | Select-Object -Last 1
        }
        if($probeMissingClock){
            $nowMs=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
            try{Assert-ProductNativeHealth $health $run $identity $probeMissingClock.Elapsed.TotalSeconds $nowMs $exiting}
            catch{
                if($_.Exception.Message -eq 'LIVE_ZERO_LOSS_GATE_FAILED'){
                    try{$health|ConvertTo-Json -Depth 10|Set-Content "$Root/watchdog-failure-probe.json" -Encoding UTF8}catch{}
                }
                throw
            }
            if($health){$probeMissingClock.Restart()}
        }
    }
    $uia.Refresh()
    if($uia.ExitCode -ne 0 -or !(Test-ProductUiaCompletion $Root $run $Mode $cycles $seconds)){throw 'UIA_RUN_FAILED'}
    $runExit=0
} catch {
    $failure=$_.Exception.Message
    try{@{run_id=$run;verdict='FAIL';reason=$failure;desktop_input_policy=$DesktopInputPolicy;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json|Set-Content "$Root/controller-result.json" -Encoding UTF8}catch{$cleanupErrors.Add('CONTROLLER_FAILURE_RECEIPT_WRITE_FAILED')}
    try{@{run_id=$run;reason=$failure}|ConvertTo-Json|Set-Content "$Root/collector-stop.json" -Encoding UTF8}catch{$cleanupErrors.Add('COLLECTOR_STOP_WRITE_FAILED')}
    # Identity is published before first-window discovery. A collector can fail
    # before the first UIA action; do not orphan that product or trust PID alone.
    $product=$null
    try{if(Test-Path "$Root/uia/product-identity.json") {
        $identity=Get-Content "$Root/uia/product-identity.json" -Raw | ConvertFrom-Json
        $product=Get-Process -Id $identity.pid -ErrorAction SilentlyContinue
        if($product -and $identity.run_id -eq $run -and
           $product.Path -eq $Executable -and $identity.executable -eq $Executable -and
           $product.StartTime.ToUniversalTime().Ticks -eq [long]$identity.start_ticks){
            $product.Kill();$null=$product.WaitForExit(5000)
        }
    }}catch{$cleanupErrors.Add('PRODUCT_FAILURE_CLEANUP_FAILED')}finally{if($product){$product.Dispose()}}
    if($uia){try{$uia.Refresh();if(!$uia.HasExited){$uia.Kill();$null=$uia.WaitForExit(5000)}}catch{$cleanupErrors.Add('UIA_FAILURE_CLEANUP_FAILED')}}
} finally {
    Remove-Item Env:LIVEKIT_UIA_GPU_BUDGET_PROBE -ErrorAction SilentlyContinue
    Remove-Item Env:LIVEKIT_UIA_GPU_ETW_DIRECTORY -ErrorAction SilentlyContinue
    Remove-Item Env:LIVEKIT_UIA_PASSWORD -ErrorAction SilentlyContinue
    if($remoteLaunchAttempted){
        try {
            $shutdownArgs=@('--root',$Root,'--run-id',$run,'--launch-attempted')
            if($AudioTimingDiagnostic){$shutdownArgs+='--timing'}
            & python "$PSScriptRoot/product_pilot_shutdown.py" @shutdownArgs
            if($LASTEXITCODE){throw 'REMOTE_SHUTDOWN_OR_EVIDENCE_FAILED'}
        } catch {
            $runExit=1
            $cleanupErrors.Add('REMOTE_CLEANUP_OR_EVIDENCE_FAILED')
            try{@{reason='REMOTE_CLEANUP_OR_EVIDENCE_FAILED';run_id=$run}|ConvertTo-Json|Set-Content "$Root/remote-cleanup-failure.json"}catch{$cleanupErrors.Add('REMOTE_FAILURE_RECEIPT_WRITE_FAILED')}
        }
    }
    $cleanup=Invoke-ProductCollectorCleanup $children $gpuTrace $GpuTraceTool $gpuSession $gpuDirectory
    if(!$cleanup.passed){$runExit=1}
    foreach($cleanupError in $cleanup.errors){$cleanupErrors.Add($cleanupError)}
    try{@{run_id=$run;collectors=$cleanup.collectors;cleanup_errors=@($cleanupErrors.ToArray())}|ConvertTo-Json -Depth 5|Set-Content "$Root/collector-exits.json" -Encoding UTF8}
    catch{$cleanupErrors.Add('COLLECTOR_EXIT_RECEIPT_WRITE_FAILED');$runExit=1}
}
try{
if(Test-Path "$Root/process-probe.jsonl"){
    python "$PSScriptRoot/product_pilot_diagnostics.py" --root $Root
    if($LASTEXITCODE){$runExit=1}
}
if(Test-Path "$Root/uia/uia-result.json"){
    $u=Get-Content "$Root/uia/uia-result.json" -Raw | ConvertFrom-Json
    $events=@(Get-WinEvent -FilterHashtable @{LogName='Application';Id=1000;StartTime=([DateTime]$u.started_utc).ToLocalTime();EndTime=([DateTime]$u.finished_utc).ToLocalTime().AddMinutes(1)} -ErrorAction SilentlyContinue | Where-Object {$_.Properties[0].Value -eq 'Cohavora.exe'} | ForEach-Object {@{utc=$_.TimeCreated.ToUniversalTime().ToString('o');application=$_.Properties[0].Value;module=$_.Properties[3].Value;exception_code=$_.Properties[6].Value;offset=$_.Properties[7].Value;process_id=$_.Properties[8].Value}})
    @{events=$events;count=$events.Count} | ConvertTo-Json -Depth 5 | Set-Content "$Root/windows-crash-event.json" -Encoding UTF8
}
python "$PSScriptRoot/verify_product_external.py" --root $Root
if($LASTEXITCODE){$runExit=1}
if($Mode -eq 'Pilot' -and (Test-Path "$Root/uia/uia-result.json") -and $u.verdict -eq 'PILOT_COMPLETE'){
    python "$PSScriptRoot/verify_product_pilot.py" --root $Root
    if($LASTEXITCODE){$runExit=1}
}
}catch{$runExit=1;$cleanupErrors.Add('TERMINAL_REVIEW_FAILED')}
finally{
    if($cleanupErrors.Count){$runExit=1}
    try{@{run_id=$run;exit_code=$runExit;verdict=$(if($runExit -eq 0){'EVIDENCE_COMPLETE'}else{'FAIL'});cleanup_errors=@($cleanupErrors.ToArray())}|ConvertTo-Json -Depth 4|Set-Content "$Root/runner-exit.json" -Encoding UTF8}
    catch{$runExit=1;Write-Error 'RUNNER_EXIT_RECEIPT_WRITE_FAILED' -ErrorAction Continue}
}
Write-Output "RUN_ROOT=$Root"
exit $runExit
