param([Parameter(Mandatory=$true)][string]$PreparedDirectory,
    [Parameter(Mandatory=$true)][ValidatePattern('^i-[a-zA-Z0-9]+$')][string]$Instance,
    [Parameter(Mandatory=$true)][string]$ServiceUrl,
    [string]$Python='python',
    [string]$Binary='',
    [switch]$LowBandwidth)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
Assert-ProbeServiceUrl $ServiceUrl
$repository = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$prepared = (Resolve-Path -LiteralPath $PreparedDirectory).Path

$remote = $null
$credentialPath = $null
$controllerCompletedCleanup = $false
$sampler = $null
$runDirectory = $null

try {
    $lowBandwidthPython = if ($LowBandwidth) { 'True' } else { 'False' }
    if ($LowBandwidth) {
        & workbench upload `
            (Join-Path $repository 'tests/runtime/tools/meeting/soak_low_bandwidth_publishers.py') `
            '/tmp/soak_low_bandwidth_publishers.py' -i $instance -f | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Low-bandwidth publisher upload failed.' }
    }
    $setup = @"
python3 - <<'PY'
import hashlib, json, os, pathlib, signal, subprocess, time, uuid, yaml
config = yaml.safe_load(pathlib.Path('/root/livekit.yaml').read_text())
key, secret = next(iter(config['keys'].items()))
env = os.environ.copy()
env.update(LIVEKIT_URL='http://127.0.0.1:17880',
           LIVEKIT_API_KEY=key, LIVEKIT_API_SECRET=secret)
room = 'soak-' + time.strftime('%Y%m%dT%H%M%SZ', time.gmtime()) + '-' + uuid.uuid4().hex[:6]
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
    options = ['lk', 'load-test', '--room', room, '--duration', '4h',
               '--video-publishers', '17', '--audio-publishers', '0', '--subscribers', '0',
               '--video-resolution', 'low', '--video-codec', 'vp8', '--num-per-second', '2',
               '--yes']
publisher = subprocess.Popen(options, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    start_new_session=True)
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
        'lk', 'token', 'create', '--room', room, '--identity', 'soak-observer',
        '--join', '--valid-for', '4h', '--token-only'], env=env,
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
    raise
PY
"@
    $setupOutput = & workbench exec -i $instance -c (Expand-ProbeCommand $setup)
    if ($LASTEXITCODE -ne 0) { throw 'Remote load setup failed.' }
    $remote = $setupOutput | ConvertFrom-Json
    if ($remote.room -notmatch '^soak-[A-Za-z0-9TZ-]+$' -or
            $remote.remote_directory -notmatch '^/tmp/soak-[A-Za-z0-9TZ-]+$' -or
            [string]$remote.publisher_pid -notmatch '^\d+$' -or
            $remote.publisher_start_ticks -notmatch '^\d+$') {
        throw 'Remote load identity is invalid.'
    }

    if ($LowBandwidth) {
        & workbench upload `
            (Join-Path $repository 'tests/runtime/tools/meeting/ecs_resource_sampler.py') `
            ($remote.remote_directory + '/ecs_resource_sampler.py') -i $instance | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'ECS sampler upload failed.' }
        $startSampler = @"
python3 - <<'PY'
import json, pathlib, subprocess, time
root=pathlib.Path('$($remote.remote_directory)')
sfu_pid=int(subprocess.check_output(['docker','inspect','-f','{{.State.Pid}}','livekit'],text=True))
sampler=subprocess.Popen(['python3',str(root/'ecs_resource_sampler.py'),
    '--output',str(root/'ecs-metrics.csv'),'--pid',str(sfu_pid),
    '--duration','10000'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,
    start_new_session=True)
time.sleep(2)
if sampler.poll() is not None: raise RuntimeError('ecs_sampler_exited_early')
ticks=pathlib.Path('/proc/'+str(sampler.pid)+'/stat').read_text().split()[21]
print(json.dumps({'pid':sampler.pid,'start_ticks':ticks}))
PY
"@
        $samplerOutput = & workbench exec -i $instance -c $startSampler
        if ($LASTEXITCODE -ne 0) { throw 'ECS sampler launch failed.' }
        $sampler = $samplerOutput | ConvertFrom-Json
    }

    $credentialPath = Join-Path $prepared ($remote.room + '-credential.json')
    & workbench download ($remote.remote_directory + '/observer.json') `
        $credentialPath -i $instance | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Observer credential download failed.' }

    Set-Location -LiteralPath $repository
    & powershell -NoProfile -ExecutionPolicy Bypass `
        -File (Join-Path $PSScriptRoot 'run-soak-and-report.ps1') `
        -PreparedDirectory $prepared -Instance $Instance -ServiceUrl $ServiceUrl -Python $Python `
        -CredentialPath $credentialPath `
        -RemoteDirectory $remote.remote_directory `
        -Room $remote.room `
        -PublisherPid $remote.publisher_pid `
        -PublisherStartTicks $remote.publisher_start_ticks `
        -PublisherKind $remote.publisher_kind
    $credentialPath = $null
    $result = Get-Content -LiteralPath (Join-Path $prepared 'active-result.json') -Raw |
        ConvertFrom-Json
    $runDirectory = [string]$result.run_directory
    $controllerCompletedCleanup = $result.room -eq $remote.room -and
        $result.cleanup_result -eq 'completed'
    if ($result.runner_exit_code -ne 0 -or -not $result.full_schedule_completed -or -not $controllerCompletedCleanup) {
        throw 'Soak did not complete successfully; retain active-result.json for its actual verdict'
    }
    if (-not $controllerCompletedCleanup) {
        throw 'Controller did not confirm remote cleanup.'
    }
} finally {
    if ($credentialPath) {
        Remove-Item -LiteralPath $credentialPath -Force -ErrorAction SilentlyContinue
    }
    if ($sampler -and $remote) {
        $stopSampler = @"
python3 - <<'PY'
import os,pathlib,signal,time
root=pathlib.Path('$($remote.remote_directory)')
pid=$($sampler.pid)
ticks='$($sampler.start_ticks)'
proc=pathlib.Path('/proc')/str(pid)
if proc.exists() and (proc/'stat').read_text().split()[21]==ticks and \
        str(root/'ecs_resource_sampler.py').encode() in (proc/'cmdline').read_bytes():
    os.kill(pid,signal.SIGINT)
    for _ in range(30):
        if not proc.exists() or (proc/'stat').read_text().split()[2]=='Z': break
        time.sleep(.1)
print({'sampler_stopped':not proc.exists() or (proc/'stat').read_text().split()[2]=='Z'})
PY
"@
        & workbench exec -i $instance -c $stopSampler
        $metricsDirectory = if ($runDirectory) { $runDirectory } else {
            Join-Path $prepared ('ecs-metrics\' + $remote.room)
        }
        New-Item -ItemType Directory -Path $metricsDirectory -Force | Out-Null
        & workbench download ($remote.remote_directory + '/ecs-metrics.csv') `
            (Join-Path $metricsDirectory 'ecs-metrics.csv') -i $instance | Out-Null
        & workbench download ($remote.remote_directory + '/ecs-metrics.status.json') `
            (Join-Path $metricsDirectory 'ecs-metrics.status.json') -i $instance | Out-Null
    }
    if ($remote -and -not $controllerCompletedCleanup) {
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
}
