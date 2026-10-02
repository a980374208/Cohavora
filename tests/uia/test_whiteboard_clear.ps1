param(
    [Parameter(Mandatory=$true)][string]$Fixture,
    [string]$OutputDirectory = "$PSScriptRoot/../../out/uia-results",
    [switch]$ProbeOnly,
    [ValidateSet('zh_CN','en_US')][string]$Language = 'zh_CN'
)
$ErrorActionPreference = 'Stop'
& "$PSScriptRoot/run_desktop.ps1" @PSBoundParameters -Scenario 'whiteboard_clear'
exit $LASTEXITCODE
