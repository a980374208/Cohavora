# Focused offline contract for the bounded native acceptance probe reader.
# Focused, offline PS5.1 selftest. Never reads a live acceptance probe, calls
# Python/SDK/remote services, or invokes either tracked runtime caller.
# The duplicate caller predicates below are intentionally limited to their
# existing JSON/context/watchdog semantics. They are not acceptance evidence.
param(
    [string]$HelperPath = (Join-Path $PSScriptRoot '../tools/product_acceptance/product_pilot_probe_tail.ps1')
)
$ErrorActionPreference = 'Stop'
$HelperPath = (Resolve-Path -LiteralPath $HelperPath).Path
. $HelperPath
if (!(Get-Command Read-ProductPilotProbeTail -CommandType Function -ErrorAction SilentlyContinue)) {
    throw 'DRAFT_HELPER_API_MISSING'
}

$script:utf8 = [Text.UTF8Encoding]::new($false, $true)
$script:lf = [string][char]10
$script:cr = [string][char]13
$script:fixtureRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ('probe-tail-fixtures-' + [Guid]::NewGuid().ToString('N'))))
$script:createdFiles = [Collections.Generic.List[string]]::new()
$script:cases = [Collections.Generic.List[string]]::new()
$null = [IO.Directory]::CreateDirectory($script:fixtureRoot)

