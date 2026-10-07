# Dot-sourced only for diagnostic PILOTs. Never writes IFEO/system settings.
if ($HeapSnapshotDiagnostic) { . (Join-Path $PSScriptRoot 'product_heap_snapshot.ps1') }
function Start-HeapDiagnosticProduct {
    $script:heapTools = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/Debuggers/x64'
    $requiredTools = if ($HeapSnapshotDiagnostic) { @('cdb.exe') } else { @('cdb.exe', 'umdh.exe') }
    foreach ($tool in $requiredTools) {
        if (!(Test-Path -LiteralPath (Join-Path $script:heapTools $tool))) { throw "HEAP_TOOL_MISSING: $tool" }
    }
    $script:heapDirectory = Join-Path $OutputDirectory 'heap-diagnostic'
    $null = New-Item -ItemType Directory -Path $script:heapDirectory
    if ($HeapSnapshotDiagnostic) {
        # Launch exactly like the ordinary driver. The short noninvasive dump
        # attaches only after release; no debugger or heap flags at startup.
        $script:heapDebugger = $null
        $marker = [ordered]@{schema=1;run_id=$script:runId;release_eligible=$false;
            diagnostic_only=$true;instrumented_diagnostic=$true;qualification_credit=0;
            purpose='normal_allocator_released_heap_snapshot';flags='none';
            flags_scope='no_heap_flags_requested_or_modified';normal_heap_flags_verified=$false;
            launch='normal_start_process';capture='cdb_noninvasive_dump';capture_phases=@('released');
            system_settings_changed=$false;share_omitted=[bool]$HeapDiagnosticNoShare;
            export_omitted=[bool]$HeapDiagnosticNoExport;isolated_uia_cycles=[bool]$IsolateUiaCycles;
            uia_client_profile=$(if($IsolateUiaCycles){'isolated_per_cycle'}else{'persistent'});
            utc=[DateTime]::UtcNow.ToString('o')}
        $markerPath = Join-Path (Split-Path $OutputDirectory) 'diagnostic-debugger.json'
        $marker | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $markerPath -Encoding UTF8
        $script:child = Start-Process -FilePath $Executable -ArgumentList '--debug' `
            -WorkingDirectory (Split-Path $Executable) -PassThru `
            -RedirectStandardOutput (Join-Path $OutputDirectory 'product.stdout.log') `
            -RedirectStandardError (Join-Path $OutputDirectory 'product.stderr.log')
        $marker.product_pid=$script:child.Id
        $marker.product_start_ticks=$script:child.StartTime.ToUniversalTime().Ticks
        $marker | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $markerPath -Encoding UTF8
        return
    }
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
        uia_client_profile=$(if($IsolateUiaCycles){'isolated_per_cycle'}else{'persistent'});
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
    $script:step = 'diagnostic history initialization'
    $deadline = [Diagnostics.Stopwatch]::StartNew()
    do {
        $script:child.Refresh()
        if ($script:child.HasExited) { throw 'HEAP_PRODUCT_EXIT_DURING_HISTORY_INITIALIZATION' }
        if (Test-Path -LiteralPath $env:LIVEKIT_UIA_PILOT_PROBE) {
            $probes = @(Read-ProductPilotProbeTail -Path $env:LIVEKIT_UIA_PILOT_PROBE -Count 3 -MaximumBytes 1048576 |
                ForEach-Object { try { $_ | ConvertFrom-Json } catch { } })
            if ($probes.Count) {
                $probe = $probes[-1]
                if ($probe.run_id -cne $script:runId) { throw 'NATIVE_CONTEXT_RUN_MISMATCH' }
                if ($probe.process_run_id -cnotmatch '^[0-9a-f]{32}$') { throw 'NATIVE_PROCESS_CONTEXT_MISSING' }
                if ($script:nativeProcessRun -and $probe.process_run_id -cne $script:nativeProcessRun) {
                    throw 'NATIVE_PROCESS_CONTEXT_CHANGED'
                }
                if ($probe.history.history_refresh_count -gt 0) {
                    $script:nativeProcessRun = $probe.process_run_id
                    return
                }
            }
        }
        Start-Sleep -Milliseconds 200
    } while ($deadline.Elapsed.TotalSeconds -lt 240)
    throw 'TIMEOUT: diagnostic history initialization'
}
function Save-HeapDiagnosticSnapshot([ValidateSet('active','released','exported','failure')][string]$Phase='released') {
    if ($HeapSnapshotDiagnostic) {
        if ($Phase -ne 'released') { return }
        $prefix = Join-Path $script:heapDirectory ('{0}-{1:d4}' -f $Phase,$script:cycle)
        if (Test-Path -LiteralPath ($prefix+'.json')) { throw 'HEAP_SNAPSHOT_ALREADY_EXISTS' }
        $identity = Get-Content -LiteralPath (Join-Path $OutputDirectory 'product-identity.json') -Raw | ConvertFrom-Json
        if ($identity.run_id -cne $script:runId -or $identity.pid -ne $script:child.Id -or
            $identity.executable -cne $Executable) { throw 'HEAP_SNAPSHOT_PRODUCT_IDENTITY_CHANGED' }
        $receipt = Invoke-ProductNormalHeapSnapshot -Identity $identity -Prefix $prefix `
            -CdbExecutable (Join-Path $script:heapTools 'cdb.exe')
        if (!$receipt -or $receipt.PSObject.TypeNames -notcontains 'Product.NormalHeapSnapshotReceipt' -or
            $receipt.status -cne 'CAPTURED_DIAGNOSTIC_ONLY' -or $receipt.capture_exit_code -ne 0 -or
            $receipt.same_process_alive_after_detach -ne $true) { throw 'HEAP_SNAPSHOT_CAPTURE_RECEIPT_INVALID' }
        [ordered]@{run_id=$script:runId;cycle_id=$script:cycleId;cycle=$script:cycle;pid=$identity.pid;
            process_start_ticks=$identity.start_ticks;phase=$Phase;allocation_stacks_only=$false;
            instrumented_diagnostic=$true;release_eligible=$false;qualification_credit=0;
            started_utc=$receipt.started_utc;finished_utc=$receipt.finished_utc;
            exit_code=$receipt.capture_exit_code;capture_receipt=$receipt} |
            ConvertTo-Json -Depth 8 | Set-Content -LiteralPath ($prefix+'.json') -Encoding UTF8
        return
    }
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
