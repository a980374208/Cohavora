param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)][ValidateSet('external','isolation','persistent')][string]$Scenario,
    [ValidateSet('apartment','query','session')][string[]]$IsolationMode=@('apartment','query','session')
)
$ErrorActionPreference='Stop'
$Executable=(Resolve-Path -LiteralPath $Executable).Path
if(Test-Path -LiteralPath $OutputDirectory){throw 'Output directory must be new'}
$root=(New-Item -ItemType Directory -Path $OutputDirectory).FullName
$tag='handles-'+$Scenario+'-'+[guid]::NewGuid().ToString('N')
$codes=@{}
$started=$false
$previousCycles=$env:LIVEKIT_TEST_SHARE_CYCLES
try {
    & "$PSScriptRoot/handle_wpr.ps1" -Action start -OutputDirectory $root -Tag $tag
    $started=$true
    if($Scenario -eq 'external'){$env:LIVEKIT_TEST_SHARE_CYCLES='24'}
    $modes=if($Scenario -eq 'isolation'){$IsolationMode}else{@($Scenario)}
    foreach($mode in $modes) {
        $arguments=switch($Scenario) {
            'external' {@('--external-wgc-window-lifecycle-probe')}
            'persistent' {@('--wgc-persistent-isolation')}
            'isolation' {@('--wgc-isolation',$mode)}
        }
        $process=Start-Process -FilePath $Executable -ArgumentList $arguments -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput (Join-Path $root "$tag-$mode.log") `
            -RedirectStandardError (Join-Path $root "$tag-$mode.stderr.log")
        try {
            # Preserve exit status for short redirected probes on Windows PowerShell.
            $null=$process.Handle
            if(-not $process.WaitForExit(240000)){throw 'Probe timed out'}
            if($null -eq $process.ExitCode){throw 'Probe exit code unavailable'}
            $codes[$mode]=$process.ExitCode
        } finally {
            if(-not $process.HasExited){$process.Kill();$process.WaitForExit()}
            $process.Dispose()
        }
    }
} finally {
    try {
        if($started){& "$PSScriptRoot/handle_wpr.ps1" -Action stop -OutputDirectory $root -Tag $tag}
    } finally {
        $env:LIVEKIT_TEST_SHARE_CYCLES=$previousCycles
        @{tag=$tag;scenario=$Scenario;probe_exit_codes=$codes;binary_sha256=(Get-FileHash $Executable).Hash} |
            ConvertTo-Json -Depth 4 | Set-Content (Join-Path $root 'result.json') -Encoding UTF8
    }
}
if(@($codes.Values | Where-Object {$_ -ne 0}).Count){throw 'Handle probe failed; inspect result.json'}
