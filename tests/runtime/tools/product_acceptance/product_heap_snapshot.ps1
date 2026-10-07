# Diagnostic-only, directly launched product snapshots. No UST/IFEO/GC/trim changes.
# A receipt proves capture and detach, not normal heap flags or runtime acceptance.
function New-ProductHeapSnapshotClock { return [Diagnostics.Stopwatch]::StartNew() }
function Get-ProductHeapSnapshotProcess([int]$TargetPid) {
    return (Get-Process -Id $TargetPid -ErrorAction Stop)
}
function Assert-ProductHeapSnapshotIdentity($Process, $Identity) {
    $Process.Refresh()
    if ($Process.HasExited -or $Process.Id -ne $Identity.pid -or
        $Process.StartTime.ToUniversalTime().Ticks -ne $Identity.start_ticks -or
        ![string]::Equals($Process.Path, $Identity.executable, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'NORMAL_HEAP_SNAPSHOT_IDENTITY_CHANGED_OR_EXITED'
    }
}
function Assert-ProductHeapSnapshotTime($Clock) {
    if ($Clock.Elapsed.TotalSeconds -ge 10) { throw 'NORMAL_HEAP_SNAPSHOT_TIMEOUT' }
}
function Assert-ProductHeapSnapshotHelperOwner($Process, $Owner) {
    # The retained Start-Process handle plus PID/start time establishes ownership
    # even before MainModule/Path becomes available, or after it disappears.
    $Process.Refresh()
    if ($Process.Id -ne $Owner.pid -or $Process.StartTime.ToUniversalTime().Ticks -ne $Owner.start_ticks) {
        throw 'NORMAL_HEAP_SNAPSHOT_HELPER_START_IDENTITY_CHANGED'
    }
    $path = $Process.Path
    if ($path -and ![string]::Equals($path, $Owner.executable, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'NORMAL_HEAP_SNAPSHOT_HELPER_START_IDENTITY_INVALID'
    }
}
function Get-ProductHeapSnapshotLength([string]$Path) {
    if (!(Test-Path -LiteralPath $Path)) { return [long]0 }
    return [long](Get-Item -LiteralPath $Path -ErrorAction Stop).Length
}
function Initialize-ProductHeapSnapshotPrivateDirectory([string]$Path) {
    $null = [IO.Directory]::CreateDirectory($Path)
    $acl = [Security.AccessControl.DirectorySecurity]::new()
    $acl.SetAccessRuleProtection($true, $false)
    foreach ($sid in @([Security.Principal.WindowsIdentity]::GetCurrent().User,
            [Security.Principal.SecurityIdentifier]::new('S-1-5-18'))) {
        $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid,
            [Security.AccessControl.FileSystemRights]::FullControl,
            [Security.AccessControl.InheritanceFlags]'ContainerInherit,ObjectInherit',
            [Security.AccessControl.PropagationFlags]::None,
            [Security.AccessControl.AccessControlType]::Allow))
    }
    Set-Acl -LiteralPath $Path -AclObject $acl -ErrorAction Stop
}
function Get-ProductHeapSnapshotHash([string]$Path, $Clock) {
    $stream = $null; $sha = $null
    try {
        # CDB has exited: require an exclusive reader and a real full-memory minidump.
        $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
        if ($stream.Length -lt 32 -or $stream.Length -gt 2147483648) { throw 'NORMAL_HEAP_SNAPSHOT_DUMP_SIZE_INVALID' }
        $header = New-Object byte[] 32
        if ($stream.Read($header, 0, 32) -ne 32 -or [BitConverter]::ToUInt32($header, 0) -ne 0x504d444d -or
            ([BitConverter]::ToUInt64($header, 24) -band 2) -eq 0) { throw 'NORMAL_HEAP_SNAPSHOT_NOT_FULL_MEMORY_DUMP' }
        $stream.Position = 0
        $sha = [Security.Cryptography.SHA256]::Create(); $buffer = New-Object byte[] 1048576
        while ($true) {
            Assert-ProductHeapSnapshotTime $Clock
            $count = $stream.Read($buffer, 0, $buffer.Length)
            if (!$count) { break }
            $null = $sha.TransformBlock($buffer, 0, $count, $buffer, 0)
        }
        $null = $sha.TransformFinalBlock([byte[]]@(), 0, 0)
        Assert-ProductHeapSnapshotTime $Clock
        return ([BitConverter]::ToString($sha.Hash).Replace('-', '').ToLowerInvariant())
    } finally { if ($stream) { $stream.Dispose() }; if ($sha) { $sha.Dispose() } }
}
function Invoke-ProductNormalHeapSnapshot {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory=$true)][pscustomobject]$Identity,
        [Parameter(Mandatory=$true)][string]$Prefix,
        [Parameter(Mandatory=$true)][string]$CdbExecutable
    )
    $workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
    $outRoot = [IO.Path]::GetFullPath((Join-Path $workspace 'out'))
    if (($Identity.pid -isnot [int] -and $Identity.pid -isnot [long]) -or $Identity.pid -le 0 -or
        $Identity.pid -gt [int]::MaxValue -or
        ($Identity.start_ticks -isnot [long] -and $Identity.start_ticks -isnot [int]) -or $Identity.start_ticks -le 0 -or
        $Identity.run_id -isnot [string] -or $Identity.run_id -cnotmatch '^[0-9a-f]{32}$' -or
        $Identity.executable -isnot [string] -or ![IO.Path]::IsPathRooted($Identity.executable)) {
        throw 'NORMAL_HEAP_SNAPSHOT_IDENTITY_INVALID'
    }
    if (![IO.Path]::IsPathRooted($Prefix) -or $Prefix -match '[^\x20-\x7e]|"') {
        throw 'NORMAL_HEAP_SNAPSHOT_PREFIX_INVALID'
    }
    $Prefix = [IO.Path]::GetFullPath($Prefix)
    if (!$Prefix.StartsWith($outRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
        $Prefix.StartsWith('\\') -or ([IO.DriveInfo]::new([IO.Path]::GetPathRoot($Prefix))).DriveType -ne [IO.DriveType]::Fixed) {
        throw 'NORMAL_HEAP_SNAPSHOT_PREFIX_NOT_LOCAL_OUT'
    }
    if (![IO.Path]::IsPathRooted($CdbExecutable) -or !(Test-Path -LiteralPath $CdbExecutable -PathType Leaf)) {
        throw 'NORMAL_HEAP_SNAPSHOT_CDB_MISSING'
    }
    $CdbExecutable = [IO.Path]::GetFullPath($CdbExecutable)
    $private = $Prefix + '.capture-private'; $dump = Join-Path $private 'snapshot.full.dmp'
    $receiptPath = $Prefix + '.capture.json'; $commands = $Prefix + '.capture.commands'
    $logPath = $Prefix + '.capture.log'; $stdout = $Prefix + '.capture.stdout'; $stderr = $Prefix + '.capture.stderr'
    $lockPath = $Prefix + '.capture.lock'
    foreach ($existing in @($private, $receiptPath, $commands, $logPath, $stdout, $stderr, $lockPath, ($Prefix+'.full.dmp'))) {
        if (Test-Path -LiteralPath $existing) { throw 'NORMAL_HEAP_SNAPSHOT_ALREADY_EXISTS' }
    }
    # Reject directory junctions/symlinks before creating artifacts or invoking CDB.
    $ancestor = [IO.Path]::GetDirectoryName($Prefix)
    while ($ancestor) {
        if (Test-Path -LiteralPath $ancestor) {
            $item = Get-Item -LiteralPath $ancestor -Force -ErrorAction Stop
            if (!$item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                throw 'NORMAL_HEAP_SNAPSHOT_REPARSE_OR_NON_DIRECTORY'
            }
        }
        $ancestor = [IO.Path]::GetDirectoryName($ancestor)
    }
    $expected = [pscustomobject]@{pid=[int]$Identity.pid;start_ticks=[long]$Identity.start_ticks;
        executable=[IO.Path]::GetFullPath($Identity.executable);run_id=$Identity.run_id}
    $target = $null; $capture = $null; $clock = $null; $captureOwner = $null; $captureIdentity = $null; $lock = $null; $failure = $null
    $record = [ordered]@{schema=1;status='FAILED';identity=$expected;instrumented_diagnostic=$true;
        release_eligible=$false;formal_credit=0;qualification_credit=0;normal_heap_flags_verified=$false;
        normal_heap_flags_verification='REQUIRES_OFFLINE_ANALYSIS';capture_limit_seconds=10;maximum_dump_bytes=[long]2147483648;
        dump_path=$dump;capture_log=$logPath;capture_stdout=$stdout;capture_stderr=$stderr;
        product_launched_by_module=$false;target_initial_launch_mode='NOT_VERIFIED_BY_SNAPSHOT_MODULE';
        ust_changed=$false;ifeo_changed=$false;forced_gc=$false;working_set_trim=$false;
        same_process_alive_after_detach=$false;capture_exit_code=$null;capture_helper_cleaned=$false}
    try {
        $target = Get-ProductHeapSnapshotProcess $expected.pid
        Assert-ProductHeapSnapshotIdentity $target $expected
        $null = $target.Handle
        $null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Prefix))
        $lock = [IO.File]::Open($lockPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        Initialize-ProductHeapSnapshotPrivateDirectory $private
        @('.echo B14_HEAP_SNAPSHOT_BEGIN', '!gflag', ('.dump /ma "'+$dump+'"'),
            '.echo B14_HEAP_SNAPSHOT_COMPLETE', 'qd') | Set-Content -LiteralPath $commands -Encoding ASCII -ErrorAction Stop
        $symbolDirs = @((Split-Path $expected.executable))
        $symbolRoot = Join-Path $outRoot 'debug-symbols/ntdll.pdb'
        if (Test-Path -LiteralPath $symbolRoot) {
            $symbolDirs += @(Get-ChildItem -LiteralPath $symbolRoot -Directory -ErrorAction Stop |
                Where-Object { !($_.Attributes -band [IO.FileAttributes]::ReparsePoint) } | Select-Object -ExpandProperty FullName)
        }
        $record.started_utc = [DateTimeOffset]::UtcNow.ToString('o')
        $clock = New-ProductHeapSnapshotClock
        Assert-ProductHeapSnapshotIdentity $target $expected
        # 10.0.17763.132 advertises -netsym:no in help but rejects it as an
        # invalid switch. -sins ignores inherited symbol-server paths; -y
        # supplies only the existing local binary/symbol directories above.
        $capture = Start-Process -FilePath $CdbExecutable -WindowStyle Hidden -PassThru `
            -ArgumentList @('-pv','-sins','-y',('"'+($symbolDirs -join ';')+'"'),
                '-p',[string]$expected.pid,'-cf',('"'+$commands+'"'),'-logo',('"'+$logPath+'"')) `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr -ErrorAction Stop
        $null = $capture.Handle
        $captureOwner = [pscustomobject]@{pid=$capture.Id;start_ticks=$capture.StartTime.ToUniversalTime().Ticks;executable=$CdbExecutable}
        $captureIdentity = [pscustomobject]@{pid=$captureOwner.pid;start_ticks=$captureOwner.start_ticks;
            executable=$CdbExecutable;observed_executable=$null;path_verified=$false;
            ownership='Start-Process retained handle and PID/start_ticks'}
        $record.capture_identity = $captureIdentity
        # Process.Path can briefly be null while a real CDB is still starting.
        # Wait inside the same capture budget; an already exited CDB is handled
        # by its exit status/output instead of treating a missing Path as failure.
        while ($true) {
            Assert-ProductHeapSnapshotTime $clock
            if ($capture.WaitForExit(0)) { break }
            Assert-ProductHeapSnapshotHelperOwner $capture $captureOwner
            $observedPath = $capture.Path
            if ($observedPath) {
                $captureIdentity.observed_executable = $observedPath
                $captureIdentity.path_verified = $true
                break
            }
            Assert-ProductHeapSnapshotIdentity $target $expected
            if ($capture.WaitForExit(25)) { break }
        }
        $exited = $capture.WaitForExit(0)
        while (!$exited) {
            Assert-ProductHeapSnapshotTime $clock
            if ((Get-ProductHeapSnapshotLength $dump) -gt 2147483648) { throw 'NORMAL_HEAP_SNAPSHOT_DUMP_SIZE_INVALID' }
            $target.Refresh()
            if ($target.HasExited) { throw 'NORMAL_HEAP_SNAPSHOT_TARGET_EXITED_DURING_CAPTURE' }
            $exited = $capture.WaitForExit(50)
        }
        $capture.Refresh(); $record.capture_exit_code = $capture.ExitCode
        Assert-ProductHeapSnapshotTime $clock
        if ($capture.ExitCode -ne 0) { throw ('NORMAL_HEAP_SNAPSHOT_CDB_FAILED: exit=' + $capture.ExitCode) }
        $log = Get-Content -LiteralPath $logPath -Raw -ErrorAction Stop
        if ($log -notmatch '(?m)^Dump successfully written\r?$' -or
            $log -notmatch '(?m)^B14_HEAP_SNAPSHOT_COMPLETE\r?$') { throw 'NORMAL_HEAP_SNAPSHOT_DUMP_NOT_CONFIRMED' }
        Assert-ProductHeapSnapshotIdentity $target $expected
        $record.dump_size_bytes = Get-ProductHeapSnapshotLength $dump
        if ($record.dump_size_bytes -le 0 -or $record.dump_size_bytes -gt 2147483648) { throw 'NORMAL_HEAP_SNAPSHOT_DUMP_SIZE_INVALID' }
        $record.dump_sha256 = Get-ProductHeapSnapshotHash $dump $clock
        Assert-ProductHeapSnapshotIdentity $target $expected
        Assert-ProductHeapSnapshotTime $clock
        $record.same_process_alive_after_detach = $true
        $record.status = 'CAPTURED_DIAGNOSTIC_ONLY'
    } catch { $failure = $_; $record.error = $_.Exception.Message }
    finally {
        if ($capture) {
            try {
                $capture.Refresh()
                if (!$capture.WaitForExit(0)) {
                    # Keep the Start-Process handle and verify exact ownership before Kill.
                    Assert-ProductHeapSnapshotHelperOwner $capture $captureOwner
                    $capture.Kill()
                    if (!$capture.WaitForExit(500)) { throw 'NORMAL_HEAP_SNAPSHOT_HELPER_CLEANUP_TIMEOUT' }
                }
                $record.capture_helper_cleaned = $true
            } catch {
                $record.cleanup_error = $_.Exception.Message
                if (!$failure) { $failure = $_; $record.error = $_.Exception.Message }
                $record.status = 'FAILED'
            }
            $capture.Dispose()
        }
        if ($target) { $target.Dispose() }
        $record.finished_utc = [DateTimeOffset]::UtcNow.ToString('o')
        if ($clock) {
            $record.capture_elapsed_seconds = $clock.Elapsed.TotalSeconds
            if ($record.capture_elapsed_seconds -ge 10 -and !$failure) {
                $failure = [InvalidOperationException]::new('NORMAL_HEAP_SNAPSHOT_TIMEOUT')
                $record.error = $failure.Message
            }
        }
        if ($failure) { $record.status = 'FAILED' }
        if ($lock) {
            try { $record | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $receiptPath -Encoding UTF8 -ErrorAction Stop }
            finally { $lock.Dispose() }
        }
    }
    if ($failure) { throw $failure }
    $receipt = [pscustomobject]$record
    $receipt.PSObject.TypeNames.Insert(0, 'Product.NormalHeapSnapshotReceipt')
    return $receipt
}
