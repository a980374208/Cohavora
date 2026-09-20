param(
    [Parameter(Mandatory=$true)][string]$TriggerExe,
    [Parameter(Mandatory=$true)][string]$AdapterLuid,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)][string]$ObserverState,
    [ValidateSet('desktop_gl_probe', 'test_participant_window_remediation')]
    [string]$ObserverProcessName = 'desktop_gl_probe',
    [ValidateSet('angle', 'dx11')][string]$ObserverApi = 'angle',
    [ValidateSet('fixture', 'live-meeting')][string]$ObserverScenario = 'fixture'
)
$ErrorActionPreference = 'Stop'
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Administrator token is required for this explicitly authorized system diagnostic.'
}
$triggerPath = (Resolve-Path -LiteralPath $TriggerExe).Path
if ($AdapterLuid -notmatch '^[0-9a-f]{8}:[0-9a-f]{8}$') { throw 'Invalid adapter LUID' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
# Do not issue a reset after the observer has timed out, while UAC was pending,
# or from a stale ready file. Require a fresh heartbeat from the live probe.
$observerPath = (Resolve-Path -LiteralPath $ObserverState).Path
$observer = Get-Content -LiteralPath $observerPath -Raw | ConvertFrom-Json
$age = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() - $observer.timestampMs
$observerProcess = Get-Process -Id $observer.pid -ErrorAction Stop
if ($observer.phase -ne 'waiting' -or $age -lt 0 -or $age -gt 3000 -or
    $observer.reason -ne '0x0' -or -not $observer.gpuReady -or
    $observerProcess.ProcessName -ne $ObserverProcessName) {
    throw 'Observer is not currently presenting with a healthy device; no reset issued.'
}
if ($ObserverProcessName -eq 'test_participant_window_remediation' -and
    ($observer.api -ne $ObserverApi -or $observer.scenario -ne $ObserverScenario -or
     $observer.smoke -ne $false -or $observer.adapterLuid -ne $AdapterLuid)) {
    throw 'Observer API identity/adapter mismatch or smoke run; no reset issued.'
}
if ($ObserverScenario -eq 'live-meeting' -and
    ($observer.meetingConnected -ne $true -or $observer.remoteTrackCount -le 0 -or
     $observer.remoteMediaFlowing -ne $true)) {
    throw 'Live meeting observer has no connected room with flowing remote media; no reset issued.'
}
if ($ObserverProcessName -eq 'test_participant_window_remediation' -and $ObserverApi -eq 'dx11') {
    if ($observer.defaultBackend -ne $true -or $observer.nativeGpuPixelsVerified -ne $true -or
        $observer.rendererDeviceReason -ne '0x0' -or $observer.deliveredToGpu -le 0) {
        throw 'Default DX11 observer has no healthy rendering/pixel evidence; no reset issued.'
    }
} elseif ($observer.swaps -le 0) {
    throw 'Observer has no successful presentations; no reset issued.'
}
$registryPath = 'HKLM:/SYSTEM/CurrentControlSet/Control/GraphicsDrivers'
$registryKey = Get-Item -LiteralPath $registryPath
$hadValue = $registryKey.GetValueNames() -contains 'TdrTestMode'
$original = if ($hadValue) { $registryKey.GetValue('TdrTestMode') } else { $null }
$kind = if ($hadValue) { $registryKey.GetValueKind('TdrTestMode').ToString() } else { $null }
if ($hadValue -and $kind -ne 'DWord') { throw 'Unexpected original registry kind; leave it unchanged.' }
@{ hadValue=$hadValue; original=$original; kind=$kind; adapter=$AdapterLuid; started=(Get-Date).ToString('o') } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'registry-before.json')
$triggerExit = -1
try {
    # Microsoft D3DKMT_ESCAPE_TDRDBGCTRL requires this test switch. Do not change
    # TdrLevel/TdrDelay/TdrLimitCount or any other recovery/security setting.
    New-ItemProperty -LiteralPath $registryPath -Name TdrTestMode -Value 1 -PropertyType DWord -Force | Out-Null
    & $triggerPath --force-adapter-tdr $AdapterLuid *> (Join-Path $OutputDirectory 'trigger-elevated.log')
    $triggerExit = $LASTEXITCODE
    # Acceptance is asynchronous. Keep the test entry enabled until the observer
    # has captured the actual loss or the bounded observation interval expires.
    if ($triggerExit -eq 0) {
        $deadline = [DateTime]::UtcNow.AddSeconds(45)
        do {
            Start-Sleep -Milliseconds 250
            $observer = Get-Content -LiteralPath $observerPath -Raw | ConvertFrom-Json
        } while ($observer.phase -eq 'waiting' -and [DateTime]::UtcNow -lt $deadline)
        $observer | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'observer-after.json')
    }
} finally {
    if ($hadValue) {
        Set-ItemProperty -LiteralPath $registryPath -Name TdrTestMode -Value $original
    } else {
        Remove-ItemProperty -LiteralPath $registryPath -Name TdrTestMode -ErrorAction Stop
    }
    $after = Get-Item -LiteralPath $registryPath
    $restored = if ($hadValue) { $after.GetValue('TdrTestMode') -eq $original } else {
        -not ($after.GetValueNames() -contains 'TdrTestMode')
    }
    @{ restored=$restored; triggerExit=$triggerExit; finished=(Get-Date).ToString('o') } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'registry-restored.json')
    if (-not $restored) { throw 'Original TdrTestMode was not restored; inspect registry-before.json.' }
}
exit $triggerExit
