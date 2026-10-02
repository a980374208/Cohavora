param([Parameter(Mandatory=$true)][string]$Fixture, [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
& "$PSScriptRoot/run_desktop.ps1" -Fixture $Fixture -OutputDirectory $OutputDirectory -Scenario settings -Language zh_CN
exit $LASTEXITCODE