function Assert-True([bool]$Value, [string]$Label) {
    if (!$Value) { throw ('DRAFT_ASSERT_TRUE: ' + $Label) }
}
function Assert-Equal($Expected, $Actual, [string]$Label) {
    if ($Actual -cne $Expected) { throw ('DRAFT_ASSERT_EQUAL: ' + $Label) }
}
function Assert-Lines([string[]]$Expected, [object[]]$Actual, [string]$Label) {
    Assert-Equal $Expected.Count $Actual.Count ($Label + '/count')
    for ($i = 0; $i -lt $Expected.Count; $i++) {
        Assert-True ([string]::Equals($Expected[$i], [string]$Actual[$i], [StringComparison]::Ordinal)) ($Label + '/line-' + $i)
    }
}
function Assert-Throws([scriptblock]$Body, [string]$Pattern, [string]$Label) {
    $caught = $null
    try { $null = & $Body } catch { $caught = $_ }
    Assert-True ($null -ne $caught) ($Label + '/must-throw')
    if ($Pattern) {
        Assert-True (($caught | Out-String) -match $Pattern) ($Label + '/error-category')
    }
}
function Invoke-Case([string]$Name, [scriptblock]$Body) {
    & $Body
    $script:cases.Add($Name)
}
function New-Fixture([ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Name, [byte[]]$Bytes) {
    $path = [IO.Path]::GetFullPath((Join-Path $script:fixtureRoot $Name))
    if ([IO.Path]::GetDirectoryName($path) -cne $script:fixtureRoot) { throw 'DRAFT_FIXTURE_PATH_ESCAPE' }
    $stream = [IO.FileStream]::new($path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
    $script:createdFiles.Add($path)
    try { $stream.Write($Bytes, 0, $Bytes.Length) } finally { $stream.Dispose() }
    return $path
}
function New-TextFixture([string]$Name, [string]$Text) {
    return (New-Fixture $Name $script:utf8.GetBytes($Text))
}
function Append-Bytes([IO.FileStream]$Stream, [byte[]]$Bytes) {
    $Stream.Write($Bytes, 0, $Bytes.Length)
    $Stream.Flush()
}
function New-ProbeJson(
    [long]$UtcMs = 100000,
    [string]$Run = 'run-a',
    [AllowNull()][string]$Session = 'sid-a',
    [string]$ProcessRun = 'process-a',
    [string]$Participant = 'participant-a',
    [int]$QueueDrops = 0,
    [int]$PendingDropped = 0,
    [int]$WriteFailures = 0,
    [int]$OrdinaryDropped = 0,
    [int]$CriticalDropped = 0,
    [int]$SinkFailures = 0
) {
    return ([ordered]@{
        run_id = $Run; process_run_id = $ProcessRun
        anonymous_session_id = $Session; participant_sha256 = $Participant
        utc_ms = $UtcMs
        history = [ordered]@{queue_drops = $QueueDrops; pending_records_dropped = $PendingDropped; write_failures = $WriteFailures}
        diagnostic = [ordered]@{dropped_ordinary = $OrdinaryDropped; dropped_critical = $CriticalDropped; sink_failures = $SinkFailures}
    } | ConvertTo-Json -Depth 5 -Compress)
}

# Copied predicate shape from Sync-ObserverContext, without writing context,
# touching caller globals, or running its Python fence. No freshness gate added.
function Read-DraftContext([string]$Path, [string]$ExpectedRun, [string]$Action, [string]$Phase, [AllowNull()][string]$PriorSession) {
    $probes = @(Read-ProductPilotProbeTail -Path $Path -Count 3 | ForEach-Object {
        try { $_ | ConvertFrom-Json } catch { }
    })
    if (!$probes.Count) { throw 'NATIVE_CONTEXT_MISSING' }
    $probe = $probes[-1]
    if ($probe.run_id -ne $ExpectedRun) { throw 'NATIVE_CONTEXT_RUN_MISMATCH' }
    $session = $PriorSession
    if ($Action -eq 'join' -and $Phase -eq 'uia_observed') {
        if (!$probe.anonymous_session_id) { throw 'NATIVE_SESSION_CONTEXT_MISSING' }
        $session = $probe.anonymous_session_id
    }
    return [pscustomobject]@{
        process_run_id = $probe.process_run_id
        participant_sha256 = $probe.participant_sha256
        anonymous_session_id = $session
    }
}

# Copied predicate shape from the invoke watchdog, with an injected clock.
# Its Tail2 has no run-id gate and no missing-health gate. Preserve both facts.
function Read-DraftWatchdog([string]$Path, [string]$LastAction, [long]$NowMs) {
    $health = Read-ProductPilotProbeTail -Path $Path -Count 2 | ForEach-Object {
        try { $_ | ConvertFrom-Json } catch { }
    } | Select-Object -Last 1
    if ($health -and $LastAction -ne 'process_exit' -and ($NowMs - $health.utc_ms) -gt 15000) {
        throw 'PROCESS_PROBE_STALE'
    }
    if ($health -and ($health.history.queue_drops -or $health.history.pending_records_dropped -or
        $health.history.write_failures -or $health.diagnostic.dropped_ordinary -or
        $health.diagnostic.dropped_critical -or $health.diagnostic.sink_failures)) {
        throw 'LIVE_ZERO_LOSS_GATE_FAILED'
    }
    return $health
}

$testFailure = $null
$cleanupErrors = [Collections.Generic.List[string]]::new()
try {
    Invoke-Case 'empty-file-and-missing-path' {
        $path = New-Fixture 'empty.jsonl' ([byte[]]@())
        Assert-Lines @() @(Read-ProductPilotProbeTail -Path $path) 'empty'
        Assert-Throws { Read-ProductPilotProbeTail -Path (Join-Path $script:fixtureRoot 'never-created.jsonl') } '' 'missing-path'
    }
    Invoke-Case 'bom-only-has-no-record' {
        $path = New-Fixture 'bom-only.jsonl' ([byte[]]@(0xEF, 0xBB, 0xBF))
        Assert-Lines @() @(Read-ProductPilotProbeTail -Path $path -Count 3) 'bom-only'
        Assert-Lines @() @(Read-ProductPilotProbeTail -Path $path -Count 1 -MaximumBytes 3) 'bom-only-exact-cap'
    }
    Invoke-Case 'append-handle-partial-json-no-lf-then-lf' {
        $first = New-ProbeJson -ProcessRun 'first'
        $second = New-ProbeJson -ProcessRun 'second'
        $path = New-TextFixture 'append.jsonl' ($first + $script:lf)
        $writer = [IO.FileStream]::new($path, [IO.FileMode]::Open, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
        try {
            $null = $writer.Seek(0, [IO.SeekOrigin]::End)
            Assert-Lines @($first) @(Read-ProductPilotProbeTail -Path $path -Count 1) 'writer-open'
            $split = [Math]::Max(1, [int]($second.Length / 2))
            $partial = $second.Substring(0, $split)
            Append-Bytes $writer $script:utf8.GetBytes($partial)
            Assert-Lines @($first, $partial) @(Read-ProductPilotProbeTail -Path $path -Count 2) 'partial-json-physical-candidate'
            $context = Read-DraftContext $path 'run-a' 'join' 'uia_observed' $null
            Assert-Equal 'first' $context.process_run_id 'partial-json-context-fallback'
            Append-Bytes $writer $script:utf8.GetBytes($second.Substring($split))
            Assert-Lines @($second) @(Read-ProductPilotProbeTail -Path $path -Count 1) 'complete-json-no-lf'
            $context = Read-DraftContext $path 'run-a' 'join' 'uia_observed' $null
            Assert-Equal 'second' $context.process_run_id 'no-lf-context-visible'
            Append-Bytes $writer $script:utf8.GetBytes($script:lf)
            Assert-Lines @($first, $second) @(Read-ProductPilotProbeTail -Path $path -Count 2) 'lf-commit-no-synthetic-empty'
        } finally { $writer.Dispose() }
    }
    Invoke-Case 'bom-crlf-empty-physical-lines' {
        $text = 'first' + $script:cr + $script:lf + $script:cr + $script:lf + 'last' + $script:cr + $script:lf
        $bytes = [byte[]](@(0xEF, 0xBB, 0xBF) + $script:utf8.GetBytes($text))
        $path = New-Fixture 'bom-crlf.jsonl' $bytes
        Assert-Lines @('first', '', 'last') @(Read-ProductPilotProbeTail -Path $path -Count 3) 'bom-at-file-start'
        Assert-Lines @('', 'last') @(Read-ProductPilotProbeTail -Path $path -Count 2) 'physical-empty-line'
        $middleBom = [string][char]0xFEFF + 'middle'
        $path2 = New-TextFixture 'nonleading-bom.jsonl' ('first' + $script:lf + $middleBom + $script:lf)
        Assert-Lines @($middleBom) @(Read-ProductPilotProbeTail -Path $path2 -Count 1) 'nonleading-bom-preserved'
        $path3 = New-TextFixture 'one-empty-line.jsonl' $script:lf
        Assert-Lines @('') @(Read-ProductPilotProbeTail -Path $path3 -Count 1) 'single-physical-empty'
    }
    Invoke-Case 'utf8-chinese-emoji-and-64k-split' {
        $chinese = [string][char]0x6D4B + [string][char]0x8BD5
        $emoji = [char]::ConvertFromUtf32(0x1F680)
        $row = $chinese + '/' + $emoji
        $path = New-TextFixture 'utf8-small.jsonl' ('old' + $script:lf + $row)
        Assert-Lines @($row) @(Read-ProductPilotProbeTail -Path $path -Count 1) 'multibyte-no-lf'
        # A 64KiB backward block starts inside the multibyte character. The
        # expected value is the independently authored full logical line.
        $largeChinese = ('p' * 23) + [string][char]0x6D4B + ('s' * 65534)
        $path2 = New-TextFixture 'utf8-chinese-block.jsonl' ('old' + $script:lf + $largeChinese + $script:lf)
        Assert-Lines @($largeChinese) @(Read-ProductPilotProbeTail -Path $path2 -Count 1) 'chinese-crosses-block'
        $largeEmoji = ('p' * 23) + $emoji + ('s' * 65533)
        $path3 = New-TextFixture 'utf8-emoji-block.jsonl' ('old' + $script:lf + $largeEmoji + $script:lf)
        Assert-Lines @($largeEmoji) @(Read-ProductPilotProbeTail -Path $path3 -Count 1) 'emoji-crosses-block'
    }
    Invoke-Case 'incomplete-utf8-tail-consumes-candidate-window' {
        $prefix = $script:utf8.GetBytes('older' + $script:lf + 'fresh' + $script:lf)
        $path = New-Fixture 'utf8-incomplete.jsonl' ([byte[]]($prefix + @(0xE6, 0xB5)))
        Assert-Lines @('fresh') @(Read-ProductPilotProbeTail -Path $path -Count 2) 'discard-fragment-without-widening'
        Assert-Lines @() @(Read-ProductPilotProbeTail -Path $path -Count 1) 'fragment-is-one-candidate'
        $writer = [IO.FileStream]::new($path, [IO.FileMode]::Open, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
        try {
            $null = $writer.Seek(0, [IO.SeekOrigin]::End)
            Append-Bytes $writer ([byte[]]@(0x8B))
            Assert-Lines @([string][char]0x6D4B) @(Read-ProductPilotProbeTail -Path $path -Count 1) 'utf8-fragment-completed'
            Append-Bytes $writer $script:utf8.GetBytes($script:lf)
            Assert-Lines @('fresh', [string][char]0x6D4B) @(Read-ProductPilotProbeTail -Path $path -Count 2) 'completed-utf8-lf'
        } finally { $writer.Dispose() }
    }
    Invoke-Case 'invalid-utf8-complete-line-is-fatal' {
        $good = $script:utf8.GetBytes('good' + $script:lf)
        $path = New-Fixture 'utf8-invalid.jsonl' ([byte[]]($good + @(0xC3, 0x28, 0x0A)))
        $emitted = [Collections.Generic.List[object]]::new()
        Assert-Throws { Read-ProductPilotProbeTail -Path $path -Count 2 | ForEach-Object { $emitted.Add($_) } } '' 'invalid-lf-utf8'
        Assert-Equal 0 $emitted.Count 'no-partial-output-before-utf8-failure'
        $path2 = New-Fixture 'utf8-incomplete-with-lf.jsonl' ([byte[]]($good + @(0xE6, 0xB5, 0x0A)))
        Assert-Throws { Read-ProductPilotProbeTail -Path $path2 -Count 2 } '' 'incomplete-but-lf-committed'
        $path3 = New-Fixture 'utf8-malformed-no-lf.jsonl' ([byte[]]($good + @(0xFF)))
        Assert-Throws { Read-ProductPilotProbeTail -Path $path3 -Count 2 } '' 'malformed-not-an-append-fragment'
        $path4 = New-Fixture 'utf8-malformed-before-fragment.jsonl' ([byte[]]($good + @(0xFF, 0xE6, 0xB5)))
        Assert-Throws { Read-ProductPilotProbeTail -Path $path4 -Count 2 } '' 'malformed-prefix-not-hidden-by-incomplete-suffix'
    }
    Invoke-Case 'bounded-window-no-full-file-fallback' {
        $path = New-TextFixture 'bounded.jsonl' ('old' + $script:lf + 'short' + $script:lf)
        Assert-Lines @('short') @(Read-ProductPilotProbeTail -Path $path -Count 1 -MaximumBytes 7) 'delimiter-fits'
        Assert-Throws { Read-ProductPilotProbeTail -Path $path -Count 2 -MaximumBytes 7 } 'NATIVE_PROBE_TAIL_WINDOW_EXCEEDED' 'earlier-start-outside-cap'
        Assert-Throws { Read-ProductPilotProbeTail -Path $path -Count 1 -MaximumBytes 3 } 'NATIVE_PROBE_TAIL_WINDOW_EXCEEDED' 'row-start-outside-cap'
        $path2 = New-TextFixture 'bounded-bof.jsonl' ('short' + $script:lf)
        Assert-Lines @('short') @(Read-ProductPilotProbeTail -Path $path2 -Count 1 -MaximumBytes 6) 'exact-bof-cap'
        $oversized = 'x' * (1048576 + 1)
        $path3 = New-TextFixture 'over-one-mib.jsonl' $oversized
        Assert-Throws { Read-ProductPilotProbeTail -Path $path3 -Count 1 } 'NATIVE_PROBE_TAIL_WINDOW_EXCEEDED' 'default-cap-not-full-read'
    }
    Invoke-Case 'parameter-bounds' {
        $path = New-TextFixture 'parameters.jsonl' ('one' + $script:lf)
        Assert-Lines @('one') @(Read-ProductPilotProbeTail -Path $path -Count 64 -MaximumBytes 1048576) 'upper-parameter-bound'
        $path2 = New-TextFixture 'one-byte.jsonl' 'x'
        Assert-Lines @('x') @(Read-ProductPilotProbeTail -Path $path2 -Count 1 -MaximumBytes 1) 'lower-parameter-bound'
        foreach ($count in @(0, 65, -1)) {
            Assert-Throws { Read-ProductPilotProbeTail -Path $path -Count $count } '' ('count-rejected-' + $count)
        }
        foreach ($limit in @(0, 1048577, -1)) {
            Assert-Throws { Read-ProductPilotProbeTail -Path $path -MaximumBytes $limit } '' ('byte-limit-rejected-' + $limit)
        }
    }
    Invoke-Case 'context-last-valid-run-session-and-projection' {
        $valid = New-ProbeJson -UtcMs 1 -ProcessRun 'native-last' -Participant 'participant-last' -Session 'sid-last'
        $path = New-TextFixture 'context-fallback.jsonl' ($valid + $script:lf + '{broken' + $script:lf + '{partial')
        $context = Read-DraftContext $path 'run-a' 'join' 'uia_observed' 'sid-old'
        Assert-Equal 'native-last' $context.process_run_id 'native-process-projection'
        Assert-Equal 'participant-last' $context.participant_sha256 'participant-projection'
        Assert-Equal 'sid-last' $context.anonymous_session_id 'observed-join-updates-session'
        $requested = Read-DraftContext $path 'run-a' 'join' 'requested' 'sid-old'
        Assert-Equal 'sid-old' $requested.anonymous_session_id 'requested-retains-session'
        $wrongRun = New-ProbeJson -Run 'different-run'
        $path2 = New-TextFixture 'context-wrong-run.jsonl' ($valid + $script:lf + $wrongRun)
        Assert-Throws { Read-DraftContext $path2 'run-a' 'join' 'uia_observed' 'sid-old' } 'NATIVE_CONTEXT_RUN_MISMATCH' 'last-valid-wrong-run'
        $noSession = New-ProbeJson -Session $null
        $path3 = New-TextFixture 'context-no-session.jsonl' $noSession
        Assert-Throws { Read-DraftContext $path3 'run-a' 'join' 'uia_observed' 'sid-old' } 'NATIVE_SESSION_CONTEXT_MISSING' 'observed-join-needs-session'
        $requested2 = Read-DraftContext $path3 'run-a' 'join' 'requested' 'sid-old'
        Assert-Equal 'sid-old' $requested2.anonymous_session_id 'requested-no-session-allowed'
        $path4 = New-TextFixture 'context-three-invalid.jsonl' ($valid + $script:lf + '{bad1' + $script:lf + '{bad2' + $script:lf + '{bad3')
        Assert-Throws { Read-DraftContext $path4 'run-a' 'join' 'uia_observed' 'sid-old' } 'NATIVE_CONTEXT_MISSING' 'no-fourth-row-fallback'
    }
    Invoke-Case 'watchdog-tail2-stale-boundary-exit-and-existing-identity-policy' {
        $healthy = New-ProbeJson -UtcMs 100000 -Run 'different-run'
        $path = New-TextFixture 'watchdog-fallback.jsonl' ($healthy + $script:lf + '{partial')
        $health = Read-DraftWatchdog $path 'share' 115000
        Assert-Equal 'different-run' $health.run_id 'no-new-watchdog-run-gate'
        Assert-Throws { Read-DraftWatchdog $path 'share' 115001 } 'PROCESS_PROBE_STALE' 'fifteen-seconds-plus-one-ms'
        $exitHealth = Read-DraftWatchdog $path 'process_exit' 200000
        Assert-Equal 100000 $exitHealth.utc_ms 'exit-exempts-freshness'
        $path2 = New-TextFixture 'watchdog-two-invalid.jsonl' ($healthy + $script:lf + '{bad1' + $script:lf + '{bad2')
        Assert-True ($null -eq (Read-DraftWatchdog $path2 'share' 200000)) 'tail2-no-third-row-fallback-no-added-missing-gate'
    }
    Invoke-Case 'watchdog-six-zero-loss-fields-even-at-process-exit' {
        foreach ($field in @('QueueDrops', 'PendingDropped', 'WriteFailures', 'OrdinaryDropped', 'CriticalDropped', 'SinkFailures')) {
            $arguments = @{UtcMs = 100000}
            $arguments[$field] = 1
            $lossy = New-ProbeJson @arguments
            $path = New-TextFixture ('loss-' + $field + '.jsonl') $lossy
            Assert-Throws { Read-DraftWatchdog $path 'process_exit' 200000 } 'LIVE_ZERO_LOSS_GATE_FAILED' ('loss-' + $field)
        }
    }
} catch {
    $testFailure = $_
} finally {
    # Only exact files created above, then this known empty directory. No
    # recursion, cross-shell deletion, wildcard deletion, or external paths.
    foreach ($path in $script:createdFiles) {
        try {
            if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($path)) -cne $script:fixtureRoot) {
                throw 'DRAFT_CLEANUP_PATH_ESCAPE'
            }
            [IO.File]::Delete($path)
        } catch { $cleanupErrors.Add($_.Exception.Message) }
    }
    try { [IO.Directory]::Delete($script:fixtureRoot, $false) }
    catch { $cleanupErrors.Add($_.Exception.Message) }
}
if ($testFailure) { throw $testFailure }
if ($cleanupErrors.Count) { throw ('DRAFT_FIXTURE_CLEANUP_FAILED: ' + ($cleanupErrors -join '; ')) }
[ordered]@{
    schema = 1; scope = 'offline-draft-selftest'; verdict = 'PASS'
    helper = $HelperPath; powershell_version = $PSVersionTable.PSVersion.ToString()
    cases_passed = $script:cases.Count; cases = @($script:cases)
    acceptance_evidence = $false; fixture_cleanup = 'COMPLETE'
} | ConvertTo-Json -Depth 5
