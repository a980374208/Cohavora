param([string]$OutputDirectory = "$PSScriptRoot/../../../out/b14-source63-20261006")
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {
    throw 'WINDOWS_POWERSHELL_51_REQUIRED'
}

# Load only the real function definitions. Never import or run the desktop driver.
$source = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../uia/product_desktop.ps1'))
$sourceBefore = Get-Item -LiteralPath $source
$sourceBytes = [IO.File]::ReadAllBytes($source)
$digest = [Security.Cryptography.SHA256]::Create()
try { $sourceSha = ([BitConverter]::ToString($digest.ComputeHash($sourceBytes))).Replace('-', '').ToLowerInvariant() }
finally { $digest.Dispose() }
$sourceText = [Text.UTF8Encoding]::new($false, $true).GetString($sourceBytes).TrimStart([char]0xFEFF)
$tokens = $null; $parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseInput($sourceText, $source, [ref]$tokens, [ref]$parseErrors)
if (@($parseErrors).Count) { throw 'PRODUCT_DESKTOP_PARSE_FAILED' }
# Compile the real native helper without importing the desktop driver.
$native = [regex]::Match($sourceText, "(?s)Add-Type @'\r?\n(.*?)\r?\n'@")
if (!$native.Success) {throw 'REAL_NATIVE_HELPER_MISSING'}
Add-Type $native.Groups[1].Value
$functions = @{}
$functionLines = [ordered]@{}
foreach ($name in @('Initialize-ProductFirstLive', 'Get-ProductLiveElapsedSeconds', 'Write-ProductLifetimeProgress', 'Wait-ProductMinimumLifetime', 'Cleanup-Product')) {
    $matches = @($ast.EndBlock.Statements | Where-Object {
        $_ -is [Management.Automation.Language.FunctionDefinitionAst] -and $_.Name -eq $name
    })
    if ($matches.Count -ne 1) { throw ('UNIQUE_REAL_FUNCTION_REQUIRED: ' + $name) }
    $functions[$name] = $matches[0]
    $functionLines[$name] = $matches[0].Extent.StartLineNumber
    . ([scriptblock]::Create($matches[0].Extent.Text))
}

