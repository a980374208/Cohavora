# Exercise the real driver functions without starting the product or touching
# the desktop. Script properties model UIA Current values changing on teardown.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$path = Join-Path $PSScriptRoot 'product_desktop.ps1'
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Driver parse error' }
foreach ($name in @('Find-Node', 'Wait-Top', 'Require-Pattern', 'Invoke', 'Share-State', 'Assert-ShareActive', 'Action')) {
    $definition = $ast.Find({param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $true)
    if (!$definition) { throw "Missing driver function: $name" }
    Invoke-Expression $definition.Extent.Text
}
$script:child = [pscustomobject]@{Id=123;HasExited=$false}
$script:nodes = @()
function Get-Nodes {
    if ($script:missingLookups -gt 0) { --$script:missingLookups; return @() }
    $script:nodes
}
function Get-LiveNode([string]$Id, $Role, [switch]$Optional) {
    # The fixture supplies a live node corresponding to the observed cached ID.
    $script:resolvedId=$Id
    return $script:nodes[0]
}
function Wait-For([string]$Description, [scriptblock]$Condition, [int]$Seconds) {
    for ($i=0; $i -lt 2; ++$i) { $result=& $Condition; if ($result) { return $result } }
    throw "TIMEOUT: $Description"
}
function Assert($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
function Make-Node([string]$Id, $Role, [switch]$Vanish) {
    $state = [pscustomobject]@{Id=$Id;Reads=0;Vanish=[bool]$Vanish;PatternAvailable=$true;ToggleState=[Windows.Automation.ToggleState]::On}
    $current = [pscustomobject]@{Name='accessible';ControlType=$Role;IsOffscreen=$false;IsEnabled=$true;State=$state}
    $current | Add-Member ScriptProperty AutomationId {
        ++$this.State.Reads
        if (!$this.State.Vanish -or $this.State.Reads -le 2) { return $this.State.Id }
        return $null
    }
    $node = [pscustomobject]@{Current=$current;State=$state}
    $node | Add-Member ScriptMethod TryGetCurrentPattern {
        param($Id,$Result)
        if (!$this.State.PatternAvailable) { return $false }
        $Result.Value = [pscustomobject]@{Supported=$true;Current=[pscustomobject]@{ToggleState=$this.State.ToggleState}}
        $Result.Value | Add-Member ScriptMethod Invoke { ++$script:invocations }
        return $true
    }
    return $node
}
$button = [Windows.Automation.ControlType]::Button
$window = [Windows.Automation.ControlType]::Window
$script:nodes = @(Make-Node 'owner.meetingLeave' $button -Vanish)
$found = Find-Node 'meetingLeave' $button -Optional
Assert ($null -ne $found) 'Transient ID must match its single observed value'
Assert ($script:nodes[0].State.Reads -eq 1) 'Identity was reread during one lookup'
Assert ($script:resolvedId -eq 'owner.meetingLeave') 'Live lookup must use the exact observed qualified ID'
$script:nodes[0].State.Reads = 2
Assert ($null -eq (Find-Node 'meetingLeave' $button -Optional)) 'Vanished ID should be absent on the next lookup'
$script:nodes = @(Make-Node 'owner.MeetingRoomWindow' $window -Vanish)
Assert ($null -ne (Wait-Top 'MeetingRoomWindow')) 'Top-level lookup lost the captured identity'
Assert ($script:nodes[0].State.Reads -eq 1) 'Top-level identity was reread'
$script:nodes = @(Make-Node 'meetingLeave' $button)
Assert ($null -ne (Find-Node 'meetingLeave' $button)) 'Exact ID did not match'
$script:nodes = @(Make-Node 'unrelatedmeetingLeave' $button)
Assert ($null -eq (Find-Node 'meetingLeave' $button -Optional)) 'Suffix without separator must not match'
$script:nodes = @((Make-Node 'meetingLeave' $button),(Make-Node 'owner.meetingLeave' $button))
try { $null=Find-Node 'meetingLeave' $button; throw 'Duplicate was accepted' }
catch { if ($_.Exception.Message -notlike 'CONTROL_COUNT:*') { throw } }
$script:nodes = @(Make-Node 'meetingLeave' $window)
try { $null=Find-Node 'meetingLeave' $button; throw 'Wrong role was accepted' }
catch { if ($_.Exception.Message -notlike 'CONTROL_CONTRACT:*') { throw } }
$node=Make-Node 'meetingLeave' $button
$node.State.PatternAvailable = $false
try { $null=Require-Pattern $node ([Windows.Automation.InvokePattern]::Pattern); throw 'Missing pattern was accepted' }
catch { if ($_.Exception.Message -notlike 'MISSING_PATTERN:*') { throw } }
$script:nodes = @(Make-Node 'meetingConsole' $button)
$script:missingLookups=1
$script:invocations=0
Invoke 'meetingConsole'
Assert ($script:invocations -eq 1) 'Discovery recovery must invoke exactly once'
$script:nodes[0].State.PatternAvailable=$false
try { Invoke 'meetingConsole'; throw 'Invoke accepted missing pattern' }
catch { if ($_.Exception.Message -notlike 'MISSING_PATTERN:*') { throw } }
Assert ($script:invocations -eq 1) 'Missing pattern must not submit an action'
$script:shareNodes=@{meetingShareScreen=(Make-Node 'meetingShareScreen' ([Windows.Automation.ControlType]::CheckBox))}
function Find-Node([string]$Id,$Role,[switch]$Optional) { return $script:shareNodes[$Id] }
Assert (!(Share-State ([Windows.Automation.ToggleState]::On))) 'Starting toggle must not imply active sharing'
$script:shareNodes.screenShareAnnotation=Make-Node 'screenShareAnnotation' $button
Assert (Share-State ([Windows.Automation.ToggleState]::On)) 'Projected active sharing was not detected'
$script:shareNodes.meetingScreenShareFailure=Make-Node 'meetingScreenShareFailure' $window
try { Assert-ShareActive; throw 'Share failure dialog was ignored' }
catch { if ($_.Exception.Message -notlike 'SCREEN_SHARE_FAILED:*') { throw } }
$script:shareNodes.Remove('meetingScreenShareFailure')
$script:shareNodes.Remove('screenShareAnnotation')
$script:records=@()
function Record([string]$Name,[string]$Phase) { $script:records+=$Phase }
try { Action 'share_stop' { Assert-ShareActive; Invoke 'meetingShareScreen' }; throw 'Stopped a failed share' }
catch { if ($_.Exception.Message -ne 'SCREEN_SHARE_LOST_DURING_ACTIVE_WINDOW') { throw } }
Assert ($script:records.Count -eq 1 -and $script:records[0] -eq 'requested') 'Lost share recorded successful observation'
Assert ($script:invocations -eq 1) 'Lost share submitted another command'
Write-Output 'PASS: identity, role, Pattern, single-invocation recovery, active-share projection and failure-stop contracts'
