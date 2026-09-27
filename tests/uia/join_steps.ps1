$edit = [Windows.Automation.ControlType]::Edit
$button = [Windows.Automation.ControlType]::Button
$checkBox = [Windows.Automation.ControlType]::CheckBox
$meetingId = Find-Control 'joinMeetingId' $edit
$idValue = Require-Pattern $meetingId ([Windows.Automation.ValuePattern]::Pattern)
Assert-That (!$idValue.Current.IsReadOnly -and $idValue.Current.Value -eq '') 'Meeting ID is initially writable and empty'
$name = Require-Pattern (Find-Control 'joinDisplayName' $edit) ([Windows.Automation.ValuePattern]::Pattern)
$name.SetValue('UIA participant')
$null = Wait-For 'Display name accepts Value.SetValue' { $name.Current.Value -eq 'UIA participant' }
foreach ($id in @('joinMicrophone','joinCamera')) {
    $toggle = Require-Pattern (Find-Control $id $checkBox) ([Windows.Automation.TogglePattern]::Pattern)
    $before = $toggle.Current.ToggleState
    $toggle.Toggle()
    $null = Wait-For "$id changes ToggleState" { $toggle.Current.ToggleState -ne $before }
}
$join = Require-Pattern (Find-Control 'joinBtn' $button) ([Windows.Automation.InvokePattern]::Pattern)
$join.Invoke()
$null = Wait-For 'Empty meeting ID produces accessible local validation' {
    $nodes = $script:window.FindAll([Windows.Automation.TreeScope]::Descendants,
        [Windows.Automation.Condition]::TrueCondition)
    @($nodes | Where-Object { $_.Current.ProcessId -eq $script:child.Id -and
        $_.Current.AutomationId.EndsWith('.joinStatus') -and
        $_.Current.ControlType -eq [Windows.Automation.ControlType]::Text -and
        ![string]::IsNullOrWhiteSpace($_.Current.Name) -and !$_.Current.IsOffscreen }).Count -eq 1
}
$advanced = Require-Pattern (Find-Control 'linkBtn' $button) ([Windows.Automation.InvokePattern]::Pattern)
$advanced.Invoke()
$server = Wait-For 'Advanced server URL appears' {
    $nodes = $script:window.FindAll([Windows.Automation.TreeScope]::Descendants,
        [Windows.Automation.Condition]::TrueCondition)
    @($nodes | Where-Object { $_.Current.ProcessId -eq $script:child.Id -and
        ($_.Current.AutomationId -eq 'joinServerUrl' -or $_.Current.AutomationId.EndsWith('.joinServerUrl')) -and
        !$_.Current.IsOffscreen }) | Select-Object -First 1
}
$serverValue = Require-Pattern $server ([Windows.Automation.ValuePattern]::Pattern)
Assert-That (!$serverValue.Current.IsReadOnly) 'Advanced server URL is writable'
$cancel = Require-Pattern (Find-Control 'cancelBtn' $button) ([Windows.Automation.InvokePattern]::Pattern)
$cancel.Invoke()
Assert-That ($script:child.WaitForExit(5000)) 'Cancel exits the local fixture without entering a meeting'
$script:deferred = @('Real join and token exchange require service and account evidence')
