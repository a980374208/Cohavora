param(
    [Parameter(Mandatory=$true)][int]$DedicatedDesktopSessionId,
    [ValidateSet('strict','diagnostic')][string]$DesktopInputPolicy='strict',
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$AudioCollector,
    [Parameter(Mandatory=$true)][string]$FirstPilot,
    [Parameter(Mandatory=$true)][string]$PilotPrefix,
    [Parameter(Mandatory=$true)][string]$PreparedDirectory,
    [Parameter(Mandatory=$true)][string]$Validation,
    [Parameter(Mandatory=$true)][string]$Gate,
    [Parameter(Mandatory=$true)][string]$FormalRoot
)
$ErrorActionPreference='Stop'
Set-Location (Resolve-Path (Join-Path $PSScriptRoot '../../../..'))
function Assert-ContinuationCurrentSource {
    & python "$PSScriptRoot/release_product_acceptance.py" --validate-only --validation $Validation --require-full-media-gpu --desktop-input-policy $DesktopInputPolicy
    if($LASTEXITCODE -ne 0){throw 'CURRENT_SOURCE_VALIDATION_REJECTED'}
}
Assert-ContinuationCurrentSource
foreach($marker in 'qualification-excluded.json','diagnostic-debugger.json') {
    if(Test-Path -LiteralPath (Join-Path $FirstPilot $marker)) {
        throw 'FIRST_PILOT_QUALIFICATION_EXCLUDED'
    }
}
# This is one bounded test invocation, not a registered or recurring task.
$deadline=[DateTime]::UtcNow.AddMinutes(20)
$firstResult=Join-Path $FirstPilot 'runner-exit.json'
while(!(Test-Path $firstResult)) {
    if([DateTime]::UtcNow -ge $deadline){throw 'FIRST_PILOT_RESULT_TIMEOUT'}
    Start-Sleep -Seconds 2
}
$result=Get-Content $firstResult -Raw | ConvertFrom-Json
if($result.verdict -ne 'EVIDENCE_COMPLETE' -or $result.exit_code -ne 0){throw 'FIRST_PILOT_FAILED'}
$pilots=@($FirstPilot)
foreach($number in 2,3) {
    $path='{0}-{1:d2}' -f $PilotPrefix,$number
    Assert-ContinuationCurrentSource
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot/invoke_product_external.ps1" -Root $path -PreparedDirectory $PreparedDirectory -DedicatedDesktopSessionId $DedicatedDesktopSessionId -DesktopInputPolicy $DesktopInputPolicy -Executable $Executable -AudioCollector $AudioCollector
    if($LASTEXITCODE -ne 0){throw "PILOT_FAILED: $path"}
    $pilots+=$path
}
& python "$PSScriptRoot/release_product_acceptance.py" --pilot $pilots[0] --pilot $pilots[1] --pilot $pilots[2] --validation $Validation --output $Gate --require-full-media-gpu
if($LASTEXITCODE -ne 0){throw 'RELEASE_GATE_REJECTED'}
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot/invoke_product_external.ps1" -Root $FormalRoot -PreparedDirectory $PreparedDirectory -DedicatedDesktopSessionId $DedicatedDesktopSessionId -DesktopInputPolicy $DesktopInputPolicy -Executable $Executable -AudioCollector $AudioCollector -Mode Formal -ReleaseGate $Gate
$formalExit=$LASTEXITCODE
& python "$PSScriptRoot/report_product_acceptance.py" --root $FormalRoot --gate $Gate
if($LASTEXITCODE -ne 0){exit 1}
exit $formalExit
