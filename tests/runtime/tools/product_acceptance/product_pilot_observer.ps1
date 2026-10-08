# Live parent checks. A result filename alone never proves a successful exit.
function Test-ProductUiaCompletion([string]$Root,[string]$RunId,[string]$Mode,[int]$Cycles,[int]$Seconds) {
    $path=Join-Path $Root 'uia/uia-result.json'
    if(!(Test-Path -LiteralPath $path)){return $false}
    $result=Read-ProductObserverSnapshot $path
    $expected=if($Mode -eq 'Formal'){'UIA_COMPLETE'}else{'PILOT_COMPLETE'}
    if($result.run_id -cne $RunId -or $result.verdict -cne $expected -or
       $result.cycles_requested -isnot [int] -or $result.cycles_requested -ne $Cycles -or
       $result.cycles_completed -isnot [int] -or $result.cycles_completed -ne $Cycles -or
       $result.minimum_seconds -isnot [int] -or $result.minimum_seconds -ne $Seconds){throw 'UIA_TERMINAL_RESULT_INVALID'}
    $identity=Read-ProductObserverSnapshot (Join-Path $Root 'uia/product-identity.json')
    $exit=Read-ProductObserverSnapshot (Join-Path $Root 'uia/process-exit.json')
    if($identity.run_id -cne $RunId -or $identity.pid -isnot [int] -or $identity.pid -le 0 -or
       $exit.run_id -cne $RunId -or $exit.pid -isnot [int] -or $exit.pid -ne $identity.pid -or
       $exit.exit_code -isnot [int] -or $exit.exit_code -ne 0 -or
       $result.product_first_live.pid -ne $identity.pid -or
       $result.product_first_live.start_ticks -ne $identity.start_ticks -or
       $result.product_first_live.executable -cne $identity.executable){throw 'UIA_TERMINAL_EXIT_INVALID'}
    return $true
}
function Assert-ProductEvidenceCollectors($Archive,$Diagnostic,[bool]$Complete,[string]$Root,[string]$RunId,[string]$Mode,[int]$Cycles,[int]$Seconds) {
    foreach($child in @($Archive,$Diagnostic)){
        $child.Refresh()
        if($child.HasExited){
            # Observe exit first, then recheck publication, including a watcher
            # which exits between the caller's last check and this Refresh.
            if(!$Complete){$Complete=Test-ProductUiaCompletion $Root $RunId $Mode $Cycles $Seconds}
            if(!$Complete){throw 'COLLECTOR_STOPPED_BEFORE_UIA_COMPLETION'}
            if($child.ExitCode -isnot [int] -or $child.ExitCode -ne 0){throw 'COLLECTOR_TERMINAL_EXIT_FAILED'}
            if($child -eq $Diagnostic){
                $receipt=Read-ProductObserverSnapshot (Join-Path $Root 'diagnostic-watcher-result.json')
                if($receipt.run_id -cne $RunId -or $receipt.status -cne 'COMPLETE'){throw 'DIAGNOSTIC_TERMINAL_RECEIPT_INVALID'}
            }else{
                $lines=@(Read-ProductPilotCompleteJsonlTail -Path (Join-Path $Root 'checkpoint-archive/collector.jsonl') -Count 1)
                if(!$lines.Count){throw 'ARCHIVE_TERMINAL_RECEIPT_MISSING'}
                $receipt=$lines[0]|ConvertFrom-Json -ErrorAction Stop
                if($receipt.run_id -cne $RunId -or $receipt.status -cne 'COMPLETE' -or
                   $receipt.event -cne 'collector.stopped'){throw 'ARCHIVE_TERMINAL_RECEIPT_INVALID'}
            }
        }
    }
    return $Complete
}
function Assert-ProductGpuHeartbeat($Health,[double]$MissingElapsedSeconds,[long]$NowMilliseconds,$Identity,[long]$MaximumBytes) {
    if(!$Health){if($MissingElapsedSeconds -gt 15){throw 'GPU_ETW_HEARTBEAT_MISSING'};return}
    foreach($field in @('utc_ms','target_pid','maximum_bytes','events_lost','realtime_buffers_lost','query_error')){
        if(($Health.$field -isnot [int] -and $Health.$field -isnot [long]) -or $Health.$field -lt 0){throw 'GPU_ETW_HEARTBEAT_INVALID'}
    }
    if($NowMilliseconds -lt $Health.utc_ms -or $NowMilliseconds-$Health.utc_ms -gt 15000 -or
       $Health.events_lost -ne 0 -or $Health.realtime_buffers_lost -ne 0 -or $Health.query_error -ne 0){throw 'GPU_ETW_STALE_OR_LOSSY'}
    if($Health.maximum_bytes -ne $MaximumBytes){throw 'GPU_ETW_HEARTBEAT_IDENTITY_INVALID'}
    if($Identity){
        # The native collector may still publish one pre-binding sample after
        # product launch. Give discovery the existing 15s bound, then require PID.
        $startMs=([DateTimeOffset]::new([DateTime]::new([long]$Identity.start_ticks,[DateTimeKind]::Utc))).ToUnixTimeMilliseconds()
        if($Health.target_pid -ne $Identity.pid -and
           !($Health.target_pid -eq 0 -and $NowMilliseconds-$startMs -le 15000)){
            throw 'GPU_ETW_HEARTBEAT_IDENTITY_INVALID'
        }
    }
}
function Test-ProductEarlyObservers([string]$Root,[string]$RunId,[string]$Mode,[int]$Cycles,[int]$Seconds,
    $Archive,$Diagnostic,$GpuTrace,[string]$GpuDirectory,$GpuHeartbeatClock,$Identity,[long]$MaximumBytes) {
    $complete=Test-ProductUiaCompletion $Root $RunId $Mode $Cycles $Seconds
    if(!$complete){
        if($GpuTrace.HasExited){throw 'GPU_ETW_COLLECTOR_STOPPED_EARLY'}
        $health=$null
        if(Test-Path -LiteralPath (Join-Path $GpuDirectory 'heartbeat.json')){
            $health=Read-GpuTraceHeartbeat (Join-Path $GpuDirectory 'heartbeat.json')
        }
        Assert-ProductGpuHeartbeat $health $GpuHeartbeatClock.Elapsed.TotalSeconds ([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()) $Identity $MaximumBytes
        if($health){$GpuHeartbeatClock.Restart()}
    }
    return (Assert-ProductEvidenceCollectors $Archive $Diagnostic $complete $Root $RunId $Mode $Cycles $Seconds)
}
