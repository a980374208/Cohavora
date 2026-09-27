$checkBox = [Windows.Automation.ControlType]::CheckBox
$button = [Windows.Automation.ControlType]::Button
$combo = [Windows.Automation.ControlType]::ComboBox
$spinner = [Windows.Automation.ControlType]::Spinner
$toolNames = @('Pen','Highlighter','Line','Rectangle','Ellipse','Arrow','Text','Eraser','Laser','Pan')
$tools = @{}
foreach ($name in $toolNames) {
    $tools[$name] = Require-Pattern (Find-Control "whiteboardTool$name" $checkBox) ([Windows.Automation.TogglePattern]::Pattern)
}
function Test-ExclusiveTool([string]$Selected) {
    foreach ($name in $toolNames) {
        $expected = if ($name -eq $Selected) { [Windows.Automation.ToggleState]::On } else { [Windows.Automation.ToggleState]::Off }
        if ($tools[$name].Current.ToggleState -ne $expected) { return $false }
    }
    return $true
}
Assert-That (Test-ExclusiveTool 'Pen') 'Only Pen is initially selected'
foreach ($name in @('Highlighter','Line','Rectangle','Ellipse','Arrow','Text','Eraser','Laser','Pan','Pen')) {
    $script:step = "Toggle $name tool"
    $tools[$name].Toggle()
    $null = Wait-For "Only $name selected after Toggle" { Test-ExclusiveTool $name }
}
# An exclusive group must not end up with zero selected tools.
$tools['Pen'].Toggle()
$null = Wait-For 'Toggling the active exclusive tool keeps Pen selected' { Test-ExclusiveTool 'Pen' }

foreach ($setting in @(
    @{id='whiteboardInkWidth'; initial=4; minimum=1; maximum=32; value=9},
    @{id='whiteboardTextSize'; initial=28; minimum=8; maximum=96; value=40}
)) {
    $range = Require-Pattern (Find-Control $setting.id $spinner) ([Windows.Automation.RangeValuePattern]::Pattern)
    Assert-That (!$range.Current.IsReadOnly) "$($setting.id) is writable"
    Assert-That ($range.Current.Value -eq $setting.initial -and
        $range.Current.Minimum -eq $setting.minimum -and $range.Current.Maximum -eq $setting.maximum) "$($setting.id) initial value and bounds"
    $script:step = "RangeValue.SetValue $($setting.id)"
    $range.SetValue($setting.value)
    $null = Wait-For "$($setting.id) changed through RangeValue" { $range.Current.Value -eq $setting.value }
    $range.SetValue($setting.initial)
    $null = Wait-For "$($setting.id) restored through RangeValue" { $range.Current.Value -eq $setting.initial }
}

