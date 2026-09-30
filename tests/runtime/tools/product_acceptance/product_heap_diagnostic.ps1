# Dot-sourced only for diagnostic PILOTs. Never writes IFEO/system settings.
function Start-HeapDiagnosticProduct {
    $script:heapTools = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/Debuggers/x64'
    foreach ($tool in @('cdb.exe', 'umdh.exe')) {
        if (!(Test-Path -LiteralPath (Join-Path $script:heapTools $tool))) { throw "HEAP_TOOL_MISSING: $tool" }
    }
    $script:heapDirectory = Join-Path $OutputDirectory 'heap-diagnostic'
    $null = New-Item -ItemType Directory -Path $script:heapDirectory
    $commands = Join-Path $script:heapDirectory 'startup.commands'
    @'
!gflag +ust
!gflag
sxi eh
sxe -c ".echo HEAP_CORRUPTION; .exr -1; .ecxr; k 50; !heap -triage; ~*k 12; qd" 0xc0000374
sxe -c ".echo ACCESS_VIOLATION; .exr -1; .ecxr; k 50; !analyze -v; ~*k 12; qd" av
g
'@ | Set-Content -LiteralPath $commands -Encoding ASCII
    $symbols = (Split-Path $Executable) + ';srv*' +
        (Join-Path (Resolve-Path (Join-Path $PSScriptRoot '../../../..')).Path 'out/debug-symbols') +
        '*https://msdl.microsoft.com/download/symbols'
    $commandText = '.sympath ' + $symbols + "`n" + (Get-Content -LiteralPath $commands -Raw)
    if ($HeapCheckOnly) { $commandText = $commandText.Replace('!gflag +ust', "!gflag +htc`n!gflag +hfc`n!gflag +hpc") }
    if ($CrashDiagnostic) { $commandText = $commandText.Replace('!gflag +ust', '.echo NORMAL_ALLOCATOR_CRASH_DIAGNOSTIC') }
    if ($HeapPageCheck) {
        # Process-local standard PageHeap. This does not claim full guard-page mode.
        $commandText = $commandText.Replace('!gflag +ust', '!gflag +hpa')
        $commandText = [regex]::Replace($commandText, '(?m)^g\r?$', "sxe -c `".echo VERIFIER_BREAKPOINT; .exr -1; kv 80; !avrf; !heap -triage; ~*k 12; qd`" bpe`ng")
        if (!$commandText.Contains('VERIFIER_BREAKPOINT')) {throw 'PAGE_CHECK_COMMAND_SETUP_FAILED'}
    }
    Set-Content -LiteralPath $commands -Value $commandText -Encoding ASCII
    $marker = [ordered]@{schema=1;run_id=$script:runId;release_eligible=$false;
        purpose='allocation_stack_and_heap_corruption_diagnosis';flags=$(if($CrashDiagnostic){'none'}elseif($HeapPageCheck){'hpa_standard'}elseif($HeapCheckOnly){'htc,hfc,hpc'}else{'ust'});
        system_settings_changed=$false;share_omitted=[bool]$HeapDiagnosticNoShare;
        isolated_uia_cycles=[bool]$IsolateUiaCycles;
        export_omitted=[bool]$HeapDiagnosticNoExport;
        utc=[DateTime]::UtcNow.ToString('o')}
    $marker | ConvertTo-Json | Set-Content (Join-Path (Split-Path $OutputDirectory) 'diagnostic-debugger.json') -Encoding UTF8
    $script:heapDebugger = Start-Process -FilePath (Join-Path $script:heapTools 'cdb.exe') `
        -WindowStyle Hidden -PassThru -WorkingDirectory (Split-Path $Executable) `
        -ArgumentList @('-xe','ld:ntdll.dll','-g','-G','-cf',('"'+$commands+'"'),
            '-logo',('"'+(Join-Path $script:heapDirectory 'debugger.txt')+'"'),('"'+$Executable+'"'),'--debug') `
        -RedirectStandardOutput (Join-Path $script:heapDirectory 'debugger.stdout') `
        -RedirectStandardError (Join-Path $script:heapDirectory 'debugger.stderr')
    $null = $script:heapDebugger.Handle
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        if ($script:heapDebugger.HasExited) { throw 'HEAP_DEBUGGER_EXITED_AT_STARTUP' }
        $candidates = @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$($script:heapDebugger.Id)" |
            Where-Object { $_.ExecutablePath -eq $Executable })
        if ($candidates.Count -gt 1) { throw 'HEAP_PRODUCT_PID_AMBIGUOUS' }
        if ($candidates.Count -eq 1) {
            $script:child = Get-Process -Id $candidates[0].ProcessId
            $marker.product_pid=$script:child.Id
            $marker.debugger_pid=$script:heapDebugger.Id
            $marker | ConvertTo-Json | Set-Content (Join-Path (Split-Path $OutputDirectory) 'diagnostic-debugger.json') -Encoding UTF8
            return
        }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'HEAP_PRODUCT_STARTUP_TIMEOUT'
}
function Wait-HeapDiagnosticHistory {
    # UST makes the existing-report scan much slower. Wait for that startup
    # work to finish before applying the unchanged media load; this run is
    # diagnostic and can never satisfy the formal acceptance gate.
    $null = Wait-For 'diagnostic history initialization' {
        $probe = Get-Content -LiteralPath $env:LIVEKIT_UIA_PILOT_PROBE -Tail 2 |
            ForEach-Object { try { $_ | ConvertFrom-Json } catch { } } | Select-Object -Last 1
        $probe -and $probe.history.history_refresh_count -gt 0
    } 240
}
function Save-HeapDiagnosticSnapshot([ValidateSet('released','failure')][string]$Phase='released') {
    if ($HeapCheckOnly -or $HeapPageCheck -or $CrashDiagnostic) { return }
    $prefix = Join-Path $script:heapDirectory ('{0}-{1:d4}' -f $Phase,$script:cycle)
    if (Test-Path -LiteralPath ($prefix+'.txt')) { throw 'HEAP_SNAPSHOT_ALREADY_EXISTS' }
    $sampleStarted = [DateTime]::UtcNow
    $capture = Start-Process -FilePath (Join-Path $script:heapTools 'umdh.exe') `
        -WindowStyle Hidden -PassThru -ArgumentList @("-p:$($script:child.Id)", ('-f:"'+$prefix+'.txt"')) `
        -RedirectStandardOutput ($prefix+'.stdout') -RedirectStandardError ($prefix+'.stderr')
    $null=$capture.Handle
    if (!$capture.WaitForExit(60000)) { $capture.Kill(); throw 'HEAP_SNAPSHOT_TIMEOUT' }
    if ($capture.ExitCode -ne 0 -or !(Test-Path -LiteralPath ($prefix+'.txt'))) { throw 'HEAP_SNAPSHOT_FAILED' }
    [ordered]@{run_id=$script:runId;cycle_id=$script:cycleId;cycle=$script:cycle;
        pid=$script:child.Id;started_utc=$sampleStarted.ToString('o');finished_utc=[DateTime]::UtcNow.ToString('o');
        phase=$Phase;allocation_stacks_only=$true;exit_code=$capture.ExitCode} |
        ConvertTo-Json | Set-Content -LiteralPath ($prefix+'.json') -Encoding UTF8
}
