param([Parameter(Mandatory=$true)][string]$PreparedDirectory,
    [Parameter(Mandatory=$true)][string]$Instance,
    [Parameter(Mandatory=$true)][string]$ServiceUrl,
    [string]$Python='python',
    [Parameter(Mandatory=$true)][string]$Binary,
    [string]$InputManifest='', [string]$Profile='', [string]$TargetConfig='',
    [ValidatePattern('^/[a-zA-Z0-9_./-]+$')][string]$RemoteConfigPath='/root/livekit.yaml',
    [string]$RemoteServiceUrl='',
    [ValidatePattern('^[a-zA-Z0-9][a-zA-Z0-9_.-]{0,127}$')][string]$SfuContainer='livekit',
    [switch]$Grid16Transport, [switch]$Grid16Transition,
      [switch]$NoSimulcast, [switch]$LowBandwidth)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
Assert-ProbeServiceUrl $ServiceUrl
$repository = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$prepared = (Resolve-Path -LiteralPath $PreparedDirectory).Path
$binaryPath = (Resolve-Path -LiteralPath $Binary).Path
if (-not (Test-Path -LiteralPath $binaryPath -PathType Leaf)) { throw 'Binary must be a file.' }
$transport = if ($TargetConfig) { 'ssh' } else { 'workbench' }
$targetConfigPath = $null
if ($transport -eq 'ssh') {
    if ($Instance -notmatch '^(?:i-[a-zA-Z0-9]+|ins-[a-zA-Z0-9]+|ssh:[a-fA-F0-9:.]+)$') {
        throw 'SSH Instance must be a bound instance identifier or ssh:<IP>.'
    }
    $targetConfigPath = (Resolve-Path -LiteralPath $TargetConfig).Path
    if (-not (Test-Path -LiteralPath $targetConfigPath -PathType Leaf)) { throw 'TargetConfig must be a file.' }
} elseif ($Instance -notmatch '^i-[a-zA-Z0-9]+$') {
    throw 'Workbench Instance must be an i-* identifier; SSH requires TargetConfig.'
}
$serviceUri = [Uri]$ServiceUrl
if (-not $RemoteServiceUrl) { $RemoteServiceUrl = "http://127.0.0.1:$($serviceUri.Port)" }
$remoteUri = $null
if (-not [Uri]::TryCreate($RemoteServiceUrl,[UriKind]::Absolute,[ref]$remoteUri) -or
        $remoteUri.Scheme -ne 'http' -or $remoteUri.Host -ne '127.0.0.1' -or
        $remoteUri.UserInfo -or $remoteUri.Query -or $remoteUri.Fragment -or
        $remoteUri.AbsolutePath -ne '/' -or $remoteUri.Port -lt 1) {
    throw 'RemoteServiceUrl must be an HTTP loopback origin without credentials or a path.'
}
if ($RemoteConfigPath.Split('/') -contains '..') { throw 'RemoteConfigPath must not traverse directories.' }

