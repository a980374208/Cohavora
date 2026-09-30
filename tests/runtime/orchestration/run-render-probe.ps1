param([Parameter(Mandatory=$true)][string]$PreparedDirectory,
    [Parameter(Mandatory=$true)][ValidatePattern('^i-[a-zA-Z0-9]+$')][string]$Instance,
    [Parameter(Mandatory=$true)][string]$ServiceUrl,
    [string]$Python='python',
    [string]$Binary='',
    [switch]$Grid16Transport, [switch]$Grid16Transition,
      [switch]$NoSimulcast, [switch]$LowBandwidth)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
Assert-ProbeServiceUrl $ServiceUrl
$repository = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$prepared = (Resolve-Path -LiteralPath $PreparedDirectory).Path

$remote = $null
$credentialPath = $null
$sampler = $null
$output = $null
$transportProbe = $Grid16Transport -or $Grid16Transition
if ($Grid16Transport -and $Grid16Transition) { throw 'Select one probe mode.' }
if ($NoSimulcast -and -not $transportProbe) { throw 'NoSimulcast requires a transport probe.' }
if ($LowBandwidth -and -not $transportProbe) { throw 'LowBandwidth requires a transport probe.' }
if ($LowBandwidth -and $NoSimulcast) { throw 'Select one publishing mode.' }

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
        & workbench upload `
            (Join-Path $repository 'tests/runtime/soak_low_bandwidth_publishers.py') `
            '/tmp/soak_low_bandwidth_publishers.py' -i $instance -f | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Low-bandwidth publisher upload failed.' }
    }
    $setup = @"
python3 - <<'PY'
import hashlib, json, os, pathlib, signal, subprocess, time, uuid, yaml
config = yaml.safe_load(pathlib.Path('/root/livekit.yaml').read_text())
key, secret = next(iter(config['keys'].items()))
env = os.environ.copy()
env.update(LIVEKIT_URL='http://127.0.0.1:17880', LIVEKIT_API_KEY=key,
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
        'lk', 'load-test', '--room', room, '--duration', '15m',
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
            ['lk', 'room', 'participants', 'list', room], env=env,
            text=True, timeout=15)
        if status['state'] != 'RUNNING' or status['alive'] != 17 or participants.count('tracks: 1') != 17:
            raise RuntimeError('low_bandwidth_publishers_not_ready')
    token = subprocess.check_output([
        'lk', 'token', 'create', '--room', room, '--identity', 'render-observer',
        '--join', '--valid-for', '15m', '--token-only'], env=env,
        text=True, timeout=15).strip()
    if not token or len(token) > 8192:
        raise RuntimeError('invalid_observer_token')
    credential = root / 'observer.json'
    with credential.open('x') as stream:
        json.dump({'LIVEKIT_URL': 'ws://123.56.225.164:17880',
                   'LIVEKIT_SOAK_TOKEN': token,
                   'LIVEKIT_SOAK_ALLOW_INSECURE': '1'}, stream)
    credential.chmod(0o600)
    ticks = pathlib.Path('/proc/' + str(publisher.pid) + '/stat').read_text().split()[21]
    metadata = {'room': room, 'remote_directory': str(root),
                'publisher_pid': publisher.pid, 'publisher_start_ticks': ticks,
                'publisher_kind': 'low_bandwidth' if ${lowBandwidthPython} else 'load_test'}
    (root / 'metadata.json').write_text(json.dumps(metadata))
    print(json.dumps(metadata))
except BaseException:
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
    $setupOutput = & workbench exec -i $instance -c (Expand-ProbeCommand $setup)
    if ($LASTEXITCODE -ne 0) { throw 'Remote load setup failed.' }
    $remote = $setupOutput | ConvertFrom-Json
    if ($remote.room -notmatch '^soak-render-[A-Za-z0-9TZ-]+$' -or
            $remote.remote_directory -notmatch '^/tmp/soak-render-[A-Za-z0-9TZ-]+$' -or
            [string]$remote.publisher_pid -notmatch '^\d+$' -or
            $remote.publisher_start_ticks -notmatch '^\d+$') {
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
        & workbench upload `
            (Join-Path $repository 'tests/runtime/ecs_resource_sampler.py') `
            ($remote.remote_directory + '/ecs_resource_sampler.py') -i $instance | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'ECS sampler upload failed.' }
        $startSampler = @"
