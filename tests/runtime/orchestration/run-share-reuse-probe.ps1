param([Parameter(Mandatory=$true)][string]$PreparedDirectory,
    [Parameter(Mandatory=$true)][ValidatePattern('^i-[a-zA-Z0-9]+$')][string]$Instance,
    [Parameter(Mandatory=$true)][string]$ServiceUrl,
    [string]$Python='python',
    [string]$Binary='',
    [ValidateRange(1, 30)][int]$Cycles = 12,
      [switch]$FirstFrameOnly, [switch]$DirectTrack, [switch]$Performance,
      [switch]$CompleteLifecycle, [switch]$TraceHandles,
      [switch]$WgcWindow, [switch]$Quality, [switch]$WgcScreen, [switch]$Attribution,
      [ValidateRange(30, 120)][int]$SampleSeconds = 30,
      [ValidateSet('', 'h264', 'vp9')][string]$Codec = '')

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
Assert-ProbeServiceUrl $ServiceUrl
if ($Quality) { $Performance = $true }

$prepared = (Resolve-Path -LiteralPath $PreparedDirectory).Path
$repository = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
if (-not $Binary) { throw 'Binary is required for this scenario' }; $binary = (Resolve-Path -LiteralPath $Binary).Path
$remote = $null
$credentialPath = $null
$exitCode = -1
$handleTraceStarted = $false

try {
    if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) {
        throw 'Screen-share runtime probe binary is missing.'
    }
    $performanceFlag = if ($Performance -or $CompleteLifecycle) { 'True' } else { 'False' }
    $setup = @'
python3 - <<'PY'
import json, os, pathlib, subprocess, time, uuid, yaml
performance = PERFORMANCE_FLAG
sample_egress = SAMPLE_EGRESS_FLAG
attribute_egress = ATTRIBUTION_FLAG
config = yaml.safe_load(pathlib.Path('/root/livekit.yaml').read_text())
key, secret = next(iter(config['keys'].items()))
env = os.environ.copy()
env.update(LIVEKIT_URL='http://127.0.0.1:17880',
           LIVEKIT_API_KEY=key, LIVEKIT_API_SECRET=secret)
room = 'share-reuse-' + time.strftime('%Y%m%dT%H%M%SZ', time.gmtime()) + '-' + uuid.uuid4().hex[:6]
token = subprocess.check_output([
    'lk', 'token', 'create', '--room', room, '--identity', 'share-reuse-probe',
    '--join', '--valid-for', '20m', '--token-only'], env=env, text=True, timeout=15).strip()
peer_token = None
if performance:
    peer_token = subprocess.check_output([
        'lk', 'token', 'create', '--room', room, '--identity', 'share-perf-receiver',
        '--join', '--valid-for', '20m', '--token-only'], env=env, text=True, timeout=15).strip()
if not token or len(token) > 8192 or (performance and (not peer_token or len(peer_token) > 8192)):
    raise RuntimeError('invalid_room_token')
path = pathlib.Path('/tmp') / (room + '-credential.json')
fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
with os.fdopen(fd, 'w') as stream:
    credentials = {'LIVEKIT_URL': 'ws://123.56.225.164:17880',
                   'LIVEKIT_TOKEN': token}
    if peer_token:
        credentials['LIVEKIT_PEER_TOKEN'] = peer_token
    json.dump(credentials, stream)
