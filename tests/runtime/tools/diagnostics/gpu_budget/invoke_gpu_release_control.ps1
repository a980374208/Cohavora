param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)][string]$Probe,
    [Parameter(Mandatory=$true)][string]$TraceTool
)
$ErrorActionPreference='Stop'
$Probe=(Resolve-Path -LiteralPath $Probe).Path
$TraceTool=(Resolve-Path -LiteralPath $TraceTool).Path
if((Split-Path (Split-Path $Probe) -Leaf) -ne 'RelWithDebInfo' -or
    (Split-Path (Split-Path $TraceTool) -Leaf) -ne 'RelWithDebInfo') {throw 'GPU_CONTROL_RELWITHDEBINFO_REQUIRED'}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $OutputDirectory) {throw 'GPU_CONTROL_OUTPUT_MUST_BE_NEW'}
$null=New-Item -ItemType Directory -Path $OutputDirectory
$session='B14-Gpu-Release-'+[guid]::NewGuid().ToString('N')
$trace=Join-Path $OutputDirectory 'gpu.etl'
$raw=Join-Path $OutputDirectory 'process-device-lifecycle.jsonl'
$principal=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
$state=[ordered]@{schema=1;status='RUNNING';diagnostic_only=$true;release_eligible=$false;
    configuration='RelWithDebInfo';session=$session;provider='Microsoft-Windows-DxgKrnl';
    provider_guid='802ec45a-1e99-4b83-9920-87c98277ba9d';keywords='0x4000000000000841';
    clock='perf';buffer_kib=64;minimum_buffers=16;maximum_buffers=64;maximum_file_mib=128;
    elevated_administrator=$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator);
    probe=$Probe;probe_sha256=(Get-FileHash -LiteralPath $Probe -Algorithm SHA256).Hash.ToLowerInvariant();
    trace_tool=$TraceTool;trace_tool_sha256=(Get-FileHash -LiteralPath $TraceTool -Algorithm SHA256).Hash.ToLowerInvariant();
    script_sha256=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant();
    started_utc=[DateTime]::UtcNow.ToString('o');trace_started=$false;trace_stopped=$false}
function Save-ControlState {
    $state.updated_utc=[DateTime]::UtcNow.ToString('o')
    $state | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $OutputDirectory 'control-state.json') -Encoding UTF8
}
$child=$null
Save-ControlState
try {
    # A unique, task-owned session. Existing kernel or WPR sessions are untouched.
    & $TraceTool --start $session $trace (Join-Path $OutputDirectory 'trace-start.json') *> (Join-Path $OutputDirectory 'trace-start.log')
    $state.trace_start_exit=$LASTEXITCODE
    if($LASTEXITCODE -ne 0) {throw 'GPU_CONTROL_ETW_START_FAILED'}
    $state.trace_started=$true
    $state.event_id_filter=(Get-Content (Join-Path $OutputDirectory 'trace-start.json') -Raw | ConvertFrom-Json).event_id_filter
    $state.trace_started_utc=[DateTime]::UtcNow.ToString('o')
    Save-ControlState
    $child=Start-Process -FilePath $Probe -WindowStyle Hidden -PassThru -ArgumentList @(('"'+$raw+'"'),'--release-control') -RedirectStandardOutput (Join-Path $OutputDirectory 'probe.stdout') -RedirectStandardError (Join-Path $OutputDirectory 'probe.stderr')
    $null=$child.Handle
    $state.probe_pid=$child.Id
    $state.probe_start_ticks=$child.StartTime.ToUniversalTime().Ticks
    Save-ControlState
    $start=[DateTime]::UtcNow
    while(!$child.WaitForExit(1000)) {
        if(([DateTime]::UtcNow-$start).TotalSeconds -gt 50) {throw 'GPU_CONTROL_PROBE_TIMEOUT'}
    }
    $child.Refresh()
    $state.probe_exit=$child.ExitCode
    if($child.ExitCode -ne 0) {throw 'GPU_CONTROL_PROBE_FAILED'}
    $state.probe_finished_utc=[DateTime]::UtcNow.ToString('o')
    Start-Sleep -Seconds 2
    & logman.exe query $session -ets *> (Join-Path $OutputDirectory 'trace-query.log')
    $state.trace_query_exit=$LASTEXITCODE
    if($LASTEXITCODE -ne 0) {throw 'GPU_CONTROL_TRACE_LOST'}
} catch {
    $state.status='FAIL'
    $state.reason=$_.Exception.Message
} finally {
    if($child -and !$child.HasExited) {
        $live=Get-Process -Id $child.Id -ErrorAction SilentlyContinue
        if($live -and $live.Path -eq $Probe -and $live.StartTime.ToUniversalTime().Ticks -eq $state.probe_start_ticks) {
            Stop-Process -Id $live.Id
        }
    }
    if($state.trace_started) {
        & $TraceTool --stop $session (Join-Path $OutputDirectory 'trace-stop.json') *> (Join-Path $OutputDirectory 'trace-stop.log')
        $state.trace_stop_exit=$LASTEXITCODE
        $state.trace_stopped=($LASTEXITCODE -eq 0)
        if(!$state.trace_stopped) {$state.status='FAIL';$state.reason='GPU_CONTROL_ETW_STOP_FAILED'}
    }
    $state.finished_utc=[DateTime]::UtcNow.ToString('o')
    Save-ControlState
}
if($state.status -ne 'FAIL') {
    & $TraceTool --read $trace (Join-Path $OutputDirectory 'events.jsonl') (Join-Path $OutputDirectory 'read-summary.json') *> (Join-Path $OutputDirectory 'decode.log')
    $state.decode_exit=$LASTEXITCODE
    $state.status=if($LASTEXITCODE -eq 0){'CAPTURE_COMPLETE_UNREVIEWED'}else{'FAIL'}
    if($LASTEXITCODE -ne 0) {$state.reason='GPU_CONTROL_ETW_DECODE_FAILED'}
    Save-ControlState
}
$state | ConvertTo-Json -Depth 5 -Compress
if($state.status -eq 'FAIL') {exit 1}
