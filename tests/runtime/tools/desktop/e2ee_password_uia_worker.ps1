param([Parameter(Mandatory=$true)][string]$RequestFile,
      [Parameter(Mandatory=$true)][string]$ResultFile)
# Dedicated public-marker fixture only. Never persist Value/Text contents.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes
$request = Get-Content -LiteralPath $RequestFile -Raw | ConvertFrom-Json
$result = @{status='FAIL'; scope='external_windows_uia_password_public_fixture'}
try {
    $root = [Windows.Automation.AutomationElement]::FromHandle([IntPtr][long]$request.hwnd)
    if ($root.Current.ProcessId -ne [int]$request.pid) {throw 'IDENTITY_MISMATCH'}
    $condition = [Windows.Automation.PropertyCondition]::new(
        [Windows.Automation.AutomationElement]::AutomationIdProperty,
        'meetingEncryptionDialog.dialogContainer.e2eeKeyInput')
    $nodes = $root.FindAll([Windows.Automation.TreeScope]::Descendants, $condition)
    $result.control_count = $nodes.Count
    if ($nodes.Count -ne 1) {throw 'EXACT_CONTROL_MISSING'}
    $node = $nodes.Item(0)
    $result.password = $node.Current.IsPassword
    $result.name_safe = !$node.Current.Name.Contains('public-uia-password-marker')
    $value = $null
    $result.value_pattern = $node.TryGetCurrentPattern([Windows.Automation.ValuePattern]::Pattern, [ref]$value)
    $result.value_safe = $true
    if ($result.value_pattern) {
        try {$result.value_safe = !$value.Current.Value.Contains('public-uia-password-marker')}
        catch [System.InvalidOperationException] {$result.value_denied = $true}
    }
    $text = $null
    $result.text_pattern = $node.TryGetCurrentPattern([Windows.Automation.TextPattern]::Pattern, [ref]$text)
    $result.text_safe = $true
    if ($result.text_pattern) {
        try {$result.text_safe = !$text.DocumentRange.GetText(4096).Contains('public-uia-password-marker')}
        catch [System.InvalidOperationException] {$result.text_denied = $true}
    }
    if ($result.password -and $result.name_safe -and $result.value_safe -and $result.text_safe) {$result.status='PASS'}
} catch {$result.error_type = $_.Exception.GetType().Name}
$result | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $ResultFile -Encoding UTF8
if ($result.status -ne 'PASS') {exit 1}
