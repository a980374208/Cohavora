param(
    [Parameter(Mandatory=$true)][string]$UiaDirectory,
    [Parameter(Mandatory=$true)][string]$Destination,
    [Parameter(Mandatory=$true)][string]$RunId,
    [int]$MaximumSeconds = 600,
    [string]$AudioCollector = ''
)
$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $Destination) { throw 'Evidence destination must be new' }
$writer = [IO.StreamWriter]::new($Destination, $false, [Text.UTF8Encoding]::new($false))
$writer.AutoFlush = $true
$deadline = [DateTime]::UtcNow.AddSeconds($MaximumSeconds)
$sequence = 0
$lastCpu = $null
$lastTime = $null
$observed = $false
$audioProcess = $null
try {
    while ([DateTime]::UtcNow -lt $deadline) {
        $actionPath = Join-Path $UiaDirectory 'uia-actions.jsonl'
        if (!(Test-Path -LiteralPath $actionPath)) { Start-Sleep -Milliseconds 200; continue }
        # Read only complete JSONL records while the UIA runner is appending.
        $actions = [IO.File]::ReadAllText($actionPath).Split("`n")
        $action = $null
        for ($i=$actions.Length-2; $i -ge 0; --$i) {
            if ($actions[$i].Trim()) { $action = $actions[$i] | ConvertFrom-Json; break }
        }
        if (!$action -or !$action.pid) { Start-Sleep -Milliseconds 200; continue }
        if ($action.run_id -ne $RunId) { throw 'run identity mismatch' }
        if ($AudioCollector -and !$audioProcess) {
            $audioOutput = Join-Path (Split-Path -Parent $Destination) 'product-audio.jsonl'
            $audioProcess = Start-Process -FilePath $AudioCollector -WindowStyle Hidden -PassThru -ArgumentList @(
                [string]$action.pid, $RunId, ('"' + $audioOutput + '"'), [string]$MaximumSeconds)
        }
        $now = [DateTime]::UtcNow
        $process = Get-Process -Id $action.pid -ErrorAction SilentlyContinue
        $gpuLocal = $null; $gpuShared = $null; $gpuReason = 'process_exited'
        $cpuPct = $null
        if ($process) {
            $observed = $true
            $cpu = $process.TotalProcessorTime.TotalSeconds
            if ($null -ne $lastCpu) {
                $cpuPct = 100 * ($cpu-$lastCpu) / ($now-$lastTime).TotalSeconds / [Environment]::ProcessorCount
            }
            $lastCpu=$cpu; $lastTime=$now
            try {
                $gpu = @(Get-CimInstance -ClassName Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory |
                    Where-Object { $_.Name -like "pid_$($action.pid)_*" })
                if ($gpu.Count -gt 0) {
                    $gpuLocal = [long](($gpu | Measure-Object -Property DedicatedUsage -Sum).Sum)
                    $gpuShared = [long](($gpu | Measure-Object -Property SharedUsage -Sum).Sum)
                    $gpuReason = 'wddm_process_counters'
                } else { $gpuReason = 'no_matching_wddm_process_instance' }
            } catch { $gpuReason = 'wddm_counter_unavailable' }
        }
        $sequence++
        $row = [ordered]@{schema=1;run_id=$RunId;sequence=$sequence;collector='external';
            utc=$now.ToString('o');pid=$action.pid;cycle=$action.cycle;
            operation_id=$action.operation_id;action=$action.action;phase=$action.phase;
            process_alive=($null -ne $process);
            private_bytes=$(if ($process) {$process.PrivateMemorySize64} else {0});
            handles=$(if ($process) {$process.HandleCount} else {0});
            threads=$(if ($process) {$process.Threads.Count} else {0});cpu_pct=$cpuPct;
            cpu_seconds=$(if ($process) {$process.TotalProcessorTime.TotalSeconds} else {$null});
            logical_processors=[Environment]::ProcessorCount;
            gpu_dedicated_bytes=$gpuLocal;gpu_shared_bytes=$gpuShared;gpu_availability=$gpuReason;
            queue_depth=$null;queue_reason='requires_in_process_probe';
            wgc_owned_handles=$null;wgc_reason='ownership_not_observed'}
        $writer.WriteLine(($row | ConvertTo-Json -Compress))
        if ($observed -and !$process) { break }
        Start-Sleep -Seconds 1
    }
    if (!$observed) { throw 'product_process_not_observed' }
} finally { $writer.Dispose() }