function Assert-True([bool]$Value, [string]$Label) {
    if (!$Value) { throw ('ASSERT_FAILED: ' + $Label) }
}
function Assert-Rejected([scriptblock]$Body, [string]$Reason, [string]$Label) {
    $caught = $null
    try { $null = & $Body } catch { $caught = $_ }
    Assert-True ($null -ne $caught) ($Label + '/must-reject')
    Assert-True ($caught.Exception.Message -eq $Reason) ($Label + '/exact-reason')
}
$script:samplePhases = [Collections.Generic.List[string]]::new()
function Sample-Resource([string]$Phase) { $script:samplePhases.Add($Phase) }
# Observe the real atomic writer's output, without replacing its implementation.
$script:realProgressWriter = (Get-Command Write-ProductLifetimeProgress -CommandType Function).ScriptBlock
$script:progressFrames = [Collections.Generic.List[object]]::new()
$script:progressHeldReader = $null
function Write-ProductLifetimeProgress {
    param($Sample, [string]$Phase)
    & $script:realProgressWriter -Sample $Sample -Phase $Phase
    $path = Join-Path $OutputDirectory 'product-lifetime-progress.json'
    $frame = [IO.File]::ReadAllText($path, [Text.UTF8Encoding]::new($false, $true)) | ConvertFrom-Json
    $script:progressFrames.Add($frame)
    if ($null -eq $script:progressHeldReader) {
        $script:progressHeldReader = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
            [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    }
}

$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$fixtureRoot = Join-Path $OutputDirectory ('lifetime-stubs-' + [guid]::NewGuid().ToString('N'))
$null = [IO.Directory]::CreateDirectory($fixtureRoot)
$stubExe = Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
$script:ownedStubs = [Collections.Generic.List[object]]::new()
$script:caseResults = [Collections.Generic.List[object]]::new()
$script:cycleWorker = $null; $script:heapDebugger = $null; $script:child = $null
$script:runClock = $null; $script:productOwnedIdentity = $null; $script:productFirstLive = $null
$script:runId = [guid]::NewGuid().ToString('N'); $script:cycle = 1; $Cycles = 1

function Start-OwnedStub([string]$Label) {
    $ready = Join-Path $fixtureRoot ($Label + '.ready')
    $stop = Join-Path $fixtureRoot ($Label + '.stop')
    $stubText = @'
$ErrorActionPreference = 'Stop'
[IO.File]::WriteAllText('__READY__', 'ready')
$deadline = [Diagnostics.Stopwatch]::StartNew()
while ($deadline.Elapsed.TotalSeconds -lt 12) {
    if ([IO.File]::Exists('__STOP__')) { exit 0 }
    Start-Sleep -Milliseconds 25
}
exit 0
'@
    $stubText = $stubText.Replace('__READY__', $ready.Replace("'", "''")).Replace('__STOP__', $stop.Replace("'", "''"))
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($stubText))
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $stubExe
    $info.Arguments = '-NoProfile -NonInteractive -EncodedCommand ' + $encoded
    $info.UseShellExecute = $false; $info.CreateNoWindow = $true
    $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $process = [Diagnostics.Process]::Start($info)
    $null = $process.Handle
    # MainModule/Path can be unavailable immediately after Process.Start. Keep
    # the exact launched path for safe failure cleanup, then prove the actual
    # process path after the stub's own readiness receipt before using it.
    $record = [pscustomobject]@{label=$Label;process=$process;pid=$process.Id;
        start_ticks=$process.StartTime.ToUniversalTime().Ticks;executable=[IO.Path]::GetFullPath($stubExe);ready=$ready;stop=$stop}
    $script:ownedStubs.Add($record)
    $wait = [Diagnostics.Stopwatch]::StartNew()
    while ($true) {
        $process.Refresh()
        if ($process.HasExited -or $wait.Elapsed.TotalSeconds -gt 5) { throw 'OWNED_STUB_START_FAILED' }
        if ([IO.File]::Exists($ready) -and $process.Path -eq $record.executable) { break }
        Start-Sleep -Milliseconds 25
    }
    return $record
}
function Assert-OwnedStubLive($Record, [string]$Label) {
    $probe = [Diagnostics.Process]::GetProcessById($Record.pid)
    try {
        $probe.Refresh()
        Assert-True (!$probe.HasExited -and $probe.StartTime.ToUniversalTime().Ticks -eq $Record.start_ticks -and
            $probe.Path -eq $Record.executable) $Label
    } finally { $probe.Dispose() }
}
function Assert-OwnedStubExited($Record, [string]$Label) {
    $probe = $null
    try { $probe = [Diagnostics.Process]::GetProcessById($Record.pid) }
    catch [ArgumentException] { return }
    try {
        $probe.Refresh()
        if (!$probe.HasExited -and $probe.StartTime.ToUniversalTime().Ticks -eq $Record.start_ticks) {
            Assert-True ($probe.Path -eq $Record.executable) ($Label + '/original-path-known')
            throw ('ASSERT_FAILED: ' + $Label + '/original-owned-process-still-live')
        }
        # A reused PID with different creation ticks is not the original stub.
        # Observe and dispose this read-only wrapper; never stop that process.
    } finally { $probe.Dispose() }
}
function Set-OwnedState($Record) {
    $script:child = $Record.process
    $script:childHandle = $Record.process.Handle
    $script:productOwnedIdentity = [ordered]@{pid=$Record.pid;start_ticks=$Record.start_ticks;executable=$Record.executable}
    $script:productFirstLive = $null
    $script:runClock = [Diagnostics.Stopwatch]::StartNew()
}
function Set-CurrentFirstLive {
    Initialize-ProductFirstLive $script:runClock.Elapsed.TotalSeconds ([DateTime]::UtcNow.ToString('o')) 'offline_stub_current_frame'
}
function Stop-ExactOwnedStub($Record) {
    $probe = $null
    try { $probe = [Diagnostics.Process]::GetProcessById($Record.pid) }
    catch [ArgumentException] { return }
    try {
        $probe.Refresh()
        if (!$probe.HasExited) {
            if ($probe.StartTime.ToUniversalTime().Ticks -ne $Record.start_ticks -or $probe.Path -ne $Record.executable) {
                throw 'TEST_STUB_IDENTITY_CHANGED_NO_STOP'
            }
            $probe.Kill()
            if (!$probe.WaitForExit(5000)) { throw 'OWNED_STUB_CLEANUP_TIMEOUT' }
        }
    } finally { if ($probe) { $probe.Dispose() } }
}
function Invoke-Case([string]$Name, [scriptblock]$Body) {
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $detail = & $Body
    $script:caseResults.Add([ordered]@{name=$Name;status='PASS';seconds=$clock.Elapsed.TotalSeconds;detail=$detail})
}