sampler_pid = None
sampler_path = None
if sample_egress:
    sampler_path = '/tmp/' + room + '-egress.jsonl'
    sampler_code = '''import json,pathlib,sys,time
with open(sys.argv[1], 'x') as out:
 for _ in range(180):
  line = next(x for x in pathlib.Path('/proc/net/dev').read_text().splitlines() if x.strip().startswith('eth0:'))
  values = [int(x) for x in line.split(':',1)[1].split()]
  out.write(json.dumps({'epoch_ms':round(time.time()*1000),'tx_bytes':values[8],'rx_bytes':values[0],'tx_drop':values[11],'rx_drop':values[3]})+'\\n'); out.flush()
  time.sleep(1)
'''
    if attribute_egress:
        sampler_code = '''import collections,json,pathlib,select,socket,struct,sys,time
# Read only the first 128 bytes in memory. Never persist packets or addresses.
sock=socket.socket(socket.AF_PACKET,socket.SOCK_RAW,socket.htons(3))
sock.bind(('eth0',0)); sock.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,8*1024*1024)
sock.setblocking(False)
groups=collections.defaultdict(lambda:[0,0]); dropped=0; seen=0
with open(sys.argv[1],'x') as out:
 deadline=time.monotonic()
 for sample in range(180):
  while time.monotonic()<deadline:
   ready,_,_=select.select([sock],[],[],max(0,deadline-time.monotonic()))
   if not ready: break
   try: data,addr=sock.recvfrom(128)
   except BlockingIOError: continue
   if len(data)<34: continue
   direction='out' if addr[2]==4 else 'in'
   eth=struct.unpack_from('!H',data,12)[0]; offset=14
   if eth==0x8100 and len(data)>=38: eth=struct.unpack_from('!H',data,16)[0];offset=18
   label='other_l2';ssrc='';pt='';port=0;size=len(data)
   if eth==0x0800 and len(data)>=offset+20:
    ihl=(data[offset]&15)*4; size=offset+struct.unpack_from('!H',data,offset+2)[0]
    proto=data[offset+9]; pos=offset+ihl; label='ip_other'
    if proto in (6,17) and len(data)>=pos+8:
     sp,dp=struct.unpack_from('!HH',data,pos);port=sp if direction=='out' else dp
     label='tcp' if proto==6 else 'udp'
     if proto==17 and port==17882:
      label='media_control';p=pos+8
      if len(data)>=p+12 and data[p]>>6==2:
       if 192<=data[p+1]<=223: label='rtcp'
       else:
        label='rtp_padding' if data[p]&32 else 'rtp';ssrc=str(struct.unpack_from('!I',data,p+8)[0]);pt=str(data[p+1]&127)
   elif eth==0x86dd and len(data)>=offset+40:
    size=offset+40+struct.unpack_from('!H',data,offset+4)[0];label='ipv6'
   key=(direction,label,port,ssrc,pt);groups[key][0]+=size;groups[key][1]+=1
  line=next(x for x in pathlib.Path('/proc/net/dev').read_text().splitlines() if x.strip().startswith('eth0:'))
  v=[int(x) for x in line.split(':',1)[1].split()]
  counters=sock.getsockopt(263,6,12);n,d=struct.unpack_from('II',counters);seen+=n;dropped+=d
  record={'epoch_ms':round(time.time()*1000),'tx_bytes':v[8],'rx_bytes':v[0],'tx_drop':v[11],'rx_drop':v[3],
   'packet_socket_seen':seen,'packet_socket_drops':dropped,'groups':[{'direction':k[0],'protocol':k[1],'local_port':k[2],'ssrc':k[3],'payload_type':k[4],'wire_bytes':val[0],'packets':val[1]} for k,val in groups.items()]}
  out.write(json.dumps(record)+'\\n');out.flush();deadline+=1
'''
    sampler_pid = subprocess.Popen(['python3', '-c', sampler_code, sampler_path],
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        start_new_session=True).pid
print(json.dumps({'room': room, 'credential_path': str(path),
                  'sampler_pid': sampler_pid, 'sampler_path': sampler_path}))
