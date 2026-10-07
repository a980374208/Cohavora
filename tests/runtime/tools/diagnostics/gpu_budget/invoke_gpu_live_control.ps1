param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)][string]$Probe,
    [Parameter(Mandatory=$true)][string]$TraceTool
)
$ErrorActionPreference='Stop'
$Probe=(Resolve-Path -LiteralPath $Probe).Path
$TraceTool=(Resolve-Path -LiteralPath $TraceTool).Path
if((Split-Path (Split-Path $Probe) -Leaf) -ne 'RelWithDebInfo' -or
   (Split-Path (Split-Path $TraceTool) -Leaf) -ne 'RelWithDebInfo'){throw 'GPU_CONTROL_RELWITHDEBINFO_REQUIRED'}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $OutputDirectory){throw 'GPU_CONTROL_OUTPUT_MUST_BE_NEW'}
$null=New-Item -ItemType Directory -Path $OutputDirectory
$session='B14-Gpu-Release-'+[guid]::NewGuid().ToString('N')
$trace=$null;$probeProcess=$null
$state=[ordered]@{schema=1;diagnostic_only=$true;release_eligible=$false;configuration='RelWithDebInfo';
    session=$session;deliberately_late_pid_binding_seconds=5;started_utc=[DateTime]::UtcNow.ToString('o');
    probe_sha256=(Get-FileHash $Probe).Hash.ToLowerInvariant();trace_tool_sha256=(Get-FileHash $TraceTool).Hash.ToLowerInvariant()}
try {
    $trace=Start-Process $TraceTool -WindowStyle Hidden -PassThru -ArgumentList @('--live',$session,('"'+$OutputDirectory+'"'),50,134217728)
    $null=$trace.Handle
    $deadline=[DateTime]::UtcNow.AddSeconds(10)
    while(!(Test-Path "$OutputDirectory/trace-ready.json")){
        if($trace.HasExited -or [DateTime]::UtcNow -gt $deadline){throw 'GPU_LIVE_CONTROL_NOT_READY'}
        Start-Sleep -Milliseconds 100
    }
    $probeProcess=Start-Process $Probe -WindowStyle Hidden -PassThru -ArgumentList @(('"'+$OutputDirectory+'/process-device-lifecycle.jsonl"'),'--release-control')
    $null=$probeProcess.Handle
    $state.probe_pid=$probeProcess.Id
    $state.probe_start_ticks=$probeProcess.StartTime.ToUniversalTime().Ticks
    # Exercise persistence of owned births and packets before PID publication.
    Start-Sleep -Seconds 5
    [IO.File]::WriteAllText("$OutputDirectory/target-pid.txt.tmp",[string]$probeProcess.Id)
    Move-Item "$OutputDirectory/target-pid.txt.tmp" "$OutputDirectory/target-pid.txt"
    if(!$probeProcess.WaitForExit(20000)){throw 'GPU_LIVE_CONTROL_PROBE_TIMEOUT'}
    $probeProcess.Refresh();$state.probe_exit=$probeProcess.ExitCode
    if($probeProcess.ExitCode -ne 0){throw 'GPU_LIVE_CONTROL_PROBE_FAILED'}
    Start-Sleep -Seconds 2
    $state.status='CAPTURE_COMPLETE_UNREVIEWED'
} catch {$state.status='FAIL';$state.reason=$_.Exception.Message}
finally {
    [IO.File]::WriteAllText("$OutputDirectory/stop",'controlled completion')
    if($trace){
        if(!$trace.WaitForExit(10000)){
            & $TraceTool --stop $session "$OutputDirectory/forced-trace-stop.json"
            if(!$trace.WaitForExit(5000)){Stop-Process -Id $trace.Id}
            $state.status='FAIL';$state.reason='GPU_LIVE_CONTROL_TRACE_STOP_TIMEOUT'
        }
        $trace.Refresh();$state.trace_exit=$trace.ExitCode
        if($trace.ExitCode -ne 0){$state.status='FAIL'}
    }
    if($probeProcess -and !$probeProcess.HasExited){Stop-Process -Id $probeProcess.Id;$state.status='FAIL'}
    $state.finished_utc=[DateTime]::UtcNow.ToString('o')
    $state | ConvertTo-Json -Depth 4 | Set-Content "$OutputDirectory/control-state.json" -Encoding UTF8
}
$state | ConvertTo-Json -Compress
if($state.status -eq 'FAIL'){exit 1}