function Get-ComboItems($Node, [int]$ExpectedCount) {
    $listCondition = New-Object Windows.Automation.PropertyCondition(
        [Windows.Automation.AutomationElement]::ControlTypeProperty, [Windows.Automation.ControlType]::List)
    $lists = $Node.FindAll([Windows.Automation.TreeScope]::Descendants, $listCondition)
    Assert-That ($lists.Count -eq 1) "$($Node.Current.AutomationId) has one selection list"
    $selection = Require-Pattern $lists[0] ([Windows.Automation.SelectionPattern]::Pattern)
    Assert-That (!$selection.Current.CanSelectMultiple) "$($Node.Current.AutomationId) is single-selection"
    $itemCondition = New-Object Windows.Automation.PropertyCondition(
        [Windows.Automation.AutomationElement]::ControlTypeProperty, [Windows.Automation.ControlType]::ListItem)
    $items = @($lists[0].FindAll([Windows.Automation.TreeScope]::Children, $itemCondition))
    Assert-That ($items.Count -eq $ExpectedCount) "$($Node.Current.AutomationId) has $ExpectedCount items"
    foreach ($item in $items) {
        Assert-That ($item.Current.ProcessId -eq $script:child.Id) 'Selection item belongs to fixture process'
    }
    return $items
}
function Select-ComboItem($Node, [int]$Index, [int]$Count, [switch]$Invoke) {
    # Index is a declared option-order contract; never use translated visible labels.
    $open = Require-Pattern $Node ([Windows.Automation.InvokePattern]::Pattern)
    $open.Invoke()
    $items = @(Get-ComboItems $Node $Count)
    $value = Require-Pattern $Node ([Windows.Automation.ValuePattern]::Pattern)
    $expectedName = $items[$Index].Current.Name
    $script:step = "Select $($Node.Current.AutomationId) index $Index"
    if ($Invoke) {
        $activate = Require-Pattern $items[$Index] ([Windows.Automation.InvokePattern]::Pattern)
        $activate.Invoke()
    } else {
        $pattern = Require-Pattern $items[$Index] ([Windows.Automation.SelectionItemPattern]::Pattern)
        $pattern.Select()
    }
    $null = Wait-For "$($Node.Current.AutomationId) selected index $Index and projected value" {
        $selected = 0
        # SelectPage rebuilds the model. Do not retain removed ListItem providers.
        $condition = New-Object Windows.Automation.PropertyCondition(
            [Windows.Automation.AutomationElement]::ControlTypeProperty, [Windows.Automation.ControlType]::ListItem)
        $items = @($Node.FindAll([Windows.Automation.TreeScope]::Descendants, $condition))
        if ($items.Count -ne $Count) { return $false }
        for ($i = 0; $i -lt $items.Count; ++$i) {
            $p = Require-Pattern $items[$i] ([Windows.Automation.SelectionItemPattern]::Pattern)
            if ($p.Current.IsSelected) { if ($i -ne $Index) { return $false }; ++$selected }
        }
        $selected -eq 1 -and $value.Current.Value -eq $expectedName
    }
}
$colors = Find-Control 'whiteboardInkColor' $combo
$null = Get-ComboItems $colors 6
$script:deferred = @('Canvas drawing, text entry, import/export, confirmation dialogs and collaboration are outside this toolbar subset')
# Selection commit is now mandatory. The former probe flag remains compatible.
Select-ComboItem $colors 1 6
Select-ComboItem $colors 0 6 -Invoke
$zoom = Find-Control 'whiteboardZoom' $combo
$zoomValue = Require-Pattern $zoom ([Windows.Automation.ValuePattern]::Pattern)
$initialZoom = $zoomValue.Current.Value
$zoomItems = @(Get-ComboItems $zoom 6)
$fitValue = $zoomItems[2].Current.Name
Assert-That ($initialZoom -eq $zoomItems[4].Current.Name -and $initialZoom -ne $fitValue) 'Fixture starts at non-fit zoom'
Select-ComboItem $zoom 5 6
$fit = Require-Pattern (Find-Control 'whiteboardFit' $button) ([Windows.Automation.InvokePattern]::Pattern)
$script:step = 'Invoke Fit page'
$fit.Invoke()
$null = Wait-For 'Fit page updates zoom to fit option' { $zoomValue.Current.Value -eq $fitValue }

$null = Find-Control 'whiteboardUndo' $button $false
$null = Find-Control 'whiteboardRedo' $button $false
foreach ($id in @('whiteboardClear','whiteboardImportImage','whiteboardExport','whiteboardClose')) {
    $null = Require-Pattern (Find-Control $id $button) ([Windows.Automation.InvokePattern]::Pattern)
}
$pages = Find-Control 'whiteboardPages' $combo
$null = Get-ComboItems $pages 1
$add = Require-Pattern (Find-Control 'whiteboardAddPage' $button) ([Windows.Automation.InvokePattern]::Pattern)
$script:step = 'Invoke Add page'
$add.Invoke()
$null = Wait-For 'Add page exposes two page choices' {
    $condition = New-Object Windows.Automation.PropertyCondition(
        [Windows.Automation.AutomationElement]::ControlTypeProperty, [Windows.Automation.ControlType]::ListItem)
    $pages.FindAll([Windows.Automation.TreeScope]::Descendants, $condition).Count -eq 2
}
$pageItems = @(Get-ComboItems $pages 2)
$pageValue = Require-Pattern $pages ([Windows.Automation.ValuePattern]::Pattern)
Assert-That ($pageValue.Current.Value -eq $pageItems[1].Current.Name -and
    $pageItems[0].Current.Name -ne $pageItems[1].Current.Name) 'Add page projects the new active page'
Select-ComboItem $pages 0 2
Select-ComboItem $pages 1 2 -Invoke
