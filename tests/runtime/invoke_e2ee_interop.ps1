param(
    [Parameter(Mandatory)][ValidatePattern('^[a-z0-9-]+$')][string]$RunId,
    [ValidateSet('shared', 'participant')][string]$Mode = 'shared',
    [ValidateSet('good', 'wrong', 'missing')][string]$KeyState = 'good',
    [ValidateSet('official-to-native', 'native-to-official')]
    [string]$Direction = 'official-to-native',
    [switch]$TwoPeer,
    [string]$OfficialSdkBin = 'E:\vsSource\WebRTC\client-sdk-cpp\build-debug\bin'
)
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$receiver = Join-Path $workspace 'build-debug/Debug/test_stream_delivery_runtime.exe'
$sender = Join-Path $workspace 'build-debug/e2ee-peer/Debug/test_e2ee_official_sender.exe'
$officialReceiver = Join-Path $workspace 'build-debug/e2ee-peer/Debug/test_e2ee_official_receiver.exe'
$evidenceGroup = if ($Direction -eq 'native-to-official') { 'e2ee-reverse-20260920' } else { 'e2ee-interop-20260920' }
$evidence = Join-Path $workspace "build-debug/evidence/$evidenceGroup/$RunId"
if ($TwoPeer -and $Direction -eq 'native-to-official') { throw 'Reverse validation requires all three peers' }
if (Test-Path -LiteralPath $evidence) { throw 'Evidence run id already exists' }
$null = New-Item -ItemType Directory -Path $evidence
$expected = if ($KeyState -eq 'good') { '2' } else { '0' }
$processes = @()

function Start-Peer([string]$Name, [string]$Executable, [string[]]$PeerArgs,
                    [string]$Token, [string]$PeerKeyState) {
    if ([string]::IsNullOrWhiteSpace($Token) -or [string]::IsNullOrWhiteSpace($env:LIVEKIT_URL)) {
        throw 'LIVEKIT_URL and three LIVEKIT_L3_TOKEN_TE* environment variables are required'
    }
    $environment = @{
        LIVEKIT_TOKEN = $Token
        LIVEKIT_L3_TOKEN_TE1 = $null
        LIVEKIT_L3_TOKEN_TE2 = $null
        LIVEKIT_L3_TOKEN_TE3 = $null
        LIVEKIT_L3_E2EE_MODE = $Mode
        LIVEKIT_L3_E2EE_KEY_STATE = $PeerKeyState
        RUST_LOG = 'off'
        PATH = "$OfficialSdkBin;$env:PATH"
    }
    $proc = Start-Process -FilePath $Executable -ArgumentList $PeerArgs -PassThru `
        -WorkingDirectory $workspace -WindowStyle Hidden -Environment $environment `
        -RedirectStandardOutput (Join-Path $evidence "$Name.stdout.log") `
        -RedirectStandardError (Join-Path $evidence "$Name.stderr.log")
    return $proc
}

function Wait-Connected($Proc, [string]$Name) {
    $deadline = [DateTime]::UtcNow.AddSeconds(25)
    $log = Join-Path $evidence "$Name.stdout.log"
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($Proc.HasExited) { throw 'Receiver exited before connection completed' }
        if ((Test-Path -LiteralPath $log) -and
            (Select-String -LiteralPath $log -SimpleMatch '[SESSION] local_identity=' -Quiet)) { return }
        Start-Sleep -Milliseconds 100
    }
    throw 'Receiver connection deadline expired'
}

try {
    if ($Direction -eq 'native-to-official') {
        $target = Start-Peer 'target' $officialReceiver @($RunId, $Mode, 'te2', $KeyState) `
            $env:LIVEKIT_L3_TOKEN_TE1 $KeyState
        $processes += $target
        Wait-Connected $target 'target'
        $observer = Start-Peer 'observer' $receiver @('--role', 'receiver', '--case', 'e2ee-outbound',
            '--run-id', $RunId, '--topic', 'lk.l3.e2ee', '--expect-sender', 'te2',
            '--expect-complete', '0', '--expect-incomplete', '0', '--timeout-sec', '60', '--settle-sec', '2') `
            $env:LIVEKIT_L3_TOKEN_TE3 'good'
        $processes += $observer
        Wait-Connected $observer 'observer'
        $native = Start-Peer 'native' $receiver @('--role', 'sender', '--case', 'e2ee-outbound',
            '--run-id', $RunId, '--topic', 'lk.l3.e2ee', '--destination', 'te1', '--observer', 'te3',
            '--expect-complete', $expected, '--expect-incomplete', '0', '--timeout-sec', '60', '--settle-sec', '2') `
            $env:LIVEKIT_L3_TOKEN_TE2 'good'
        $processes += $native
        $names = @('target', 'observer', 'native')
    } else {
        $common = @('--role', 'receiver', '--case', 'e2ee-interop', '--run-id', $RunId,
            '--topic', 'lk.l3.e2ee', '--expect-sender', 'te2', '--expect-incomplete', '0',
            '--timeout-sec', '60', '--settle-sec', '2')
        $target = Start-Peer 'target' $receiver ($common + @('--expect-complete', $expected)) `
            $env:LIVEKIT_L3_TOKEN_TE1 $KeyState
        $processes += $target
        Wait-Connected $target 'target'
        $observerIdentity = '-'
        $names = @('target', 'official')
        if (-not $TwoPeer) {
            $observer = Start-Peer 'observer' $receiver ($common + @('--expect-complete', '0')) `
                $env:LIVEKIT_L3_TOKEN_TE3 'good'
            $processes += $observer
            Wait-Connected $observer 'observer'
            $observerIdentity = 'te3'
            $names = @('target', 'observer', 'official')
        }
        $official = Start-Peer 'official' $sender @($RunId, $Mode, 'te1', $observerIdentity, $expected) `
            $env:LIVEKIT_L3_TOKEN_TE2 'good'
        $processes += $official
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(80)
    while (@($processes | Where-Object { -not $_.HasExited }).Count -gt 0) {
        if ([DateTime]::UtcNow -ge $deadline) { throw 'Matrix process deadline expired' }
        Start-Sleep -Milliseconds 100
    }
    foreach ($proc in $processes) { $proc.WaitForExit() }
    $passed = @($processes | Where-Object ExitCode -ne 0).Count -eq 0
    Write-Output "[MATRIX] run=$RunId direction=$Direction mode=$Mode key_state=$KeyState passed=$passed two_peer=$TwoPeer"
    foreach ($name in $names) {
        Write-Output "[PEER] $name"
        Get-Content -LiteralPath (Join-Path $evidence "$name.stdout.log") |
            Select-String '^\[(E2EE|RECEIVE_SUMMARY|RESULT|RESULT_DETAIL|OFFICIAL|STREAM|ERROR)\]' |
            ForEach-Object { $_.Line }
    }
    if (-not $passed) { exit 1 }
} catch {
    # Never echo an exception that might contain a process environment/credential.
    Write-Output '[MATRIX] INCONCLUSIVE setup_or_process_failure; inspect sanitized peer logs'
    exit 2
} finally {
    foreach ($proc in $processes) {
        if (-not $proc.HasExited) { $proc.Kill($true); $proc.WaitForExit() }
    }
}
