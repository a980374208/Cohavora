param(
    [ValidateSet('Driver','Product')][string]$Role='Driver',
    [ValidateSet('Complete','Hang','Crash','Stall')][string]$Scenario='Complete'
)
# Disposable Windows UIA fixture, independent of installed product binaries.
$ErrorActionPreference='Stop'
if ($Role -eq 'Product') {
    Add-Type -AssemblyName PresentationFramework
    $form=[Windows.Window]::new()
    $form.Title='UIA retest disposable fixture';$form.Width=320;$form.Height=120
    [Windows.Automation.AutomationProperties]::SetName($form,'UIA retest disposable fixture')
    $button=[Windows.Controls.Button]::new()
    $button.Name='retestAction';$button.Content='Run test action'
    $button.Add_Click({
        if ($Scenario -eq 'Hang') {[Threading.Thread]::Sleep(30000)}
        elseif ($Scenario -eq 'Crash') {[Environment]::Exit(17)}
        else {$button.Content='Action observed'}
    })
    $form.Content=$button
    $null=$form.ShowDialog()
    exit 0
}
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes
$root=$env:UIA_RETEST_TEST_OUTPUT
$directory=Join-Path $root 'uia'
$null=New-Item -ItemType Directory -Path $directory
$child=$null
try {
    # This is the visible interactive target; the surrounding driver is hidden.
    $child=Start-Process powershell.exe -PassThru -ArgumentList @(
        '-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$PSCommandPath+'"'),
        '-Role','Product','-Scenario',$Scenario) -RedirectStandardError (Join-Path $directory 'fixture.stderr')
    $null=$child.Handle
    @{run_id=$env:LIVEKIT_UIA_RUN_ID;pid=$child.Id;start_ticks=$child.StartTime.ToUniversalTime().Ticks} |
        ConvertTo-Json | Set-Content (Join-Path $directory 'product-identity.json.tmp') -Encoding UTF8
    Move-Item (Join-Path $directory 'product-identity.json.tmp') (Join-Path $directory 'product-identity.json')
    $deadline=[DateTime]::UtcNow.AddSeconds(15)
    $window=$null
    $windowCondition=[Windows.Automation.AndCondition]::new(
        [Windows.Automation.PropertyCondition]::new([Windows.Automation.AutomationElement]::ProcessIdProperty,[int]$child.Id),
        [Windows.Automation.PropertyCondition]::new([Windows.Automation.AutomationElement]::NameProperty,'UIA retest disposable fixture'))
    do {
        $child.Refresh()
        if ($child.HasExited) {throw 'FIXTURE_EARLY_EXIT'}
        $window=[Windows.Automation.AutomationElement]::RootElement.FindFirst([Windows.Automation.TreeScope]::Children,$windowCondition)
        if ($window) {break}
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    if (!$window) {
        [Windows.Automation.AutomationElement]::RootElement.FindAll([Windows.Automation.TreeScope]::Children,
            [Windows.Automation.PropertyCondition]::new([Windows.Automation.AutomationElement]::ProcessIdProperty,[int]$child.Id)) |
            ForEach-Object {Write-Output ("fixture window: " + $_.Current.Name + ' ' + $_.Current.ControlType.ProgrammaticName)}
        throw 'FIXTURE_WINDOW_MISSING'
    }
    $condition=[Windows.Automation.PropertyCondition]::new([Windows.Automation.AutomationElement]::NameProperty,'Run test action')
    $deadline=[DateTime]::UtcNow.AddSeconds(5)
    do {
        $button=$window.FindFirst([Windows.Automation.TreeScope]::Descendants,$condition)
        if ($button) {break}
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    if (!$button) {throw 'FIXTURE_UIA_BUTTON_MISSING'}
    $pattern=$button.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern)
    @{run_id=$env:LIVEKIT_UIA_RUN_ID;pid=$child.Id;action='fixture_invoke';phase='requested';cycle=0} |
        ConvertTo-Json -Compress | Add-Content (Join-Path $directory 'uia-actions.jsonl') -Encoding UTF8
    if ($Scenario -ne 'Stall') {$pattern.Invoke()}
    if ($Scenario -ne 'Complete') {Start-Sleep -Seconds 30;throw 'FAULT_NOT_DETECTED'}
    $deadline=[DateTime]::UtcNow.AddSeconds(5)
    do {
        if ($button.Current.Name -eq 'Action observed') {break}
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($button.Current.Name -ne 'Action observed') {throw 'UIA_ACTION_NOT_OBSERVED'}
    @{run_id=$env:LIVEKIT_UIA_RUN_ID;pid=$child.Id;action='process_exit';phase='requested';cycle=0} |
        ConvertTo-Json -Compress | Add-Content (Join-Path $directory 'uia-actions.jsonl') -Encoding UTF8
    $window.GetCurrentPattern([Windows.Automation.WindowPattern]::Pattern).Close()
    if (!$child.WaitForExit(5000)) {throw 'FIXTURE_CLOSE_FAILED'}
    @{run_id=$env:LIVEKIT_UIA_RUN_ID;verdict='PROBED'} | ConvertTo-Json |
        Set-Content (Join-Path $directory 'uia-result.json') -Encoding UTF8
} finally {
    if ($child) {
        if (!$child.HasExited) {$child.Kill();$child.WaitForExit()}
        $child.Dispose()
    }
}
