param(
    [Parameter(Mandatory=$true)][string]$Fixture,
    [string]$OutputDirectory = "$PSScriptRoot/../../out/uia-results",
    [switch]$ProbeOnly,
    [switch]$ProbeComboSelection,
    [ValidateSet('zh_CN','en_US')][string]$Language = 'zh_CN'
)
& "$PSScriptRoot/run_desktop.ps1" @PSBoundParameters -Scenario 'whiteboard'
exit $LASTEXITCODE
