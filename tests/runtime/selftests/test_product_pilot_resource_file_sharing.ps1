param([switch]$LegacySharing, [string]$OutputDirectory="$PSScriptRoot/../../../out/resource-file-sharing")
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../tools/product_acceptance/product_pilot_probe_tail.ps1')
$tokens=$null; $parseErrors=$null
$source=Join-Path $PSScriptRoot '../tools/product_acceptance/product_pilot_resources.ps1'
$ast=[Management.Automation.Language.Parser]::ParseFile($source,[ref]$tokens,[ref]$parseErrors)
if ($parseErrors.Count) {throw 'RESOURCE_COLLECTOR_PARSE_ERROR'}
foreach($name in @('Read-ProductPilotResourceAction','Read-ProductPilotResourceIdentity','Assert-ProductPilotResourceIdentity')) {
    $definition=$ast.Find({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name},$true)
    if (!$definition) {throw ('RESOURCE_FUNCTION_MISSING: '+$name)}
    Invoke-Expression $definition.Extent.Text
}
$null=New-Item -ItemType Directory -Path $OutputDirectory -Force
$fixtureDirectory=Join-Path $OutputDirectory ([guid]::NewGuid().ToString('N'))
$null=New-Item -ItemType Directory -Path $fixtureDirectory
$path=Join-Path $fixtureDirectory 'uia-actions.jsonl'
$identityPath=Join-Path $fixtureDirectory 'product-identity.json'
$encoding=[Text.UTF8Encoding]::new($false)
$runId=[guid]::NewGuid().ToString('N')
$fixtureProcess=Get-Process -Id $PID
$identity=[pscustomobject]@{run_id=$runId;pid=$fixtureProcess.Id;
    start_ticks=$fixtureProcess.StartTime.ToUniversalTime().Ticks;executable=$fixtureProcess.Path}
