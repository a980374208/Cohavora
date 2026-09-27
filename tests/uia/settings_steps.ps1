$button = [Windows.Automation.ControlType]::Button
$checkBox = [Windows.Automation.ControlType]::CheckBox
$navigation = Find-Control 'settingsNavigation' ([Windows.Automation.ControlType]::List)
$items = @($navigation.FindAll([Windows.Automation.TreeScope]::Children,
    [Windows.Automation.Condition]::TrueCondition) | Where-Object {
        $_.Current.ControlType -eq [Windows.Automation.ControlType]::ListItem })
Assert-That ($items.Count -eq 4) 'Settings navigation exposes four items'
foreach ($item in $items) {
    Assert-That ($item.Current.ProcessId -eq $script:child.Id -and
        ![string]::IsNullOrWhiteSpace($item.Current.Name)) 'Navigation item is named and process-scoped'
    $null = Require-Pattern $item ([Windows.Automation.SelectionItemPattern]::Pattern)
}
$camera = Require-Pattern (Find-Control 'settingsJoinCamera' $checkBox) ([Windows.Automation.TogglePattern]::Pattern)
$before = $camera.Current.ToggleState
$camera.Toggle()
$null = Wait-For 'General camera preference toggles' { $camera.Current.ToggleState -ne $before }
$expected = $camera.Current.ToggleState -eq [Windows.Automation.ToggleState]::On
$null = Wait-For 'Session preference records camera toggle' {
    $state = Get-LatestState
    $null -ne $state -and $state.camera -eq $expected -and
        $state.settingsFileExists -and $state.settingsIsolated -and
        $state.storedCamera -eq $expected
}
$audio = Require-Pattern $items[2] ([Windows.Automation.SelectionItemPattern]::Pattern)
$audio.Select()
$aec = Wait-For 'Audio page exposes AEC' {
    $nodes = $script:window.FindAll([Windows.Automation.TreeScope]::Descendants,
        [Windows.Automation.Condition]::TrueCondition)
    @($nodes | Where-Object { $_.Current.ProcessId -eq $script:child.Id -and
        ($_.Current.AutomationId -eq 'echoCancellationCheckBox' -or
        $_.Current.AutomationId.EndsWith('.echoCancellationCheckBox')) -and !$_.Current.IsOffscreen }) | Select-Object -First 1
}
$aecToggle = Require-Pattern $aec ([Windows.Automation.TogglePattern]::Pattern)
$aecBefore = $aecToggle.Current.ToggleState
$aecToggle.Toggle()
$null = Wait-For 'AEC preference toggles' { $aecToggle.Current.ToggleState -ne $aecBefore }
$expectedAec = $aecToggle.Current.ToggleState -eq [Windows.Automation.ToggleState]::On
$null = Wait-For 'Session preference records AEC toggle' {
    $state = Get-LatestState
    $null -ne $state -and $state.aec -eq $expectedAec -and
        $state.settingsFileExists -and $state.settingsIsolated -and
        $state.storedAec -eq $expectedAec
}
$general = Require-Pattern $items[0] ([Windows.Automation.SelectionItemPattern]::Pattern)
$general.Select()
$null = Find-Control 'settingsJoinCamera' $checkBox
$close = Require-Pattern (Find-Control 'closeButton' $button) ([Windows.Automation.InvokePattern]::Pattern)
$close.Invoke()
Assert-That ($script:child.WaitForExit(5000)) 'Settings closes through Invoke'
$script:deferred = @('Camera preview, microphone test and actual devices require hardware evidence')