$failure = $null
$cleanupErrors = [Collections.Generic.List[string]]::new()
try {
    Invoke-Case 'real_driver_completion_and_failure_cleanup_wiring' {
        $main = @($ast.EndBlock.Statements | Where-Object { $_ -is [Management.Automation.Language.TryStatementAst] })
        Assert-True ($main.Count -eq 1 -and $null -ne $main[0].Finally) 'unique-main-try-finally'
        $normalCommands = @($main[0].Body.FindAll({param($n) $n -is [Management.Automation.Language.CommandAst] -and
            $n.GetCommandName() -in @('Wait-ProductMinimumLifetime', 'Stop-Product')}, $true))
        Assert-True ($normalCommands.Count -eq 2 -and $normalCommands[0].GetCommandName() -eq 'Wait-ProductMinimumLifetime' -and
            $normalCommands[1].GetCommandName() -eq 'Stop-Product') 'normal-wait-before-stop'
        $finallyCleanup = @($main[0].Finally.FindAll({param($n) $n -is [Management.Automation.Language.CommandAst] -and
            $n.GetCommandName() -eq 'Cleanup-Product'}, $true))
        Assert-True ($finallyCleanup.Count -eq 1) 'real-finally-calls-cleanup'
        $failureWait = @($main[0].Finally.FindAll({param($n) $n -is [Management.Automation.Language.CommandAst] -and
            $n.GetCommandName() -eq 'Wait-ProductMinimumLifetime'}, $true))
        foreach ($catch in $main[0].CatchClauses) {
            $failureWait += @($catch.FindAll({param($n) $n -is [Management.Automation.Language.CommandAst] -and
                $n.GetCommandName() -eq 'Wait-ProductMinimumLifetime'}, $true))
        }
        Assert-True ($failureWait.Count -eq 0) 'failure-path-never-calls-lifetime-wait'
        $stop = @($ast.EndBlock.Statements | Where-Object {
            $_ -is [Management.Automation.Language.FunctionDefinitionAst] -and $_.Name -eq 'Stop-Product'
        })
        Assert-True ($stop.Count -eq 1) 'unique-stop-function'
        $samples = @($stop[0].FindAll({param($n) $n -is [Management.Automation.Language.AssignmentStatementAst] -and
            $n.Left -is [Management.Automation.Language.VariableExpressionAst] -and
            $n.Left.VariablePath.UserPath -eq 'script:productLiveElapsedBeforeClose'}, $true))
        $closes = @($stop[0].FindAll({param($n) $n -is [Management.Automation.Language.InvokeMemberExpressionAst] -and
            $n.Member.Value -eq 'Close'}, $true))
        Assert-True ($samples.Count -eq 1 -and $closes.Count -eq 1 -and
            $samples[0].Extent.EndOffset -le $closes[0].Extent.StartOffset) 'real-preclose-sample-before-close'
        $getCalls = @($samples[0].Right.FindAll({param($n) $n -is [Management.Automation.Language.CommandAst] -and
            $n.GetCommandName() -eq 'Get-ProductLiveElapsedSeconds'}, $true))
        Assert-True ($getCalls.Count -eq 1) 'real-preclose-sample-uses-real-get'
        return 'AST wiring only; no UI or Close invoked'
    }
    $owned = Start-OwnedStub 'owned'
    Invoke-Case 'delayed_first_live_origin_wait_and_preclose_sample' {
        Set-OwnedState $owned
        $OutputDirectory = $fixtureRoot
        $MinimumSeconds = 2
        Start-Sleep -Milliseconds 2200
        $driverElapsedBeforeOrigin = $script:runClock.Elapsed.TotalSeconds
        Assert-True ($driverElapsedBeforeOrigin -gt $MinimumSeconds) 'driver-clock-already-exceeds-floor'
        Set-CurrentFirstLive
        $script:samplePhases.Clear()
        $waitClock = [Diagnostics.Stopwatch]::StartNew()
        Wait-ProductMinimumLifetime
        $beforeClose = Get-ProductLiveElapsedSeconds
        Assert-True ($beforeClose -ge $MinimumSeconds) 'actual-preclose-sample-meets-floor'
        Assert-True ($waitClock.Elapsed.TotalSeconds -ge 1.8) 'floor-starts-at-first-live-not-driver-start'
        Assert-True ($script:samplePhases.Count -ge 1 -and
            @($script:samplePhases | Where-Object { $_ -ne 'minimum_lifetime_wait' }).Count -eq 0) 'wait-samples-real-phase'
        Assert-OwnedStubLive $owned 'owned-still-live-at-preclose-sample'
        Assert-True ($script:progressFrames.Count -ge 3) 'multiple-waiting-frames-and-completion'
        $previous = $null
        $expectedFields = @('schema','run_id','pid','start_ticks','executable','cycle','cycles','required_seconds',
            'live_elapsed_seconds','run_clock_elapsed_seconds','utc','phase')
        for ($index = 0; $index -lt $script:progressFrames.Count; $index++) {
            $frame = $script:progressFrames[$index]
            Assert-True (@(Compare-Object $expectedFields @($frame.PSObject.Properties.Name)).Count -eq 0) 'exact-progress-schema'
            Assert-True ($frame.schema -eq 1 -and $frame.run_id -eq $script:runId -and $frame.pid -eq $owned.pid -and
                $frame.start_ticks -eq $owned.start_ticks -and $frame.executable -eq $owned.executable -and
                $frame.cycle -eq 1 -and $frame.cycles -eq 1 -and $frame.required_seconds -eq $MinimumSeconds) 'progress-owner-and-final-cycle'
            Assert-True ([DateTimeOffset]::Parse($frame.utc).Offset -eq [TimeSpan]::Zero) 'progress-utc'
            if ($previous) {
                Assert-True ($frame.live_elapsed_seconds -gt $previous.live_elapsed_seconds -and
                    $frame.run_clock_elapsed_seconds -gt $previous.run_clock_elapsed_seconds) 'progress-monotonic'
            }
            if ($index -lt $script:progressFrames.Count - 1) {
                Assert-True ($frame.phase -eq 'waiting' -and $frame.live_elapsed_seconds -lt $MinimumSeconds) 'same-sample-waiting-state'
            } else {
                Assert-True ($frame.phase -eq 'complete' -and $frame.live_elapsed_seconds -ge $MinimumSeconds) 'same-sample-complete-state'
            }
            $previous = $frame
        }
        Assert-True (![IO.File]::Exists((Join-Path $fixtureRoot 'product-lifetime-progress.json.tmp'))) 'no-temporary-progress-left'
        return [ordered]@{minimum_seconds=$MinimumSeconds;driver_elapsed_before_origin=$driverElapsedBeforeOrigin;
            wait_seconds=$waitClock.Elapsed.TotalSeconds;preclose_live_seconds=$beforeClose;resource_samples=$script:samplePhases.Count;
            progress_frames=@($script:progressFrames);atomic_replace_with_open_delete_shared_reader=$true}
    }
    Invoke-Case 'real_initialize_rejects_owned_pid_ticks_path_mismatch' {
        foreach ($field in @('pid', 'start_ticks', 'executable')) {
            Set-OwnedState $owned
            if ($field -eq 'executable') { $script:productOwnedIdentity[$field] += '.different' }
            else { $script:productOwnedIdentity[$field]++ }
            Assert-Rejected { Set-CurrentFirstLive } 'PRODUCT_FIRST_LIVE_IDENTITY_CHANGED_OR_EXITED' ('initialize-' + $field)
            Assert-True ($null -eq $script:productFirstLive) 'invalid-origin-not-established'
        }
        Set-OwnedState $owned
        return 'PID, creation ticks and executable path each rejected'
    }
    Invoke-Case 'real_get_and_wait_reject_pid_ticks_path_mismatch' {
        Set-OwnedState $owned
        Set-CurrentFirstLive
        $OutputDirectory = $fixtureRoot
        $MinimumSeconds = 1
        foreach ($field in @('pid', 'start_ticks', 'executable')) {
            $saved = $script:productFirstLive[$field]
            try {
                if ($field -eq 'executable') { $script:productFirstLive[$field] += '.different' }
                else { $script:productFirstLive[$field]++ }
                Assert-Rejected { Get-ProductLiveElapsedSeconds } 'PRODUCT_FIRST_LIVE_IDENTITY_CHANGED_OR_EXITED' ('get-' + $field)
                $samplesBefore = $script:samplePhases.Count
                $framesBefore = $script:progressFrames.Count
                Assert-Rejected { Wait-ProductMinimumLifetime } 'PRODUCT_FIRST_LIVE_IDENTITY_CHANGED_OR_EXITED' ('wait-' + $field)
                Assert-True ($script:samplePhases.Count -eq $samplesBefore) 'invalid-owner-not-sampled'
                Assert-True ($script:progressFrames.Count -eq $framesBefore) 'invalid-owner-no-progress-published'
            } finally { $script:productFirstLive[$field] = $saved }
        }
        return 'Real functions reject each mismatch before sampling; owned reference retained'
    }
    Invoke-Case 'real_progress_writer_rejects_wrong_owner_nonfinal_cycle_and_bad_phase' {
        Set-OwnedState $owned
        Set-CurrentFirstLive
        $OutputDirectory = $fixtureRoot
        $MinimumSeconds = 28800
        $sample = Get-ProductLiveElapsedSeconds -AsSample
        $before = [IO.File]::ReadAllText((Join-Path $fixtureRoot 'product-lifetime-progress.json'))
        foreach ($field in @('pid','start_ticks','executable')) {
            $saved = $sample.$field
            try {
                if ($field -eq 'executable') {$sample.$field += '.different'} else {$sample.$field++}
                Assert-Rejected { Write-ProductLifetimeProgress -Sample $sample -Phase waiting } 'PRODUCT_LIFETIME_PROGRESS_SAMPLE_IDENTITY_INVALID' ('writer-' + $field)
            } finally {$sample.$field = $saved}
        }
        $script:cycle = 0
        try {
            Assert-Rejected { Write-ProductLifetimeProgress -Sample $sample -Phase waiting } 'PRODUCT_LIFETIME_PROGRESS_FINAL_CYCLE_REQUIRED' 'nonfinal-cycle'
        } finally {$script:cycle = 1}
        Assert-Rejected { Write-ProductLifetimeProgress -Sample $sample -Phase complete } 'PRODUCT_LIFETIME_PROGRESS_PHASE_OR_CLOCK_INVALID' 'premature-complete'
        Assert-True ([IO.File]::ReadAllText((Join-Path $fixtureRoot 'product-lifetime-progress.json')) -ceq $before) 'rejected-writes-preserve-progress-bytes'
        return 'Real writer rejects mismatched owner, wrong cycle and premature complete without changing the last frame'
    }
    Invoke-Case 'real_failure_cleanup_ignores_long_floor_preserves_unbound_stub' {
        Set-OwnedState $owned
        Set-CurrentFirstLive
        $MinimumSeconds = 28800
        Assert-True ((Get-ProductLiveElapsedSeconds) -lt $MinimumSeconds) 'floor-not-met-before-cleanup'
        $sentinel = Start-OwnedStub 'unbound-sentinel'
        Assert-OwnedStubLive $owned 'original-owned-live-before-real-cleanup'
        $cleanupClock = [Diagnostics.Stopwatch]::StartNew()
        Cleanup-Product
        Assert-True ($null -eq $script:child) 'real-cleanup-clears-owned-reference'
        Assert-True ($cleanupClock.Elapsed.TotalSeconds -lt 5) 'real-cleanup-not-blocked-by-eight-hour-floor'
        Assert-OwnedStubExited $owned 'original-owned-exited-by-real-cleanup-before-finally'
        Assert-OwnedStubLive $sentinel 'unbound-stub-untouched-by-real-cleanup'
        return [ordered]@{minimum_seconds=$MinimumSeconds;cleanup_seconds=$cleanupClock.Elapsed.TotalSeconds;
            sentinel_pid=$sentinel.pid;sentinel_start_ticks=$sentinel.start_ticks;unbound_sentinel_alive=$true}
    }
    Invoke-Case 'real_initialize_get_wait_reject_early_owned_exit' {
        $short = Start-OwnedStub 'early-exit'
        Set-OwnedState $short
        Set-CurrentFirstLive
        $MinimumSeconds = 28800
        [IO.File]::WriteAllText($short.stop, 'stop')
        Assert-True ($short.process.WaitForExit(5000)) 'real-short-stub-exited'
        Assert-Rejected { Get-ProductLiveElapsedSeconds } 'PRODUCT_FIRST_LIVE_IDENTITY_CHANGED_OR_EXITED' 'get-after-exit'
        Assert-Rejected { Wait-ProductMinimumLifetime } 'PRODUCT_FIRST_LIVE_IDENTITY_CHANGED_OR_EXITED' 'wait-after-exit'
        $script:productFirstLive = $null
        Assert-Rejected { Set-CurrentFirstLive } 'PRODUCT_FIRST_LIVE_IDENTITY_CHANGED_OR_EXITED' 'initialize-after-exit'
        Cleanup-Product
        return 'Real exited Process object rejected; cleanup called with the original owned reference'
    }
} catch { $failure = $_ }
finally {
    if ($script:progressHeldReader) {$script:progressHeldReader.Dispose()}
    foreach ($record in $script:ownedStubs) {
        try { Stop-ExactOwnedStub $record } catch { $cleanupErrors.Add($_.Exception.Message) }
        try { $record.process.Dispose() } catch { $cleanupErrors.Add($_.Exception.Message) }
        foreach ($path in @($record.ready, $record.stop)) {
            try {
                if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($path)) -ne $fixtureRoot) { throw 'STUB_CLEANUP_PATH_ESCAPE' }
                [IO.File]::Delete($path)
            } catch { $cleanupErrors.Add($_.Exception.Message) }
        }
    }
    foreach ($name in @('product-lifetime-progress.json','product-lifetime-progress.json.tmp')) {
        try {[IO.File]::Delete((Join-Path $fixtureRoot $name))} catch {$cleanupErrors.Add($_.Exception.Message)}
    }
    try { [IO.Directory]::Delete($fixtureRoot, $false) } catch { $cleanupErrors.Add($_.Exception.Message) }
}
$sourceAfter = Get-Item -LiteralPath $source
$sourceStable = $sourceBefore.Length -eq $sourceAfter.Length -and $sourceBefore.LastWriteTimeUtc.Ticks -eq $sourceAfter.LastWriteTimeUtc.Ticks
$verdict = if ($failure -or $cleanupErrors.Count -or !$sourceStable) { 'FAIL' } else { 'PASS' }
$evidence = [ordered]@{schema=1;scope='offline real extracted lifetime functions with hidden test-owned stubs';verdict=$verdict;
    source=$source;source_sha256=$sourceSha;source_stat_stable_over_test=$sourceStable;function_lines=$functionLines;
    powershell_version=$PSVersionTable.PSVersion.ToString();cases_passed=$script:caseResults.Count;cases=@($script:caseResults);
    failure=$(if ($failure) { $failure.Exception.Message } else { $null });cleanup_errors=@($cleanupErrors);
    stub_identities=@($script:ownedStubs | ForEach-Object { [ordered]@{label=$_.label;pid=$_.pid;start_ticks=$_.start_ticks;executable=$_.executable} });
    boundaries=[ordered]@{product_started=$false;sdk_started=$false;network_access=$false;uia_run=$false;
        minimum_floor_predicate_reimplemented=$false;failure_cleanup_uses_real_function=$true;preclose_ui_wiring='AST only';
        foreign_reference_injected_into_cleanup=$false;foreign_cleanup_safety_not_proven=$true;formal_credit=0;qualification_credit=0};
    observed_utc=[DateTime]::UtcNow.ToString('o')}
$evidencePath = Join-Path $OutputDirectory ('lifetime-wait-' + [guid]::NewGuid().ToString('N') + '.json')
[IO.File]::WriteAllText($evidencePath, ($evidence | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
[ordered]@{verdict=$verdict;cases_passed=$script:caseResults.Count;evidence=$evidencePath;source_sha256=$sourceSha} | ConvertTo-Json -Compress
if ($verdict -ne 'PASS') { exit 1 }