PY
'@
    $setup = $setup.Replace('PERFORMANCE_FLAG', $performanceFlag)
    $setup = $setup.Replace('SAMPLE_EGRESS_FLAG', $(if ($Performance -and -not $Quality) { 'True' } else { 'False' }))
    $setup = $setup.Replace('ATTRIBUTION_FLAG', $(if ($Attribution) { 'True' } else { 'False' }))
    $setupOutput = & workbench exec -i $instance -c (Expand-ProbeCommand $setup)
    if ($LASTEXITCODE -ne 0) { throw 'Remote token generation failed.' }
    $remote = $setupOutput | ConvertFrom-Json
    if ($remote.room -notmatch '^share-reuse-[A-Za-z0-9TZ-]+$' -or
        $remote.credential_path -ne ('/tmp/' + $remote.room + '-credential.json')) {
        throw 'Remote token metadata is invalid.'
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
    $env:LIVEKIT_TEST_SHARE_CYCLES = [string]$Cycles
    $env:LIVEKIT_TEST_SHARE_SAMPLE_SECONDS = [string]$SampleSeconds
    if ($WgcScreen) { $env:LIVEKIT_TEST_WGC_SCREEN = '1' }
    if ($WgcWindow) { $env:LIVEKIT_TEST_WGC_WINDOW = '1' }
    if ($Quality) { $env:LIVEKIT_TEST_SHARE_QUALITY = '1' }
    if ($FirstFrameOnly) { $env:LIVEKIT_TEST_FIRST_FRAME_ONLY = '1' }
    if ($Codec) { $env:LIVEKIT_TEST_SHARE_CODEC = $Codec }
    $log = Join-Path $prepared ($remote.room + '-publisher.log')
    $ErrorActionPreference = 'Continue'
    $probeArgument = if ($CompleteLifecycle) { '--lifecycle-probe' } elseif ($Performance) { '--performance-probe' } elseif ($DirectTrack) {
        '--direct-track-lifecycle-probe'
    } else { '--publisher-lifecycle-probe' }
    if ($TraceHandles) {
        & (Join-Path $PSScriptRoot '../etw/handle_wpr.ps1') -Action start -OutputDirectory $prepared -Tag $remote.room
        $traceStart = Get-Content (Join-Path $prepared ($remote.room + "-start.json")) -Raw | ConvertFrom-Json
        if ($traceStart.exit_code -ne 0) { throw 'Handle tracing start failed.' }
        $handleTraceStarted = $true
    }
    & $binary $probeArgument > $log 2>&1
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    $egressPath = $null
    if ($remote.sampler_path -eq ('/tmp/' + $remote.room + '-egress.jsonl')) {
        $egressPath = Join-Path $prepared ($remote.room + '-egress.jsonl')
        & workbench download $remote.sampler_path $egressPath -i $instance | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'ECS numeric sample download failed.' }
    }
    $env:LIVEKIT_TOKEN = $null
    $env:LIVEKIT_PEER_TOKEN = $null
    $credential = $null

    $samples = @(foreach ($line in Get-Content -LiteralPath $log) {
        if ($line -match '^\[LIFECYCLE\] (\{.*\})$') {
            $Matches[1] | ConvertFrom-Json
        }
    })
    $captureEvents = @(foreach ($line in Get-Content -LiteralPath $log) {
        if ($line -match '^\[CAPTURE_PROBE\] (\{.*\})$') {
            $Matches[1] | ConvertFrom-Json
        }
    })
    $rtcSamples = @(foreach ($line in Get-Content -LiteralPath $log) {
        if ($line -match '^\[SHARE_RTC\] (\{.*\})$') {
            $Matches[1] | ConvertFrom-Json
        }
    })
    $baseline = $samples | Where-Object phase -eq 'baseline' | Select-Object -First 1
    $settled = $samples | Where-Object phase -eq 'settled' | Select-Object -Last 1
    $stopped = @($samples | Where-Object phase -eq 'stopped')
    $resultLine = (Select-String -LiteralPath $log -Pattern '^\[RESULT\]' |
        Select-Object -Last 1).Line
    $performanceLine = (Select-String -LiteralPath $log -Pattern '^\[SHARE_PERF\] ' |
        Select-Object -Last 1).Line
    $performanceResult = if ($performanceLine) {
        ($performanceLine -replace '^\[SHARE_PERF\] ', '') | ConvertFrom-Json
    } else { $null }
    $qualityLine = (Select-String -LiteralPath $log -Pattern '^\[SHARE_QUALITY\] ' |
        Select-Object -Last 1).Line
    $qualityResult = if ($qualityLine) {
        ($qualityLine -replace '^\[SHARE_QUALITY\] ', '') | ConvertFrom-Json
    } else { $null }
    $result = [ordered]@{
        schema = 1
        room = $remote.room
        probe = $probeArgument.TrimStart('-')
        first_frame_only = [bool]$FirstFrameOnly
        codec = if ($Codec) { $Codec } else { 'default' }
        egress_attribution_requested = [bool]$Attribution
        wgc_screen_requested = [bool]$WgcScreen
        wgc_window_requested = [bool]$WgcWindow
        exit_code = $exitCode
        result = $resultLine
        performance = $performanceResult
        quality = $qualityResult
        ecs_egress_file = $egressPath
        binary_sha256 = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash
        log = Split-Path -Leaf $log
        log_sha256 = (Get-FileHash -LiteralPath $log -Algorithm SHA256).Hash
        samples = $samples.Count
        capture_events = $captureEvents
        rtc_samples = $rtcSamples
        stopped_cycles = $stopped.Count
        baseline = $baseline
        first_stopped = if ($stopped.Count) { $stopped[0] } else { $null }
        settled = $settled
        private_growth_mib = if ($baseline -and $settled) {
            [math]::Round(($settled.private_bytes - $baseline.private_bytes) / 1MB, 3)
        } else { $null }
        warm_private_growth_mib = if ($stopped.Count -and $settled) {
            [math]::Round(($settled.private_bytes - $stopped[0].private_bytes) / 1MB, 3)
        } else { $null }
        warm_handle_delta = if ($stopped.Count -and $settled) {
            $settled.handles - $stopped[0].handles
        } else { $null }
        warm_thread_delta = if ($stopped.Count -and $settled) {
            $settled.threads - $stopped[0].threads
        } else { $null }
        performance_pass = if ($Quality) {
            [bool]($exitCode -eq 0 -and $qualityResult -and $qualityResult.status -eq 'PASS')
        } elseif ($Performance) {
            [bool]($exitCode -eq 0 -and $performanceResult -and
                $performanceResult.capture_frames -gt 0 -and
                $performanceResult.remote_valid_frames -gt 0 -and
                $performanceResult.remote_width -ge 160 -and
                $performanceResult.remote_height -ge 100)
        } else { $null }
        stable_baseline = if ($Performance) { $null } else {
            [bool]($exitCode -eq 0 -and $stopped.Count -eq $Cycles -and
                (-not $WgcWindow -or (
                    @($captureEvents | Where-Object phase -eq 'session_closed').Count -eq $Cycles -and
                    @($captureEvents | Where-Object phase -eq 'frame_pool_closed').Count -eq $Cycles -and
                    @($captureEvents | Where-Object phase -eq 'd3d_released').Count -eq $Cycles -and
                    @($captureEvents | Where-Object phase -eq 'close_failed').Count -eq 0 -and
                    @($captureEvents | Where-Object { $_.phase -eq 'backend_frame' -and $_.capturer_id -eq 1 }).Count -eq $Cycles)) -and
                $settled -and $stopped[0] -and
                ($settled.private_bytes - $stopped[0].private_bytes) -le 8MB -and
                ($settled.handles - $stopped[0].handles) -le 32 -and
                ($settled.threads - $stopped[0].threads) -le 2 -and
                $settled.publisher_transceivers -eq $stopped[0].publisher_transceivers -and
                $settled.capture_live -eq 0 -and $settled.track_live -eq 0 -and
                $settled.source_live -eq 0 -and $settled.preview_live -eq 0)
        }
    }
    $resultPath = Join-Path $prepared ($remote.room + '-result.json')
    $result | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $resultPath -Encoding UTF8
    Write-Output ([ordered]@{ result_path = $resultPath; result = $resultLine;
        capture_events = $captureEvents.Count; rtc_samples = $rtcSamples.Count;
        warm_private_growth_mib = $result.warm_private_growth_mib;
        warm_handle_delta = $result.warm_handle_delta;
        warm_thread_delta = $result.warm_thread_delta } | ConvertTo-Json)
    if ($exitCode -ne 0) { exit $exitCode }
    if ($Performance -and -not $result.performance_pass) { exit 1 }
    if (-not $Performance -and -not $result.stable_baseline) { exit 1 }
} finally {
    if ($handleTraceStarted) {
        try {
        & (Join-Path $PSScriptRoot '../etw/handle_wpr.ps1') -Action stop -OutputDirectory $prepared -Tag $remote.room
        } catch {
            Write-Warning 'WPR stop/save did not complete; credential cleanup will continue. Verify the dedicated trace separately.'
        }
    }
    $env:LIVEKIT_TOKEN = $null
    $env:LIVEKIT_PEER_TOKEN = $null
    $env:LIVEKIT_TEST_WGC_SCREEN = $null
    $env:LIVEKIT_TEST_WGC_WINDOW = $null
    $env:LIVEKIT_TEST_SHARE_QUALITY = $null
    if ($credentialPath -and (Test-Path -LiteralPath $credentialPath)) {
        Remove-Item -LiteralPath $credentialPath -Force
    }
    if ($remote -and $remote.credential_path -match '^/tmp/share-reuse-[A-Za-z0-9TZ-]+-credential\.json$') {
        & workbench exec -i $instance -c ("rm -f -- '" + $remote.credential_path + "'") | Out-Null
    }
    if ($remote -and $remote.sampler_path -eq ('/tmp/' + $remote.room + '-egress.jsonl') -and
        $remote.sampler_pid -match '^\d+$') {
        $cleanup = "python3 -c 'import os,pathlib,signal; p=" + $remote.sampler_pid +
            "; f=""" + $remote.sampler_path + """; c=pathlib.Path(""/proc/""+str(p)+""/cmdline""); " +
            "os.kill(p,signal.SIGTERM) if c.exists() and f.encode() in c.read_bytes() else None; pathlib.Path(f).unlink(missing_ok=True)'"
        & workbench exec -i $instance -c $cleanup | Out-Null
    }
}
