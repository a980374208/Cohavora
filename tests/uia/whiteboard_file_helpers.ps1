function Get-FileDialog {
    $condition = New-Object Windows.Automation.PropertyCondition(
        [Windows.Automation.AutomationElement]::ClassNameProperty, 'class QFileDialog')
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
$button = [Windows.Automation.ControlType]::Button
function Open-FileDialog([string]$Kind, [string]$Stage) {
    $id = if ($Kind -eq 'Import') { 'whiteboardImportImage' } else { 'whiteboardExport' }
    (Require-Pattern (Find-Control $id $button) ([Windows.Automation.InvokePattern]::Pattern)).Invoke()
    $dialog = Wait-For "$Stage dialog appears" { Get-FileDialog }
    Assert-That ($dialog.Current.AutomationId.EndsWith(".whiteboard${Kind}Dialog")) "$Stage stable dialog identity"
    Assert-That ($dialog.Current.ControlType -eq [Windows.Automation.ControlType]::Window) "$Stage Window role"
    Assert-That (Require-Pattern $dialog ([Windows.Automation.WindowPattern]::Pattern)).Current.IsModal "$Stage modal"
    Save-Tree "$Stage-dialog-tree" $dialog
    return $dialog
}
function Close-FileDialog($Dialog, [string]$Stage) {
    (Require-Pattern (Find-Control 'whiteboardFileCancel' $button $true $Dialog) ([Windows.Automation.InvokePattern]::Pattern)).Invoke()
    Wait-FileClosed $Stage
}
function Wait-FileClosed([string]$Stage) {
    $null = Wait-For "$Stage dismissed" { $null -eq (Get-FileDialog) }
    $null = Wait-For "$Stage parent enabled" { $script:window.Current.IsEnabled }
}
function Set-FilePath($Dialog, [string]$Path) {
    $value = Require-Pattern (Find-Control 'fileNameEdit' ([Windows.Automation.ControlType]::Edit) $true $Dialog) ([Windows.Automation.ValuePattern]::Pattern)
    Assert-That (!$value.Current.IsReadOnly) 'filename is writable'
    $value.SetValue($Path)
    $null = Wait-For 'filename Value roundtrip' { $value.Current.Value -ceq $Path }
}
function Get-OverwriteDialog {
    $condition = New-Object Windows.Automation.PropertyCondition(
        [Windows.Automation.AutomationElement]::ClassNameProperty, 'class QMessageBox')
    $nodes = @($script:window.FindAll([Windows.Automation.TreeScope]::Descendants, $condition) |
        Where-Object { $_.Current.ProcessId -eq $script:child.Id })
    if ($nodes.Count -gt 1) { throw 'Ambiguous overwrite dialog' }
    if ($nodes.Count -eq 1) { return $nodes[0] }
}
function Open-OverwriteConfirmation($Dialog, [string]$Stage) {
    (Require-Pattern (Find-Control 'whiteboardFileAccept' $button $true $Dialog) ([Windows.Automation.InvokePattern]::Pattern)).Invoke()
    $box = Wait-For "$Stage overwrite confirmation appears" { Get-OverwriteDialog }
    Save-Tree "$Stage-confirmation-tree" $box
    Assert-That ($box.Current.AutomationId -eq 'whiteboardOverwriteConfirmation' -or
        $box.Current.AutomationId.EndsWith('.whiteboardOverwriteConfirmation')) "$Stage stable confirmation identity"
    Assert-That ($box.Current.ControlType -eq [Windows.Automation.ControlType]::Window) "$Stage confirmation Window role"
    Assert-That (Require-Pattern $box ([Windows.Automation.WindowPattern]::Pattern)).Current.IsModal "$Stage confirmation modal"
    $null = Require-Pattern (Find-Control 'whiteboardOverwriteConfirm' $button $true $box) ([Windows.Automation.InvokePattern]::Pattern)
    $cancel = Find-Control 'whiteboardOverwriteCancel' $button $true $box
    $null = Require-Pattern $cancel ([Windows.Automation.InvokePattern]::Pattern)
    $null = Wait-For "$Stage No has default focus" { $cancel.Current.HasKeyboardFocus }
    return $box
}
