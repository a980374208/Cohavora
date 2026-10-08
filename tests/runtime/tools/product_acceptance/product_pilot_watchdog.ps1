# Shared monotonic run budget and fail-closed live observer decisions.
function Read-ProductObserverSnapshot([string]$Path, [int]$MaximumBytes = 8192) {
    $stream=$null
    try {
        $stream=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,
            ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        if($stream.Length -gt $MaximumBytes){throw 'OBSERVER_SNAPSHOT_TOO_LARGE'}
        $bytes=New-Object byte[] ($MaximumBytes+1)
        $count=0
        while($count -lt $bytes.Length){
            $read=$stream.Read($bytes,$count,$bytes.Length-$count)
            if(!$read){break};$count+=$read
        }
        if($count -gt $MaximumBytes){throw 'OBSERVER_SNAPSHOT_TOO_LARGE'}
        $text=[Text.UTF8Encoding]::new($false,$true).GetString($bytes,0,$count).TrimStart([char]0xFEFF)
        $value=ConvertFrom-Json -InputObject $text -ErrorAction Stop
        if(!$value -or $value -is [Array]){throw 'OBSERVER_SNAPSHOT_INVALID'}
        return $value
    } finally {if($stream){$stream.Dispose()}}
}
function Assert-ProductRunBudget($Budget,[string]$RunId,[int]$MaximumSeconds) {
    $keys=@('schema','run_id','maximum_seconds','start_qpc_ticks','frequency_hz','clock_source')
    if(@($Budget.PSObject.Properties).Count -ne $keys.Count -or
       @($Budget.PSObject.Properties.Name | Where-Object {$_ -notin $keys}).Count -or
       $Budget.schema -isnot [int] -or $Budget.schema -ne 1 -or $Budget.run_id -cne $RunId -or $RunId -cnotmatch '^[a-f0-9]{32}$' -or
       $Budget.maximum_seconds -isnot [int] -or $Budget.maximum_seconds -ne $MaximumSeconds -or $MaximumSeconds -le 0 -or
       $Budget.clock_source -cne 'QueryPerformanceCounter' -or
       ($Budget.start_qpc_ticks -isnot [long] -and $Budget.start_qpc_ticks -isnot [int]) -or $Budget.start_qpc_ticks -le 0 -or
       ($Budget.frequency_hz -isnot [long] -and $Budget.frequency_hz -isnot [int]) -or
       $Budget.frequency_hz -ne [Diagnostics.Stopwatch]::Frequency -or ![Diagnostics.Stopwatch]::IsHighResolution -or
       $Budget.start_qpc_ticks -gt [Diagnostics.Stopwatch]::GetTimestamp()) {throw 'RUN_BUDGET_INVALID'}
}
function Write-ProductRunBudget([string]$Path,[string]$RunId,[int]$MaximumSeconds) {
    $budget=[pscustomobject]@{schema=1;run_id=$RunId;maximum_seconds=$MaximumSeconds;
        start_qpc_ticks=[Diagnostics.Stopwatch]::GetTimestamp();frequency_hz=[Diagnostics.Stopwatch]::Frequency;
        clock_source='QueryPerformanceCounter'}
    Assert-ProductRunBudget $budget $RunId $MaximumSeconds
    $stream=[IO.File]::Open($Path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    try {$bytes=[Text.UTF8Encoding]::new($false).GetBytes(($budget|ConvertTo-Json -Compress));$stream.Write($bytes,0,$bytes.Length)}
    finally {$stream.Dispose()}
    return $budget
}
function Read-ProductRunBudget([string]$Path,[string]$RunId,[int]$MaximumSeconds) {
    $budget=Read-ProductObserverSnapshot $Path
    Assert-ProductRunBudget $budget $RunId $MaximumSeconds
    return $budget
}
function Get-ProductRunBudgetElapsed($Budget) {
    $now=[Diagnostics.Stopwatch]::GetTimestamp()
    if($now -lt $Budget.start_qpc_ticks -or $Budget.frequency_hz -ne [Diagnostics.Stopwatch]::Frequency){throw 'RUN_CLOCK_REGRESSED_OR_CHANGED'}
    return ($now-$Budget.start_qpc_ticks)/[double]$Budget.frequency_hz
}
function Test-ProductFiniteNumber($Value) {
    return (($Value -is [double] -or $Value -is [single] -or $Value -is [int] -or $Value -is [long]) -and
        ![double]::IsNaN([double]$Value) -and ![double]::IsInfinity([double]$Value))
}
function Assert-ProductObserverIdentity($Value,$Identity,[string]$RunId) {
    if(!$Value -or !$Identity -or $Value.run_id -cne $RunId -or $Identity.run_id -cne $RunId -or
       $Identity.pid -isnot [int] -or $Value.pid -isnot [int] -or $Value.pid -le 0 -or $Value.pid -ne $Identity.pid -or
       ($Value.start_ticks -isnot [long] -and $Value.start_ticks -isnot [int]) -or $Value.start_ticks -le 0 -or
       $Value.start_ticks -ne $Identity.start_ticks -or $Value.executable -isnot [string] -or
       $Value.executable -cne $Identity.executable -or ![IO.Path]::IsPathRooted($Value.executable)) {throw 'OBSERVER_PRODUCT_IDENTITY_INVALID'}
}
function Get-ProductObserverAgeMilliseconds([string]$Utc,[long]$NowMilliseconds) {
    $stamp=[DateTimeOffset]::Parse($Utc,[Globalization.CultureInfo]::InvariantCulture)
    if($stamp.Offset -ne [TimeSpan]::Zero){throw 'OBSERVER_UTC_INVALID'}
    $age=$NowMilliseconds-$stamp.ToUnixTimeMilliseconds()
    if($age -lt 0){throw 'OBSERVER_CLOCK_REGRESSED'}
    return $age
}
function Assert-ProductActionProgress($Action,[string]$RunId,[double]$LaunchElapsedSeconds,[long]$NowMilliseconds,
    $LifetimeProgress,$Identity,[int]$Cycles,[int]$RequiredSeconds,$LiveProduct) {
    if(!$Action){if($LaunchElapsedSeconds -gt 400){throw 'UIA_STARTUP_WATCHDOG_TIMEOUT'};return}
    if($Action.run_id -cne $RunId -or $Action.pid -isnot [int] -or $Action.pid -le 0){throw 'UIA_ACTION_IDENTITY_INVALID'}
    $idle=Get-ProductObserverAgeMilliseconds $Action.utc $NowMilliseconds
    if($idle -le 400000){return}
    # The final immutable complete frame bridges the remote exit fence before
    # process_exit is appended. It expires under the same 400s progress budget;
    # it never refreshes an old exit action or proves that an exited PID is live.
    if(!$LifetimeProgress -or $Action.action -eq 'process_exit'){throw 'UIA_OPERATION_WATCHDOG_TIMEOUT'}
    Assert-ProductObserverIdentity $LifetimeProgress $Identity $RunId
    $keys=@('schema','run_id','pid','start_ticks','executable','cycle','cycles','required_seconds',
        'live_elapsed_seconds','run_clock_elapsed_seconds','utc','phase')
    if(@($LifetimeProgress.PSObject.Properties).Count -ne $keys.Count -or
       @($LifetimeProgress.PSObject.Properties.Name | Where-Object {$_ -notin $keys}).Count -or
       $LifetimeProgress.schema -isnot [int] -or $LifetimeProgress.schema -ne 1 -or $LifetimeProgress.phase -cnotin @('waiting','complete') -or
       $Action.pid -ne $Identity.pid -or
       $LifetimeProgress.cycle -isnot [int] -or $LifetimeProgress.cycle -ne $Cycles -or $Action.cycle -isnot [int] -or $Action.cycle -ne $Cycles -or
       $LifetimeProgress.cycles -isnot [int] -or $LifetimeProgress.cycles -ne $Cycles -or
       $LifetimeProgress.required_seconds -isnot [int] -or $LifetimeProgress.required_seconds -ne $RequiredSeconds -or
       !(Test-ProductFiniteNumber $LifetimeProgress.live_elapsed_seconds) -or $LifetimeProgress.live_elapsed_seconds -lt 0 -or
       (($LifetimeProgress.phase -ceq 'waiting') -ne ($LifetimeProgress.live_elapsed_seconds -lt $RequiredSeconds)) -or
       !(Test-ProductFiniteNumber $LifetimeProgress.run_clock_elapsed_seconds) -or
       $LifetimeProgress.run_clock_elapsed_seconds -lt $LifetimeProgress.live_elapsed_seconds -or
       !(Test-ProductFiniteNumber $Action.elapsed_seconds) -or $Action.elapsed_seconds -gt $LifetimeProgress.run_clock_elapsed_seconds -or
       (Get-ProductObserverAgeMilliseconds $LifetimeProgress.utc $NowMilliseconds) -gt 400000) {throw 'UIA_OPERATION_WATCHDOG_TIMEOUT'}
    if(!$LiveProduct){throw 'LIFETIME_PROGRESS_PRODUCT_NOT_LIVE'}
    $LiveProduct.Refresh()
    if($LiveProduct.HasExited -or $LiveProduct.Id -ne $Identity.pid -or
       $LiveProduct.StartTime.ToUniversalTime().Ticks -ne $Identity.start_ticks -or
       $LiveProduct.Path -cne $Identity.executable){throw 'LIFETIME_PROGRESS_PRODUCT_IDENTITY_CHANGED'}
}
function Assert-ProductNativeHealth($Health,[string]$RunId,$Identity,[double]$MissingElapsedSeconds,[long]$NowMilliseconds,[bool]$Exiting) {
    if(!$Health){if(!$Exiting -and $MissingElapsedSeconds -gt 15){throw 'PROCESS_PROBE_MISSING_OR_INVALID'};return}
    if($Health.run_id -cne $RunId -or !$Identity -or $Identity.run_id -cne $RunId -or
       $Health.gpu_budget.pid -isnot [int] -or $Health.gpu_budget.pid -ne $Identity.pid -or
       ($Health.utc_ms -isnot [long] -and $Health.utc_ms -isnot [int])){throw 'PROCESS_PROBE_IDENTITY_INVALID'}
    if(!$Exiting -and ($NowMilliseconds-$Health.utc_ms -gt 15000 -or $NowMilliseconds -lt $Health.utc_ms)){throw 'PROCESS_PROBE_STALE'}
    foreach($section in @('history','diagnostic')){
        $fields=if($section -eq 'history'){@('queue_drops','pending_records_dropped','write_failures')}else{@('dropped_ordinary','dropped_critical','sink_failures')}
        foreach($name in $fields){
            $value=$Health.$section.$name
            if(($value -isnot [long] -and $value -isnot [int]) -or $value -lt 0){throw 'PROCESS_PROBE_HEALTH_INVALID'}
            if($value -ne 0){throw 'LIVE_ZERO_LOSS_GATE_FAILED'}
        }
    }
}
function Assert-ProductResourceHealth($Resource,[string]$RunId,$Identity,[double]$MissingElapsedSeconds,[long]$NowMilliseconds,[bool]$Exiting) {
    if($Exiting){return}
    if(!$Resource){if($MissingElapsedSeconds -gt 10){throw 'RESOURCE_SAMPLE_MISSING'};return}
    Assert-ProductObserverIdentity $Resource $Identity $RunId
    if((Get-ProductObserverAgeMilliseconds $Resource.utc $NowMilliseconds) -gt 10000){throw 'RESOURCE_SAMPLE_STALE'}
    if($Resource.process_alive -ne $true){throw 'RESOURCE_PRODUCT_STOPPED_EARLY'}
}
function Invoke-ProductCollectorCleanup($Children,$GpuTrace,[string]$GpuTraceTool,[string]$GpuSession,[string]$GpuDirectory) {
    $rows=[Collections.Generic.List[object]]::new()
    $errors=[Collections.Generic.List[string]]::new()
    if($GpuTrace){try{[IO.File]::WriteAllText((Join-Path $GpuDirectory 'stop'),'stop')}catch{$errors.Add('GPU_STOP_FILE_FAILED')}}
    foreach($child in $Children){
        $forced=$false;$code=$null;$errorCode=$null;$childId=$null
        try {
            $childId=$child.Id
            $forced=!$child.WaitForExit(10000)
            if($forced){
                if($GpuTrace -and $child.Id -eq $GpuTrace.Id){
                    try{& $GpuTraceTool --stop $GpuSession (Join-Path $GpuDirectory 'forced-trace-stop.json');if($LASTEXITCODE){$errors.Add('GPU_SESSION_STOP_FAILED')}}catch{$errors.Add('GPU_SESSION_STOP_FAILED')}
                    $null=$child.WaitForExit(5000)
                }
                $child.Refresh()
                if(!$child.HasExited){$child.Kill()}
                if(!$child.WaitForExit(5000)){throw 'COLLECTOR_KILL_TIMEOUT'}
            }
            $child.Refresh();$code=$child.ExitCode
        } catch {
            # Continue all remaining owned Process objects even if this cleanup fails.
            $errorCode='COLLECTOR_CLEANUP_FAILED';$errors.Add($errorCode)
            try{
                $child.Refresh()
                if(!$child.HasExited){$child.Kill();$forced=$true;$null=$child.WaitForExit(5000)}
                $child.Refresh();if($child.HasExited){$code=$child.ExitCode}
            }catch{}
        }
        $rows.Add([ordered]@{pid=$childId;exit_code=$code;forced_stop=$forced;cleanup_error=$errorCode})
    }
    return [pscustomobject]@{collectors=@($rows.ToArray());errors=@($errors.ToArray());passed=($errors.Count -eq 0 -and @($rows|Where-Object {$_.forced_stop -or $null -eq $_.exit_code -or $_.exit_code -ne 0}).Count -eq 0)}
}