$remote = $null
$credentialPath = $null
$sampler = $null
$output = $null
$cleanupIssues = [Collections.Generic.List[string]]::new()
$transportProbe = $Grid16Transport -or $Grid16Transition
if ($Grid16Transport -and $Grid16Transition) { throw 'Select one probe mode.' }
if ($NoSimulcast -and -not $transportProbe) { throw 'NoSimulcast requires a transport probe.' }
if ($LowBandwidth -and -not $transportProbe) { throw 'LowBandwidth requires a transport probe.' }
if ($LowBandwidth -and $NoSimulcast) { throw 'Select one publishing mode.' }
if ([bool]$InputManifest -ne [bool]$Profile) { throw 'InputManifest and Profile must be supplied together.' }
if ($transportProbe -and -not $LowBandwidth -and -not $InputManifest) { throw 'Original-load grid16 probes require InputManifest and Profile.' }
$manifestPath = $profilePath = $null
$sshIdentityArguments = @()
if ($InputManifest) {
    $manifestPath = (Resolve-Path -LiteralPath $InputManifest).Path
    $profilePath = (Resolve-Path -LiteralPath $Profile).Path
    $freezeArguments = @('-B', (Join-Path $repository 'tests/runtime/tools/meeting/b11_input_freeze.py'),
        'verify', '--manifest', $manifestPath, '--executable', $binaryPath, '--profile', $profilePath,
        '--require-remote', '--instance', $Instance, '--service-url', $ServiceUrl, '--transport', $transport)
    if ($targetConfigPath) { $freezeArguments += @('--target-config', $targetConfigPath) }
    $frozenOutput = & $Python @freezeArguments
    if ($LASTEXITCODE -ne 0) { throw 'B11 frozen inputs did not verify; no remote operation was started.' }
    $frozenProfile = Get-Content -LiteralPath $profilePath -Raw | ConvertFrom-Json
    $profileTransport = if ($frozenProfile.transport) { $frozenProfile.transport } else { 'workbench' }
    if ($profileTransport -cne $transport) { throw 'B11 profile transport differs from the requested transport.' }
    if ($transport -eq 'ssh') {
        $profileTargetConfig = $frozenProfile.target_config
        if (-not [IO.Path]::IsPathRooted($profileTargetConfig)) { $profileTargetConfig = Join-Path $repository $profileTargetConfig }
        if ((Resolve-Path -LiteralPath $profileTargetConfig).Path -cne $targetConfigPath) {
            throw 'B11 profile SSH target differs from the requested target.'
        }
        $frozenManifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
        $sshIdentity = $frozenManifest.inputs.remote_transport_identity
        if ($sshIdentity.transport -cne 'ssh' -or
                $sshIdentity.config_sha256 -cnotmatch '^[0-9a-f]{64}$' -or
                $sshIdentity.known_hosts_sha256 -cnotmatch '^[0-9a-f]{64}$' -or
                $sshIdentity.key_public_fingerprint -cnotmatch '^SHA256:[A-Za-z0-9+/]{43}$') {
            throw 'B11 frozen SSH identity is missing or invalid.'
        }
        $sshIdentityArguments = @('--expected-config-sha256', $sshIdentity.config_sha256,
            '--expected-known-hosts-sha256', $sshIdentity.known_hosts_sha256,
            '--expected-key-public-fingerprint', $sshIdentity.key_public_fingerprint)
    }
    $expectedMode = if ($Grid16Transition) { 'grid16_transition' } elseif ($Grid16Transport) { 'grid16_transport' } else { 'render' }
    $expectedKind = if ($LowBandwidth) { 'low_bandwidth' } else { 'load_test' }
    if ($frozenProfile.probe_mode -ne $expectedMode -or
            $frozenProfile.target.config_path -cne $RemoteConfigPath -or
            $frozenProfile.target.local_service_url -cne $RemoteServiceUrl -or
            $frozenProfile.target.sfu_container -cne $SfuContainer -or
            $frozenProfile.publisher.kind -cne $expectedKind -or
            $frozenProfile.publisher.simulcast -ne (-not [bool]$NoSimulcast) -or
            $frozenProfile.publisher.count -ne 17 -or $frozenProfile.publisher.subscribers -ne 0 -or
            $frozenProfile.publisher.resolution -cne 'low' -or $frozenProfile.publisher.codec -cne 'vp8' -or
            $frozenProfile.publisher.duration -cne '15m' -or $frozenProfile.publisher.num_per_second -ne 5) {
        throw 'B11 profile differs from the requested probe or deployment.'
    }
} else {
    & $Python -B -c "import sys; sys.path.insert(0, sys.argv[1]); import meeting_soak; from pathlib import Path; meeting_soak.verify_runtime_binary(Path(sys.argv[2]))" `
        (Join-Path $repository 'tests/runtime/tools/meeting') $binaryPath
    if ($LASTEXITCODE -ne 0) { throw 'RelWithDebInfo runtime binary identity did not verify.' }
}
$serviceUrlJson = $ServiceUrl | ConvertTo-Json -Compress
$remoteServiceUrlJson = $RemoteServiceUrl | ConvertTo-Json -Compress
$remoteConfigJson = $RemoteConfigPath | ConvertTo-Json -Compress
$sfuContainerJson = $SfuContainer | ConvertTo-Json -Compress
$dockerArgumentsPython = if ($transport -eq 'ssh') { "['sudo', '-n', 'docker']" } else { "['docker']" }
$lkFallbackPython = if ($transport -eq 'ssh') { 'True' } else { 'False' }

function Invoke-B11Remote {
    param([ValidateSet('exec','upload','download')][string]$Operation,
        [string]$Command='', [string]$LocalPath='', [string]$RemotePath='', [switch]$Overwrite)
    if ($transport -eq 'workbench') {
        switch ($Operation) {
            exec { $result = & workbench exec -i $Instance -c $Command }
            upload {
                $arguments = @('upload', $LocalPath, $RemotePath, '-i', $Instance)
                if ($Overwrite) { $arguments += '-f' }
                $result = & workbench @arguments
            }
            download { $result = & workbench download $RemotePath $LocalPath -i $Instance }
        }
        $global:LASTEXITCODE = $LASTEXITCODE
        return $result
    }
    $commandFile = $null
    try {
        $arguments = @('-B', (Join-Path $repository 'tests/runtime/tools/meeting/b11_remote.py'),
            $Operation, '--target-config', $targetConfigPath) + $sshIdentityArguments
        if ($Operation -eq 'exec') {
            $commandFile = Join-Path $prepared ('.b11-command-' + [Guid]::NewGuid().ToString('N') + '.sh')
            $lfCommand = $Command.Replace("`r`n", "`n").Replace("`r", "`n")
            [IO.File]::WriteAllText($commandFile, $lfCommand, [Text.UTF8Encoding]::new($false))
            $arguments += @('--command-file', $commandFile)
        } else {
            $arguments += @('--local-path', $LocalPath, '--remote-path', $RemotePath)
            if ($Overwrite) { $arguments += '--overwrite' }
        }
        $result = & $Python @arguments
        $resultCode = $LASTEXITCODE
        $global:LASTEXITCODE = $resultCode
        if ($resultCode -ne 0) { throw ('B11 SSH ' + $Operation + ' failed.') }
        try { $response = ($result -join "`n") | ConvertFrom-Json } catch { throw 'B11 SSH response is invalid.' }
        if ($response.ok -ne $true -or $response.operation -cne $Operation -or $response.returncode -ne 0) {
            throw ('B11 SSH ' + $Operation + ' did not complete.')
        }
        if ($Operation -eq 'exec') {
            if ($response.stdout -isnot [string]) { throw 'B11 SSH execution metadata is invalid.' }
            return $response.stdout
        }
    } finally {
        if ($commandFile) { Remove-Item -LiteralPath $commandFile -Force -ErrorAction Stop }
    }
}

