param(
    [Parameter(Mandatory=$true)][ValidateSet('soak','render','recovery','share')][string]$Scenario,
    [Parameter(Mandatory=$true)][string]$PreparedDirectory,
    [Parameter(Mandatory=$true)][ValidatePattern('^i-[a-zA-Z0-9]+$')][string]$Instance,
    [Parameter(Mandatory=$true)][string]$ServiceUrl,
    [string]$Python='python',
    [string]$Binary='',
    [ValidateSet('recovery','full','lifecycle','diagnostic','network')][string]$RecoveryMode='recovery',
    [switch]$LowBandwidth, [switch]$Grid16Transport, [switch]$Grid16Transition, [switch]$NoSimulcast,
    [ValidateRange(1,30)][int]$Cycles=12,
    [switch]$FirstFrameOnly, [switch]$DirectTrack, [switch]$Performance,
    [switch]$CompleteLifecycle, [switch]$TraceHandles, [switch]$WgcWindow,
    [switch]$Quality, [switch]$WgcScreen, [switch]$Attribution,
    [ValidateRange(30,120)][int]$SampleSeconds=30,
    [ValidateSet('','h264','vp9')][string]$Codec='',
    [switch]$Plan
)
$ErrorActionPreference='Stop'
. "$PSScriptRoot/orchestration/common.ps1"
Assert-ProbeServiceUrl $ServiceUrl
$scripts=@{soak='run-current-full-soak';render='run-render-probe';recovery='run-recovery-service-probe';share='run-share-reuse-probe'}
$script=Join-Path $PSScriptRoot ('orchestration/'+$scripts[$Scenario]+'.ps1')
$prepared=(Resolve-Path -LiteralPath $PreparedDirectory).Path
$parameters=@{PreparedDirectory=$prepared;Instance=$Instance;ServiceUrl=$ServiceUrl;Python=$Python}
if($Binary -and $Scenario -notin @('recovery','share')){throw 'Binary applies only to recovery/share; soak uses its prepared plan'}
if($Scenario -in @('recovery','share')) {
    if(-not $Binary){throw 'Binary is required for recovery/share'}
    $parameters.Binary=(Resolve-Path -LiteralPath $Binary).Path
}
$supported=(Get-Command -Name $script).Parameters
foreach($key in $PSBoundParameters.Keys) {
    if($key -in @('Scenario','PreparedDirectory','Instance','ServiceUrl','Python','Binary','Plan')){continue}
    $target=if($key -eq 'RecoveryMode'){'Mode'}else{$key}
    if(-not $supported.ContainsKey($target)){throw "Option $key is not supported by $Scenario"}
    $parameters[$target]=$PSBoundParameters[$key]
}
if($Plan) {
    @{scenario=$Scenario;script=$script;parameters=$parameters;execution='NOT_RUN'} | ConvertTo-Json -Depth 4
    return
}
if($Scenario -eq 'soak') {
    foreach($name in @('active-controller.json','active-result.json','active-controller.log')) {
        if(Test-Path -LiteralPath (Join-Path $prepared $name)){throw 'Use a new prepared directory; historical controller evidence exists'}
    }
    if(-not(Test-Path -LiteralPath (Join-Path $prepared 'plan.json'))){throw 'Run meeting_soak.py prepare first'}
}
# Each invocation should be launched with powershell -File, isolating probe environment variables.
$oldLocation=Get-Location
$previousEnvironment=@{}
Get-ChildItem Env: | Where-Object Name -like 'LIVEKIT_*' | ForEach-Object {$previousEnvironment[$_.Name]=$_.Value}
$lockPath=Join-Path $prepared 'diagnostic-probe.lock'
$lock=[IO.File]::Open($lockPath,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
try {
    $global:LASTEXITCODE=0
    & $script @parameters
    if($LASTEXITCODE -ne 0){throw "Probe exited with code $LASTEXITCODE"}
} finally {
    Get-ChildItem Env: | Where-Object Name -like 'LIVEKIT_*' | ForEach-Object {
        [Environment]::SetEnvironmentVariable($_.Name,$null,'Process')
    }
    foreach($key in $previousEnvironment.Keys){[Environment]::SetEnvironmentVariable($key,$previousEnvironment[$key],'Process')}
    $lock.Dispose()
    Remove-Item -LiteralPath $lockPath
    Set-Location -LiteralPath $oldLocation.Path
}
