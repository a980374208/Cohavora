$button = [Windows.Automation.ControlType]::Button
$initial = Wait-For 'Synthetic participant projection produces multiple pages' {
    $state = Get-LatestState
    if ($null -ne $state -and $state.pages -gt 1 -and $state.page -eq 0 -and $state.selectedVideos -eq 9) { return $state }
}
$previous = Find-Control 'previousVideoPage' $button $false
$next = Find-Control 'nextVideoPage' $button
$indicator = Find-Control 'videoPageIndicator' ([Windows.Automation.ControlType]::Text)
Assert-That ($indicator.Current.Name -eq "1 / $($initial.pages)") 'Page indicator exposes initial numeric state'
$back = Require-Pattern $previous ([Windows.Automation.InvokePattern]::Pattern)
$forward = Require-Pattern $next ([Windows.Automation.InvokePattern]::Pattern)
$lastPage = $initial.pages - 1
for ($page = 1; $page -le $lastPage; ++$page) {
    $forward.Invoke()
    $expectedPage = $page
    $null = Wait-For "Next page accepts plan $expectedPage" {
        $state = Get-LatestState
        $null -ne $state -and $state.page -eq $expectedPage -and
            $state.selectedVideos -ge 1 -and $state.selectedVideos -le 9 -and
            $indicator.Current.Name -eq "$(1 + $expectedPage) / $($initial.pages)"
    }
}
$null = Find-Control 'nextVideoPage' $button $false
$null = Find-Control 'previousVideoPage' $button
for ($page = $lastPage - 1; $page -ge 0; --$page) {
    $back.Invoke()
    $expectedPage = $page
    $null = Wait-For "Previous page accepts plan $expectedPage" {
        $state = Get-LatestState
        $null -ne $state -and $state.page -eq $expectedPage -and $state.selectedVideos -ge 1 -and
            $indicator.Current.Name -eq "$(1 + $expectedPage) / $($initial.pages)"
    }
}
$null = Find-Control 'previousVideoPage' $button $false
$size = Find-Control 'videoPageSize' ([Windows.Automation.ControlType]::ComboBox)
$sizeOpen = Require-Pattern $size ([Windows.Automation.InvokePattern]::Pattern)
$sizeValue = Require-Pattern $size ([Windows.Automation.ValuePattern]::Pattern)
foreach ($choice in @(@{index=0; size=4}, @{index=1; size=9})) {
    $sizeOpen.Invoke()
    $lists = @($size.FindAll([Windows.Automation.TreeScope]::Descendants,
        [Windows.Automation.Condition]::TrueCondition) | Where-Object {
            $_.Current.ControlType -eq [Windows.Automation.ControlType]::List })
    Assert-That ($lists.Count -eq 1) 'Page size exposes one UIA selection list'
    $items = @($lists[0].FindAll([Windows.Automation.TreeScope]::Children,
        [Windows.Automation.Condition]::TrueCondition) | Where-Object {
            $_.Current.ControlType -eq [Windows.Automation.ControlType]::ListItem })
    Assert-That ($items.Count -eq 3) 'Page size exposes three UIA choices'
    $selection = Require-Pattern $items[$choice.index] ([Windows.Automation.SelectionItemPattern]::Pattern)
    $selection.Select()
    $expectedSize = $choice.size
    $null = Wait-For "Page size Selection commits $expectedSize" {
        $state = Get-LatestState
        $null -ne $state -and $state.pageSize -eq $expectedSize -and
            $state.selectedVideos -eq $expectedSize -and $sizeValue.Current.Value -eq "$expectedSize"
    }
}
foreach ($case in @(
    @{id='meetingParticipants'; sidebar=1},
    @{id='meetingChat'; sidebar=2},
    @{id='meetingChat'; sidebar=0}
)) {
    $invoke = Require-Pattern (Find-Control $case.id $button) ([Windows.Automation.InvokePattern]::Pattern)
    $invoke.Invoke()
    $null = Wait-For "$($case.id) updates local sidebar to $($case.sidebar)" {
        $state = Get-LatestState
        $null -ne $state -and $state.sidebar -eq $case.sidebar
    }
}
$whiteboard = Require-Pattern (Find-Control 'meetingWhiteboard' $button) ([Windows.Automation.InvokePattern]::Pattern)
$whiteboard.Invoke()
$null = Wait-For 'Toolbar opens whiteboard model' { $state = Get-LatestState; $null -ne $state -and $state.whiteboard }
$whiteboard.Invoke()
$null = Wait-For 'Toolbar closes whiteboard model' { $state = Get-LatestState; $null -ne $state -and !$state.whiteboard }
$script:deferred = @('Real media frames, audio, RTP, sharing and service membership require independent runtime evidence')