# Signal only the frozen process identity, then verify it actually exits.
$ownedStopPython = @'
def stop_owned(pid, ticks, matches, interrupt_seconds=3):
    proc = pathlib.Path('/proc') / str(pid)
    def current_command():
        try:
            fields = (proc / 'stat').read_text().split()
            if fields[21] != str(ticks) or fields[2] == 'Z':
                return None
            return (proc / 'cmdline').read_bytes().split(b'\0')
        except FileNotFoundError:
            return None
    command = current_command()
    result = {'owned': False, 'stopped': False}
    if command is None:
        result['stopped'] = True
        return result
    if not matches(command):
        result['reason'] = 'ownership_mismatch'
        return result
    result['owned'] = True
    for sig, grace in ((signal.SIGINT, interrupt_seconds), (signal.SIGTERM, 5)):
        command = current_command()
        if command is None:
            result['stopped'] = True
            return result
        if not matches(command):
            result['reason'] = 'ownership_mismatch'
            return result
        try:
            os.kill(pid, sig)
        except ProcessLookupError:
            result['stopped'] = True
            return result
        deadline = time.monotonic() + grace
        while time.monotonic() < deadline:
            if current_command() is None:
                result['stopped'] = True
                return result
            time.sleep(.1)
    result['stopped'] = current_command() is None
    if not result['stopped']:
        result['reason'] = 'stop_timeout'
    return result