python3 - <<'PY'
import json, pathlib, subprocess, time
root=pathlib.Path('$($remote.remote_directory)')
sfu_pid=int(subprocess.check_output(['docker','inspect','-f','{{.State.Pid}}','livekit'],text=True))
sampler=subprocess.Popen(['python3',str(root/'ecs_resource_sampler.py'),
    '--output',str(root/'ecs-metrics.csv'),'--pid',str(sfu_pid),
    '--duration','600'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,
    start_new_session=True)
time.sleep(2)
if sampler.poll() is not None:
    raise RuntimeError('ecs_sampler_exited_early')
ticks=pathlib.Path('/proc/'+str(sampler.pid)+'/stat').read_text().split()[21]
print(json.dumps({'pid':sampler.pid,'start_ticks':ticks,'sfu_pid':sfu_pid}))
PY
"@
        $samplerOutput = & workbench exec -i $instance -c $startSampler
        if ($LASTEXITCODE -ne 0) { throw 'ECS sampler launch failed.' }
        $sampler = $samplerOutput | ConvertFrom-Json
        if ([string]$sampler.pid -notmatch '^\d+$' -or
                $sampler.start_ticks -notmatch '^\d+$') {
            throw 'ECS sampler identity is invalid.'
        }
    }
    $credentialPath = Join-Path $prepared ($remote.room + '-credential.json')
    & workbench download ($remote.remote_directory + '/observer.json') `
        $credentialPath -i $instance | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Observer credential download failed.' }
    $credential = Get-Content -LiteralPath $credentialPath -Raw | ConvertFrom-Json
    Remove-Item -LiteralPath $credentialPath -Force
    $credentialPath = $null
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
    if ($Grid16Transition) {
        & $Python -B 'tests/runtime/meeting_render_probe.py' `
            --output $output --grid16-transition
    } elseif ($Grid16Transport) {
        & $Python -B 'tests/runtime/meeting_render_probe.py' `
            --output $output --grid16-transport
    } else {
        & $Python -B 'tests/runtime/meeting_render_probe.py' --output $output
    }
    $probeExitCode = $LASTEXITCODE
    Write-Output "probe_exit_code=$probeExitCode"
    Write-Output "evidence=$output"
    if ($probeExitCode -ne 0) { throw "Render probe did not complete (exit code $probeExitCode)." }
} finally {
    Remove-Item Env:LIVEKIT_URL -ErrorAction SilentlyContinue
    Remove-Item Env:LIVEKIT_SOAK_TOKEN -ErrorAction SilentlyContinue
    Remove-Item Env:LIVEKIT_SOAK_ALLOW_INSECURE -ErrorAction SilentlyContinue
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
proc=pathlib.Path('/proc')/str(pid)
result={'pid':pid,'owned':False,'stopped':False}
if proc.exists():
    current=(proc/'stat').read_text().split()[21]
    command=(proc/'cmdline').read_bytes().split(b'\0')
    if current==ticks and str(root/'ecs_resource_sampler.py').encode() in command:
        result['owned']=True
        os.kill(pid,signal.SIGINT)
        for _ in range(30):
            if not proc.exists() or (proc/'stat').read_text().split()[2]=='Z':
                break
            time.sleep(.1)
        if proc.exists() and (proc/'stat').read_text().split()[2]!='Z':
            os.kill(pid,signal.SIGTERM)
        result['stopped']=True
    else:
        result['reason']='ownership_mismatch'
else:
    result['stopped']=True
print(json.dumps(result))
PY
"@
        & workbench exec -i $instance -c $stopSampler
        if ($output) {
            New-Item -ItemType Directory -Path $output -Force | Out-Null
            & workbench download ($remote.remote_directory + '/ecs-metrics.csv') `
                (Join-Path $output 'ecs-metrics.csv') -i $instance | Out-Null
            & workbench download ($remote.remote_directory + '/ecs-metrics.status.json') `
                (Join-Path $output 'ecs-metrics.status.json') -i $instance | Out-Null
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
proc = pathlib.Path('/proc') / str(pid)
result = {'room': room, 'publisher_pid': pid, 'publisher_stopped': False}
if proc.exists():
    current = (proc / 'stat').read_text().split()[21]
    command = (proc / 'cmdline').read_bytes().split(b'\0')
    marker = b'soak_low_bandwidth_publishers.py' if '$($remote.publisher_kind)' == 'low_bandwidth' else b'load-test'
    if current == ticks and room.encode() in command and any(marker in arg for arg in command):
        os.kill(pid, signal.SIGINT)
        for _ in range(20):
            if not proc.exists() or (proc / 'stat').read_text().split()[2] == 'Z':
                break
            time.sleep(.25)
        if proc.exists() and (proc / 'stat').read_text().split()[2] != 'Z':
            os.kill(pid, signal.SIGTERM)
        result['publisher_stopped'] = True
    else:
        result['reason'] = 'ownership_mismatch'
else:
    result['publisher_stopped'] = True
if '$($remote.publisher_kind)' == 'low_bandwidth':
    status_path = root / 'publishers-status.json'
    if status_path.exists():
        status = json.loads(status_path.read_text())
        for child in status.get('children', []):
            child_proc = pathlib.Path('/proc') / str(child['pid'])
            if child_proc.exists() and child.get('start_ticks') and \
                    (child_proc / 'stat').read_text().split()[21] == child['start_ticks'] and \
                    room.encode() in (child_proc / 'cmdline').read_bytes():
                os.kill(child['pid'], signal.SIGTERM)
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
        & workbench exec -i $instance -c $cleanup
    }
    [void][RenderProbePowerState]::SetThreadExecutionState($continuous)
}
