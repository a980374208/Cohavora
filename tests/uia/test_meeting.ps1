param([Parameter(Mandatory=$true)][string]$Fixture, [string]$OutputDirectory)
& "$PSScriptRoot/run_desktop.ps1" -Fixture $Fixture -OutputDirectory $OutputDirectory -Scenario meeting -Language zh_CN
exit $LASTEXITCODE
