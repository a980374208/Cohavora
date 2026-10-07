param([string]$EvidenceDirectory="$PSScriptRoot/../../../out/b14-repair-20261007")
$ErrorActionPreference='Stop'
if($PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1){throw 'WINDOWS_POWERSHELL_51_REQUIRED'}
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes
$driver=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../uia/product_desktop.ps1'))
$testRoot=Join-Path ([IO.Path]::GetFullPath($EvidenceDirectory)) ('save-tree-snapshot-'+[guid]::NewGuid().ToString('N'))
$null=[IO.Directory]::CreateDirectory($testRoot)
$tokens=$null;$parseErrors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($driver,[ref]$tokens,[ref]$parseErrors)
if($parseErrors.Count){throw 'DRIVER_PARSE_ERROR'}
$functionHashes=[ordered]@{}
foreach($name in @('Get-SaveTreePatternMap','New-SaveTreeCacheRequest','Get-SaveTreeNodes','Convert-CachedSaveTreeRow','Save-Tree')){
    $definitions=@($ast.FindAll({param($node);$node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name},$true))
    if($definitions.Count -ne 1){throw ('PRODUCTION_FUNCTION_NOT_UNIQUE: '+$name)}
    $bytes=[Text.Encoding]::UTF8.GetBytes($definitions[0].Extent.Text)
    $digest=[Security.Cryptography.SHA256]::Create()
    try{$functionHashes[$name]=[BitConverter]::ToString($digest.ComputeHash($bytes)).Replace('-','').ToLowerInvariant()}
    finally{$digest.Dispose()}
    Invoke-Expression $definitions[0].Extent.Text
}
$cases=[Collections.Generic.List[object]]::new()
$script:OutputDirectory=$testRoot
$script:step='save-tree-offline-selftest'
function Assert-True([bool]$Value,[string]$Reason){if(!$Value){throw ('ASSERT_FAILED: '+$Reason)}}
function Assert-Rejected([scriptblock]$Body,[string]$Pattern){
    $message=$null
    try{$null=& $Body}catch{$message=$_.Exception.Message}
    Assert-True ($null -ne $message -and $message -match $Pattern) ('expected '+$Pattern+' got '+$message)
}
function Case([string]$Name,[scriptblock]$Body){$null=& $Body;$cases.Add(@{name=$Name;status='PASS'})}
function Start-Process {throw 'OFFLINE_TEST_MUST_NOT_LAUNCH_PROCESSES'}
function Get-Process {throw 'OFFLINE_TEST_MUST_NOT_QUERY_PROCESSES'}
function Start-Sleep {
    param([int]$Milliseconds)
    Assert-True ($Milliseconds -eq 200) 'retry must retain 200 ms policy'
    $script:sleeps.Add($Milliseconds)
}
function New-CachedNode($Map,$EnabledPatternIds,[string]$Id='fixture-id'){
    $values=@{}
    foreach($entry in $Map){$values[$entry.Property.Id]=($entry.Pattern.Id -in $EnabledPatternIds)}
    $node=[pscustomobject]@{Cached=[pscustomobject]@{AutomationId=$Id;ControlType=[pscustomobject]@{ProgrammaticName='ControlType.Button'};
        Name='PRIVATE_NAME_MUST_NOT_BE_EXPORTED';ProcessId=4321;IsEnabled=$true;IsOffscreen=$false};
        Values=$values;Reads=0;CurrentReads=0;PatternAcquisitions=0;Gone=$false}
    $node|Add-Member ScriptProperty Current {$this.CurrentReads++;throw 'LIVE_CURRENT_FORBIDDEN'}
    $node|Add-Member ScriptMethod GetSupportedPatterns {$this.PatternAcquisitions++;throw 'LIVE_PATTERN_FORBIDDEN'}
    $node|Add-Member ScriptMethod GetCurrentPattern {$this.PatternAcquisitions++;throw 'LIVE_PATTERN_FORBIDDEN'}
    $node|Add-Member ScriptMethod TryGetCurrentPattern {$this.PatternAcquisitions++;throw 'LIVE_PATTERN_FORBIDDEN'}
    $node|Add-Member ScriptMethod GetCachedPattern {$this.PatternAcquisitions++;throw 'CACHED_PATTERN_OBJECT_FORBIDDEN'}
    $node|Add-Member ScriptMethod GetCachedPropertyValue {
        param($Property)
        $this.Reads++
        if($this.Gone){throw [Windows.Automation.ElementNotAvailableException]::new('fake retired node')}
        if(!$this.Values.ContainsKey($Property.Id)){throw 'MISSING_CACHED_PROPERTY'}
        return $this.Values[$Property.Id]
    }
    return $node
}
function Reset-FakeTree {
    $script:child=[pscustomobject]@{Id=4321;HasExited=$false;ExitCode=7}
    $script:rootDiscoveries=0;$script:cacheAcquisitions=0;$script:findCalls=0
    $script:failureMode='none';$script:requestModeObserved=$false
    $script:sleeps=[Collections.Generic.List[int]]::new()
    $script:rootNode=New-CachedNode $script:map @($script:map[0].Pattern.Id) 'root'
    $script:descendantNodes=@(New-CachedNode $script:map @($script:map[1].Pattern.Id) 'child')
    $script:fakeElement=[pscustomobject]@{}
    $script:fakeElement|Add-Member ScriptMethod GetUpdatedCache {
        param($Request)
        $script:cacheAcquisitions++
        Assert-True ($Request.AutomationElementMode -eq [Windows.Automation.AutomationElementMode]::None) 'cache acquisition mode'
        Assert-True ([object]::ReferenceEquals([Windows.Automation.CacheRequest]::Current,$Request)) 'request activated during root acquisition'
        $script:requestModeObserved=$true
        if($script:failureMode -eq 'exit'){$script:child.HasExited=$true;throw [Windows.Automation.ElementNotAvailableException]::new('target exited')}
        if($script:failureMode -eq 'persistent' -or ($script:failureMode -eq 'once' -and $script:cacheAcquisitions -eq 1)){
            throw [Windows.Automation.ElementNotAvailableException]::new('transient fake provider')
        }
        return $script:rootNode
    }
    $script:fakeElement|Add-Member ScriptMethod FindAll {
        param($Scope,$Condition)
        $script:findCalls++
        Assert-True ($Scope -eq [Windows.Automation.TreeScope]::Descendants) 'only descendant acquisition'
        Assert-True ($Condition.Property -eq [Windows.Automation.AutomationElement]::ProcessIdProperty -and $Condition.Value -eq 4321) 'exact process filter'
        Assert-True ([Windows.Automation.CacheRequest]::Current.AutomationElementMode -eq [Windows.Automation.AutomationElementMode]::None) 'descendants remain cache-only'
        return $script:descendantNodes
    }
}
function Get-ProcessRoots {
    $script:rootDiscoveries++
    return [pscustomobject]@{Element=$script:fakeElement}
}
function Read-Tree([string]$Name){return (Get-Content -LiteralPath (Join-Path $testRoot ($Name+'.json')) -Raw|ConvertFrom-Json)}
# A PowerShell ScriptMethod adds ScriptMethodRuntimeException around its thrown
# exception. Use a real .NET method only for the typed UIA disappearance case.
Add-Type -ReferencedAssemblies ([Windows.Automation.AutomationPattern].Assembly.Location) -TypeDefinition @'
public sealed class OfflineUnavailableCachedTreeNode {
    public object Cached { get; set; }
    public object GetCachedPropertyValue(object property) {
        throw new System.Windows.Automation.ElementNotAvailableException("fake retired node");
    }
}
'@
$failure=$null
try {
    $script:map=@(Get-SaveTreePatternMap)
    Case 'framework_pattern_universe_and_scalar_cache_request' {
        Assert-True ($script:map.Count -eq 21 -and @($script:map.Property.Id|Sort-Object -Unique).Count -eq 21 -and @($script:map.Pattern.Id|Sort-Object -Unique).Count -eq 21) '21 bijective mappings'
        $schema=[Windows.Automation.AutomationElement].Assembly.GetType('MS.Internal.Automation.Schema',$true)
        $table=$schema.GetMethod('GetPatternInfoTable',[Reflection.BindingFlags]'Public,NonPublic,Static').Invoke($null,@())
        $actualPatterns=@(foreach($row in $table){
            $id=$row.GetType().GetField('_id',[Reflection.BindingFlags]'NonPublic,Instance').GetValue($row)
            if($null -ne $id){$id}
        })
        Assert-True (($script:map.Pattern.Id -join ',') -ceq ($actualPatterns.Id -join ',')) 'actual GetSupportedPatterns metadata universe and order'
        $request=New-SaveTreeCacheRequest $script:map
        Assert-True ($request.AutomationElementMode -eq [Windows.Automation.AutomationElementMode]::None -and $request.TreeScope -eq [Windows.Automation.TreeScope]::Element) 'cache-only element scope'
        $flags=[Reflection.BindingFlags]'NonPublic,Instance'
        $properties=@([Windows.Automation.CacheRequest].GetField('_properties',$flags).GetValue($request))
        $patternObjects=@([Windows.Automation.CacheRequest].GetField('_patterns',$flags).GetValue($request))
        $expected=@([Windows.Automation.AutomationElement]::RuntimeIdProperty.Id)
        foreach($name in @('AutomationId','ControlType','Name','ProcessId','IsEnabled','IsOffscreen')){$expected+=[Windows.Automation.AutomationElement]::($name+'Property').Id}
        $expected+=@($script:map.Property.Id)
        Assert-True (($properties.Id -join ',') -ceq ($expected -join ',')) '27 explicit scalar properties plus Framework implicit RuntimeId'
        Assert-True ($patternObjects.Count -eq 0) 'no cached pattern objects'
    }
    Case 'cached_projection_patterns_schema_singleton_and_privacy' {
        $none=New-CachedNode $script:map @()
        $row=Convert-CachedSaveTreeRow $none $script:map
        Assert-True ($row.patterns.Count -eq 0 -and $none.Reads -eq 21) 'empty pattern set'
        foreach($entry in $script:map){
            $node=New-CachedNode $script:map @($entry.Pattern.Id)
            $row=Convert-CachedSaveTreeRow $node $script:map
            Assert-True ($row.patterns.Count -eq 1 -and $row.patterns[0] -ceq $entry.Pattern.ProgrammaticName) 'one-hot pattern mapping'
            Assert-True ($node.CurrentReads -eq 0 -and $node.PatternAcquisitions -eq 0) 'no live property or pattern access'
        }
        Reset-FakeTree
        $script:rootNode=New-CachedNode $script:map @($script:map.Pattern.Id)
        $script:descendantNodes=@()
        Save-Tree 'singleton'
        $json=Get-Content -LiteralPath (Join-Path $testRoot 'singleton.json') -Raw
        $rows=Read-Tree 'singleton'
        Assert-True ($json.TrimStart().StartsWith('[') -and @($rows).Count -eq 1) 'real Save-Tree singleton remains JSON array'
        Assert-True (!$json.Contains('PRIVATE_NAME_MUST_NOT_BE_EXPORTED') -and $rows.name_present) 'name value remains private'
        Assert-True ((@($rows.PSObject.Properties.Name) -join ',') -ceq 'id,role,name_present,pid,enabled,offscreen,patterns') 'seven fields unchanged'
        Assert-True (($rows.patterns -join ',') -ceq ($script:map.Pattern.ProgrammaticName -join ',')) 'all true preserves pattern table order'
        Assert-True ($script:rootNode.CurrentReads -eq 0 -and $script:rootNode.PatternAcquisitions -eq 0) 'Save-Tree does not acquire live data'
    }
    Case 'production_tree_acquisition_and_transient_rediscovery' {
        Reset-FakeTree
        $before=[Windows.Automation.CacheRequest]::Current
        $script:failureMode='once'
        Save-Tree 'transient'
        $rows=Read-Tree 'transient'
        Assert-True (@($rows).Count -eq 2 -and (@($rows.id) -join ',') -ceq 'root,child') 'real production helper returns root and descendants'
        Assert-True ($script:rootDiscoveries -eq 2 -and $script:cacheAcquisitions -eq 2 -and $script:findCalls -eq 1) 'transient error rediscovers roots'
        Assert-True ($script:sleeps.Count -eq 1 -and $script:sleeps[0] -eq 200) 'one bounded retry interval'
        Assert-True ([object]::ReferenceEquals([Windows.Automation.CacheRequest]::Current,$before)) 'cache activation disposed after failure and success'
        Assert-True ($script:requestModeObserved) 'production cache request exercised'
    }
    Case 'persistent_acquisition_failure_and_process_exit_stay_visible' {
        Reset-FakeTree;$script:failureMode='persistent'
        Assert-Rejected {Save-Tree 'persistent'} '^UIA_TREE_UNAVAILABLE: pid=4321 '
        Assert-True ($script:rootDiscoveries -eq 10 -and $script:cacheAcquisitions -eq 10 -and $script:sleeps.Count -eq 9) 'ten attempts, nine 200 ms retry intervals'
        Assert-True (!(Test-Path -LiteralPath (Join-Path $testRoot 'persistent.json'))) 'no fabricated empty success file'
        Reset-FakeTree;$script:failureMode='exit'
        Assert-Rejected {Save-Tree 'exited'} '^PROCESS_EXIT: save-tree-offline-selftest code=7$'
        Assert-True ($script:rootDiscoveries -eq 1 -and $script:sleeps.Count -eq 0) 'process exit stops immediately'
        Assert-True (!(Test-Path -LiteralPath (Join-Path $testRoot 'exited.json'))) 'process exit is not serialized as success'
    }
    Case 'invalid_cached_data_propagates_and_retired_node_is_skipped' {
        Reset-FakeTree;$script:rootNode.Values.Remove($script:map[0].Property.Id)
        Assert-Rejected {Save-Tree 'missing'} 'MISSING_CACHED_PROPERTY'
        Assert-True (!(Test-Path -LiteralPath (Join-Path $testRoot 'missing.json'))) 'missing data not written as success'
        foreach($invalid in @('True',1,$null)){
            Reset-FakeTree;$script:rootNode.Values[$script:map[0].Property.Id]=$invalid
            Assert-Rejected {Save-Tree 'invalid'} 'SAVE_TREE_PATTERN_AVAILABILITY_INVALID'
            Assert-True ($script:rootDiscoveries -eq 1 -and $script:sleeps.Count -eq 0) 'projection errors are not acquisition retries'
        }
        Reset-FakeTree
        $retired=[OfflineUnavailableCachedTreeNode]::new();$retired.Cached=$script:rootNode.Cached
        $script:rootNode=$retired
        Save-Tree 'retired-node'
        $rows=Read-Tree 'retired-node'
        Assert-True (@($rows).Count -eq 1 -and $rows.id -ceq 'child') 'only typed disappeared node is skipped'
    }
}catch{$failure=$_}
$report=[ordered]@{schema=1;status=$(if($failure){'FAIL'}else{'PASS'});kind='offline_production_cached_tree_contract';
    powershell=$PSVersionTable.PSVersion.ToString();cases=@($cases.ToArray());driver_sha256=(Get-FileHash -LiteralPath $driver).Hash.ToLowerInvariant();
    selftest_sha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash.ToLowerInvariant();function_sha256=$functionHashes;
    assemblies=@([Windows.Automation.AutomationElement].Assembly,[Windows.Automation.AutomationPattern].Assembly|ForEach-Object {@{name=$_.FullName;sha256=(Get-FileHash -LiteralPath $_.Location).Hash.ToLowerInvariant()}});
    real_provider_queries=0;desktop_observed=$false;product_started=$false;fixture_started=$false;runtime_credit=0;
    qualification_credit=0;formal_credit=0;evidence_directory=$testRoot;
    limitation='Fake cached nodes and real Framework metadata only; no live provider timing, product memory, media, or long stability acceptance'}
if($failure){$report.error=$failure.Exception.Message;$report.stack=$failure.ScriptStackTrace}
$report|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $testRoot 'result.json') -Encoding UTF8
$report|ConvertTo-Json -Depth 8 -Compress
if($failure){throw $failure}
