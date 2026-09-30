param([Parameter(Mandatory=$true)][string]$PreparedDirectory,
    [Parameter(Mandatory=$true)][ValidatePattern('^i-[a-zA-Z0-9]+$')][string]$Instance,
    [Parameter(Mandatory=$true)][string]$ServiceUrl,
    [string]$Python='python',
    [string]$Binary='',
    [ValidateSet('recovery', 'full', 'lifecycle', 'diagnostic', 'network')][string]$Mode = 'recovery')

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
Assert-ProbeServiceUrl $ServiceUrl

$prepared = (Resolve-Path -LiteralPath $PreparedDirectory).Path
$repository = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
if (-not $Binary) { throw 'Binary is required for this scenario' }; $binary = (Resolve-Path -LiteralPath $Binary).Path
$remote = $null
$credentialPath = $null
$process = $null
$proxy = $null
$faultTriggered = $false

try {
    $setup = @'
python3 - <<'PY'
import json, os, pathlib, subprocess, time, uuid, yaml
config = yaml.safe_load(pathlib.Path('/root/livekit.yaml').read_text())
key, secret = next(iter(config['keys'].items()))
env = os.environ.copy()
env.update(LIVEKIT_URL='http://127.0.0.1:17880',
           LIVEKIT_API_KEY=key, LIVEKIT_API_SECRET=secret)
room = 'logging-l3-' + time.strftime('%Y%m%dT%H%M%SZ', time.gmtime()) + '-' + uuid.uuid4().hex[:6]
tokens = {}
for identity in ('logging-l3-sender', 'logging-l3-receiver'):
    tokens[identity] = subprocess.check_output([
        'lk', 'token', 'create', '--room', room, '--identity', identity,
        '--join', '--valid-for', '10m', '--token-only'], env=env, text=True, timeout=15).strip()
    if not tokens[identity] or len(tokens[identity]) > 8192:
        raise RuntimeError('invalid_room_token')
path = pathlib.Path('/tmp') / (room + '-credential.json')
fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
with os.fdopen(fd, 'w') as stream:
    json.dump({'LIVEKIT_URL': 'ws://123.56.225.164:17880',
               'LIVEKIT_TOKEN': tokens['logging-l3-sender'],
               'LIVEKIT_PEER_TOKEN': tokens['logging-l3-receiver']}, stream)
print(json.dumps({'room': room, 'credential_path': str(path)}))
PY
'@
    $setupOutput = & workbench exec -i $instance -c (Expand-ProbeCommand $setup)
    if ($LASTEXITCODE -ne 0) { throw 'Remote token generation failed.' }
    $remote = $setupOutput | ConvertFrom-Json
    if ($remote.room -notmatch '^logging-l3-[A-Za-z0-9TZ-]+$' -or
        $remote.credential_path -ne ('/tmp/' + $remote.room + '-credential.json')) {
        throw ('Remote token metadata is invalid: room=' + $remote.room +
            ' credential_path=' + $remote.credential_path)
    }

    $credentialPath = Join-Path $prepared ($remote.room + '-credential.json')
    & workbench download $remote.credential_path $credentialPath -i $instance | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Room token download failed.' }
    $credential = Get-Content -LiteralPath $credentialPath -Raw | ConvertFrom-Json
    Remove-Item -LiteralPath $credentialPath -Force
    $credentialPath = $null

    $env:LIVEKIT_URL = $credential.LIVEKIT_URL
    $env:LIVEKIT_TOKEN = $credential.LIVEKIT_TOKEN
    $env:LIVEKIT_PEER_TOKEN = $credential.LIVEKIT_PEER_TOKEN
    $env:LIVEKIT_TEST_ALLOW_INSECURE = '1'
    $env:LIVEKIT_TEST_MEDIA_DIAGNOSTICS = '1'
    if ($Mode -eq 'network') {
        $repo = $repository
        $proxyScript = Join-Path $repo 'tests\runtime\livekit_signal_fault_proxy.py'
        $proxyStdout = Join-Path $prepared ($remote.room + '-proxy-stdout.log')
        $proxyStderr = Join-Path $prepared ($remote.room + '-proxy-stderr.log')
        $proxy = Start-Process -FilePath (Get-Command $Python).Source -PassThru -WindowStyle Hidden `
            -ArgumentList @($proxyScript, '--listen-port', '17881', '--control-port', '17882',
                '--target-host', ([Uri]$ServiceUrl).Host, '--target-port', ([string]([Uri]$ServiceUrl).Port)) `
            -RedirectStandardOutput $proxyStdout -RedirectStandardError $proxyStderr
        $readyDeadline = [DateTime]::UtcNow.AddSeconds(10)
        while ([DateTime]::UtcNow -lt $readyDeadline) {
            if ($proxy.HasExited) { throw 'Signaling fault proxy exited before ready.' }
            if ((Test-Path -LiteralPath $proxyStdout) -and
                (Select-String -LiteralPath $proxyStdout -Pattern '^\[PROXY_READY\]$' -Quiet)) { break }
            Start-Sleep -Milliseconds 100
        }
        if ([DateTime]::UtcNow -ge $readyDeadline) { throw 'Signaling fault proxy did not become ready.' }
        $env:LIVEKIT_PEER_URL = 'ws://127.0.0.1:17881'
    }
    if ($Mode -eq 'diagnostic' -or $Mode -eq 'network') {
        $diagnosticRoot = Join-Path $prepared ($remote.room + '-diagnostics')
        New-Item -ItemType Directory -Path $diagnosticRoot -ErrorAction Stop | Out-Null
        $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent().User
        $acl = New-Object System.Security.AccessControl.DirectorySecurity
        $acl.SetOwner($identity)
        $acl.SetAccessRuleProtection($true, $false)
        $rule = New-Object System.Security.AccessControl.FileSystemAccessRule(
            $identity, [System.Security.AccessControl.FileSystemRights]::FullControl,
            [System.Security.AccessControl.InheritanceFlags]'ContainerInherit, ObjectInherit',
            [System.Security.AccessControl.PropagationFlags]::None,
            [System.Security.AccessControl.AccessControlType]::Allow)
        $acl.AddAccessRule($rule)
        Set-Acl -LiteralPath $diagnosticRoot -AclObject $acl -ErrorAction Stop
        $env:LIVEKIT_TEST_DIAG_ROOT = $diagnosticRoot
    }
    $stdout = Join-Path $prepared ($remote.room + '-stdout.log')
    $stderr = Join-Path $prepared ($remote.room + '-stderr.log')
    if ($Mode -ne 'full') {
        $probeArgument = if ($Mode -eq 'network') {
            '--network-fault-probe'
        } elseif ($Mode -eq 'recovery' -or $Mode -eq 'diagnostic') {
            '--recovery-and-leave'
        } else {
            '--lifecycle-probe'
        }
        $process = Start-Process -FilePath $binary -ArgumentList $probeArgument `
            -PassThru -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    } else {
        $process = Start-Process -FilePath $binary -PassThru -WindowStyle Hidden `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    }
    $null = $process.Handle
    $env:LIVEKIT_TOKEN = $null
    $env:LIVEKIT_PEER_TOKEN = $null
    $env:LIVEKIT_PEER_URL = $null
    $env:LIVEKIT_TEST_DIAG_ROOT = $null
    $credential = $null

    $samples = @()
    while (!$process.HasExited) {
        if ($Mode -eq 'network') {
            if (!$faultTriggered -and (Test-Path -LiteralPath $stdout) -and
                (Select-String -LiteralPath $stdout -Pattern '^\[FAULT_READY\] signaling_tcp$' -Quiet)) {
                $control = [System.Net.Sockets.TcpClient]::new('127.0.0.1', 17882)
                try {
                    $stream = $control.GetStream()
                    $writer = [System.IO.StreamWriter]::new($stream)
                    $writer.AutoFlush = $true
                    $reader = [System.IO.StreamReader]::new($stream)
                    $writer.WriteLine('fault 3')
                    if ($reader.ReadLine() -ne 'OK') { throw 'Signaling fault proxy rejected the command.' }
                    $faultTriggered = $true
                } finally {
                    $control.Dispose()
                }
            }
            Start-Sleep -Milliseconds 200
            $process.Refresh()
            continue
        }
        if ($Mode -eq 'lifecycle' -or $Mode -eq 'diagnostic') {
            Start-Sleep -Seconds 2
            $process.Refresh()
            continue
        }
        $room = $remote.room
        $sample = @"
python3 - <<'PY'
import json, os, pathlib, re, subprocess
import yaml
config = yaml.safe_load(pathlib.Path('/root/livekit.yaml').read_text())
key, secret = next(iter(config['keys'].items()))
env = os.environ.copy()
env.update(LIVEKIT_URL='http://127.0.0.1:17880',
           LIVEKIT_API_KEY=key, LIVEKIT_API_SECRET=secret)
room = '$room'
result = {}
for identity in ('logging-l3-sender', 'logging-l3-receiver'):
    try:
        output = subprocess.check_output([
            'lk', 'room', 'participants', 'get', '--room', room, identity],
            env=env, text=True, stderr=subprocess.DEVNULL, timeout=5)
        sources = re.findall(r'\x22?source\x22?\s*:\s*\x22?([A-Z_0-9]+)',
                             output, re.IGNORECASE)
        result[identity] = {
            'sources': sources,
        }
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
        result[identity] = None
print(json.dumps(result))
PY
"@
        $snapshotOutput = & workbench exec -i $instance -c (Expand-ProbeCommand $sample)
        if ($LASTEXITCODE -eq 0) {
            $samples += [pscustomobject]@{
                time_utc = [DateTime]::UtcNow.ToString('o')
                tracks = ($snapshotOutput | ConvertFrom-Json)
            }
        }
        Start-Sleep -Seconds 2
        $process.Refresh()
    }

    $process.WaitForExit()
    $nativeExitCode = $process.ExitCode
    if ($null -eq $nativeExitCode) { throw 'Probe process exit code is unavailable.' }
    $resultLine = (Select-String -LiteralPath $stdout -Pattern '^\[RESULT\]' |
        Select-Object -Last 1).Line
    $diagnosticLine = $null
    $diagnosticFiles = @()
    if ($Mode -eq 'diagnostic' -or $Mode -eq 'network') {
        $diagnosticLine = (Select-String -LiteralPath $stdout -Pattern '^\[DIAGNOSTIC\]' |
            Select-Object -Last 1).Line
        $diagnosticFiles = @(Get-ChildItem -LiteralPath $diagnosticRoot -Recurse -File -Filter 'segment-*.jsonl')
    }
    $diagnosticPassed = ($Mode -ne 'diagnostic' -and $Mode -ne 'network') -or (
        $diagnosticLine -match '^\[DIAGNOSTIC\] accepted=([1-9][0-9]*) written=([1-9][0-9]*) dropped_ordinary=([0-9]+) dropped_critical=0 sink_failures=0 drain=1$' -and
        $diagnosticFiles.Count -gt 0)
    $validatedCode = if ($nativeExitCode -eq 0 -and
        $resultLine -eq '[RESULT] SCREEN_SHARE_L3 PASS' -and $diagnosticPassed -and
        ($Mode -ne 'network' -or ($faultTriggered -and
            (Select-String -LiteralPath $stdout -Pattern '^\[CASE\] actual_signaling_network_fault PASS' -Quiet)))) { 0 } else { 1 }
    $cases = @(Select-String -LiteralPath $stdout -Pattern '^\[CASE\]|^\[FAILURE\]|^\[SCREEN_DELIVERY_STATE\]' |
        ForEach-Object Line)
    $result = [ordered]@{
        schema = 1
        room = $remote.room
        mode = $Mode
        exit_code = $validatedCode
        process_exit_code = $nativeExitCode
        exit_code_source = 'process_exit_and_result_markers'
        result = $resultLine
        diagnostic = $diagnosticLine
        diagnostic_segment_count = $diagnosticFiles.Count
        fault_triggered = $faultTriggered
        cases = $cases
        server_track_samples = $samples
        binary_sha256 = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash
        stdout_log = Split-Path -Leaf $stdout
        stdout_sha256 = (Get-FileHash -LiteralPath $stdout -Algorithm SHA256).Hash
    }
    $resultPath = Join-Path $prepared ($remote.room + '-result.json')
    $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resultPath -Encoding UTF8
    Write-Output ($result | ConvertTo-Json -Depth 8)
    if ($validatedCode -ne 0) { exit $validatedCode }
} finally {
    $env:LIVEKIT_TOKEN = $null
    $env:LIVEKIT_PEER_TOKEN = $null
    $env:LIVEKIT_PEER_URL = $null
    $env:LIVEKIT_TEST_MEDIA_DIAGNOSTICS = $null
    $env:LIVEKIT_TEST_DIAG_ROOT = $null
    if ($proxy -and !$proxy.HasExited) {
        $proxy.Kill()
        $proxy.WaitForExit()
    }
    if ($credentialPath -and (Test-Path -LiteralPath $credentialPath)) {
        Remove-Item -LiteralPath $credentialPath -Force
    }
    if ($remote -and $remote.credential_path -match '^/tmp/logging-l3-[A-Za-z0-9TZ-]+-credential\.json$') {
        & workbench exec -i $instance -c ("rm -f -- '" + $remote.credential_path + "'") | Out-Null
    }
}
