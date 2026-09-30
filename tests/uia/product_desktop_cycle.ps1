param([Parameter(Mandatory=$true)][string]$Configuration)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes
$config=Get-Content -LiteralPath $Configuration -Raw | ConvertFrom-Json
$OutputDirectory=$config.output_directory
$Executable=$config.executable
$started=[DateTime]::Parse($config.started_utc).ToUniversalTime()
$Cycles=[int]$config.cycles
$MinimumSeconds=[int]$config.minimum_seconds
$ShareSeconds=[int]$config.share_seconds
$LogPairSeconds=[int]$config.log_pair_seconds
$StopSettleSeconds=[int]$config.stop_settle_seconds
$RoomSettleSeconds=[int]$config.room_settle_seconds
$HeapDiagnostic=[bool]$config.heap_diagnostic
$HeapDiagnosticNoShare=[bool]$config.no_share
$HeapDiagnosticNoExport=[bool]$config.no_export
$HeapCheckOnly=[bool]$config.heap_check_only
$HeapPageCheck=[bool]$config.heap_page_check
$CrashDiagnostic=[bool]$config.crash_diagnostic
$ProbeOnly=$false
$DedicatedDesktopSessionId=[int]$config.dedicated_session
$script:desktopBaseline=$config.desktop_baseline
$script:runId=$config.run_id
$script:cycle=[int]$config.cycle
$script:cycleId=$config.cycle_id
$script:nativeProcessRun=$null
$script:nativeSession=$null
$script:participantHash=$null
$script:operation=''
$script:step='isolated-cycle-start'
$script:rootCache=@{}
$script:lastResourceSample=[DateTime]::MinValue
$script:layout='unknown'
$script:child=$null
$resultPath=$config.result_path
$exitCode=1
try {
    $script:child=Get-Process -Id $config.product_pid
    if ($script:child.Path -ne $Executable -or $script:child.StartTime.ToUniversalTime().Ticks -ne [long]$config.product_start_ticks) {
        throw 'ISOLATED_CYCLE_PRODUCT_IDENTITY_CHANGED'
    }
    . (Join-Path $PSScriptRoot 'product_desktop_evidence.ps1')
    # Reuse the actual driver functions; the child performs only one cycle.
    # It never starts, closes, or kills the product owned by the supervisor.
    $tokens=$null;$parseErrors=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'product_desktop.ps1'),[ref]$tokens,[ref]$parseErrors)
    if ($parseErrors.Count) {throw 'ISOLATED_CYCLE_DRIVER_PARSE_FAILED'}
    foreach ($definition in $ast.FindAll({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst]},$false)) {
        Invoke-Expression $definition.Extent.Text
    }
    if ($HeapDiagnostic) {
        . (Join-Path $PSScriptRoot '../runtime/tools/product_acceptance/product_heap_diagnostic.ps1')
        $script:heapDirectory=Join-Path $OutputDirectory 'heap-diagnostic'
        $script:heapTools=Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/Debuggers/x64'
    }
    Run-Cycle
    @{run_id=$script:runId;cycle=$script:cycle;cycle_id=$script:cycleId;product_pid=$script:child.Id;
        client_pid=$PID;status='COMPLETE';native_process_run=$script:nativeProcessRun;
        native_session=$script:nativeSession;participant_hash=$script:participantHash;layout=$script:layout} |
        ConvertTo-Json | Set-Content -LiteralPath $resultPath -Encoding UTF8
    $exitCode=0
} catch {
    $failure=$_.Exception.Message
    if (Get-Command Save-Tree -ErrorAction SilentlyContinue) {
        try {Save-Tree ('cycle-{0:d4}-worker-failure' -f $script:cycle)} catch {}
    }
    @{run_id=$script:runId;cycle=$script:cycle;cycle_id=$script:cycleId;product_pid=$config.product_pid;
        client_pid=$PID;status='FAIL';step=$script:step;reason=$failure} |
        ConvertTo-Json | Set-Content -LiteralPath $resultPath -Encoding UTF8
} finally {
    if ($script:child) {$script:child.Dispose()}
}
exit $exitCode
