param(
    [Parameter(Mandatory=$true)][string]$Root,
    [Parameter(Mandatory=$true)][string]$PreparedDirectory,
    [string]$Executable='out/build/windows-vs2026-dev/src/app/Debug/Cohavora.exe',
    [string]$AudioCollector='out/build/windows-vs2026-dev/Debug/product_audio_loopback.exe',
    [ValidateSet('Pilot','Formal')][string]$Mode='Pilot',
    [string]$ReleaseGate='',
    [int]$DedicatedDesktopSessionId=0,
    [switch]$HeapDiagnostic,
    [switch]$HeapSnapshotDiagnostic,
    [switch]$HeapDiagnosticPersistentUia,
    [switch]$HeapDiagnosticNoShare,
    [switch]$HeapCheckOnly,
    [switch]$HeapDiagnosticNoExport,
    [switch]$HeapPageCheck,
    [switch]$CrashDiagnostic,
    [ValidateRange(2,10)][int]$DiagnosticCycles=2,
    [switch]$IsolateUiaCycles
)
$ErrorActionPreference='Stop'
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
$workspace=(Resolve-Path (Join-Path $PSScriptRoot '../../../..')).Path
Set-Location $workspace
$Executable=(Resolve-Path -LiteralPath $Executable).Path
$AudioCollector=(Resolve-Path -LiteralPath $AudioCollector).Path
$PreparedDirectory=(Resolve-Path -LiteralPath $PreparedDirectory).Path
$setup=Get-Content "$PreparedDirectory/setup.json" -Raw | ConvertFrom-Json
if ($setup.status -ne 'PREPARED' -or $setup.meeting_id -cnotmatch '^[0-9]{9}$') {throw 'PREPARED_MEETING_INVALID'}
$target=Get-Content (Join-Path $PSScriptRoot 'product_aliyun_target.json') -Raw | ConvertFrom-Json
if ($setup.service_url.TrimEnd('/') -ne $target.service_url) {throw 'TEST_SERVICE_UNEXPECTED'}
. (Join-Path $workspace 'tests/uia/product_desktop_evidence.ps1')
if ($Mode -eq 'Formal' -or $DedicatedDesktopSessionId -gt 0) {
    $desktopBaseline=Assert-DedicatedDesktop $DedicatedDesktopSessionId
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
$limits=Get-Content (Join-Path $PSScriptRoot 'product_external_limits.json') -Raw | ConvertFrom-Json
if($Mode -eq 'Formal') {
    if(!$ReleaseGate){throw 'FORMAL_RELEASE_GATE_REQUIRED'}
    $gate=Get-Content -LiteralPath $ReleaseGate -Raw | ConvertFrom-Json
    if($gate.verdict -ne 'READY' -or !$gate.historical_crash_regression_closed){throw 'FORMAL_GATE_NOT_READY'}
    foreach($entry in $gate.inputs.PSObject.Properties) {
        if((Get-FileHash -LiteralPath $entry.Name -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.Value){throw 'RELEASE_INPUT_CHANGED'}
    }
    if((Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant() -ne $gate.product_sha256){throw 'RELEASE_BINARY_CHANGED'}
    if((Get-PSDrive -Name ([IO.Path]::GetPathRoot($workspace).Substring(0,1))).Free -lt $limits.minimum_free_disk_bytes){throw 'EVIDENCE_DISK_BUDGET_UNAVAILABLE'}
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
    share_seconds=60;log_pair_seconds=35;requires_context=$true;explicit_microphone_unmute=$true;
    server_provider='aliyun';server_instance_id=$target.instance_id;
    server_cpu=$target.server_cpu;server_memory_gib=$target.server_memory_gib;server_bandwidth_mbps=$target.server_bandwidth_mbps;
    load=@{video_publishers=10;width=160;height=90;fps=5;video_bps_each=40000;video_codec='VP8';audio_bps=24000;simulcast=$false}}
$plan | ConvertTo-Json -Depth 6 | Set-Content "$Root/plan.json" -Encoding UTF8
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'product_external_limits.json') -Destination "$Root/limits.json"
$hashes=[ordered]@{}
$paths=@($Executable,$AudioCollector,(Join-Path $workspace 'tests/uia/product_desktop.ps1'),(Join-Path $workspace 'tests/uia/product_desktop_cycle.ps1'),(Join-Path $workspace 'tests/uia/product_desktop_evidence.ps1')) +
    @(Get-ChildItem -LiteralPath @($PSScriptRoot, (Join-Path $PSScriptRoot '../../selftests'), (Join-Path $PSScriptRoot '../../probes')) -File | Where-Object {$_.Name -match 'product_(pilot|audio|external|heap|meeting|aliyun)|invoke_product_external|verify_product'} | Select-Object -ExpandProperty FullName)
foreach($path in $paths){$hashes[$path]=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()}
$hashes | ConvertTo-Json | Set-Content "$Root/executed-inputs.json" -Encoding UTF8
$children=@();$uia=$null;$remoteStarted=$false;$failure=$null;$runExit=1
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
    $formalArg=if($Mode -eq 'Formal'){'--formal'}elseif($HeapDiagnostic){'--diagnostic'}else{''}
    Invoke-TestRemote "$remote/venv/bin/python --version; $remote/bootstrap/bin/uv pip freeze --python $remote/venv/bin/python" | Set-Content "$Root/remote-environment.txt" -Encoding UTF8
    if ($desktopBaseline) {$null=Assert-DedicatedDesktop $DedicatedDesktopSessionId $desktopBaseline}
    Invoke-TestRemote "nohup $remote/venv/bin/python $remote/product_pilot_remote.py --dependencies $remote/collector-python --config $($target.livekit_config) --output $remoteRun --run-id $run --room $meetingId --seconds $maximum $formalArg > $remote/pilot-$prefix.stderr 2>&1 < /dev/null &"
    if($LASTEXITCODE){throw 'REMOTE_START_FAILED'}
    $remoteStarted=$true
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
    if ($desktopBaseline) {$null=Assert-DedicatedDesktop $DedicatedDesktopSessionId $desktopBaseline}
    $resource=Start-Process powershell.exe -WindowStyle Hidden -PassThru -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',"$PSScriptRoot/product_pilot_resources.ps1",'-UiaDirectory',"$Root/uia",'-Destination',"$Root/external-resources.jsonl",'-RunId',$run,'-MaximumSeconds',$maximum,'-AudioCollector',$AudioCollector) -RedirectStandardOutput "$Root/resources.stdout" -RedirectStandardError "$Root/resources.stderr"
    $children+=$resource
    $null=$resource.Handle
    $archive=Start-Process python.exe -WindowStyle Hidden -PassThru -ArgumentList @("$PSScriptRoot/product_pilot_checkpoints.py",'--probe',"$Root/process-probe.jsonl",'--result',"$Root/uia/uia-result.json",'--output',"$Root/checkpoint-archive",'--run-id',$run,'--seconds',$maximum,'--maximum-bytes',$archiveBudget) -RedirectStandardOutput "$Root/archive.stdout" -RedirectStandardError "$Root/archive.stderr"
    $children+=$archive
    $null=$archive.Handle
    $diagnostic=Start-Process python.exe -WindowStyle Hidden -PassThru -ArgumentList @("$PSScriptRoot/product_pilot_diagnostics.py",'--root',$Root,'--watch','--seconds',$maximum) -RedirectStandardOutput "$Root/diagnostic.stdout" -RedirectStandardError "$Root/diagnostic.stderr"
    $children+=$diagnostic
    $null=$diagnostic.Handle
    $uiaArgs=@('-NoProfile','-ExecutionPolicy','Bypass','-File',"$workspace/tests/uia/product_desktop.ps1",'-Executable',$Executable,'-OutputDirectory',"$Root/uia",'-RunId',$run,'-Cycles',$cycles,'-MinimumSeconds',$seconds,'-ShareSeconds',60,'-LogPairSeconds',35,'-StopSettleSeconds',10,'-RoomSettleSeconds',10)
    if ($DedicatedDesktopSessionId -gt 0) {$uiaArgs+=@('-DedicatedDesktopSessionId',$DedicatedDesktopSessionId)}
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
    $uia=Start-Process powershell.exe -WindowStyle Hidden -PassThru -ArgumentList $uiaArgs -RedirectStandardOutput "$Root/uia.stdout" -RedirectStandardError "$Root/uia.stderr"
    $uiaHandle=$uia.Handle
    Remove-Item Env:LIVEKIT_UIA_PASSWORD
    @{uia_pid=$uia.Id;resource_pid=$resource.Id;archive_pid=$archive.Id;diagnostic_pid=$diagnostic.Id;run_id=$run} | ConvertTo-Json | Set-Content "$Root/collectors.json" -Encoding UTF8
    $started=[DateTime]::UtcNow
    while(!$uia.WaitForExit(1000)) {
        if ($desktopBaseline) {
            Get-DesktopEvidenceState | ConvertTo-Json -Compress | Add-Content "$Root/desktop-observations.jsonl" -Encoding UTF8
            $null=Assert-DedicatedDesktop $DedicatedDesktopSessionId $desktopBaseline
        }
        if(([DateTime]::UtcNow-$started).TotalSeconds -gt $maximum){throw 'RUN_WATCHDOG_TIMEOUT'}
        if((Get-PSDrive -Name ([IO.Path]::GetPathRoot($workspace).Substring(0,1))).Free -lt 5GB){throw 'EVIDENCE_DISK_RESERVE_EXHAUSTED'}
        if(Test-Path "$Root/uia/uia-result.json"){continue}
        if($archive.HasExited -or $diagnostic.HasExited){throw 'COLLECTOR_STOPPED_BEFORE_UIA_COMPLETION'}
        if(Test-Path "$Root/uia/uia-actions.jsonl") {
            $last=Get-Content "$Root/uia/uia-actions.jsonl" -Tail 1 | ConvertFrom-Json
            $idle=([DateTime]::UtcNow-[DateTime]$last.utc).TotalSeconds
            if($resource.HasExited -and $last.action -ne 'process_exit'){throw 'RESOURCE_COLLECTOR_STOPPED_EARLY'}
            # Includes the deliberate per-cycle dwell through the 8h schedule.
            if($idle -gt 400){throw 'UIA_OPERATION_WATCHDOG_TIMEOUT'}
        }
        if(Test-Path "$Root/process-probe.jsonl"){
            $health=Get-Content "$Root/process-probe.jsonl" -Tail 2 | ForEach-Object {try{$_ | ConvertFrom-Json}catch{}} | Select-Object -Last 1
            if($health -and $last.action -ne 'process_exit' -and ([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()-$health.utc_ms) -gt 15000){throw 'PROCESS_PROBE_STALE'}
            if($health -and ($health.history.queue_drops -or $health.history.pending_records_dropped -or $health.history.write_failures -or $health.diagnostic.dropped_ordinary -or $health.diagnostic.dropped_critical -or $health.diagnostic.sink_failures)){
                $health | ConvertTo-Json -Depth 10 | Set-Content "$Root/watchdog-failure-probe.json" -Encoding UTF8
                throw 'LIVE_ZERO_LOSS_GATE_FAILED'
            }
        }
    }
    $uia.Refresh()
    $result=Get-Content "$Root/uia/uia-result.json" -Raw | ConvertFrom-Json
    if($result.verdict -notin @('PILOT_COMPLETE','UIA_COMPLETE')){throw 'UIA_RUN_FAILED'}
    $runExit=0
} catch {
    $failure=$_.Exception.Message
    @{run_id=$run;verdict='FAIL';reason=$failure;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$Root/controller-result.json" -Encoding UTF8
    @{run_id=$run;reason=$failure} | ConvertTo-Json | Set-Content "$Root/collector-stop.json" -Encoding UTF8
    if(Test-Path "$Root/uia/uia-actions.jsonl") {
        $last=Get-Content "$Root/uia/uia-actions.jsonl" -Tail 1 | ConvertFrom-Json
        $product=Get-Process -Id $last.pid -ErrorAction SilentlyContinue
        if($product -and $product.Path -eq $Executable){Stop-Process -Id $product.Id}
    }
    if($uia -and !$uia.HasExited){Stop-Process -Id $uia.Id}
} finally {
    Remove-Item Env:LIVEKIT_UIA_PASSWORD -ErrorAction SilentlyContinue
    if($remoteStarted){
        try {
            Invoke-TestRemote "if test -d $remoteRun; then touch $remoteRun/stop; fi"
            for($i=0;$i -lt 15;++$i){
                $last=Invoke-TestRemote "if test -f $remoteRun/remote.jsonl; then tail -n 1 $remoteRun/remote.jsonl; else echo null; fi" | ConvertFrom-Json
                if($last.event -eq 'collector.stopped'){break}
                Start-Sleep -Seconds 1
            }
            if ($last.event -ne 'collector.stopped') {$runExit=1}
            & workbench download "$remoteRun/remote.jsonl" "$Root/remote.jsonl" -i $target.instance_id -r $target.region
            if ($LASTEXITCODE) {$runExit=1}
        } catch {
            $runExit=1
            @{reason='REMOTE_CLEANUP_OR_EVIDENCE_FAILED';run_id=$run} | ConvertTo-Json | Set-Content "$Root/remote-cleanup-failure.json"
        }
    }
    $childResults=@(foreach($child in $children){
        $timedOut=!$child.WaitForExit(10000)
        if($timedOut){Stop-Process -Id $child.Id; $child.WaitForExit(); $runExit=1}
        $child.Refresh()
        if($null -eq $child.ExitCode -or $child.ExitCode -ne 0){$runExit=1}
        @{pid=$child.Id;exit_code=$child.ExitCode;forced_stop=$timedOut}
    })
    @{run_id=$run;collectors=$childResults} | ConvertTo-Json -Depth 4 | Set-Content "$Root/collector-exits.json" -Encoding UTF8
}
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
@{run_id=$run;exit_code=$runExit;verdict=$(if($runExit -eq 0){'EVIDENCE_COMPLETE'}else{'FAIL'})} | ConvertTo-Json | Set-Content "$Root/runner-exit.json" -Encoding UTF8
Write-Output "RUN_ROOT=$Root"
exit $runExit
