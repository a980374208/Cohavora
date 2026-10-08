param(
    [Parameter(Mandatory=$true)][string]$UiaDirectory,
    [Parameter(Mandatory=$true)][string]$Destination,
    [Parameter(Mandatory=$true)][string]$RunId,
    [int]$MaximumSeconds = 600,
    [string]$AudioCollector = '',
    [string]$RunBudgetPath = ''
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'product_pilot_probe_tail.ps1')
function Read-ProductPilotResourceAction([string]$Path) {
    $lines = @(Read-ProductPilotCompleteJsonlTail -Path $Path -Count 1 -MaximumBytes 1048576)
    if (!$lines.Count) { return $null }
    $action = $lines[0] | ConvertFrom-Json -ErrorAction Stop
    if ($action -isnot [pscustomobject] -or $action.schema -ne 1 -or
        $action.utc -isnot [string] -or $action.run_id -isnot [string] -or
        ($action.pid -isnot [int] -and $action.pid -isnot [long]) -or $action.pid -le 0) {
        throw 'RESOURCE_UIA_ACTION_RECORD_INVALID'
    }
    return $action
}
function Read-ProductPilotResourceIdentity([string]$Path) {
    $stream = $null
    try {
        $share = [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete
        $stream = [IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,$share)
        [long]$length = $stream.Length
        if ($length -le 0 -or $length -gt 65536) { throw 'RESOURCE_PRODUCT_IDENTITY_SIZE_INVALID' }
        $bytes = New-Object byte[] ([int]$length)
        [int]$readTotal = 0
        while ($readTotal -lt $length) {
            [int]$read = $stream.Read($bytes,$readTotal,[int]$length-$readTotal)
            if ($read -eq 0) { throw 'RESOURCE_PRODUCT_IDENTITY_TRUNCATED' }
            $readTotal += $read
        }
        [int]$start = 0
        if ($length -ge 3 -and $bytes[0] -eq 239 -and $bytes[1] -eq 187 -and $bytes[2] -eq 191) { $start=3 }
        $utf8 = [Text.UTF8Encoding]::new($false,$true)
        $identity = $utf8.GetString($bytes,$start,[int]$length-$start) | ConvertFrom-Json -ErrorAction Stop
        return $identity
    } finally { if ($stream) { $stream.Dispose() } }
}
function Assert-ProductPilotResourceIdentity($Action,$Identity,$Process,[string]$ExpectedRunId) {
    if ($Identity -isnot [pscustomobject] -or $Identity.run_id -cne $ExpectedRunId -or
        ($Identity.pid -isnot [int] -and $Identity.pid -isnot [long]) -or $Identity.pid -le 0 -or
        ($Identity.start_ticks -isnot [long] -and $Identity.start_ticks -isnot [int]) -or
        $Identity.start_ticks -le 0 -or $Identity.executable -isnot [string] -or
        ![IO.Path]::IsPathRooted($Identity.executable)) { throw 'RESOURCE_PRODUCT_IDENTITY_INVALID' }
    if ($Action.run_id -cne $ExpectedRunId -or $Action.pid -ne $Identity.pid) {
        throw 'RESOURCE_ACTION_PRODUCT_IDENTITY_MISMATCH'
    }
    if ($Process -and ($Process.Id -ne $Identity.pid -or
        $Process.StartTime.ToUniversalTime().Ticks -ne $Identity.start_ticks -or
        ![string]::Equals($Process.Path,$Identity.executable,[StringComparison]::OrdinalIgnoreCase))) {
        throw 'RESOURCE_LIVE_PRODUCT_IDENTITY_MISMATCH'
    }
}
function Initialize-ProductPilotResourceJob {
    if (-not ('ProductPilotResourceOwnerJob' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class ProductPilotResourceOwnerJob {
    [StructLayout(LayoutKind.Sequential)] struct BasicLimits {
        public long ProcessTime, JobTime;
        public uint Flags;
        public UIntPtr MinimumWorkingSet, MaximumWorkingSet;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass, SchedulingClass;
    }
    [StructLayout(LayoutKind.Sequential)] struct IoCounters {
        public ulong ReadOps, WriteOps, OtherOps, ReadBytes, WriteBytes, OtherBytes;
    }
    [StructLayout(LayoutKind.Sequential)] struct ExtendedLimits {
        public BasicLimits Basic;
        public IoCounters Io;
        public UIntPtr ProcessMemory, JobMemory, PeakProcessMemory, PeakJobMemory;
    }
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr CreateJobObject(IntPtr attributes, string name);
    [DllImport("kernel32.dll", SetLastError=true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool SetInformationJobObject(IntPtr job, int informationClass,
        ref ExtendedLimits information, uint informationLength);
    [DllImport("kernel32.dll", SetLastError=true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
    [DllImport("kernel32.dll", SetLastError=true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool IsProcessInJob(IntPtr process, IntPtr job,
        [MarshalAs(UnmanagedType.Bool)] out bool member);
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    // This unnamed handle is not inheritable. Keep the raw handle until OS
    // process teardown: closing it in finally would also kill this owner and
    // overwrite a normal exit. No managed finalizer may close it early.
    static IntPtr ownedJob = IntPtr.Zero;
    static readonly object gate = new object();
    public static void Initialize() {
        lock (gate) {
            if (ownedJob != IntPtr.Zero) return;
            IntPtr candidate = CreateJobObject(IntPtr.Zero, null);
            if (candidate == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "RESOURCE_JOB_CREATE_FAILED");
            bool assigned = false;
            try {
                ExtendedLimits limits = new ExtendedLimits();
                limits.Basic.Flags = 0x2000; // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
                if (!SetInformationJobObject(candidate, 9, ref limits,
                        (uint)Marshal.SizeOf(typeof(ExtendedLimits))))
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "RESOURCE_JOB_LIMIT_FAILED");
                if (!AssignProcessToJobObject(candidate, GetCurrentProcess()))
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "RESOURCE_JOB_ASSIGN_FAILED");
                assigned = true;
                ownedJob = candidate;
            } finally {
                if (!assigned) CloseHandle(candidate);
            }
        }
    }
    public static bool Contains(IntPtr processHandle) {
        bool member;
        if (ownedJob == IntPtr.Zero || !IsProcessInJob(processHandle, ownedJob, out member))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "RESOURCE_JOB_MEMBERSHIP_QUERY_FAILED");
        return member;
    }
}
'@
    }
    [ProductPilotResourceOwnerJob]::Initialize()
}
if ($RunBudgetPath) {
    . (Join-Path $PSScriptRoot 'product_pilot_watchdog.ps1')
    $runBudget = Read-ProductRunBudget -Path $RunBudgetPath -RunId $RunId -MaximumSeconds $MaximumSeconds
} else { $runBudget=$null }
$resourceClock = [Diagnostics.Stopwatch]::StartNew()
function Get-ResourceBudgetElapsed {
    if ($runBudget) { return (Get-ProductRunBudgetElapsed -Budget $runBudget) }
    return $resourceClock.Elapsed.TotalSeconds
}
if (Test-Path -LiteralPath $Destination) { throw 'Evidence destination must be new' }
$writer = [IO.StreamWriter]::new($Destination, $false, [Text.UTF8Encoding]::new($false))
$writer.AutoFlush = $true
$sequence = 0
$lastCpu = $null
$lastTime = $null
$observed = $false
$audioProcess = $null
$resourceFailure = $null
try {
    # Initialize before the first audio child is launched. Incompatible nested
    # jobs fail closed here; no audio process has been started yet.
    if ($AudioCollector) { Initialize-ProductPilotResourceJob }
    while ((Get-ResourceBudgetElapsed) -lt $MaximumSeconds) {
        $actionPath = Join-Path $UiaDirectory 'uia-actions.jsonl'
        if (!(Test-Path -LiteralPath $actionPath)) { Start-Sleep -Milliseconds 200; continue }
        $action = Read-ProductPilotResourceAction $actionPath
        if (!$action) { Start-Sleep -Milliseconds 200; continue }
        $identity = Read-ProductPilotResourceIdentity (Join-Path $UiaDirectory 'product-identity.json')
        $process = Get-Process -Id $action.pid -ErrorAction SilentlyContinue
        if ($process -and $process.HasExited) { $process=$null }
        Assert-ProductPilotResourceIdentity $action $identity $process $RunId
        if ($AudioCollector -and !$audioProcess) {
            $audioOutput = Join-Path (Split-Path -Parent $Destination) 'product-audio.jsonl'
            # Native collector accepts integer seconds. Round only its remaining
            # duration up; the shared QPC deadline remains the controller's gate.
            $audioSeconds = [int][Math]::Ceiling($MaximumSeconds - (Get-ResourceBudgetElapsed))
            if ($audioSeconds -lt 1) { throw 'RESOURCE_AUDIO_START_BUDGET_EXHAUSTED' }
            # Direct CreateProcess inheritance closes the launch-before-receipt
            # window; ShellExecute/brokers must not create an unowned child.
            $audioStart=[Diagnostics.ProcessStartInfo]::new()
            $audioStart.FileName=$AudioCollector
            $audioStart.Arguments=('{0} {1} "{2}" {3}' -f $identity.pid,$RunId,$audioOutput,$audioSeconds)
            $audioStart.UseShellExecute=$false
            $audioStart.CreateNoWindow=$true
            $audioProcess=[Diagnostics.Process]::Start($audioStart)
            $null=$audioProcess.Handle
            if (![ProductPilotResourceOwnerJob]::Contains($audioProcess.Handle)) {
                if (!$audioProcess.HasExited) { $audioProcess.Kill();$null=$audioProcess.WaitForExit(5000) }
                throw 'RESOURCE_AUDIO_JOB_INHERITANCE_FAILED'
            }
        }
        $now = [DateTime]::UtcNow
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
            utc=$now.ToString('o');pid=$identity.pid;start_ticks=$identity.start_ticks;executable=$identity.executable;cycle=$action.cycle;
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
} catch {
    $resourceFailure=$_
    throw
} finally {
    try { $writer.Dispose() } finally {
        # Normal target exit gives WASAPI time to flush its own terminal event.
        # Owner termination closes the job in the kernel, including forced kills
        # that never run this finally block. Preserve any original failure.
        if ($audioProcess) {
            try {
                if (!$audioProcess.WaitForExit(10000)) { throw 'RESOURCE_AUDIO_CHILD_SHUTDOWN_TIMEOUT' }
                $audioProcess.Refresh()
                if ($audioProcess.ExitCode -ne 0) { throw 'RESOURCE_AUDIO_CHILD_FAILED' }
            } catch {
                if (!$resourceFailure) { throw }
                Write-Error ('AUDIO_CLEANUP_FAILED: '+$_.Exception.Message) -ErrorAction Continue
            }
        }
    }
}
