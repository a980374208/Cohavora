param(
    [Parameter(Mandatory=$true)][string]$EvidenceDirectory,
    [Parameter(Mandatory=$true)][string]$PreparedDirectory,
    [Parameter(Mandatory=$true)][string]$Validation,
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$AudioCollector,
    [Parameter(Mandatory=$true)][int]$DedicatedDesktopSessionId,
    [ValidateSet('strict','diagnostic')][string]$DesktopInputPolicy='strict',
    [switch]$AudioTimingDiagnostic,
    [switch]$GpuBudgetDiagnostic,
    [ValidateRange(-10,0)][int]$DiagnosticReceiverNice=0,
    [switch]$DiagnosticNoRealtime
)
$ErrorActionPreference='Stop'
if($DiagnosticReceiverNice -ne 0 -and !$AudioTimingDiagnostic){throw 'RECEIVER_PRIORITY_REQUIRES_TIMING_DIAGNOSTIC'}
if($DiagnosticNoRealtime -and (!$AudioTimingDiagnostic -or $DiagnosticReceiverNice -ne 0)){throw 'NO_REALTIME_REQUIRES_TIMING_DIAGNOSTIC_NICE_ZERO'}
$workspace=(Resolve-Path (Join-Path $PSScriptRoot '../../../..')).Path
Set-Location $workspace
& python "$PSScriptRoot/release_product_acceptance.py" --validate-only --validation $Validation --require-full-media-gpu --desktop-input-policy $DesktopInputPolicy
if($LASTEXITCODE -ne 0){throw 'CURRENT_SOURCE_VALIDATION_REJECTED'}
$EvidenceDirectory=(Resolve-Path -LiteralPath $EvidenceDirectory).Path
$Executable=(Resolve-Path -LiteralPath $Executable).Path
$AudioCollector=(Resolve-Path -LiteralPath $AudioCollector).Path
if ((Split-Path (Split-Path $Executable) -Leaf) -ne 'RelWithDebInfo' -or
    (Split-Path (Split-Path $AudioCollector) -Leaf) -ne 'RelWithDebInfo') {
    throw 'B14_RELWITHDEBINFO_REQUIRED'
}
. (Join-Path $workspace 'tests/uia/product_desktop_evidence.ps1')
$desktop=Assert-DedicatedDesktop $DedicatedDesktopSessionId -InputPolicy $DesktopInputPolicy
$lock=$null
$wakeRequested=$false
$outcome=[ordered]@{status='RUNNING';phase='preflight';pid=$PID;
    configuration='RelWithDebInfo';started_utc=[DateTime]::UtcNow.ToString('o');
    evidence_directory=$EvidenceDirectory;dedicated_session=$DedicatedDesktopSessionId;desktop_input_policy=$DesktopInputPolicy}
function Save-ControllerState {
    $outcome.updated_utc=[DateTime]::UtcNow.ToString('o')
    $outcome | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'controller-state.json') -Encoding UTF8
}
try {
    $lock=[IO.File]::Open((Join-Path $workspace 'out/product-acceptance.lock'),
        [IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
public static class B14WakeRequest {
    [DllImport("kernel32.dll")]
    public static extern uint SetThreadExecutionState(uint flags);
}
'@
    if ([B14WakeRequest]::SetThreadExecutionState([uint32]2147483651) -eq 0) {
        throw 'B14_WAKE_REQUEST_FAILED'
    }
    $wakeRequested=$true
    $first=Join-Path $EvidenceDirectory 'pilot-01'
    $prefix=Join-Path $EvidenceDirectory 'pilot'
    $gate=Join-Path $EvidenceDirectory 'formal-release-gate.json'
    $formal=Join-Path $EvidenceDirectory 'formal-8h100'
    $outcome.phase='pilot-01'
    Save-ControllerState
    $diagnosticArgs=@()
    if($AudioTimingDiagnostic){$diagnosticArgs+='-AudioTimingDiagnostic'}
    if($AudioTimingDiagnostic){$diagnosticArgs+=@('-DiagnosticReceiverNice',$DiagnosticReceiverNice.ToString())}
    if($DiagnosticNoRealtime){$diagnosticArgs+='-DiagnosticNoRealtime'}
    if($GpuBudgetDiagnostic){$diagnosticArgs+='-GpuBudgetDiagnostic'}
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot/invoke_product_external.ps1" -Root $first -PreparedDirectory $PreparedDirectory -DedicatedDesktopSessionId $DedicatedDesktopSessionId -DesktopInputPolicy $DesktopInputPolicy -Executable $Executable -AudioCollector $AudioCollector @diagnosticArgs
    $firstExit=$LASTEXITCODE
    if($AudioTimingDiagnostic -or $GpuBudgetDiagnostic){
        $outcome.diagnostic_only=$true
        $outcome.release_eligible=$false
        $outcome.run_exit_code=$firstExit
        $outcome.status=if($firstExit -eq 0){'DIAGNOSTIC_COMPLETE'}else{'FAIL'}
        $outcome.phase=if($GpuBudgetDiagnostic){'gpu-budget-diagnostic-reviewed'}else{'audio-timing-diagnostic-reviewed'}
    }else{
        if ($firstExit -ne 0) {throw 'FIRST_PILOT_FAILED'}
        $null=Assert-DedicatedDesktop $DedicatedDesktopSessionId $desktop $DesktopInputPolicy
        $outcome.phase='pilot-02-03-and-gated-formal'
        Save-ControllerState
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot/continue_product_acceptance.ps1" -FirstPilot $first -PilotPrefix $prefix -PreparedDirectory $PreparedDirectory -Validation $Validation -Gate $gate -FormalRoot $formal -DedicatedDesktopSessionId $DedicatedDesktopSessionId -DesktopInputPolicy $DesktopInputPolicy -Executable $Executable -AudioCollector $AudioCollector
        if ($LASTEXITCODE -ne 0) {throw 'B14_CONTINUATION_FAILED'}
        $outcome.status='COMPLETE'
        $outcome.phase='formal-reviewed'
    }
} catch {
    $outcome.status='FAIL'
    $outcome.reason=$_.Exception.Message
} finally {
    if ($wakeRequested) {$null=[B14WakeRequest]::SetThreadExecutionState([uint32]2147483648)}
    if ($lock) {$lock.Dispose()}
    $outcome.finished_utc=[DateTime]::UtcNow.ToString('o')
    Save-ControllerState
}
if ($outcome.status -notin @('COMPLETE','DIAGNOSTIC_COMPLETE')) {exit 1}
