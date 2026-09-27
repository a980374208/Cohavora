function Get-ClearDialog {
    $condition = New-Object Windows.Automation.PropertyCondition(
        [Windows.Automation.AutomationElement]::ClassNameProperty, 'class QMessageBox')
    $found = @{}
    $roots = [Windows.Automation.AutomationElement]::RootElement.FindAll(
        [Windows.Automation.TreeScope]::Children, $pidCondition)
    foreach ($root in $roots) {
        foreach ($node in $root.FindAll([Windows.Automation.TreeScope]::Subtree, $condition)) {
            if ($node.Current.ProcessId -eq $script:child.Id) { $found[($node.GetRuntimeId() -join ',')] = $node }
        }
    }
    if ($found.Count -gt 1) { throw 'Ambiguous process-scoped confirmation dialogs' }
    if ($found.Count -eq 1) { return @($found.Values)[0] }
}
$button = [Windows.Automation.ControlType]::Button
$clear = Require-Pattern (Find-Control 'whiteboardClear' $button) ([Windows.Automation.InvokePattern]::Pattern)
if ($ProbeOnly) {
    $clear.Invoke()
    $dialog = Wait-For 'clear confirmation appears' { Get-ClearDialog }
    Save-Tree 'clear-dialog-tree' $dialog
    return
}
function Read-Model {
    $latest = Get-ChildItem -LiteralPath $OutputDirectory -Filter 'model-state.json.*' |
        Where-Object { $_.Name -match '^model-state\.json\.\d{8}$' } |
        Sort-Object Name | Select-Object -Last 1
    if (!$latest) { return $null }
    $model = Get-Content -LiteralPath $latest.FullName -Raw | ConvertFrom-Json
    if ($model.pid -ne $script:child.Id) { throw 'Model witness PID mismatch' }
    return $model
}
function Save-Model($Model, [string]$Name) {
    $Model | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 "$OutputDirectory/$Name.json"
}
function Open-Confirmation([string]$Stage) {
    $script:step = "$Stage Invoke Clear"
    $clear.Invoke()
    $dialog = Wait-For "$Stage confirmation appears" { Get-ClearDialog }
    Assert-That ($dialog.Current.AutomationId -eq 'whiteboardClearConfirmation' -or
        $dialog.Current.AutomationId.EndsWith('.whiteboardClearConfirmation')) "$Stage stable dialog ID"
    Assert-That ($dialog.Current.ControlType -eq [Windows.Automation.ControlType]::Window) "$Stage dialog role Window"
    $windowPattern = Require-Pattern $dialog ([Windows.Automation.WindowPattern]::Pattern)
    Assert-That $windowPattern.Current.IsModal "$Stage dialog is modal"
    $cancel = Find-Control 'whiteboardClearCancel' $button $true $dialog
    $null = Require-Pattern $cancel ([Windows.Automation.InvokePattern]::Pattern)
    $null = Require-Pattern (Find-Control 'whiteboardClearConfirm' $button $true $dialog) ([Windows.Automation.InvokePattern]::Pattern)
    $null = Wait-For "$Stage Cancel has initial keyboard focus" { $cancel.Current.HasKeyboardFocus }
    Save-Tree "$Stage-dialog-tree" $dialog
    return $dialog
}
function Wait-Closed([string]$Stage) {
    $null = Wait-For "$Stage dialog dismissed" { $null -eq (Get-ClearDialog) }
    $null = Wait-For "$Stage parent interactive again" { $script:window.Current.IsEnabled }
}
$baseline = Wait-For 'fresh seeded model witness' { $m = Read-Model; if ($m -and $m.objects -eq 1 -and $m.canUndo) { $m } }
Save-Model $baseline 'model-before'
$null = Find-Control 'whiteboardUndo' $button $true
$dialog = Open-Confirmation 'cancel'
$cancel = Require-Pattern (Find-Control 'whiteboardClearCancel' $button $true $dialog) ([Windows.Automation.InvokePattern]::Pattern)
$cancel.Invoke()
Wait-Closed 'cancel'
$afterCancel = Wait-For 'Cancel preserves complete model snapshot' {
    $m = Read-Model
    if ($m.sample -gt $baseline.sample -and $m.snapshot -ceq $baseline.snapshot -and $m.canUndo) { $m }
}
Save-Model $afterCancel 'model-after-cancel'
$null = Find-Control 'whiteboardUndo' $button $true

$dialog = Open-Confirmation 'close'
$windowPattern = Require-Pattern $dialog ([Windows.Automation.WindowPattern]::Pattern)
$windowPattern.Close()
Wait-Closed 'close'
$afterClose = Wait-For 'Window.Close preserves complete model snapshot' {
    $m = Read-Model
    if ($m.sample -gt $afterCancel.sample -and $m.snapshot -ceq $baseline.snapshot -and $m.canUndo) { $m }
}
Save-Model $afterClose 'model-after-close'

$dialog = Open-Confirmation 'confirm'
$confirm = Require-Pattern (Find-Control 'whiteboardClearConfirm' $button $true $dialog) ([Windows.Automation.InvokePattern]::Pattern)
$confirm.Invoke()
Wait-Closed 'confirm'
$afterClear = Wait-For 'Confirm removes objects and undo history on same page' {
    $m = Read-Model
    if ($m.sample -gt $afterClose.sample -and $m.objects -eq 0 -and
        $m.pageId -eq $baseline.pageId -and $m.epoch -gt $baseline.epoch -and !$m.canUndo -and !$m.canRedo) { $m }
}
Save-Model $afterClear 'model-after-confirm'
$null = Find-Control 'whiteboardUndo' $button $false
$null = Find-Control 'whiteboardRedo' $button $false
$script:deferred = @('Remote collaboration, persistence and rendered pixel validation are not covered by this local confirmation fixture')
