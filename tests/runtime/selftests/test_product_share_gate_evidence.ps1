param([string]$EvidenceDirectory="$PSScriptRoot/../../../out/share-gate-selftest")
$ErrorActionPreference='Stop'
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes
$driver=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../uia/product_desktop.ps1'))
$testRoot=Join-Path ([IO.Path]::GetFullPath($EvidenceDirectory)) ([guid]::NewGuid().ToString('N'))
$null=[IO.Directory]::CreateDirectory($testRoot)
$tokens=$null;$parseErrors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($driver,[ref]$tokens,[ref]$parseErrors)
if($parseErrors.Count){throw 'DRIVER_PARSE_ERROR'}
$functionHashes=[ordered]@{}
$productionDefinitions=@{}
foreach($name in @('New-TreeCacheRequest','Get-Nodes','Set-ShareQueryField','Get-LiveNode','Find-Node','Get-ShareGateNode','Share-State','Assert-ShareActive','Save-ShareStateFailure')){
    $definitions=@($ast.FindAll({param($node);$node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name},$true))
    if($definitions.Count -ne 1){throw ('PRODUCTION_FUNCTION_NOT_UNIQUE: '+$name)}
    $digest=[Security.Cryptography.SHA256]::Create()
    try{$functionHashes[$name]=[BitConverter]::ToString($digest.ComputeHash([Text.Encoding]::UTF8.GetBytes($definitions[0].Extent.Text))).Replace('-','').ToLowerInvariant()}
    finally{$digest.Dispose()}
    $productionDefinitions[$name]=$definitions[0].Extent.Text
    Invoke-Expression $definitions[0].Extent.Text
}
$cases=[Collections.Generic.List[object]]::new()
function Assert-True([bool]$Value,[string]$Reason){if(!$Value){throw ('ASSERT_FAILED: '+$Reason)}}
function Assert-Rejected([scriptblock]$Body,[string]$Pattern){
    $message=$null
    try{$null=& $Body}catch{$message=$_.Exception.Message}
    Assert-True ($null -ne $message -and $message -match $Pattern) ('expected '+$Pattern+' got '+$message)
}
function Case([string]$Name,[scriptblock]$Body){$null=& $Body;$cases.Add(@{name=$Name;status='PASS'})}
function Start-Process {throw 'OFFLINE_TEST_MUST_NOT_LAUNCH_PROCESSES'}
function Get-Process {throw 'OFFLINE_TEST_MUST_NOT_QUERY_PROCESSES'}
function New-FakeNode([string]$Id,[Windows.Automation.ControlType]$Role,[bool]$Offscreen=$false){
    [pscustomobject]@{Current=[pscustomobject]@{AutomationId=$Id;ControlType=$Role;Name='FIXTURE_PRIVATE_NAME';ProcessId=4321;IsEnabled=$true;IsOffscreen=$Offscreen}}
}
function Reset-Fixture {
    $script:OutputDirectory=Join-Path $testRoot ([guid]::NewGuid().ToString('N'))
    $null=[IO.Directory]::CreateDirectory($script:OutputDirectory)
    $script:child=[pscustomobject]@{Id=4321;HasExited=$false;ExitCode=0}
    $script:runId='offline-run';$script:cycle=1;$script:cycleId='offline-cycle';$script:operation='offline-operation'
    $script:runClock=[Diagnostics.Stopwatch]::StartNew()
    $script:shareStateFailure=$null;$script:shareStateObservation=$null;$script:shareNodeQueryObservation=$null
    $script:calls=[Collections.Generic.List[string]]::new()
    $script:cachedNodes=@(New-FakeNode 'meetingShareScreen' ([Windows.Automation.ControlType]::CheckBox);New-FakeNode 'screenShareAnnotation' ([Windows.Automation.ControlType]::Button))
    $script:liveNodes=@($script:cachedNodes)
    $script:queryError=$false;$script:patternError=$false;$script:toggleReads=0
    $script:toggleState=[Windows.Automation.ToggleState]::On
}
function Get-Nodes {
    param([switch]$Live,[switch]$TopLevel,[switch]$WindowsOnly)
    $script:calls.Add('cached:'+$(if($WindowsOnly){'window'}else{'all'}))
    if($script:queryError){throw [InvalidOperationException]::new('FIXTURE_QUERY_ERROR')}
    if($WindowsOnly){return @($script:cachedNodes|Where-Object {$_.Current.ControlType -eq [Windows.Automation.ControlType]::Window})}
    return $script:cachedNodes
}
function Get-ProcessRoots {
    $element=[pscustomobject]@{}
    $element|Add-Member ScriptMethod FindAll {
        param($Scope,$Condition)
        $idCondition=@($Condition.GetConditions()|Where-Object {$_.Property -eq [Windows.Automation.AutomationElement]::AutomationIdProperty})[0]
        $id=[string]$idCondition.Value
        $script:calls.Add('live:'+$id)
        return @($script:liveNodes|Where-Object {$_.Current.AutomationId -eq $id})
    }
    [pscustomobject]@{Current=[pscustomobject]@{AutomationId='fixture-root'};Element=$element}
}
function Require-Pattern($Node,$Pattern){
    $script:calls.Add('pattern')
    if($script:patternError){throw [InvalidOperationException]::new('FIXTURE_PATTERN_ERROR')}
    $current=[pscustomobject]@{}
    $current|Add-Member ScriptProperty ToggleState {$script:toggleReads++;return $script:toggleState}
    [pscustomobject]@{Current=$current}
}
function Failure-Files {@(Get-ChildItem -LiteralPath $script:OutputDirectory -Filter 'share-state-failure*.json')}

