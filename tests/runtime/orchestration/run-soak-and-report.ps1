param([Parameter(Mandatory=$true)][string]$PreparedDirectory,
    [Parameter(Mandatory=$true)][ValidatePattern('^i-[a-zA-Z0-9]+$')][string]$Instance,
    [Parameter(Mandatory=$true)][string]$ServiceUrl,
    [string]$Python='python',
    [string]$Binary='',

    [Parameter(Mandatory = $true)]
    [string]$CredentialPath,
    [Parameter(Mandatory = $true)]
    [string]$RemoteDirectory,
    [Parameter(Mandatory = $true)]
    [string]$Room,
    [Parameter(Mandatory = $true)]
    [int]$PublisherPid,
    [Parameter(Mandatory = $true)]
    [string]$PublisherStartTicks,
    [ValidateSet('load_test', 'low_bandwidth')]
    [string]$PublisherKind = 'load_test'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
Assert-ProbeServiceUrl $ServiceUrl
$repository = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$prepared = (Resolve-Path -LiteralPath $PreparedDirectory).Path
$runs = Join-Path $prepared 'runs'
$controllerState = Join-Path $prepared 'active-controller.json'
$controllerLog = Join-Path $prepared 'active-controller.log'
$resultPath = Join-Path $prepared 'active-result.json'

if ($RemoteDirectory -notmatch '^/tmp/soak-[A-Za-z0-9TZ-]+$' -or
        $Room -notmatch '^soak-[A-Za-z0-9TZ-]+$' -or
        $PublisherStartTicks -notmatch '^\d+$') {
    throw 'Invalid owned-load identity.'
}

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class SoakPowerState {
    [DllImport("kernel32.dll")]
    public static extern uint SetThreadExecutionState(uint flags);
}
'@

$esContinuous = [Convert]::ToUInt32('80000000', 16)
$esSystemRequired = [uint32]0x00000001
$esDisplayRequired = [uint32]0x00000002
$startedUtc = [DateTime]::UtcNow.ToString('o')
$runnerExitCode = $null
$runDirectory = $null
$fullScheduleCompleted = $false
$cleanupResult = 'not_attempted'
$controllerError = $null
$summaryStatus = $null
$scheduleStatus = $null
$l3Status = $null
$measuredSeconds = 0
$completedCommands = 0
$scheduledCommands = 0

function Write-ControllerState([string]$status) {
    $state = [ordered]@{
        schema = 1
        status = $status
        started_utc = $startedUtc
        updated_utc = [DateTime]::UtcNow.ToString('o')
        controller_pid = $PID
        room = $Room
        remote_directory = $RemoteDirectory
        publisher_pid = $PublisherPid
        runner_exit_code = $runnerExitCode
        run_directory = $runDirectory
        full_schedule_completed = $fullScheduleCompleted
        cleanup_result = $cleanupResult
        controller_error = $controllerError
        power_action = 'none'
    }
    $temporary = "$controllerState.tmp"
    $state | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $temporary -Encoding UTF8
    Move-Item -LiteralPath $temporary -Destination $controllerState -Force
}

try {
    [void][SoakPowerState]::SetThreadExecutionState(
        $esContinuous -bor $esSystemRequired -bor $esDisplayRequired)
    Write-ControllerState 'STARTING'

    $credential = Get-Content -LiteralPath $CredentialPath -Raw | ConvertFrom-Json
    Remove-Item -LiteralPath $CredentialPath -Force
    if ([string]::IsNullOrWhiteSpace($credential.LIVEKIT_URL) -or
            [string]::IsNullOrWhiteSpace($credential.LIVEKIT_SOAK_TOKEN)) {
        throw 'Downloaded observer credential is incomplete.'
    }

    $env:LIVEKIT_URL = [string]$credential.LIVEKIT_URL
    $env:LIVEKIT_SOAK_TOKEN = [string]$credential.LIVEKIT_SOAK_TOKEN
    $env:LIVEKIT_SOAK_ALLOW_INSECURE = [string]$credential.LIVEKIT_SOAK_ALLOW_INSECURE
    $credential = $null

    Set-Location -LiteralPath $repository
    Write-ControllerState 'RUNNING'
    $runnerStartedUtc = [DateTime]::UtcNow
    & $Python -B 'tests/runtime/meeting_soak.py' run --prepared $prepared *>> $controllerLog
    $runnerExitCode = $LASTEXITCODE

    $latest = Get-ChildItem -LiteralPath $runs -Directory |
        Where-Object {
            $_.CreationTimeUtc -ge $runnerStartedUtc.AddSeconds(-5) -and
            (Test-Path -LiteralPath (Join-Path $_.FullName 'summary.json'))
        } |
        Sort-Object LastWriteTimeUtc -Descending |
        Select-Object -First 1
    if ($null -ne $latest) {
        $runDirectory = $latest.FullName
        $summary = Get-Content -LiteralPath (Join-Path $latest.FullName 'summary.json') -Raw |
            ConvertFrom-Json
        $summaryStatus = [string]$summary.status
        $scheduleStatus = [string]$summary.schedule_status
        $l3Status = [string]$summary.l3_status
        $measuredSeconds = [double]$summary.measured_seconds
        $completedCommands = [int]$summary.completed_commands
        $scheduledCommands = [int]$summary.scheduled_commands
        $fullScheduleCompleted =
            $summary.schedule_status -eq 'PASS' -and
            $measuredSeconds -ge 9000 -and
            $completedCommands -eq $scheduledCommands
    }
} catch {
    $controllerError = $_.Exception.Message
    Add-Content -LiteralPath $controllerLog -Value (
        '[controller] ' + [DateTime]::UtcNow.ToString('o') + ' ' + $controllerError)
} finally {
    Remove-Item Env:LIVEKIT_URL -ErrorAction SilentlyContinue
    Remove-Item Env:LIVEKIT_SOAK_TOKEN -ErrorAction SilentlyContinue
    Remove-Item Env:LIVEKIT_SOAK_ALLOW_INSECURE -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $CredentialPath -Force -ErrorAction SilentlyContinue

    try {
        $remoteCleanup = @"
python3 - <<'PY'
import json, os, signal, time
root='$RemoteDirectory'
room='$Room'
pid=$PublisherPid
expected_ticks='$PublisherStartTicks'
proc='/proc/'+str(pid)
result={'schema':1,'room':room,'publisher_pid':pid,'finished_unix':int(time.time())}
if not os.path.exists(proc):
    result.update(publisher_stopped=True, stop_result='already_exited')
else:
    ticks=open(proc+'/stat').read().split()[21]
    cmd=open(proc+'/cmdline','rb').read().split(b'\0')
    marker=b'soak_low_bandwidth_publishers.py' if '$PublisherKind' == 'low_bandwidth' else b'load-test'
    if ticks != expected_ticks or room.encode() not in cmd or not any(marker in arg for arg in cmd):
        result.update(publisher_stopped=False, stop_result='ownership_mismatch_no_signal_sent')
    else:
        os.kill(pid, signal.SIGINT)
        for _ in range(60):
            if not os.path.exists(proc) or open(proc+'/stat').read().split()[2] == 'Z':
                break
            time.sleep(.25)
        alive=os.path.exists(proc) and open(proc+'/stat').read().split()[2] != 'Z'
        if alive:
            os.kill(pid, signal.SIGTERM)
            time.sleep(.5)
        stopped=not os.path.exists(proc) or open(proc+'/stat').read().split()[2] == 'Z'
        result.update(publisher_stopped=stopped, stop_result='signalled_owned_publisher')
if '$PublisherKind' == 'low_bandwidth':
    status_path=root+'/publishers-status.json'
    owned_children=[]
    if os.path.isfile(status_path):
        status=json.load(open(status_path))
        for child in status.get('children',[]):
            child_proc='/proc/'+str(child['pid'])
            if os.path.exists(child_proc) and child.get('start_ticks') and \
                    open(child_proc+'/stat').read().split()[21]==child['start_ticks'] and \
                    room.encode() in open(child_proc+'/cmdline','rb').read():
                os.kill(child['pid'],signal.SIGTERM)
                owned_children.append(child)
    time.sleep(2)
    result['children_stopped']=all(
        not os.path.exists('/proc/'+str(child['pid'])) or
        open('/proc/'+str(child['pid'])+'/stat').read().split()[2]=='Z'
        for child in owned_children)
    result['child_cleanup_signalled']=len(owned_children)
credential=root+'/observer.json'
if os.path.isfile(credential):
    os.unlink(credential)
result['credential_removed']=not os.path.exists(credential)
with open(root+'/cleanup.json','w') as f:
    json.dump(result,f)
print(json.dumps(result))
PY
"@
        $cleanupOutput = & workbench exec -i $Instance -c $remoteCleanup 2>&1
        $cleanupOutput | Add-Content -LiteralPath $controllerLog
        if ($LASTEXITCODE -eq 0) {
            $cleanup = ($cleanupOutput | Select-Object -Last 1) | ConvertFrom-Json
            $cleanupResult = if ($cleanup.publisher_stopped -and $cleanup.credential_removed -and
                ($PublisherKind -ne 'low_bandwidth' -or $cleanup.children_stopped)) {
                'completed'
            } else { 'incomplete' }
        } else {
            $cleanupResult = 'workbench_failed'
        }
    } catch {
        $cleanupResult = 'exception'
        Add-Content -LiteralPath $controllerLog -Value (
            '[cleanup] ' + [DateTime]::UtcNow.ToString('o') + ' ' + $_.Exception.Message)
    }

    [void][SoakPowerState]::SetThreadExecutionState($esContinuous)
    $finalStatus = if ($fullScheduleCompleted) { 'COMPLETE' } else { 'STOPPED_REQUIRES_REVIEW' }
    $result = [ordered]@{
        schema = 1
        status = $finalStatus
        room = $Room
        started_utc = $startedUtc
        finished_utc = [DateTime]::UtcNow.ToString('o')
        run_directory = $runDirectory
        runner_exit_code = $runnerExitCode
        summary_status = $summaryStatus
        schedule_status = $scheduleStatus
        l3_status = $l3Status
        measured_seconds = $measuredSeconds
        completed_commands = $completedCommands
        scheduled_commands = $scheduledCommands
        full_schedule_completed = $fullScheduleCompleted
        cleanup_result = $cleanupResult
        controller_error = $controllerError
        power_action = 'none'
    }
    $result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $resultPath -Encoding UTF8
    Write-ControllerState $finalStatus
    $result | ConvertTo-Json -Depth 5 | Write-Output
}