$identity | ConvertTo-Json -Compress | Set-Content -LiteralPath $identityPath -Encoding UTF8
function Fixture-Action([int]$Sequence) {
    [pscustomobject]@{schema=1;run_id=$runId;pid=$fixtureProcess.Id;sequence=$Sequence;cycle=1;
        operation_id=1;action='fixture';phase='completed';utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json -Compress
}
$first=Fixture-Action 1
$second=Fixture-Action 2
$third=Fixture-Action 3
[IO.File]::WriteAllText($path,$first+"`n",$encoding)
if($LegacySharing){
    $reader=[IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    try{[IO.File]::AppendAllText($path,$second+"`n",$encoding)}finally{$reader.Dispose()}
    throw 'LEGACY_SHARING_FAILURE_NOT_REPRODUCED'
}

# The real append writer stays open across reads of a partial and completed row.
$writer=[IO.FileStream]::new($path,[IO.FileMode]::Append,[IO.FileAccess]::Write,[IO.FileShare]::Read)
try{
    $bytes=$encoding.GetBytes($second+"`n"+'{"sequence":3')
    $writer.Write($bytes,0,$bytes.Length);$writer.Flush()
    $action=Read-ProductPilotResourceAction $path
    if($action.sequence -ne 2){throw 'RESOURCE_PARTIAL_ROW_ACCEPTED'}
    # Complete the intentionally incomplete JSON row while retaining its handle.
    [void]$writer.SetLength($writer.Length-$encoding.GetByteCount('{"sequence":3'))
    [void]$writer.Seek(0,[IO.SeekOrigin]::End)
    $bytes=$encoding.GetBytes($third+"`r`n")
    $writer.Write($bytes,0,$bytes.Length);$writer.Flush()
    $action=Read-ProductPilotResourceAction $path
    if($action.sequence -ne 3){throw 'RESOURCE_COMPLETED_APPEND_NOT_OBSERVED'}
}finally{$writer.Dispose()}
if([IO.File]::ReadAllText($path,$encoding) -cne ($first+"`n"+$second+"`n"+$third+"`r`n")){throw 'RESOURCE_RAW_EVIDENCE_CHANGED'}
$actualIdentity=Read-ProductPilotResourceIdentity $identityPath
Assert-ProductPilotResourceIdentity $action $actualIdentity $fixtureProcess $runId
Assert-ProductPilotResourceIdentity $action $actualIdentity $null $runId
foreach($mode in @('action_pid','run','ticks_type','live_ticks','live_path')){
    $candidateAction=$action | ConvertTo-Json -Compress | ConvertFrom-Json
    $candidateIdentity=$actualIdentity | ConvertTo-Json -Compress | ConvertFrom-Json
    $candidateProcess=$fixtureProcess
    switch($mode){
        'action_pid' {$candidateAction.pid++}
        'run' {$candidateIdentity.run_id=[guid]::NewGuid().ToString('N')}
        'ticks_type' {$candidateIdentity.start_ticks=[double]$candidateIdentity.start_ticks}
        'live_ticks' {$candidateProcess=[pscustomobject]@{Id=$fixtureProcess.Id;StartTime=$fixtureProcess.StartTime.AddTicks(1);Path=$fixtureProcess.Path}}
        'live_path' {$candidateProcess=[pscustomobject]@{Id=$fixtureProcess.Id;StartTime=$fixtureProcess.StartTime;Path=($fixtureProcess.Path+'.wrong')}}
    }
    $failed=$false
    try{Assert-ProductPilotResourceIdentity $candidateAction $candidateIdentity $candidateProcess $runId}catch{$failed=$true}
    if(!$failed){throw ('RESOURCE_IDENTITY_FAILURE_HIDDEN: '+$mode)}
}
foreach($bad in @('[]','{"schema":1,"utc":[],"run_id":"'+$runId+'","pid":'+$fixtureProcess.Id+'}','{invalid')){
    [IO.File]::WriteAllText($path,$first+"`n"+$bad+"`n",$encoding)
    $failed=$false
    try{$unused=Read-ProductPilotResourceAction $path}catch{$failed=$true}
    if(!$failed){throw 'RESOURCE_MALFORMED_COMPLETE_ROW_HIDDEN'}
}
$failed=$false
try{$unused=Read-ProductPilotResourceAction ($path+'.missing')}catch{$failed=$true}
if(!$failed){throw 'RESOURCE_MISSING_ACTION_HIDDEN'}
[IO.File]::WriteAllText($path,$third+"`n",$encoding)

# Short real collector execution binds rows to this PowerShell fixture process;
# it starts no product/audio/SDK/service and uses the actual shared QPC marker.
. (Join-Path $PSScriptRoot '../tools/product_acceptance/product_pilot_watchdog.ps1')
$budgetPath=Join-Path $fixtureDirectory 'run-clock.json'
$null=Write-ProductRunBudget -Path $budgetPath -RunId $runId -MaximumSeconds 2
$destination=Join-Path $fixtureDirectory 'external-resources.jsonl'
& $source -UiaDirectory $fixtureDirectory -Destination $destination -RunId $runId -MaximumSeconds 2 -RunBudgetPath $budgetPath
$rows=@(Get-Content -LiteralPath $destination | ForEach-Object {$_ | ConvertFrom-Json})
if(!$rows.Count){throw 'RESOURCE_SHORT_COLLECTOR_EMITTED_NO_ROW'}
foreach($row in $rows){
    if($row.run_id -cne $runId -or $row.pid -ne $identity.pid -or $row.start_ticks -ne $identity.start_ticks -or
        $row.executable -cne $identity.executable -or !$row.process_alive){throw 'RESOURCE_ROW_PRODUCT_IDENTITY_NOT_BOUND'}
}
$expiredPath=Join-Path $fixtureDirectory 'expired-run-clock.json'
$expired=[pscustomobject]@{schema=1;run_id=$runId;maximum_seconds=2;
    start_qpc_ticks=([Diagnostics.Stopwatch]::GetTimestamp()-3*[Diagnostics.Stopwatch]::Frequency);
    frequency_hz=[Diagnostics.Stopwatch]::Frequency;clock_source='QueryPerformanceCounter'}
[IO.File]::WriteAllText($expiredPath,($expired|ConvertTo-Json -Compress),$encoding)
$failed=$false
try{& $source -UiaDirectory $fixtureDirectory -Destination (Join-Path $fixtureDirectory 'expired-resources.jsonl') -RunId $runId -MaximumSeconds 2 -RunBudgetPath $expiredPath}catch{$failed=$true}
if(!$failed -or (Get-Item -LiteralPath (Join-Path $fixtureDirectory 'expired-resources.jsonl')).Length -ne 0){throw 'RESOURCE_SHARED_BUDGET_RESTARTED'}
Write-Output 'PASS: real open append writer; newest newline-complete scalar action; unchanged raw evidence; run/PID/start_ticks/executable fail-closed binding; strict selected JSON; missing path; short collector rows; actual shared QPC budget and expired-budget rejection'