'@
$originalLocation = Get-Location
$originalEnvironment = @{}
foreach ($name in @('LIVEKIT_URL','LIVEKIT_SOAK_TOKEN','LIVEKIT_SOAK_ALLOW_INSECURE')) {
    $originalEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class RenderProbePowerState {
    [DllImport("kernel32.dll")]
    public static extern uint SetThreadExecutionState(uint flags);
}
'@
$continuous = [Convert]::ToUInt32('80000000', 16)
[void][RenderProbePowerState]::SetThreadExecutionState($continuous -bor 3)

try {
    $noSimulcastPython = if ($NoSimulcast) { 'True' } else { 'False' }
    $lowBandwidthPython = if ($LowBandwidth) { 'True' } else { 'False' }
    if ($LowBandwidth) {
        Invoke-B11Remote -Operation upload -LocalPath (Join-Path $repository 'tests/runtime/tools/meeting/soak_low_bandwidth_publishers.py') `
            -RemotePath '/tmp/soak_low_bandwidth_publishers.py' -Overwrite | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Low-bandwidth publisher upload failed.' }
    }
    $setup = @"
python3 - <<'PY'
import hashlib, json, os, pathlib, shutil, signal, subprocess, time, uuid, yaml
config_path = pathlib.Path($remoteConfigJson)
config_bytes = config_path.read_bytes()
config = yaml.safe_load(config_bytes)
lk_path = shutil.which('lk')
if not lk_path and ${lkFallbackPython}:
    candidate = pathlib.Path.home() / '.local/bin/lk'
    if candidate.is_file():
        lk_path = str(candidate)
if not lk_path:
    raise RuntimeError('livekit_cli_missing')
lk_path = str(pathlib.Path(lk_path).resolve())
sfu_image = subprocess.check_output($dockerArgumentsPython + ['inspect', '-f', '{{.Image}}', $sfuContainerJson],
    text=True, timeout=15).strip()
if not sfu_image.startswith('sha256:') or len(sfu_image) != 71:
    raise RuntimeError('sfu_image_identity_unavailable')
remote_inputs = {'config_sha256': hashlib.sha256(config_bytes).hexdigest(),
    'lk_sha256': hashlib.sha256(pathlib.Path(lk_path).read_bytes()).hexdigest(), 'lk_path': lk_path,
    'sfu_image': sfu_image, 'logical_cpus': os.cpu_count(),
    'mem_total_kib': int(next(line.split()[1] for line in pathlib.Path('/proc/meminfo').read_text().splitlines()
                              if line.startswith('MemTotal:'))),
    'public_egress_verification': 'NOT_RUN', 'source_dimensions_fps': 'NOT_VERIFIED'}
key, secret = next(iter(config['keys'].items()))
env = os.environ.copy()
env['PATH'] = str(pathlib.Path(lk_path).parent) + os.pathsep + env.get('PATH', '')
env.update(LIVEKIT_URL=$remoteServiceUrlJson, LIVEKIT_API_KEY=key,
           LIVEKIT_API_SECRET=secret)
room = 'soak-render-' + time.strftime('%Y%m%dT%H%M%SZ', time.gmtime()) + '-' + uuid.uuid4().hex[:6]
root = pathlib.Path('/tmp') / room
root.mkdir(mode=0o700)
if ${lowBandwidthPython}:
    source = pathlib.Path('/tmp/soak-low-bandwidth-160x90-8fps-vp8-4h.ivf')
    expected = '4c089c3a9125375271c48444c50e839667cc0d34b85d738d4acf1811301476c4'
    if hashlib.sha256(source.read_bytes()).hexdigest() != expected:
        raise RuntimeError('low_bandwidth_source_hash_mismatch')
    options = ['python3', '/tmp/soak_low_bandwidth_publishers.py',
               '--room', room, '--source', str(source),
               '--directory', str(root), '--count', '17']
else:
    options = [
        lk_path, 'load-test', '--room', room, '--duration', '15m',
        '--video-publishers', '17', '--subscribers', '0',
        '--video-resolution', 'low', '--video-codec', 'vp8',
        '--num-per-second', '5', '--yes']
    if ${noSimulcastPython}:
        options.append('--no-simulcast')
publisher = subprocess.Popen(options, env=env,
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
try:
    time.sleep(20 if ${lowBandwidthPython} else 2)
    if publisher.poll() is not None:
        raise RuntimeError('publisher_exited_early')
    if ${lowBandwidthPython}:
        status = json.loads((root / 'publishers-status.json').read_text())
        participants = subprocess.check_output(
            [lk_path, 'room', 'participants', 'list', room], env=env,
            text=True, timeout=15)
        if status['state'] != 'RUNNING' or status['alive'] != 17 or participants.count('tracks: 1') != 17:
            raise RuntimeError('low_bandwidth_publishers_not_ready')
    token = subprocess.check_output([
        lk_path, 'token', 'create', '--room', room, '--identity', 'render-observer',
        '--join', '--valid-for', '15m', '--token-only'], env=env,
        text=True, timeout=15).strip()
    if not token or len(token) > 8192:
        raise RuntimeError('invalid_observer_token')
    credential = root / 'observer.json'
    with credential.open('x') as stream:
        json.dump({'LIVEKIT_URL': $serviceUrlJson,
                   'LIVEKIT_SOAK_TOKEN': token,
                   'LIVEKIT_SOAK_ALLOW_INSECURE': '1'}, stream)
    credential.chmod(0o600)
    ticks = pathlib.Path('/proc/' + str(publisher.pid) + '/stat').read_text().split()[21]
    metadata = {'room': room, 'remote_directory': str(root),
                'publisher_pid': publisher.pid, 'publisher_start_ticks': ticks,
                'publisher_kind': 'low_bandwidth' if ${lowBandwidthPython} else 'load_test',
                'remote_inputs': remote_inputs}
    (root / 'metadata.json').write_text(json.dumps(metadata))
    print(json.dumps(metadata))
except BaseException:
    credential = root / 'observer.json'
    if credential.exists():
        credential.unlink()
    if publisher.poll() is None:
        publisher.send_signal(signal.SIGINT)
    try:
        publisher.wait(timeout=20)
    except subprocess.TimeoutExpired:
        publisher.terminate()
        status_path = root / 'publishers-status.json'
        if ${lowBandwidthPython} and status_path.exists():
            for child in json.loads(status_path.read_text()).get('children', []):
                proc = pathlib.Path('/proc') / str(child['pid'])
                if proc.exists() and child.get('start_ticks') and \
                        (proc / 'stat').read_text().split()[21] == child['start_ticks'] and \
                        room.encode() in (proc / 'cmdline').read_bytes():
                    os.kill(child['pid'], signal.SIGTERM)
    raise
PY
"@
    $setupOutput = Invoke-B11Remote -Operation exec -Command $setup
    if ($LASTEXITCODE -ne 0) { throw 'Remote load setup failed.' }
    $remote = $setupOutput | ConvertFrom-Json
    if ($remote.room -notmatch '^soak-render-[A-Za-z0-9TZ-]+$' -or
            $remote.remote_directory -notmatch '^/tmp/soak-render-[A-Za-z0-9TZ-]+$' -or
            [string]$remote.publisher_pid -notmatch '^\d+$' -or
            $remote.publisher_start_ticks -notmatch '^\d+$' -or
            $remote.publisher_kind -notin @('load_test','low_bandwidth')) {
        $remote = $null
        throw 'Remote load identity is invalid.'
    }
    if ($transportProbe) {
        $runName = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '-' +
            [Guid]::NewGuid().ToString('N').Substring(0, 8)
        $folder = if ($LowBandwidth) { 'grid16-low-bandwidth' }
                  elseif ($Grid16Transition) { 'grid16-transition' }
                  elseif ($NoSimulcast) { 'grid16-no-simulcast' }
                  else { 'grid16-transport' }
        $output = Join-Path $prepared ($folder + '\' + $runName)
        New-Item -ItemType Directory -Path (Split-Path $output -Parent) -Force | Out-Null
        Invoke-B11Remote -Operation upload -LocalPath (Join-Path $repository 'tests/runtime/tools/meeting/ecs_resource_sampler.py') `
            -RemotePath ($remote.remote_directory + '/ecs_resource_sampler.py') | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'ECS sampler upload failed.' }
        $startSampler = @"
python3 - <<'PY'
import json, pathlib, subprocess, time
root=pathlib.Path('$($remote.remote_directory)')
sfu_pid=int(subprocess.check_output($dockerArgumentsPython + ['inspect','-f','{{.State.Pid}}',$sfuContainerJson],text=True))
sampler=subprocess.Popen(['python3',str(root/'ecs_resource_sampler.py'),
    '--output',str(root/'ecs-metrics.csv'),'--pid',str(sfu_pid),
    '--duration','600'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,
    start_new_session=True)
try:
    time.sleep(2)
    if sampler.poll() is not None:
        raise RuntimeError('ecs_sampler_exited_early')
    ticks=pathlib.Path('/proc/'+str(sampler.pid)+'/stat').read_text().split()[21]
    print(json.dumps({'pid':sampler.pid,'start_ticks':ticks,'sfu_pid':sfu_pid}))
except BaseException:
    if sampler.poll() is None:
        sampler.terminate()
        sampler.wait(timeout=10)
    raise
PY
"@
        $samplerOutput = Invoke-B11Remote -Operation exec -Command $startSampler
        if ($LASTEXITCODE -ne 0) { throw 'ECS sampler launch failed.' }
        $samplerCandidate = $samplerOutput | ConvertFrom-Json
        if ([string]$samplerCandidate.pid -notmatch '^\d+$' -or
                $samplerCandidate.start_ticks -notmatch '^\d+$' -or
                [string]$samplerCandidate.sfu_pid -notmatch '^\d+$') {
            throw 'ECS sampler identity is invalid.'
        }
        $sampler = $samplerCandidate
    }
    $credentialPath = Join-Path $prepared ($remote.room + '-credential.json')
    Invoke-B11Remote -Operation download -RemotePath ($remote.remote_directory + '/observer.json') `
        -LocalPath $credentialPath | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Observer credential download failed.' }
    $credential = Get-Content -LiteralPath $credentialPath -Raw | ConvertFrom-Json
    Remove-Item -LiteralPath $credentialPath -Force
    $credentialPath = $null
    if ($credential.LIVEKIT_URL -cne $ServiceUrl -or
            $credential.LIVEKIT_SOAK_ALLOW_INSECURE -cne '1' -or
            $credential.LIVEKIT_SOAK_TOKEN -isnot [string] -or
            -not $credential.LIVEKIT_SOAK_TOKEN -or $credential.LIVEKIT_SOAK_TOKEN.Length -gt 8192) {
        $credential = $null
        throw 'Observer credential does not match the bound service.'
    }
    $env:LIVEKIT_URL = [string]$credential.LIVEKIT_URL
    $env:LIVEKIT_SOAK_TOKEN = [string]$credential.LIVEKIT_SOAK_TOKEN
    $env:LIVEKIT_SOAK_ALLOW_INSECURE = [string]$credential.LIVEKIT_SOAK_ALLOW_INSECURE
    $credential = $null
    if (-not $transportProbe) {
        $runName = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '-' +
            [Guid]::NewGuid().ToString('N').Substring(0, 8)
        $output = Join-Path $prepared ('render-probes\' + $runName)
    }
    Set-Location -LiteralPath $repository
    $probeArguments = @('-B', 'tests/runtime/tools/meeting/meeting_render_probe.py',
        '--output', $output, '--executable', $binaryPath)
    if ($manifestPath) { $probeArguments += @('--input-manifest', $manifestPath, '--profile', $profilePath) }
    if ($Grid16Transition) {
        $probeArguments += '--grid16-transition'
    } elseif ($Grid16Transport) {
        $probeArguments += '--grid16-transport'
    }
    & $Python @probeArguments
    $probeExitCode = $LASTEXITCODE
    Write-Output "probe_exit_code=$probeExitCode"
    Write-Output "evidence=$output"
    if ($probeExitCode -ne 0) { throw "Render probe did not complete (exit code $probeExitCode)." }
} finally {
  try {
    if ($credentialPath) {
        Remove-Item -LiteralPath $credentialPath -Force -ErrorAction SilentlyContinue
    }
    if ($sampler) {
        $stopSampler = @"
python3 - <<'PY'
import json, os, pathlib, signal, time
root=pathlib.Path('$($remote.remote_directory)')
pid=$($sampler.pid)
ticks='$($sampler.start_ticks)'
$ownedStopPython
result={'pid':pid, **stop_owned(pid, ticks,
    lambda command: str(root/'ecs_resource_sampler.py').encode() in command)}
print(json.dumps(result))
PY
"@
        try {
            $samplerCleanupOutput = Invoke-B11Remote -Operation exec -Command $stopSampler
            if ($LASTEXITCODE -ne 0 -or ($samplerCleanupOutput | ConvertFrom-Json).stopped -ne $true) {
                throw 'sampler_cleanup_incomplete'
            }
            Write-Output $samplerCleanupOutput
        }
        catch { $cleanupIssues.Add('sampler_cleanup'); Write-Warning 'ECS sampler cleanup failed; publisher cleanup will still be attempted.' }
        if ($output) {
            try { New-Item -ItemType Directory -Path $output -Force | Out-Null
            Invoke-B11Remote -Operation download -RemotePath ($remote.remote_directory + '/metadata.json') `
                -LocalPath (Join-Path $output 'remote-inputs.json') | Out-Null
            if ($LASTEXITCODE -ne 0) { throw 'remote_metadata_download_failed' }
            Invoke-B11Remote -Operation download -RemotePath ($remote.remote_directory + '/ecs-metrics.csv') `
                -LocalPath (Join-Path $output 'ecs-metrics.csv') | Out-Null
            if ($LASTEXITCODE -ne 0) { throw 'ecs_metrics_download_failed' }
            Invoke-B11Remote -Operation download -RemotePath ($remote.remote_directory + '/ecs-metrics.status.json') `
                -LocalPath (Join-Path $output 'ecs-metrics.status.json') | Out-Null
            if ($LASTEXITCODE -ne 0) { throw 'ecs_metrics_status_download_failed' } }
            catch { $cleanupIssues.Add('sampler_evidence'); Write-Warning 'ECS sampler evidence download failed; publisher cleanup will still be attempted.' }
        }
    }
    if ($remote) {
        $cleanup = @"
python3 - <<'PY'
import json, os, pathlib, signal, time
root = pathlib.Path('$($remote.remote_directory)')
pid = $($remote.publisher_pid)
ticks = '$($remote.publisher_start_ticks)'
room = '$($remote.room)'
$ownedStopPython
result = {'room': room, 'publisher_pid': pid, 'publisher_stopped': False}
marker = b'soak_low_bandwidth_publishers.py' if '$($remote.publisher_kind)' == 'low_bandwidth' else b'load-test'
stopped = stop_owned(pid, ticks,
    lambda command: room.encode() in command and any(marker in arg for arg in command), 5)
result['publisher_stopped'] = stopped['stopped']
if not stopped['stopped']:
    result['reason'] = stopped.get('reason', 'stop_incomplete')
if '$($remote.publisher_kind)' == 'low_bandwidth':
    status_path = root / 'publishers-status.json'
    if status_path.exists():
        status = json.loads(status_path.read_text())
        child_results = []
        for child in status.get('children', []):
            if not child.get('start_ticks'):
                child_results.append(False)
                continue
            child_results.append(stop_owned(child['pid'], child['start_ticks'],
                lambda command: room.encode() in command)['stopped'])
        result['children_stopped'] = all(child_results)
        result['publisher_stopped'] = result['publisher_stopped'] and result['children_stopped']
        result['publisher_state'] = status['state']
        result['publisher_alive'] = status['alive']
credential = root / 'observer.json'
if credential.exists():
    credential.unlink()
result['credential_removed'] = not credential.exists()
(root / 'cleanup.json').write_text(json.dumps(result))
print(json.dumps(result))
PY
"@
        try {
            $publisherCleanupOutput = Invoke-B11Remote -Operation exec -Command $cleanup
            $publisherCleanup = $publisherCleanupOutput | ConvertFrom-Json
            if ($LASTEXITCODE -ne 0 -or $publisherCleanup.publisher_stopped -ne $true -or
                    $publisherCleanup.credential_removed -ne $true) { throw 'publisher_cleanup_incomplete' }
            Write-Output $publisherCleanupOutput
        }
        catch { $cleanupIssues.Add('publisher_cleanup'); Write-Warning 'Remote publisher cleanup failed; inspect the owned run before retrying.' }
    }
  } finally {
    foreach ($name in $originalEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $originalEnvironment[$name], 'Process')
    }
    Set-Location -LiteralPath $originalLocation
    [void][RenderProbePowerState]::SetThreadExecutionState($continuous)
  }
  if ($cleanupIssues.Count) { throw ('Render probe cleanup or evidence incomplete: ' + ($cleanupIssues -join ',')) }
}