# Cases below call the production predicates and discovery functions. Mocked
# boundaries contain only immutable node fixtures and never query a UIA provider.
$failure=$null
try {
    Case 'active_pass_has_original_query_order_and_no_evidence_file' {
        Reset-Fixture;Assert-ShareActive
        Assert-True ($script:toggleReads -eq 1) 'single toggle read'
        Assert-True ((Failure-Files).Count -eq 0) 'PASS must not persist failure evidence'
        Assert-True (($script:calls -join ',') -ceq 'cached:window,live:meetingScreenShareFailure,cached:all,live:meetingShareScreen,pattern,cached:all,live:screenShareAnnotation') 'original acquisition order'
    }
    Case 'missing_button_records_cached_and_live_absence' {
        Reset-Fixture;$script:cachedNodes=@();$script:liveNodes=@()
        Assert-Rejected {Assert-ShareActive} '^SCREEN_SHARE_LOST_DURING_ACTIVE_WINDOW$'
        $r=$script:shareStateFailure;$q=$r.queries.share_button
        Assert-True ($r.decision_branch -ceq 'share_button_missing' -and $r.toggle_read_status -ceq 'NOT_READ') 'missing button branch'
        Assert-True ($q.cached_candidate_count -eq 0 -and $q.cached_match_count -eq 0 -and $q.live_match_count -eq 0 -and $q.fallback_reason -ceq 'cached_absent' -and $q.result -ceq 'NULL_MISSING') 'real Find/Live absence trace'
        Assert-True ($null -eq $r.queries.annotation -and $script:toggleReads -eq 0) 'missing button short circuit'
        Assert-True ((Failure-Files).Count -eq 1 -and $null -ne $q.finished_utc) 'failure file and query time'
        $disk=Get-Content -LiteralPath (Failure-Files)[0].FullName -Raw|ConvertFrom-Json
        Assert-True ($disk.decision_branch -ceq $r.decision_branch -and $disk.expected_toggle_state -ceq 'On') 'disk schema'
    }
    Case 'off_and_indeterminate_preserve_annotation_short_circuit' {
        foreach($state in @([Windows.Automation.ToggleState]::Off,[Windows.Automation.ToggleState]::Indeterminate)){
            Reset-Fixture;$script:toggleState=$state
            Assert-Rejected {Assert-ShareActive} '^SCREEN_SHARE_LOST_DURING_ACTIVE_WINDOW$'
            $r=$script:shareStateFailure
            Assert-True ($r.actual_toggle_state -ceq $state.ToString() -and $r.toggle_read_status -ceq 'READ' -and $r.decision_branch -ceq 'toggle_mismatch') 'toggle mismatch evidence'
            Assert-True ($null -eq $r.queries.annotation -and $script:toggleReads -eq 1 -and $script:calls.Count -eq 5) 'no annotation query and one toggle read'
        }
    }
    Case 'annotation_missing_and_cached_offscreen_live_offscreen' {
        Reset-Fixture;$script:cachedNodes=@($script:cachedNodes[0]);$script:liveNodes=@($script:cachedNodes)
        Assert-Rejected {Assert-ShareActive} '^SCREEN_SHARE_LOST_DURING_ACTIVE_WINDOW$'
        Assert-True ($script:shareStateFailure.decision_branch -ceq 'annotation_missing' -and $script:shareStateFailure.queries.annotation.result -ceq 'NULL_MISSING') 'annotation absence'
        Reset-Fixture;$script:cachedNodes[1].Current.IsOffscreen=$true
        Assert-Rejected {Assert-ShareActive} '^SCREEN_SHARE_LOST_DURING_ACTIVE_WINDOW$'
        $q=$script:shareStateFailure.queries.annotation
        Assert-True ($q.cached_candidate_count -eq 2 -and $q.cached_match_count -eq 1 -and $q.cached_optional_offscreen -eq $true -and $q.live_match_count -eq 1 -and $q.live_optional_offscreen -eq $true -and $q.fallback_reason -ceq 'cached_offscreen' -and $q.result -ceq 'NULL_OFFSCREEN') 'real cached/live offscreen fallback'
    }
    Case 'named_error_dialog_preserves_original_exception_and_short_circuit' {
        Reset-Fixture;$script:cachedNodes=@(New-FakeNode 'meetingScreenShareFailure' ([Windows.Automation.ControlType]::Window));$script:liveNodes=@($script:cachedNodes)
        Assert-Rejected {Assert-ShareActive} '^SCREEN_SHARE_FAILED: named product error dialog observed$'
        $r=$script:shareStateFailure
        Assert-True ($r.decision_branch -ceq 'named_error_dialog' -and $r.queries.error_dialog.result -ceq 'NODE' -and $null -eq $r.queries.share_button -and $script:toggleReads -eq 0) 'dialog branch'
    }
    Case 'query_exception_and_pattern_exception_remain_failures' {
        Reset-Fixture;$script:queryError=$true
        Assert-Rejected {Assert-ShareActive} 'FIXTURE_QUERY_ERROR'
        $r=$script:shareStateFailure;$q=$r.queries.error_dialog
        Assert-True ($q.result -ceq 'EXCEPTION' -and $q.stage -ceq 'cached_enumeration' -and $q.error_type -match 'InvalidOperationException' -and $null -ne $q.hresult -and $null -ne $q.finished_utc) 'query exception captured without empty success'
        Reset-Fixture;$script:patternError=$true
        Assert-Rejected {Assert-ShareActive} 'FIXTURE_PATTERN_ERROR'
        Assert-True ($script:shareStateFailure.toggle_read_status -ceq 'READ_FAILED' -and $script:shareStateFailure.decision_branch -ceq 'toggle_read' -and $null -eq $script:shareStateFailure.queries.annotation) 'pattern failure branch'
    }
    Case 'duplicate_live_node_preserves_control_count_exception' {
        Reset-Fixture;$script:liveNodes=@($script:liveNodes)+@($script:liveNodes[0])
        Assert-Rejected {Assert-ShareActive} 'CONTROL_COUNT: meetingShareScreen=2'
        Assert-True ($script:shareStateFailure.queries.share_button.live_match_count -eq 2 -and $script:shareStateFailure.queries.share_button.result -ceq 'EXCEPTION') 'live duplicate trace'
    }
    Case 'create_new_never_overwrites_and_write_error_keeps_memory_and_original_failure' {
        Reset-Fixture;$script:toggleState=[Windows.Automation.ToggleState]::Off
        Assert-Rejected {Assert-ShareActive} '^SCREEN_SHARE_LOST_DURING_ACTIVE_WINDOW$'
        $path=(Failure-Files)[0].FullName;$hash=(Get-FileHash -LiteralPath $path).Hash
        $script:toggleState=[Windows.Automation.ToggleState]::Indeterminate
        Assert-Rejected {Assert-ShareActive} '^SCREEN_SHARE_LOST_DURING_ACTIVE_WINDOW$'
        Assert-True ((Get-FileHash -LiteralPath $path).Hash -ceq $hash) 'existing evidence bytes unchanged'
        Assert-True ($script:shareStateFailure.actual_toggle_state -ceq 'Indeterminate' -and $null -ne $script:shareStateFailure.evidence_write_error_type) 'latest in-memory failure survives CreateNew rejection'
        Reset-Fixture;$script:OutputDirectory=Join-Path $script:OutputDirectory 'nonexistent';$script:toggleState=[Windows.Automation.ToggleState]::Off
        Assert-Rejected {Assert-ShareActive} '^SCREEN_SHARE_LOST_DURING_ACTIVE_WINDOW$'
        Assert-True ($script:shareStateFailure.actual_toggle_state -ceq 'Off' -and $null -ne $script:shareStateFailure.evidence_write_error_type) 'missing directory does not mask original failure'
    }
    Case 'real_cached_enumeration_retains_retry_policy_and_error_history' {
        Reset-Fixture
        Invoke-Expression $productionDefinitions['Get-Nodes']
        $script:attempts=0;$script:sleepCount=0;$script:persistentCacheFailure=$false
        function Start-Sleep {
            param([int]$Milliseconds)
            Assert-True ($Milliseconds -eq 200) 'unchanged retry delay'
            $script:sleepCount++
        }
        function Get-ProcessRoots {
            $element=[pscustomobject]@{}
            $element|Add-Member ScriptMethod FindAll {
                param($Scope,$Condition)
                if($Condition -is [Windows.Automation.AndCondition] -and @($Condition.GetConditions()|Where-Object {$_.Property -eq [Windows.Automation.AutomationElement]::AutomationIdProperty}).Count){return @()}
                $script:attempts++
                Assert-True ([Windows.Automation.CacheRequest]::Current.AutomationElementMode -eq [Windows.Automation.AutomationElementMode]::None) 'real cache-only request'
                if($script:persistentCacheFailure -or $script:attempts -eq 1){throw [Windows.Automation.ElementNotAvailableException]::new('FIXTURE_CACHE_UNAVAILABLE')}
                return @()
            }
            [pscustomobject]@{Current=[pscustomobject]@{AutomationId='fixture-root';ControlType=[Windows.Automation.ControlType]::Window};Element=$element}
        }
        Assert-Rejected {Assert-ShareActive} '^SCREEN_SHARE_LOST_DURING_ACTIVE_WINDOW$'
        $q=$script:shareStateFailure.queries.error_dialog
        Assert-True ($q.cache_attempts -eq 2 -and @($q.cache_errors).Count -eq 1 -and $q.cache_errors[0].attempt -eq 1 -and $null -ne $q.cache_errors[0].hresult -and $script:sleepCount -eq 1) 'successful bounded retry is recorded'
        Reset-Fixture;$script:attempts=0;$script:sleepCount=0;$script:persistentCacheFailure=$true
        Assert-Rejected {Assert-ShareActive} 'UIA_TREE_UNAVAILABLE: pid=4321'
        $q=$script:shareStateFailure.queries.error_dialog
        Assert-True ($q.cache_attempts -eq 10 -and @($q.cache_errors).Count -eq 10 -and $script:sleepCount -eq 9 -and $q.result -ceq 'EXCEPTION') 'exhaustion stays exception with all original retries'
    }
}catch{$failure=$_}
$report=[ordered]@{schema=1;status=$(if($failure){'FAIL'}else{'PASS'});kind='offline_share_gate_evidence';cases=@($cases.ToArray());
    driver_sha256=(Get-FileHash -LiteralPath $driver).Hash.ToLowerInvariant();selftest_sha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash.ToLowerInvariant();
    function_sha256=$functionHashes;real_provider_queries=0;desktop_observed=$false;product_started=$false;runtime_credit=0;formal_credit=0;
    evidence_directory=$testRoot;limitation='Offline fixtures verify evidence and predicate preservation, not product, UIA timing, media, or stability acceptance'}
if($failure){$report.error=$failure.Exception.Message;$report.stack=$failure.ScriptStackTrace}
$report|ConvertTo-Json -Depth 12|Set-Content -LiteralPath (Join-Path $testRoot 'result.json') -Encoding UTF8
$report|ConvertTo-Json -Depth 12 -Compress
if($failure){throw $failure}
