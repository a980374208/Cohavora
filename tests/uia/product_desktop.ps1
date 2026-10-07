param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [int]$DedicatedDesktopSessionId = 0,
    [int]$Cycles = 100,
    [int]$MinimumSeconds = 28800,
    [int]$ShareSeconds = 60,
    [int]$LogPairSeconds = 30,
    [int]$StopSettleSeconds = 10,
    [int]$RoomSettleSeconds = 10,
    [ValidatePattern('^[0-9a-f]{32}$')][string]$RunId = [Guid]::NewGuid().ToString('N'),
    [switch]$ProbeOnly,
    [switch]$Pilot,
    [switch]$HeapDiagnostic,
    [switch]$HeapSnapshotDiagnostic,
    [switch]$HeapDiagnosticPersistentUia,
    [switch]$HeapDiagnosticNoShare,
    [switch]$HeapCheckOnly,
    [switch]$HeapDiagnosticNoExport,
    [switch]$HeapPageCheck,
    [switch]$CrashDiagnostic,
    [switch]$IsolateUiaCycles,
    [switch]$Retest,
    [switch]$RetestSmoke,
    [int]$SteadySeconds = 1800,
    [int]$MixedSeconds = 7200
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'product_desktop_evidence.ps1')
. (Join-Path $PSScriptRoot '../runtime/tools/product_acceptance/product_pilot_probe_tail.ps1')
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class ProductDesktop {
    [DllImport("user32.dll")] static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern bool GetUserObjectInformation(IntPtr h, int index, StringBuilder value, int length, out int needed);
    [DllImport("user32.dll")] static extern bool CloseDesktop(IntPtr h);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool GetExitCodeProcess(IntPtr h, out uint code);
    public static uint ExitCode(IntPtr handle) {
        uint code;
        if (!GetExitCodeProcess(handle, out code))
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        return code;
    }
    public static bool Unlocked() {
        var h = OpenInputDesktop(0, false, 1);
        if (h == IntPtr.Zero) return false;
        try { var name = new StringBuilder(256); int needed;
            return GetUserObjectInformation(h, 2, name, 512, out needed) && name.ToString() == "Default";
        } finally { CloseDesktop(h); }
    }
}
'@
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Output directory must be new' }
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$null = New-Item -ItemType Directory -Path $OutputDirectory
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$script:runId = $RunId
$script:cycle = 0
$script:completed = 0
$script:child = $null
$script:rootCache = @{}
$script:step = 'prerequisites'
$script:operation = ''
$script:cycleId = ''
$script:nativeProcessRun = $null
$script:nativeSession = $null
$script:participantHash = $null
$script:lastResourceSample = [DateTime]::MinValue
$script:layout = 'unknown'
$started = [DateTime]::UtcNow
$script:runClock = [Diagnostics.Stopwatch]::StartNew()
$oldAppData = $env:APPDATA
$oldLocalAppData = $env:LOCALAPPDATA
$oldQtPlatform = $env:QT_QPA_PLATFORM
$oldQtAccessibility = $env:QT_ACCESSIBILITY
$oldSettingsRoot = $env:LIVEKIT_UIA_SETTINGS_ROOT
if ($HeapDiagnosticNoShare -and !$HeapDiagnostic) { throw 'NO_SHARE_REQUIRES_HEAP_DIAGNOSTIC' }
if ($HeapSnapshotDiagnostic -and (!$HeapDiagnostic -or $HeapCheckOnly -or $HeapPageCheck -or $CrashDiagnostic)) { throw 'HEAP_SNAPSHOT_REQUIRES_EXCLUSIVE_HEAP_DIAGNOSTIC' }
if ($HeapCheckOnly -and !$HeapDiagnostic) { throw 'HEAP_CHECK_REQUIRES_DIAGNOSTIC' }
if ($HeapDiagnosticNoExport -and !$HeapDiagnostic) { throw 'NO_EXPORT_REQUIRES_HEAP_DIAGNOSTIC' }
if ($HeapPageCheck -and (!$HeapDiagnostic -or $HeapCheckOnly)) { throw 'PAGE_CHECK_REQUIRES_EXCLUSIVE_HEAP_DIAGNOSTIC' }
if ($CrashDiagnostic -and (!$HeapDiagnostic -or $HeapCheckOnly -or $HeapPageCheck)) { throw 'CRASH_DIAGNOSTIC_REQUIRES_EXCLUSIVE_MODE' }
if ($IsolateUiaCycles -and !$HeapDiagnostic) {throw 'ISOLATED_CLIENT_EXPERIMENT_REQUIRES_DIAGNOSTIC'}
if ($HeapDiagnosticPersistentUia -and !$HeapDiagnostic) {throw 'PERSISTENT_CLIENT_REQUIRES_HEAP_DIAGNOSTIC'}
if ($HeapDiagnosticPersistentUia -and $IsolateUiaCycles) {throw 'HEAP_UIA_PROFILES_CONFLICT'}
if ($HeapDiagnostic) {
    # Isolate clients by default. Retaining a client is an explicit diagnostic
    # profile for reproducing target-side UIA reference retention.
    $IsolateUiaCycles = !$HeapDiagnosticPersistentUia
    if (!$Pilot -or $ProbeOnly) { throw 'HEAP_DIAGNOSTIC_REQUIRES_PILOT' }
    . (Join-Path $PSScriptRoot '../runtime/tools/product_acceptance/product_heap_diagnostic.ps1')
}
function Record([string]$Action, [string]$Phase) {
    if ($env:LIVEKIT_UIA_REMOTE_CONTEXT -eq '1') { Sync-ObserverContext $Action $Phase }
    $row = [ordered]@{schema=1; run_id=$script:runId; cycle=$script:cycle;
        cycle_id=$script:cycleId;process_run_id=$script:nativeProcessRun;
        anonymous_session_id=$script:nativeSession;
        participant_sha256=$script:participantHash;
        operation_id=$script:operation; action=$Action; phase=$Phase;
        pid=$(if ($script:child) { $script:child.Id } else { $null }); utc=[DateTime]::UtcNow.ToString('o');
        elapsed_seconds=$(if ($script:runClock) {$script:runClock.Elapsed.TotalSeconds} else {$null})}
    [IO.File]::AppendAllText((Join-Path $OutputDirectory 'uia-actions.jsonl'),
        (($row | ConvertTo-Json -Compress) + "`n"), [Text.UTF8Encoding]::new($false))
}
function Sync-ObserverContext([string]$Action, [string]$Phase) {
    $probes = @(Read-ProductPilotProbeTail -Path $env:LIVEKIT_UIA_PILOT_PROBE -Count 3 -MaximumBytes 1048576 | ForEach-Object {
        try { $_ | ConvertFrom-Json } catch { }
    })
    if (!$probes.Count) { throw 'NATIVE_CONTEXT_MISSING' }
    $probe = $probes[-1]
    if ($probe.run_id -ne $script:runId) { throw 'NATIVE_CONTEXT_RUN_MISMATCH' }
    $script:nativeProcessRun = $probe.process_run_id
    $script:participantHash = $probe.participant_sha256
    if ($Action -eq 'join' -and $Phase -eq 'uia_observed') {
        if (!$probe.anonymous_session_id) { throw 'NATIVE_SESSION_CONTEXT_MISSING' }
        $script:nativeSession = $probe.anonymous_session_id
    }
    $context = [ordered]@{run_id=$script:runId;cycle=$script:cycle;cycle_id=$script:cycleId;
        operation_id=$script:operation;action=$Action;phase=$Phase;pid=$script:child.Id;
        process_run_id=$script:nativeProcessRun;anonymous_session_id=$script:nativeSession;
        participant_sha256=$script:participantHash}
    $path = Join-Path $OutputDirectory 'current-operation.json'
    $context | ConvertTo-Json -Compress | Set-Content ($path+'.tmp') -Encoding UTF8
    Move-Item -LiteralPath ($path+'.tmp') -Destination $path -Force
    & python (Join-Path $PSScriptRoot '../runtime/tools/product_acceptance/product_pilot_context.py') --file $path
    if ($LASTEXITCODE -ne 0) { throw 'REMOTE_CONTEXT_FENCE_FAILED' }
}
function Save-Result([string]$Verdict, [string]$Reason) {
    [ordered]@{schema=1; run_id=$script:runId; verdict=$Verdict; reason=$Reason;
        cycles_requested=$Cycles; cycles_completed=$script:completed;
        minimum_seconds=$MinimumSeconds; started_utc=$started.ToString('o');
        finished_utc=[DateTime]::UtcNow.ToString('o'); ui_only=$true;
        retest=[bool]$Retest; smoke=[bool]$RetestSmoke} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'uia-result.json') -Encoding UTF8
}
function Sample-Resource([string]$Phase) {
    if (!$script:child) { return }
    $now = [DateTime]::UtcNow
    if ($Phase -eq 'active' -and ($now - $script:lastResourceSample).TotalSeconds -lt 1) { return }
    if ($script:desktopBaseline) {
        $state=Get-DesktopEvidenceState
        $state | ConvertTo-Json -Compress | Add-Content (Join-Path $OutputDirectory 'desktop-observations.jsonl') -Encoding UTF8
        $null=Assert-DedicatedDesktop $DedicatedDesktopSessionId $script:desktopBaseline
    }
    $script:lastResourceSample = $now
    $process = Get-Process -Id $script:child.Id -ErrorAction SilentlyContinue
    $row = [ordered]@{schema=1; run_id=$script:runId; cycle=$script:cycle;
        pid=$script:child.Id; operation_id=$script:operation; utc=$now.ToString('o');
        phase=$Phase; layout=$script:layout; process_alive=($null -ne $process);
        private_bytes=$(if ($process) { $process.PrivateMemorySize64 } else { 0 });
        handles=$(if ($process) { $process.HandleCount } else { 0 });
        threads=$(if ($process) { $process.Threads.Count } else { 0 });
        gpu_local_bytes=$null; gpu_nonlocal_bytes=$null; queue_depth=$null}
    [IO.File]::AppendAllText((Join-Path $OutputDirectory 'uia-resources.jsonl'),
        (($row | ConvertTo-Json -Compress) + "`n"), [Text.UTF8Encoding]::new($false))
}
function New-TreeCacheRequest {
    $request = [Windows.Automation.CacheRequest]::new()
    $request.AutomationElementMode = [Windows.Automation.AutomationElementMode]::None
    $request.TreeScope = [Windows.Automation.TreeScope]::Element
    foreach ($property in @('AutomationId','ControlType','Name','ProcessId','IsEnabled','IsOffscreen','NativeWindowHandle','RuntimeId')) {
        $request.Add([Windows.Automation.AutomationElement]::("${property}Property"))
    }
    return $request
}
function Get-ProcessRoots {
    $condition = [Windows.Automation.PropertyCondition]::new(
        [Windows.Automation.AutomationElement]::ProcessIdProperty, [int]$script:child.Id)
    # Obtain the desktop handle before activating cache-only mode. No other
    # process's descendants are traversed.
    $desktop = [Windows.Automation.AutomationElement]::RootElement
    $scope = (New-TreeCacheRequest).Activate()
    try { $roots = $desktop.FindAll([Windows.Automation.TreeScope]::Children, $condition) }
    finally { $scope.Dispose() }
    $next = @{}
    foreach ($root in $roots) {
        $info = $root.Cached
        $runtimeId = $root.GetCachedPropertyValue([Windows.Automation.AutomationElement]::RuntimeIdProperty) -join '.'
        $key = "$($script:child.Id):$($info.NativeWindowHandle):$runtimeId"
        if (!$info.NativeWindowHandle) { throw 'UIA_ROOT_WINDOW_HANDLE_MISSING' }
        $live = $script:rootCache[$key]
        if (!$live) {
            $live = [Windows.Automation.AutomationElement]::FromHandle([IntPtr]$info.NativeWindowHandle)
        }
        if ($live.Current.ProcessId -ne $script:child.Id) { throw 'UIA_ROOT_PROCESS_CHANGED' }
        $next[$key] = $live
        [pscustomobject]@{Element=$live;Current=$info}
    }
    $script:rootCache = $next
}
function Get-Nodes([switch]$Live, [switch]$TopLevel, [switch]$WindowsOnly) {
    if (!$script:child) { throw "Product missing during $script:step" }
    if ($script:child.HasExited) {
        throw "PROCESS_EXIT: $script:step code=$($script:child.ExitCode)"
    }
    $condition = New-Object Windows.Automation.PropertyCondition(
        [Windows.Automation.AutomationElement]::ProcessIdProperty, [int]$script:child.Id)
    if ($WindowsOnly) {
        $condition = [Windows.Automation.AndCondition]::new($condition,
            [Windows.Automation.PropertyCondition]::new(
                [Windows.Automation.AutomationElement]::ControlTypeProperty,
                [Windows.Automation.ControlType]::Window))
    }
    for ($attempt = 0; $attempt -lt 10; ++$attempt) {
        try {
            $nodes = foreach ($root in Get-ProcessRoots) {
                if ($Live) {
                    if (!$WindowsOnly -or $root.Current.ControlType -eq [Windows.Automation.ControlType]::Window) {$root.Element}
                    if ($TopLevel) { continue }
                    $root.Element.FindAll([Windows.Automation.TreeScope]::Descendants, $condition)
                } else {
                    # Full FindAll results create native UiaNode references in
                    # the product that survive managed client GC. Tree-only
                    # discovery needs immutable property data, not live nodes.
                    if (!$WindowsOnly -or $root.Current.ControlType -eq [Windows.Automation.ControlType]::Window) {
                        [pscustomobject]@{Current=$root.Current}
                    }
                    if ($TopLevel) { continue }
                    $scope = (New-TreeCacheRequest).Activate()
                    try { $children = $root.Element.FindAll([Windows.Automation.TreeScope]::Descendants, $condition) }
                    finally { $scope.Dispose() }
                    foreach ($node in $children) { [pscustomobject]@{Current=$node.Cached} }
                }
            }
            return @($nodes)
        } catch {
            if ($script:child.HasExited) {
                throw "PROCESS_EXIT: $script:step code=$($script:child.ExitCode)"
            }
            if ($attempt -eq 9) {
                throw "UIA_TREE_UNAVAILABLE: pid=$($script:child.Id) $($_.Exception.Message)"
            }
            Start-Sleep -Milliseconds 200
        }
    }
}
function Get-SaveTreePatternMap {
    # Preserve the Framework GetSupportedPatterns table order, but record
    # Boolean capability snapshots without acquiring live pattern objects.
    $names=@('Invoke','Selection','Value','RangeValue','Scroll','ExpandCollapse',
        'Grid','GridItem','MultipleView','Window','SelectionItem','Dock','Table',
        'TableItem','Text','Toggle','Transform','ScrollItem','SynchronizedInput',
        'VirtualizedItem','ItemContainer')
    foreach($name in $names){
        $type=[Windows.Automation.AutomationElement].Assembly.GetType('System.Windows.Automation.'+$name+'Pattern',$true)
        $pattern=$type.GetField('Pattern',[Reflection.BindingFlags]'Public,Static').GetValue($null)
        $availability=[Windows.Automation.AutomationElement]::('Is'+$name+'PatternAvailableProperty')
        [pscustomobject]@{Property=$availability;Pattern=$pattern}
    }
}
function New-SaveTreeCacheRequest($PatternMap) {
    $request=[Windows.Automation.CacheRequest]::new()
    $request.AutomationElementMode=[Windows.Automation.AutomationElementMode]::None
    $request.TreeScope=[Windows.Automation.TreeScope]::Element
    foreach($name in @('AutomationId','ControlType','Name','ProcessId','IsEnabled','IsOffscreen')){
        $request.Add([Windows.Automation.AutomationElement]::($name+'Property'))
    }
    foreach($entry in $PatternMap){$request.Add($entry.Property)}
    return $request
}
function Get-SaveTreeNodes($PatternMap) {
    $condition=[Windows.Automation.PropertyCondition]::new(
        [Windows.Automation.AutomationElement]::ProcessIdProperty,[int]$script:child.Id)
    # Match Get-Nodes' bounded acquisition retry, rediscovering roots after
    # a Qt dialog disappears. Failed acquisition never becomes an empty tree.
    for($attempt=0;$attempt -lt 10;++$attempt){
        try {
            $nodes=@(foreach($root in Get-ProcessRoots){
                $request=New-SaveTreeCacheRequest $PatternMap
                $scope=$request.Activate()
                try {
                    $snapshot=$root.Element.GetUpdatedCache($request)
                    $descendants=$root.Element.FindAll([Windows.Automation.TreeScope]::Descendants,$condition)
                }finally{$scope.Dispose()}
                $snapshot
                foreach($node in $descendants){$node}
            })
            return $nodes
        }catch{
            if($script:child.HasExited){
                throw "PROCESS_EXIT: $script:step code=$($script:child.ExitCode)"
            }
            if($attempt -eq 9){
                throw "UIA_TREE_UNAVAILABLE: pid=$($script:child.Id) $($_.Exception.Message)"
            }
            Start-Sleep -Milliseconds 200
        }
    }
}
function Convert-CachedSaveTreeRow($Node,$PatternMap) {
    $c=$Node.Cached
    $patterns=@(foreach($entry in $PatternMap){
        $available=$Node.GetCachedPropertyValue($entry.Property)
        if($available -isnot [bool]){throw 'SAVE_TREE_PATTERN_AVAILABILITY_INVALID'}
        if($available){$entry.Pattern.ProgrammaticName}
    })
    return [ordered]@{id=$c.AutomationId;role=$c.ControlType.ProgrammaticName;
        name_present=![string]::IsNullOrWhiteSpace($c.Name);pid=$c.ProcessId;
        enabled=$c.IsEnabled;offscreen=$c.IsOffscreen;patterns=$patterns}
}
function Save-Tree([string]$Name) {
    if (!$script:child -or $script:child.HasExited) { return }
    $map=@(Get-SaveTreePatternMap)
    $rows = foreach ($node in Get-SaveTreeNodes $map) {
        try {
            Convert-CachedSaveTreeRow $node $map
        } catch [Windows.Automation.ElementNotAvailableException] { }
    }
    ConvertTo-Json -InputObject @($rows) -Depth 4 |
        Set-Content -LiteralPath (Join-Path $OutputDirectory "$Name.json") -Encoding UTF8
}
function Get-LiveNode([string]$Id, [Windows.Automation.ControlType]$Role, [switch]$Optional) {
    $condition = [Windows.Automation.AndCondition]::new(
        [Windows.Automation.PropertyCondition]::new([Windows.Automation.AutomationElement]::ProcessIdProperty,[int]$script:child.Id),
        [Windows.Automation.PropertyCondition]::new([Windows.Automation.AutomationElement]::AutomationIdProperty,$Id))
    $matches = @(foreach ($root in Get-ProcessRoots) {
        if ($root.Current.AutomationId -eq $Id) { $root.Element }
        # A parented Qt dialog is a Window in the accessibility subtree even
        # when it is not exposed as a direct desktop child.
        $root.Element.FindAll([Windows.Automation.TreeScope]::Descendants,$condition)
    })
    if ($Optional -and !$matches.Count) { return $null }
    if ($matches.Count -ne 1) { throw "CONTROL_COUNT: $Id=$($matches.Count) pid=$($script:child.Id)" }
    $c = $matches[0].Current
    if ($Optional -and $c.IsOffscreen) { return $null }
    if ($c.ProcessId -ne $script:child.Id -or $c.ControlType -ne $Role -or
        [string]::IsNullOrWhiteSpace($c.Name) -or $c.IsOffscreen) { throw "CONTROL_CONTRACT: $Id changed after discovery" }
    return $matches[0]
}
function Find-Node([string]$Id, [Windows.Automation.ControlType]$Role, [switch]$Optional) {
    $script:step = "discover $Id"
    # Query only Window-role descendants, including parented Qt dialogs. Keep
    # discovery cache-only rather than retaining a full set of live controls.
    $candidates = if ($Role -eq [Windows.Automation.ControlType]::Window) {
        @(Get-Nodes -WindowsOnly)
    } else { @(Get-Nodes) }
    $matches = @(foreach ($node in $candidates) {
        try {
            $c = $node.Current
            $automationId = [string]$c.AutomationId
            if ($automationId -eq $Id -or $automationId.EndsWith(".$Id", [StringComparison]::Ordinal)) {
                [pscustomobject]@{Current=$c;Id=$automationId}
            }
        } catch [Windows.Automation.ElementNotAvailableException] { }
    })
    # A cache-only enumeration and a live provider query can straddle a Qt
    # layout/visibility update. Confirm absence against the live provider;
    # Get-LiveNode still enforces unique identity, role and visible state.
    if ($Optional -and $matches.Count -eq 0) { return Get-LiveNode $Id $Role -Optional }
    if ($matches.Count -ne 1) { throw "CONTROL_COUNT: $Id=$($matches.Count) pid=$($script:child.Id)" }
    $c = $matches[0].Current
    if ($Optional -and $c.IsOffscreen) { return Get-LiveNode $matches[0].Id $Role -Optional }
    if ($c.ControlType -ne $Role -or [string]::IsNullOrWhiteSpace($c.Name) -or $c.IsOffscreen) {
        throw "CONTROL_CONTRACT: $Id role=$($c.ControlType.ProgrammaticName) enabled=$($c.IsEnabled) offscreen=$($c.IsOffscreen)"
    }
    return Get-LiveNode $matches[0].Id $Role -Optional:$Optional
}
function Test-MeetingWindowClosed {
    # Closure is a window-state observation, not another action on a button
    # whose provider is being destroyed. Keep all actionable-node checks.
    $windows = @(foreach ($node in Get-Nodes -WindowsOnly) {
        $info = $node.Current
        $windowId = [string]$info.AutomationId
        if ($windowId -eq 'MeetingRoomWindow' -or $windowId.EndsWith('.MeetingRoomWindow',[StringComparison]::Ordinal)) {
            if ($info.ProcessId -ne $script:child.Id -or $info.ControlType -ne [Windows.Automation.ControlType]::Window) {
                throw 'CONTROL_CONTRACT: meeting window identity changed'
            }
            $node
        }
    })
    if ($windows.Count -gt 1) {throw 'CONTROL_COUNT: MeetingRoomWindow duplicate'}
    if ($windows.Count) {return $false}
    $homeControl = Find-Node 'mainJoinMeeting' ([Windows.Automation.ControlType]::Button) -Optional
    return $null -ne $homeControl -and $homeControl.Current.IsEnabled
}
function Wait-For([string]$Description, [scriptblock]$Condition, [int]$Seconds = 45) {
    $script:step = $Description
    $deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    do {
        if ($script:child.HasExited) { throw "PROCESS_EXIT: $Description code=$($script:child.ExitCode)" }
        if (!$ProbeOnly) {
            try {
                $departure = @(Get-ProcessRoots | Where-Object {
                    $departureAutomationId=[string]$_.Current.AutomationId
                    $departureAutomationId -eq 'meetingDepartureNotice' -or $departureAutomationId.EndsWith('.meetingDepartureNotice',[StringComparison]::Ordinal) })
            } catch {
                if ($script:child.HasExited) {
                    throw "PROCESS_EXIT: $Description code=$($script:child.ExitCode)"
                }
                Start-Sleep -Milliseconds 200
                continue
            }
            if ($departure) {
                throw "MEETING_DEPARTURE_NOTICE: $Description"
            }
        }
        Sample-Resource 'active'
        try { $value = & $Condition; if ($value) { return $value } }
        catch [Windows.Automation.ElementNotAvailableException] { }
        Start-Sleep -Seconds 1
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "TIMEOUT: $Description"
}
function Require-Pattern($Node, [Windows.Automation.AutomationPattern]$Id) {
    $pattern = $null
    if (!$Node.TryGetCurrentPattern($Id, [ref]$pattern)) {
        throw "MISSING_PATTERN: $($Node.Current.AutomationId) $($Id.ProgrammaticName)"
    }
    return $pattern
}
function Invoke([string]$Id, [Windows.Automation.ControlType]$Role = [Windows.Automation.ControlType]::Button) {
    # A layout update can briefly remove a provider. Retry discovery only;
    # never repeat an action whose submission may already have succeeded.
    $node = Wait-For "$Id available for invoke" { Find-Node $Id $Role -Optional } 15
    if (!$node.Current.IsEnabled) { throw "DISABLED_CONTROL: $Id" }
    (Require-Pattern $node ([Windows.Automation.InvokePattern]::Pattern)).Invoke()
}
function Set-Value([string]$Id, [string]$Value) {
    $pattern = Require-Pattern (Find-Node $Id ([Windows.Automation.ControlType]::Edit)) ([Windows.Automation.ValuePattern]::Pattern)
    if ($pattern.Current.IsReadOnly) { throw "READ_ONLY: $Id" }
    $pattern.SetValue($Value)
}
function Toggle-To([string]$Id, [Windows.Automation.ToggleState]$Expected) {
    $node = Wait-For "$Id available" {
        Find-Node $Id ([Windows.Automation.ControlType]::CheckBox) -Optional
    } 15
    $pattern = Require-Pattern $node ([Windows.Automation.TogglePattern]::Pattern)
    if ($pattern.Current.ToggleState -ne $Expected) { $pattern.Toggle() }
    $null = Wait-For "$Id toggle=$Expected" {
        if ($Id -eq 'meetingMicrophone' -and $Expected -eq [Windows.Automation.ToggleState]::On) {
            $unavailable = Find-Node 'meetingMicrophoneUnavailable' ([Windows.Automation.ControlType]::Window) -Optional
            if ($unavailable) {
                Save-Tree ('cycle-{0:d4}-microphone-unavailable' -f $script:cycle)
                Invoke 'meetingMicrophoneUnavailableDismiss'
                [ordered]@{run_id=$script:runId;cycle=$script:cycle;operation_id=$script:operation;
                    pid=$script:child.Id;utc=[DateTime]::UtcNow.ToString('o');device='microphone';
                    result='DEFERRED';reason='product_reported_no_capture_endpoint'} |
                    ConvertTo-Json -Compress | Add-Content (Join-Path $OutputDirectory 'uia-device-outcomes.jsonl') -Encoding UTF8
                return $true # unavailable outcome is separately checked against OS evidence
            }
        }
        $control = Find-Node $Id ([Windows.Automation.ControlType]::CheckBox) -Optional
        if (!$control) { return $false }
        (Require-Pattern $control ([Windows.Automation.TogglePattern]::Pattern)).Current.ToggleState -eq $Expected
    }
}
function Action([string]$Name, [scriptblock]$Body) {
    $script:operation = [Guid]::NewGuid().ToString('N')
    Record $Name 'requested'
    & $Body
    Record $Name 'uia_observed'
}
function Wait-Top([string]$Id) {
    return Wait-For "$Id window" {
        $window = Find-Node $Id ([Windows.Automation.ControlType]::Window) -Optional
        if ($window) {
            $null = Require-Pattern $window ([Windows.Automation.WindowPattern]::Pattern)
            $window
        }
    }
}
function Select-FirstShareSource {
    $combo = Find-Node 'screenShareSource' ([Windows.Automation.ControlType]::ComboBox)
    $list = Wait-For 'share source list' {
        $lists = @($combo.FindAll([Windows.Automation.TreeScope]::Descendants,
            [Windows.Automation.Condition]::TrueCondition) | Where-Object {
                $_.Current.ControlType -eq [Windows.Automation.ControlType]::List })
        if ($lists.Count -eq 1) { $lists[0] }
    }
    $items = @($list.FindAll([Windows.Automation.TreeScope]::Children,
        [Windows.Automation.Condition]::TrueCondition) | Where-Object {
            $_.Current.ControlType -eq [Windows.Automation.ControlType]::ListItem })
    if ($items.Count -lt 1) { throw 'NO_SHARE_SOURCE' }
    $selectedName = $items[0].Current.Name
    $selection = Require-Pattern $items[0] ([Windows.Automation.SelectionItemPattern]::Pattern)
    if (!$selection.Current.IsSelected) { $selection.Select() }
    $null = Wait-For 'share source committed' {
        $current = Find-Node 'screenShareSource' ([Windows.Automation.ControlType]::ComboBox) -Optional
        if (!$current) { return $false }
        $value = Require-Pattern $current ([Windows.Automation.ValuePattern]::Pattern)
        $value.Current.Value -eq $selectedName
    }
    Invoke 'screenShareAccept'
}
function Share-State([Windows.Automation.ToggleState]$Expected) {
    if (Find-Node 'meetingScreenShareFailure' ([Windows.Automation.ControlType]::Window) -Optional) {
        throw 'SCREEN_SHARE_FAILED: named product error dialog observed'
    }
    $node = Find-Node 'meetingShareScreen' ([Windows.Automation.ControlType]::CheckBox) -Optional
    if (!$node) { return $false }
    $stateMatches = (Require-Pattern $node ([Windows.Automation.TogglePattern]::Pattern)).Current.ToggleState -eq $Expected
    if ($Expected -eq [Windows.Automation.ToggleState]::On) {
        # Toggle On also represents Starting/StopFailed. The annotation control
        # is visible only after the native projection reaches Active.
        return $stateMatches -and ($null -ne (Find-Node 'screenShareAnnotation' ([Windows.Automation.ControlType]::Button) -Optional))
    }
    return $stateMatches
}
function Assert-ShareActive {
    if (!(Share-State ([Windows.Automation.ToggleState]::On))) { throw 'SCREEN_SHARE_LOST_DURING_ACTIVE_WINDOW' }
}
function Start-Product {
    $profile = Join-Path $OutputDirectory 'profile'
    $env:APPDATA = Join-Path $profile 'Roaming'
    $env:LOCALAPPDATA = Join-Path $profile 'Local'
    $env:LIVEKIT_UIA_SETTINGS_ROOT = Join-Path $profile 'Settings'
    $null = New-Item -ItemType Directory -Force -Path $env:APPDATA,$env:LOCALAPPDATA
    $env:QT_QPA_PLATFORM = 'windows'
    $env:QT_ACCESSIBILITY = '1'
    if ($HeapDiagnostic) {
        Start-HeapDiagnosticProduct
    } else {
        $script:child = Start-Process -FilePath $Executable -ArgumentList '--debug' `
        -WorkingDirectory (Split-Path $Executable) -PassThru `
        -RedirectStandardOutput (Join-Path $OutputDirectory 'product.stdout.log') `
        -RedirectStandardError (Join-Path $OutputDirectory 'product.stderr.log')
    }
    # Retain the native handle before exit. Windows PowerShell's Start-Process
    # wrapper can otherwise return a null ExitCode even after WaitForExit.
    $script:childHandle = $script:child.Handle
    # Publish identity before any UIA call: even first-window discovery can hang.
    @{run_id=$script:runId;pid=$script:child.Id;executable=$Executable;
        start_ticks=$script:child.StartTime.ToUniversalTime().Ticks} | ConvertTo-Json |
        Set-Content (Join-Path $OutputDirectory 'product-identity.json.tmp') -Encoding UTF8
    Move-Item (Join-Path $OutputDirectory 'product-identity.json.tmp') (Join-Path $OutputDirectory 'product-identity.json')
    if ($HeapDiagnostic) { Wait-HeapDiagnosticHistory }
    $null = Wait-For 'product login or main window' {
            (Find-Node 'mainJoinMeeting' ([Windows.Automation.ControlType]::Button) -Optional) -or
            (Find-Node 'loginAccount' ([Windows.Automation.ControlType]::Edit) -Optional)
        }
        Save-Tree ("cycle-{0:d4}-initial" -f $script:cycle)
        if ($ProbeOnly) {
            $tabs = @(Get-Nodes -Live | Where-Object {
                $_.Current.ControlType -eq [Windows.Automation.ControlType]::TabItem })
            if ($tabs.Count -ne 3) { throw "GUEST_TAB_COUNT: $($tabs.Count)" }
            (Require-Pattern $tabs[2] ([Windows.Automation.InvokePattern]::Pattern)).Invoke()
            Invoke 'guestBtn'
            $null = Wait-For 'guest main window' {
                Find-Node 'mainJoinMeeting' ([Windows.Automation.ControlType]::Button) -Optional
            }
            Save-Tree 'probe-main'
            Invoke-AccountTelemetry -Seconds 5
            $null = Wait-Top 'telemetryPostMeetingDialog'
            Save-Tree 'probe-telemetry-dialog'
            Invoke 'telemetryPostClose'
            return
        }
        if (!(Find-Node 'mainJoinMeeting' ([Windows.Automation.ControlType]::Button) -Optional)) {
            Action 'login' {
                Set-Value 'loginAccount' $env:LIVEKIT_UIA_ACCOUNT
                Set-Value 'loginPassword' $env:LIVEKIT_UIA_PASSWORD
                if ($env:LIVEKIT_UIA_SERVICE_URL) {
                    if (!(Find-Node 'serverBaseUrl' ([Windows.Automation.ControlType]::Edit) -Optional)) {
                        Invoke 'serverSettingsToggle'
                    }
                    $null = Wait-For 'server settings field' {
                        Find-Node 'serverBaseUrl' ([Windows.Automation.ControlType]::Edit) -Optional
                    }
                    Set-Value 'serverBaseUrl' $env:LIVEKIT_UIA_SERVICE_URL
                }
                Invoke 'primaryBtn'
                $null = Wait-For 'main window after account login' {
                    Find-Node 'mainJoinMeeting' ([Windows.Automation.ControlType]::Button) -Optional
                } 90
            }
        }
}
function Run-Cycle {
    Action 'join' {
            Invoke 'mainJoinMeeting'
            $null = Wait-Top 'joinMeetingDialog'
            Set-Value 'joinMeetingId' $env:LIVEKIT_UIA_MEETING_ID
            Set-Value 'joinDisplayName' ("U-{0}-{1:d4}" -f $script:runId.Substring(0,8),$script:cycle)
            Toggle-To 'joinMicrophone' ([Windows.Automation.ToggleState]::On)
            Toggle-To 'joinCamera' ([Windows.Automation.ToggleState]::On)
            Invoke 'joinBtn'
            $null = Wait-Top 'MeetingRoomWindow'
            $null = Wait-For 'remote video paging controls' {
                Find-Node 'videoPageSize' ([Windows.Automation.ControlType]::ComboBox) -Optional
            } 90
            $layoutPattern = Require-Pattern `
                (Find-Node 'videoPageSize' ([Windows.Automation.ControlType]::ComboBox)) `
                ([Windows.Automation.ValuePattern]::Pattern)
            $script:layout = $layoutPattern.Current.Value
            $null = Wait-For 'meeting local startup ready' {
                $control = Find-Node 'meetingShareScreen' ([Windows.Automation.ControlType]::CheckBox) -Optional
                $control -and $control.Current.IsEnabled
            } 90
            # Meeting policy can override the join preference and publish a
            # muted microphone. Explicitly operate the product control and
            # verify its Toggle state; SFU/RTP evidence verifies delivery.
            Toggle-To 'meetingMicrophone' ([Windows.Automation.ToggleState]::On)
            Save-Tree ('cycle-{0:d4}-meeting' -f $script:cycle)
            Sample-Resource 'joined'
        }
        $retestPhase = if ($script:cycle -eq 1) {'steady'} else {'mixed'}
        if ($Retest) {
            $phaseSeconds = if ($script:cycle -eq 1) {$SteadySeconds} else {$MixedSeconds / ($Cycles - 1)}
            Record ('retest_' + $retestPhase) 'started'
            $script:retestDeadline = [Diagnostics.Stopwatch]::StartNew()
        }
        if (!$Retest -or $script:cycle -gt 1) {
        Action 'page' {
            $null = Find-Node 'videoPageIndicator' ([Windows.Automation.ControlType]::Text)
            $null = Wait-For 'first video page ready' {
                $previous = Find-Node 'previousVideoPage' ([Windows.Automation.ControlType]::Button) -Optional
                $next = Find-Node 'nextVideoPage' ([Windows.Automation.ControlType]::Button) -Optional
                $previous -and $next -and (-not $previous.Current.IsEnabled) -and $next.Current.IsEnabled
            }
            Invoke 'nextVideoPage'
            $null = Wait-For 'second video page' {
                $previous = Find-Node 'previousVideoPage' ([Windows.Automation.ControlType]::Button) -Optional
                $previous -and $previous.Current.IsEnabled
            }
            Invoke 'previousVideoPage'
            $null = Wait-For 'first video page restored' {
                $previous = Find-Node 'previousVideoPage' ([Windows.Automation.ControlType]::Button) -Optional
                $previous -and (-not $previous.Current.IsEnabled)
            }
        }
        if (!$HeapDiagnosticNoShare) {
        Action 'share_start' {
            $null = Wait-For 'share control ready after local startup' {
                $control = Find-Node 'meetingShareScreen' ([Windows.Automation.ControlType]::CheckBox) -Optional
                $control -and $control.Current.IsEnabled
            } 90
            Invoke 'meetingShareScreen' ([Windows.Automation.ControlType]::CheckBox)
            $null = Wait-Top 'screen-share-picker'
            Select-FirstShareSource
            $null = Wait-For 'share control active' { Share-State ([Windows.Automation.ToggleState]::On) } 90
            $activeUntil = [DateTime]::UtcNow.AddSeconds($ShareSeconds)
            while ([DateTime]::UtcNow -lt $activeUntil) {
                if ($script:child.HasExited) { throw 'PROCESS_EXIT_DURING_SHARE' }
                Assert-ShareActive
                Sample-Resource 'share_active'
                Start-Sleep -Seconds 1
            }
        }
        Action 'share_stop' {
            Assert-ShareActive # A failed share would turn this command into a new start.
            Invoke 'meetingShareScreen' ([Windows.Automation.ControlType]::CheckBox)
            $null = Wait-For 'share control idle' { Share-State ([Windows.Automation.ToggleState]::Off) } 90
            Sample-Resource 'share_stopped'
        }
        $stopUntil = [DateTime]::UtcNow.AddSeconds($StopSettleSeconds)
        while ([DateTime]::UtcNow -lt $stopUntil) {
            if ($script:child.HasExited) { throw 'PROCESS_EXIT_AFTER_SHARE_STOP' }
            Sample-Resource 'share_settled'
            Start-Sleep -Seconds 1
        }
        }
        Action 'logging' {
            Invoke 'meetingConsole'
            $null = Wait-Top 'meetingLogConsole'
            if ($env:LIVEKIT_UIA_LOG_PAIR -eq '1') {
                # Stop admission before persistence so events produced between
                # the two UIA calls cannot receive an unpersisted sequence.
                Toggle-To 'consoleCollectDiagnostics' ([Windows.Automation.ToggleState]::Off)
            }
            Toggle-To 'consoleSaveLogs' ([Windows.Automation.ToggleState]::Off)
            $offStart = [DateTime]::UtcNow
            while (([DateTime]::UtcNow - $offStart).TotalSeconds -lt $LogPairSeconds) {
                if ($script:child.HasExited) { throw 'PROCESS_EXIT_DURING_LOG_PAIR' }
                Sample-Resource 'log_off'
                Start-Sleep -Seconds 1
            }
            $offEnd = [DateTime]::UtcNow
            Toggle-To 'consoleSaveLogs' ([Windows.Automation.ToggleState]::On)
            if ($env:LIVEKIT_UIA_LOG_PAIR -eq '1') {
                # Restore persistence before admitting new diagnostic events.
                Toggle-To 'consoleCollectDiagnostics' ([Windows.Automation.ToggleState]::On)
            }
            $onStart = [DateTime]::UtcNow
            while (([DateTime]::UtcNow - $onStart).TotalSeconds -lt $LogPairSeconds) {
                if ($script:child.HasExited) { throw 'PROCESS_EXIT_DURING_LOG_PAIR' }
                Sample-Resource 'log_on'
                Start-Sleep -Seconds 1
            }
            $window = [ordered]@{run_id=$script:runId; cycle=$script:cycle;
                operation_id=$script:operation; pid=$script:child.Id;
                production_and_persistence=($env:LIVEKIT_UIA_LOG_PAIR -eq '1');
                off_start_utc=$offStart.ToString('o'); off_end_utc=$offEnd.ToString('o');
                on_start_utc=$onStart.ToString('o'); on_end_utc=[DateTime]::UtcNow.ToString('o')}
            [IO.File]::AppendAllText((Join-Path $OutputDirectory 'uia-log-windows.jsonl'),
                (($window | ConvertTo-Json -Compress) + "`n"), [Text.UTF8Encoding]::new($false))
        }
        }
        if ($Retest) {
            # Dwell remains in the joined room. The external supervisor checks
            # WM_NULL independently; these acknowledgements also prove UIA progress.
            while ($script:retestDeadline.Elapsed.TotalSeconds -lt $phaseSeconds) {
                if ($script:child.HasExited) {throw 'PROCESS_EXIT_DURING_RETEST'}
                if (!(Find-Node 'meetingLeave' ([Windows.Automation.ControlType]::Button) -Optional)) {
                    throw 'RETEST_MEETING_WINDOW_LOST'
                }
                if (Find-Node 'meetingDepartureNotice' ([Windows.Automation.ControlType]::Window) -Optional) {
                    throw 'RETEST_MEETING_DEPARTED'
                }
                Sample-Resource ('retest_' + $retestPhase)
                Record ('retest_' + $retestPhase) 'heartbeat'
                Start-Sleep -Seconds 2
            }
            Record ('retest_' + $retestPhase) 'completed'
        }
        $releaseDue = if ($Retest) {[DateTime]::UtcNow} else {$started.AddSeconds($script:cycle * $MinimumSeconds / $Cycles)}
        while ([DateTime]::UtcNow -lt $releaseDue) {
            if ($script:child.HasExited) { throw 'PROCESS_EXIT_BEFORE_LEAVE' }
            Sample-Resource 'settled'
            Start-Sleep -Seconds 1
        }
        if ($HeapDiagnostic) { Save-HeapDiagnosticSnapshot 'active' }
        Action 'leave' {
            Invoke 'meetingLeave'
            $null = Wait-Top 'meetingLeaveConfirmation'
            Invoke 'meetingLeaveConfirm'
            $null = Wait-For 'meeting window closed' {
                Test-MeetingWindowClosed
            } 90
            Sample-Resource 'room_released'
        }
        $roomUntil = [DateTime]::UtcNow.AddSeconds($RoomSettleSeconds)
        while ([DateTime]::UtcNow -lt $roomUntil) {
            if ($script:child.HasExited) { throw 'PROCESS_EXIT_AFTER_LEAVE' }
            Sample-Resource 'room_settled'
            Start-Sleep -Seconds 1
        }
        if ($HeapDiagnostic) { Save-HeapDiagnosticSnapshot }
        if ($HeapDiagnosticNoExport) { return }
        Action 'export' {
            $reportDeadline = [DateTime]::UtcNow.AddSeconds(90)
            do {
                Invoke-AccountTelemetry
                $null = Wait-Top 'telemetryPostMeetingDialog'
                if ((Find-Node 'telemetryExport' ([Windows.Automation.ControlType]::Button)).Current.IsEnabled) {
                    break
                }
                Invoke 'telemetryPostClose'
                Start-Sleep -Seconds 1
            } while ([DateTime]::UtcNow -lt $reportDeadline)
            if ([DateTime]::UtcNow -ge $reportDeadline) { throw 'TIMEOUT: completed telemetry report unavailable' }
            Invoke 'telemetryExport'
            $null = Wait-Top 'telemetryExportDirectory'
            $destination = Join-Path $OutputDirectory ("export-{0:d4}" -f $script:cycle)
            $null = New-Item -ItemType Directory -Path $destination
            Set-Value 'fileNameEdit' $destination
            Invoke 'telemetryExportAccept'
            $null = Wait-For 'diagnostic support bundle artifact' {
                @(Get-ChildItem -LiteralPath $destination -Filter 'cohavora-diagnostic-bundle-*' -Directory).Count -eq 1
            } 90
            $null = Wait-Top 'meetingTelemetryExportResult'
            Invoke 'meetingTelemetryExportDismiss'
            Invoke 'telemetryPostClose'
        }
        if ($HeapDiagnostic) { Save-HeapDiagnosticSnapshot 'exported' }
}
function Stop-Product {
        Action 'process_exit' {
            $main = Wait-Top 'MeetingMainWindow'
            (Require-Pattern $main ([Windows.Automation.WindowPattern]::Pattern)).Close()
            if (!$script:child.WaitForExit(60000)) { throw 'PRODUCT_SHUTDOWN_TIMEOUT' }
            $exitCode = [ProductDesktop]::ExitCode($script:childHandle)
            [ordered]@{pid=$script:child.Id;run_id=$script:runId;exit_code=$exitCode;
                utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json |
                Set-Content -LiteralPath (Join-Path $OutputDirectory 'process-exit.json') -Encoding UTF8
            if ($exitCode -ne 0) { throw "PRODUCT_EXIT_CODE: $exitCode" }
        }
        Sample-Resource 'final_exit'
}
function Run-IsolatedCycle {
    $script:rootCache = @{}
    $directory=Join-Path $OutputDirectory 'cycle-workers'
    $null=New-Item -ItemType Directory -Path $directory -Force
    $prefix=Join-Path $directory ('cycle-{0:d4}' -f $script:cycle)
    $configuration=[ordered]@{output_directory=$OutputDirectory;executable=$Executable;
        started_utc=$started.ToString('o');cycles=$Cycles;minimum_seconds=$MinimumSeconds;
        dedicated_session=$DedicatedDesktopSessionId;desktop_baseline=$script:desktopBaseline;
        share_seconds=$ShareSeconds;log_pair_seconds=$LogPairSeconds;stop_settle_seconds=$StopSettleSeconds;
        room_settle_seconds=$RoomSettleSeconds;heap_diagnostic=[bool]$HeapDiagnostic;no_share=[bool]$HeapDiagnosticNoShare;
        heap_snapshot_diagnostic=[bool]$HeapSnapshotDiagnostic;
        no_export=[bool]$HeapDiagnosticNoExport;heap_check_only=[bool]$HeapCheckOnly;heap_page_check=[bool]$HeapPageCheck;
        crash_diagnostic=[bool]$CrashDiagnostic;run_id=$script:runId;cycle=$script:cycle;cycle_id=$script:cycleId;
        product_pid=$script:child.Id;product_start_ticks=$script:child.StartTime.ToUniversalTime().Ticks;result_path=($prefix+'.result.json')}
    $configuration | ConvertTo-Json | Set-Content -LiteralPath ($prefix+'.config.json') -Encoding UTF8
    $script:cycleWorker=Start-Process powershell.exe -WindowStyle Hidden -PassThru -ArgumentList @(
        '-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+(Join-Path $PSScriptRoot 'product_desktop_cycle.ps1')+'"'),
        '-Configuration',('"'+$prefix+'.config.json"')) -RedirectStandardOutput ($prefix+'.stdout') -RedirectStandardError ($prefix+'.stderr')
    $null=$script:cycleWorker.Handle
    $deadline=[DateTime]::UtcNow.AddSeconds(600)
    try {
        while (!$script:cycleWorker.WaitForExit(1000)) {
            if ($script:child.HasExited) {throw 'PRODUCT_EXIT_DURING_ISOLATED_CYCLE'}
            if ([DateTime]::UtcNow -gt $deadline) {throw 'ISOLATED_CYCLE_TIMEOUT'}
        }
        $script:cycleWorker.Refresh()
        $result=Get-Content -LiteralPath ($prefix+'.result.json') -Raw | ConvertFrom-Json
        if ($result.run_id -ne $script:runId -or $result.cycle_id -ne $script:cycleId -or
            $result.product_pid -ne $script:child.Id -or $result.client_pid -ne $script:cycleWorker.Id) {throw 'ISOLATED_CYCLE_RESULT_IDENTITY_MISMATCH'}
        if ($script:cycleWorker.ExitCode -ne 0 -or $result.status -ne 'COMPLETE') {throw "ISOLATED_CYCLE_FAILED: $($result.reason)"}
        $script:nativeProcessRun=$result.native_process_run
        $script:nativeSession=$result.native_session
        $script:participantHash=$result.participant_hash
        $script:layout=$result.layout
    } finally {
        if (!$script:cycleWorker.HasExited) {$script:cycleWorker.Kill();$script:cycleWorker.WaitForExit()}
        $script:cycleWorker.Dispose();$script:cycleWorker=$null
    }
}
function Cleanup-Product {
        if ($script:cycleWorker -and !$script:cycleWorker.HasExited) {$script:cycleWorker.Kill();$script:cycleWorker.WaitForExit()}
        if ($script:child -and !$script:child.HasExited) {
            $script:child.Kill()
            $script:child.WaitForExit()
        }
        if ($script:child) { $script:child.Dispose(); $script:child = $null }
        if ($script:heapDebugger -and !$script:heapDebugger.WaitForExit(10000)) {
            $script:heapDebugger.Kill()
        }
}
try {
    if (![Environment]::UserInteractive -or ![ProductDesktop]::Unlocked()) {
        Save-Result 'NOT_RUN' 'unlocked_interactive_default_desktop_required'; exit 77
    }
    if ($ProbeOnly -and $Pilot) { throw 'Select either ProbeOnly or Pilot' }
    if ($RetestSmoke -and !$Retest) {throw 'SMOKE_REQUIRES_RETEST'}
    if ($Retest) {
        if ($Pilot -or $ProbeOnly -or $HeapDiagnostic -or $IsolateUiaCycles) {throw 'RETEST_PROFILE_CONFLICT'}
        if ($Cycles -lt 3 -or $SteadySeconds -lt 1 -or $MixedSeconds -lt 2) {throw 'INVALID_RETEST_PLAN'}
        if (!$RetestSmoke -and ($SteadySeconds -lt 1800 -or $MixedSeconds -lt 7200)) {throw 'RETEST_DURATION_TOO_SHORT'}
        $MinimumSeconds = $SteadySeconds + $MixedSeconds
    }
    if (!$ProbeOnly -and (!$env:LIVEKIT_UIA_ACCOUNT -or !$env:LIVEKIT_UIA_PASSWORD -or
        !$env:LIVEKIT_UIA_MEETING_ID -or (!$Pilot -and $DedicatedDesktopSessionId -le 0))) {
        Save-Result 'NOT_RUN' 'dedicated_desktop_account_and_meeting_required'; exit 77
    }
    if ($Pilot -and ($Cycles -lt 2 -or $MinimumSeconds -lt (240 * $Cycles))) {
        throw 'PILOT_PROFILE_REQUIRED: at least two complete cycles in one process, 240 seconds per cycle'
    }
    if (!$ProbeOnly -and !$Pilot -and !$Retest -and ($Cycles -ne 100 -or $MinimumSeconds -lt 28800 -or
        $ShareSeconds -lt 60 -or $LogPairSeconds -lt 30 -or
        $StopSettleSeconds -lt 10 -or $RoomSettleSeconds -lt 10)) {
        throw 'FORMAL_PROFILE_REQUIRED: 100 complete cycles and at least 8 hours'
    }
    if ($DedicatedDesktopSessionId -gt 0) {
        $script:desktopBaseline=Assert-DedicatedDesktop $DedicatedDesktopSessionId
        $script:desktopBaseline | ConvertTo-Json | Set-Content (Join-Path $OutputDirectory 'desktop-baseline.json') -Encoding UTF8
    }
    $script:cycle = 1
    $script:cycleId = [guid]::NewGuid().ToString('N')
    Start-Product
    for ($index=1; $index -le $(if ($ProbeOnly) { 1 } else { $Cycles }); ++$index) {
        $script:cycle = $index
        if ($index -gt 1) {
            $script:cycleId = [guid]::NewGuid().ToString('N')
            $script:nativeSession = $null
        }
        if (!$ProbeOnly -and !$Retest) {
            $due = $started.AddSeconds(($index - 1) * $MinimumSeconds / $Cycles)
            while ([DateTime]::UtcNow -lt $due) { Start-Sleep -Seconds 1 }
        }
        if (!$ProbeOnly) {
            if ($IsolateUiaCycles) {Run-IsolatedCycle} else {Run-Cycle}
        }
        if (!$ProbeOnly) { ++$script:completed }
    }
    if (!$ProbeOnly) { Stop-Product }
    if ($ProbeOnly) { Save-Result 'PROBED' 'tree_observation_only' }
    elseif ($Retest) { Save-Result 'RETEST_COMPLETE' $(if ($RetestSmoke) {'short_smoke_only'} else {'ui_lifecycle_scope_independent_media_witnesses_required'}) }
    elseif ($Pilot) { Save-Result 'PILOT_COMPLETE' 'not_formal_acceptance' }
    elseif (([DateTime]::UtcNow - $started).TotalSeconds -lt $MinimumSeconds) {
        Save-Result 'INCONCLUSIVE' 'duration_shorter_than_eight_hours'; exit 2
    } else { Save-Result 'UIA_COMPLETE' 'independent_witnesses_required' }
} catch {
    $failure = $_.ToString() + "`n" + $_.ScriptStackTrace
    if ($HeapDiagnostic -and $script:child -and !$script:child.HasExited) {
        try { Save-HeapDiagnosticSnapshot 'failure' } catch { }
    }
    try { Save-Tree ("cycle-{0:d4}-failure" -f $script:cycle) } catch { }
    Save-Result 'FAIL' $failure
    Write-Error "FAIL at $script:step : $failure" -ErrorAction Continue
    exit 1
} finally {
    Cleanup-Product
    $env:APPDATA = $oldAppData
    $env:LOCALAPPDATA = $oldLocalAppData
    $env:QT_QPA_PLATFORM = $oldQtPlatform
    $env:QT_ACCESSIBILITY = $oldQtAccessibility
    $env:LIVEKIT_UIA_SETTINGS_ROOT = $oldSettingsRoot
}
exit 0
