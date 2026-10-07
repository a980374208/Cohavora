# Exercise the real driver functions without starting the product or touching
# the desktop. Script properties model UIA Current values changing on teardown.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$path = Join-Path $PSScriptRoot 'product_desktop.ps1'
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Driver parse error' }
foreach ($name in @('Find-Node', 'Test-MeetingWindowClosed', 'Wait-Top', 'Require-Pattern', 'Invoke', 'Share-State', 'Assert-ShareActive', 'Action')) {
    $definition = $ast.Find({param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $true)
    if (!$definition) { throw "Missing driver function: $name" }
    Invoke-Expression $definition.Extent.Text
}
$script:child = [pscustomobject]@{Id=123;HasExited=$false}
$script:nodes = @()
function Get-Nodes([switch]$TopLevel, [switch]$WindowsOnly) {
    if (!$TopLevel -and !$WindowsOnly) { ++$script:fullTreeCalls }
    if ($script:missingLookups -gt 0) { --$script:missingLookups; return @() }
    if ($script:useDiscoverySnapshot) { return $script:discoverySnapshot }
    if ($WindowsOnly) {return @($script:nodes | Where-Object {$_.Current.ControlType -eq [Windows.Automation.ControlType]::Window})}
    $script:nodes
}
function Get-ProcessRoots {
    foreach ($node in $script:nodes) { [pscustomobject]@{Current=$node.Current;Element=$node} }
}
function Get-LiveNode([string]$Id, $Role, [switch]$Optional) {
    # Resolve live identity independently of the discovery snapshot.
    $script:resolvedId=$Id
    $matches=@($script:nodes | Where-Object {
        $_.State.Id -eq $Id -and (!$_.State.Vanish -or $_.State.Reads -le 2)
    })
    if ($Optional -and !$matches.Count) { return $null }
    if ($matches.Count -ne 1) { throw "CONTROL_COUNT: $Id=$($matches.Count) pid=$($script:child.Id)" }
    $c=$matches[0].Current
    if ($Optional -and $c.IsOffscreen) { return $null }
    if ($c.ProcessId -ne $script:child.Id -or $c.ControlType -ne $Role -or
        [string]::IsNullOrWhiteSpace($c.Name) -or $c.IsOffscreen) { throw "CONTROL_CONTRACT: $Id changed after discovery" }
    return $matches[0]
}
function Wait-For([string]$Description, [scriptblock]$Condition, [int]$Seconds) {
    for ($i=0; $i -lt 2; ++$i) { $result=& $Condition; if ($result) { return $result } }
    throw "TIMEOUT: $Description"
}
function Assert($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
function Make-Node([string]$Id, $Role, [switch]$Vanish) {
    $state = [pscustomobject]@{Id=$Id;Reads=0;Vanish=[bool]$Vanish;PatternAvailable=$true;ToggleState=[Windows.Automation.ToggleState]::On}
    $current = [pscustomobject]@{Name='accessible';ControlType=$Role;ProcessId=123;IsOffscreen=$false;IsEnabled=$true;State=$state}
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
$script:fullTreeCalls=0
Assert ($null -ne (Wait-Top 'MeetingRoomWindow')) 'Top-level lookup lost the captured identity'
Assert ($script:fullTreeCalls -eq 0) 'Top-level wait must not enumerate descendants'
Assert ($script:nodes[0].State.Reads -eq 1) 'Top-level identity was reread'
$script:nodes = @(Make-Node 'meetingLeave' $button)
Assert ($null -ne (Find-Node 'meetingLeave' $button)) 'Exact ID did not match'
$script:nodes = @(Make-Node 'unrelatedmeetingLeave' $button)
Assert ($null -eq (Find-Node 'meetingLeave' $button -Optional)) 'Suffix without separator must not match'
$script:nodes = @(Make-Node 'screenShareAnnotation' $button)
$script:missingLookups=1
Assert ($null -ne (Find-Node 'screenShareAnnotation' $button -Optional)) 'A live annotation control was lost by an empty discovery snapshot'
$cached=Make-Node 'screenShareAnnotation' $button
$cached.Current.IsOffscreen=$true
$script:discoverySnapshot=@($cached)
$script:useDiscoverySnapshot=$true
Assert ($null -ne (Find-Node 'screenShareAnnotation' $button -Optional)) 'Stale offscreen discovery hid a visible live control'
$script:nodes[0].Current.IsOffscreen=$true
Assert ($null -eq (Find-Node 'screenShareAnnotation' $button -Optional)) 'A live hidden annotation control was accepted'
$script:nodes=@()
Assert ($null -eq (Find-Node 'screenShareAnnotation' $button -Optional)) 'A removed live annotation control was accepted'
$script:useDiscoverySnapshot=$false
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
$room=Make-Node 'MeetingRoomWindow' $window
$homeControl=Make-Node 'mainJoinMeeting' $button
$script:nodes=@($room,$homeControl)
Assert (!(Test-MeetingWindowClosed)) 'A visible room cannot be closed'
$room.Current.Name=''
Assert (!(Test-MeetingWindowClosed)) 'A tearing-down provider is still a present room'
$room.Current.IsOffscreen=$true
Assert (!(Test-MeetingWindowClosed)) 'A hidden room cannot prove destruction'
$script:nodes=@($homeControl)
Assert (Test-MeetingWindowClosed) 'Closed room and actionable home must complete the leave observation'
$homeControl.Current.IsEnabled=$false
Assert (!(Test-MeetingWindowClosed)) 'A disabled home cannot complete leave'
$homeControl.Current.IsEnabled=$true
$homeControl.Current.IsOffscreen=$true
Assert (!(Test-MeetingWindowClosed)) 'An offscreen home cannot complete leave'
$homeControl.Current.IsOffscreen=$false
$script:nodes=@()
Assert (!(Test-MeetingWindowClosed)) 'An empty tree cannot complete leave'
$script:nodes=@((Make-Node 'MeetingRoomWindow' $window),(Make-Node 'owner.MeetingRoomWindow' $window),$homeControl)
try {$null=Test-MeetingWindowClosed; throw 'Duplicate room was accepted'}
catch {if ($_.Exception.Message -notlike 'CONTROL_COUNT:*') {throw}}
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
$definition=$ast.Find({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Get-Nodes'},$true)
Invoke-Expression $definition.Extent.Text
function Get-ProcessRoots {
    $element=[pscustomobject]@{Current=[pscustomobject]@{AutomationId='fixture'}}
    $element | Add-Member ScriptMethod FindAll { throw 'DESCENDANT_SCAN_FORBIDDEN' }
    [pscustomobject]@{Current=$element.Current;Element=$element}
}
Assert (@(Get-Nodes -TopLevel).Count -eq 1) 'Cache-only window lookup entered descendants'
Assert (@(Get-Nodes -TopLevel -Live).Count -eq 1) 'Live window lookup entered descendants'
Write-Output 'PASS: identity, role, Pattern, single-invocation recovery, window-closure observation, active-share projection and failure-stop contracts'
