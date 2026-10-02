param(
    [Parameter(Mandatory=$true)][int]$ProductProcessId,
    [Parameter(Mandatory=$true)][string]$Output,
    [ValidateRange(1,20)][int]$Rounds=20
)
$ErrorActionPreference='Stop'
if (Test-Path -LiteralPath $Output) { throw 'Evidence output must be new' }
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$condition=[Windows.Automation.PropertyCondition]::new(
    [Windows.Automation.AutomationElement]::ProcessIdProperty,$ProductProcessId)
$rows=[Collections.Generic.List[object]]::new()
for ($round=0;$round -lt $Rounds;$round++) {
    $process=Get-Process -Id $ProductProcessId
    if ($process.ProcessName -ne 'uia_entry_fixture') { throw 'Unexpected product process' }
    $timer=[Diagnostics.Stopwatch]::StartNew()
    $windows=[Windows.Automation.AutomationElement]::RootElement.FindAll(
        [Windows.Automation.TreeScope]::Children,$condition)
    if ($windows.Count -lt 1) { throw 'No product windows' }
    $controls=0
    foreach ($window in $windows) {
        if ($window.Current.ProcessId -ne $ProductProcessId) { throw 'UIA process mismatch' }
        $controls+=$window.FindAll([Windows.Automation.TreeScope]::Descendants,
            [Windows.Automation.Condition]::TrueCondition).Count
    }
    $timer.Stop()
    $rows.Add([ordered]@{round=$round;pid=$ProductProcessId;windows=$windows.Count;
        controls=$controls;elapsed_ms=$timer.Elapsed.TotalMilliseconds;utc_ms=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()})
    Start-Sleep -Milliseconds 200
}
@{status='PASS';pid=$ProductProcessId;rounds=$Rounds;scope='Separate bounded UIA interference run; never used for performance sampling';rows=$rows} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $Output -Encoding utf8
