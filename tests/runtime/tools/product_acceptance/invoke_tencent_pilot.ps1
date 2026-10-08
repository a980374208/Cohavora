param(
    [Parameter(Mandatory=$true)][string]$PreparedDirectory,
    [Parameter(Mandatory=$true)][string]$TargetConfig,
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][Alias('EvidenceRoot')][string]$Root,
    [string]$MeetingId,
    [ValidateRange(2,100)][int]$Cycles=2,
    [ValidateRange(0,172800)][int]$MinimumSeconds=0,
    [switch]$HeapDiagnostic,
    [switch]$HeapSnapshotDiagnostic,
    [switch]$HeapDiagnosticPersistentUia,
    [switch]$HeapDiagnosticNoShare,
    [switch]$HeapDiagnosticNoExport,
    [switch]$PrepareOnly
)
$ErrorActionPreference='Stop'
$domain=$PSScriptRoot
$runnerSource=$PSCommandPath
$workspace=[IO.Path]::GetFullPath((Join-Path $domain '../../../..'))
. (Join-Path $domain 'product_pilot_watchdog.ps1')
. (Join-Path $domain 'product_pilot_probe_tail.ps1')
. (Join-Path $domain 'product_pilot_observer.ps1')

function ConvertTo-TencentJsonIntegers($Value) {
    # PS7 parses JSON integers as Int64; the existing PS5 collectors use Int32
    # for bounded schema/PID/count fields. Never coerce floats, booleans or text.
    if($Value -is [long] -and $Value -ge [int]::MinValue -and $Value -le [int]::MaxValue){return [int]$Value}
    if($Value -is [pscustomobject]){
        foreach($property in $Value.PSObject.Properties){$property.Value=ConvertTo-TencentJsonIntegers $property.Value}
    } elseif($Value -is [Array]){
        for($index=0;$index -lt $Value.Length;$index++){$Value[$index]=ConvertTo-TencentJsonIntegers $Value[$index]}
        return ,$Value
    }
    return $Value
}
function Assert-TencentJsonHost($Parameters=(Get-Command Microsoft.PowerShell.Utility\ConvertFrom-Json).Parameters) {
    if(!$Parameters.ContainsKey('DateKind')){throw 'TENCENT_PWSH_DATEKIND_STRING_REQUIRED'}
}
function ConvertFrom-Json {
    [CmdletBinding()]
    param([Parameter(Mandatory=$true,ValueFromPipeline=$true)][AllowEmptyString()][string]$InputObject)
    process {
        Assert-TencentJsonHost
        $parameters=@{InputObject=$InputObject;ErrorAction='Stop';DateKind='String'}
        $value=Microsoft.PowerShell.Utility\ConvertFrom-Json @parameters
        ConvertTo-TencentJsonIntegers $value
    }
}
function Get-TencentSafeFailure($ErrorRecord) {
    $code=($ErrorRecord.Exception.Message -split ':',2)[0]
    if($code -cmatch '^[A-Z][A-Z0-9_]{2,95}$'){return $code}
    return 'TENCENT_OPERATION_FAILED'
}
function Get-TencentTextHash([string]$Value) {
    $hash=[Security.Cryptography.SHA256]::Create()
    try{return ([BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes($Value))).Replace('-','').ToLowerInvariant())}
    finally{$hash.Dispose()}
}
function Read-TencentSourceAst([string]$Path) {
    $tokens=$null;$errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile($Path,[ref]$tokens,[ref]$errors)
    if($errors.Count){throw 'TENCENT_DEPENDENCY_PARSE_FAILED'}
    return $ast
}
function Get-TencentBinaryIdentity([string]$Path) {
    try {
        $identity=& (Join-Path $domain '../diagnostics/verify_runtime_binary.ps1') -Executable $Path -ExpectedExecutableName 'Cohavora.exe' -Configuration RelWithDebInfo | ConvertFrom-Json
        if($identity.configuration -cne 'RelWithDebInfo' -or $identity.binary_sha256 -cnotmatch '^[0-9a-f]{64}$'){throw 'TENCENT_BINARY_NOT_VERIFIED'}
        return $identity
    } catch {throw 'TENCENT_BINARY_NOT_VERIFIED'}
}
function Assert-TencentDedicatedHost([string]$ScriptPath,
    [string[]]$Arguments=[Environment]::GetCommandLineArgs(),
    [string]$HostPath=[Diagnostics.Process]::GetCurrentProcess().MainModule.FileName) {
    $dedicated=$false
    for($index=0;$index -lt $Arguments.Count-1;$index++){
        if($Arguments[$index] -ieq '-File'){
            try{$dedicated=[IO.Path]::GetFullPath($Arguments[$index+1]) -ieq [IO.Path]::GetFullPath($ScriptPath)}catch{$dedicated=$false}
            break
        }
    }
    if(!$dedicated -or [IO.Path]::GetFileName($HostPath) -ine 'pwsh.exe'){throw 'TENCENT_DEDICATED_PWSH_FILE_HOST_REQUIRED'}
}
function Read-TencentDiagnosticInputs([string]$PreparedPath,[string]$TargetPath,[string]$ProductPath,
    [string]$OutputRoot,[string]$RequestedMeeting,[int]$Count,[int]$Seconds) {
    if($Count -lt 2 -or $Count -gt 100 -or $Seconds -lt 240*$Count -or $Seconds -gt 172800){throw 'TENCENT_DIAGNOSTIC_PROFILE_INVALID'}
    $prepared=(Resolve-Path -LiteralPath $PreparedPath -ErrorAction Stop).ProviderPath
    $targetPath=(Resolve-Path -LiteralPath $TargetPath -ErrorAction Stop).ProviderPath
    $productPath=(Resolve-Path -LiteralPath $ProductPath -ErrorAction Stop).ProviderPath
    $setupPath=Join-Path $prepared 'setup.json'
    $setup=Read-ProductObserverSnapshot $setupPath 65536
    $target=Read-ProductObserverSnapshot $targetPath 65536
    $provider=if($target.provider){$target.provider}else{$target.server_provider}
    if($target.schema -isnot [int] -or $target.schema -ne 1 -or $provider -cne 'tencent' -or
        ($target.provider -and $target.server_provider -and $target.provider -cne $target.server_provider)){throw 'TENCENT_TARGET_CONFIG_INVALID'}
    if($setup.status -cne 'PREPARED' -or $setup.meeting_id -isnot [string] -or $setup.meeting_id -cnotmatch '^[0-9]{9}$' -or
        $setup.account -isnot [string] -or [string]::IsNullOrWhiteSpace($setup.account) -or $setup.account.Length -gt 256 -or $setup.account.Contains([char]0)){throw 'TENCENT_PREPARED_MEETING_INVALID'}
    if($RequestedMeeting -and $RequestedMeeting -cne $setup.meeting_id){throw 'TENCENT_MEETING_BINDING_MISMATCH'}
    $endpoints=@()
    foreach($value in @($setup.service_url,$target.service_url)){
        $uri=$null
        if($value -isnot [string] -or ![Uri]::TryCreate($value,[UriKind]::Absolute,[ref]$uri) -or
            $uri.Scheme -cnotin @('http','https') -or !$uri.Host -or $uri.UserInfo -or $uri.Query -or $uri.Fragment){throw 'TENCENT_SERVICE_URL_INVALID'}
        $endpoints+= $uri.AbsoluteUri.TrimEnd('/')
    }
    if($endpoints[0] -cne $endpoints[1]){throw 'TENCENT_TARGET_BINDING_MISMATCH'}
    $out=[IO.Path]::GetFullPath($OutputRoot)
    $allowed=[IO.Path]::GetFullPath((Join-Path $workspace 'out'))+[IO.Path]::DirectorySeparatorChar
    if(!$out.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $out)){throw 'TENCENT_NEW_REPOSITORY_EVIDENCE_ROOT_REQUIRED'}
    if(!(Test-Path -LiteralPath (Join-Path $prepared 'password.dpapi') -PathType Leaf)){throw 'TENCENT_CREDENTIAL_FILE_MISSING'}
    $binary=Get-TencentBinaryIdentity $productPath
    $sourcePaths=@($runnerSource,$targetPath,$setupPath,$productPath,
        (Join-Path $domain '../diagnostics/verify_runtime_binary.ps1'),
        (Join-Path $workspace 'tests/uia/product_desktop.ps1'),
        (Join-Path $workspace 'tests/uia/product_desktop_cycle.ps1'),
        (Join-Path $workspace 'tests/uia/product_desktop_evidence.ps1'))
    foreach($name in @('product_pilot_resources.ps1','product_pilot_watchdog.ps1','product_pilot_probe_tail.ps1','product_pilot_observer.ps1',
        'product_pilot_checkpoints.py','product_pilot_archive.py','product_pilot_run_budget.py','product_pilot_diagnostics.py',
        'product_pilot_privacy.py','product_meeting_fixture.ps1','product_heap_diagnostic.ps1','product_heap_snapshot.ps1')){
        $sourcePaths+=Join-Path $domain $name
    }
    $fingerprints=[ordered]@{}
    foreach($path in $sourcePaths){$fingerprints[$path]=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()}
    return [pscustomobject]@{Setup=$setup;Prepared=$prepared;Root=$out;Executable=$productPath;Binary=$binary;
        Fingerprints=$fingerprints;Cycles=$Count;Seconds=$Seconds;MaximumSeconds=$Seconds+420}
}
function Assert-TencentFrozenInputs($Inputs) {
    foreach($path in $Inputs.Fingerprints.Keys){
        if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $Inputs.Fingerprints[$path]){throw 'TENCENT_INPUT_FINGERPRINT_CHANGED'}
    }
}
function Read-TencentDiagnosticPassword($Inputs) {
    try {
        $secret=(Get-Content -LiteralPath (Join-Path $Inputs.Prepared 'password.dpapi') -Raw -ErrorAction Stop).Trim() | ConvertTo-SecureString -ErrorAction Stop
        return [Net.NetworkCredential]::new('', $secret).Password
    } catch {throw 'TENCENT_CREDENTIAL_READ_FAILED'}
    finally{$secret=$null}
}
function Invoke-TencentMeetingPreflight($Inputs,[string]$Password) {
    # Reuse only the existing query transaction prefix. No meeting is created,
    # and its raw business receipt is deliberately excluded from this evidence.
    $ast=Read-TencentSourceAst (Join-Path $domain 'product_meeting_fixture.ps1')
    $request=@($ast.EndBlock.Statements | Where-Object {$_ -is [Management.Automation.Language.FunctionDefinitionAst] -and $_.Name -eq 'Request-MeetingApi'})
    $transaction=@($ast.EndBlock.Statements | Where-Object {$_ -is [Management.Automation.Language.TryStatementAst]})
    if($request.Count -ne 1 -or $transaction.Count -ne 1){throw 'TENCENT_MEETING_SOURCE_BOUNDARY_INVALID'}
    $prefix=@();$boundaryFound=$false
    foreach($statement in $transaction[0].Body.Statements){
        if($statement -is [Management.Automation.Language.AssignmentStatementAst] -and $statement.Left.Extent.Text -eq '$evidence'){$boundaryFound=$true;break}
        $prefix+=$statement.Extent.Text
    }
    if(!$boundaryFound){throw 'TENCENT_MEETING_SOURCE_BOUNDARY_INVALID'}
    $setup=$Inputs.Setup;$password=$Password;$token=$null;$CreateNew=$false
    $MinimumRemainingSeconds=$Inputs.MaximumSeconds;$OutputDirectory=$Inputs.Root
    try {
        . ([scriptblock]::Create($request[0].Extent.Text))
        . ([scriptblock]::Create(($prefix -join [Environment]::NewLine)))
        return [ordered]@{status='VERIFIED';owner_verified=$true;remaining_seconds=$remaining;
            required_seconds=$MinimumRemainingSeconds;meeting_sha256=(Get-TencentTextHash $setup.meeting_id)}
    } finally{$password=$null;$token=$null;$login=$null;$created=$null;$secret=$null;$old=$null;$detail=$null}
}
function Initialize-TencentDiagnosticJob {
    $ast=Read-TencentSourceAst (Join-Path $domain 'product_pilot_resources.ps1')
    $node=@($ast.EndBlock.Statements | Where-Object {$_ -is [Management.Automation.Language.FunctionDefinitionAst] -and $_.Name -eq 'Initialize-ProductPilotResourceJob'})
    if($node.Count -ne 1){throw 'TENCENT_JOB_SOURCE_BOUNDARY_INVALID'}
    . ([scriptblock]::Create($node[0].Extent.Text))
    Initialize-ProductPilotResourceJob
}
function Test-TencentJobMembership($Process) {[ProductPilotResourceOwnerJob]::Contains($Process.Handle)}
function ConvertTo-TencentProcessArgument([string]$Value) {
    if($Value.Contains([char]0)){throw 'TENCENT_PROCESS_ARGUMENT_INVALID'}
    return '"'+[regex]::Replace([regex]::Replace($Value,'(\\*)"', '$1$1\"'),'(\\+)$','$1$1')+'"'
}
function Start-TencentProcess($StartInfo) {[Diagnostics.Process]::Start($StartInfo)}
function Start-TencentOwnedChild($State,[string]$Role,[string]$FilePath,[string[]]$Arguments,[hashtable]$Environment=@{}) {
    $start=[Diagnostics.ProcessStartInfo]::new()
    $start.FileName=$FilePath;$start.UseShellExecute=$false;$start.CreateNoWindow=$true
    $start.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
    $start.WorkingDirectory=$workspace
    $start.Arguments=(@($Arguments | ForEach-Object {ConvertTo-TencentProcessArgument $_}) -join ' ')
    foreach($name in @('LIVEKIT_UIA_ACCOUNT','LIVEKIT_UIA_PASSWORD','LIVEKIT_UIA_MEETING_ID','LIVEKIT_UIA_SERVICE_URL',
        'LIVEKIT_UIA_REMOTE_CONTEXT','LIVEKIT_UIA_GPU_ETW_DIRECTORY','LIVEKIT_UIA_GPU_BUDGET_PROBE',
        'LIVEKIT_UIA_RUN_ID','LIVEKIT_UIA_LOG_PAIR','LIVEKIT_UIA_PILOT_PROBE')){
        $start.EnvironmentVariables.Remove($name)
    }
    foreach($name in $Environment.Keys){$start.EnvironmentVariables[$name]=[string]$Environment[$name]}
    $process=Start-TencentProcess $start
    if(!$process){throw 'TENCENT_CHILD_START_FAILED'}
    # Register ownership before any post-launch check can throw.
    $State.Children.Add($process)
    $null=$process.Handle
    if(!(Test-TencentJobMembership $process)){throw 'TENCENT_CHILD_JOB_MEMBERSHIP_FAILED'}
    $State.Roles[$Role]=$process
    return $process
}
function Get-TencentOwnedProduct($Inputs,[string]$RunId) {
    $path=Join-Path $Inputs.Root 'uia/product-identity.json'
    if(!(Test-Path -LiteralPath $path)){return $null}
    $identity=Read-ProductObserverSnapshot $path
    if($identity.run_id -cne $RunId -or $identity.executable -cne $Inputs.Executable -or
        $identity.pid -isnot [int] -or $identity.pid -le 0){throw 'TENCENT_PRODUCT_IDENTITY_INVALID'}
    $product=Get-Process -Id $identity.pid -ErrorAction SilentlyContinue
    if(!$product){return $null}
    try {
        $null=$product.Handle
        if($product.StartTime.ToUniversalTime().Ticks -ne $identity.start_ticks -or
            $product.Path -cne $Inputs.Executable -or !(Test-TencentJobMembership $product)){throw 'TENCENT_PRODUCT_OWNERSHIP_INVALID'}
        return $product
    } catch {$product.Dispose();throw}
}
function Wait-TencentDiagnosticUia($Inputs,$State,[string]$RunId,$Budget) {
    $uia=$State.Roles['uia'];$archive=$State.Roles['archive'];$diagnostic=$State.Roles['diagnostic'];$resource=$State.Roles['resource']
    $launch=[Diagnostics.Stopwatch]::StartNew();$identity=$null;$probeMissing=$null;$resourceMissing=$null
    while(!$uia.WaitForExit(500)){
        if((Get-ProductRunBudgetElapsed $Budget) -gt $Inputs.MaximumSeconds){throw 'TENCENT_RUN_WATCHDOG_TIMEOUT'}
        $complete=Test-ProductUiaCompletion $Inputs.Root $RunId 'Pilot' $Inputs.Cycles $Inputs.Seconds
        $null=Assert-ProductEvidenceCollectors $archive $diagnostic $complete $Inputs.Root $RunId 'Pilot' $Inputs.Cycles $Inputs.Seconds
        $action=$null;$actionPath=Join-Path $Inputs.Root 'uia/uia-actions.jsonl'
        if(Test-Path -LiteralPath $actionPath){
            $lines=@(Read-ProductPilotCompleteJsonlTail -Path $actionPath -Count 1)
            if($lines.Count){$action=$lines[0]|ConvertFrom-Json -ErrorAction Stop}
        }
        if(!$identity -and (Test-Path -LiteralPath (Join-Path $Inputs.Root 'uia/product-identity.json'))){
            $identity=Read-ProductObserverSnapshot (Join-Path $Inputs.Root 'uia/product-identity.json')
            $owned=Get-TencentOwnedProduct $Inputs $RunId
            if(!$owned){throw 'TENCENT_PRODUCT_NOT_LIVE'}
            $owned.Dispose();$probeMissing=[Diagnostics.Stopwatch]::StartNew()
        }
        $now=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
        $progress=$null;$liveProduct=$null
        try {
            if($action -and (Get-ProductObserverAgeMilliseconds $action.utc $now) -gt 400000 -and
                (Test-Path -LiteralPath (Join-Path $Inputs.Root 'uia/product-lifetime-progress.json'))){
                $progress=Read-ProductObserverSnapshot (Join-Path $Inputs.Root 'uia/product-lifetime-progress.json')
                $liveProduct=Get-TencentOwnedProduct $Inputs $RunId
            }
            Assert-ProductActionProgress $action $RunId $launch.Elapsed.TotalSeconds $now $progress $identity $Inputs.Cycles $Inputs.Seconds $liveProduct
        } finally{if($liveProduct){$liveProduct.Dispose()}}
        $exiting=$action -and $action.action -ceq 'process_exit'
        if($action -and !$resourceMissing){$resourceMissing=[Diagnostics.Stopwatch]::StartNew()}
        if($resource.HasExited -and $action -and !$exiting -and !$complete){throw 'TENCENT_RESOURCE_STOPPED_EARLY'}
        if($resourceMissing){
            $sample=$null;$resourcePath=Join-Path $Inputs.Root 'external-resources.jsonl'
            if(Test-Path -LiteralPath $resourcePath){
                $lines=@(Read-ProductPilotCompleteJsonlTail -Path $resourcePath -Count 1)
                if($lines.Count){$sample=$lines[0]|ConvertFrom-Json -ErrorAction Stop}
            }
            Assert-ProductResourceHealth $sample $RunId $identity $resourceMissing.Elapsed.TotalSeconds $now $exiting
            if($sample){$resourceMissing.Restart()}
        }
        if($probeMissing){
            $probe=$null;$probePath=Join-Path $Inputs.Root 'process-probe.jsonl'
            if(Test-Path -LiteralPath $probePath){
                $lines=@(Read-ProductPilotCompleteJsonlTail -Path $probePath -Count 1)
                if($lines.Count){$probe=$lines[0]|ConvertFrom-Json -ErrorAction Stop}
            }
            Assert-ProductNativeHealth $probe $RunId $identity $probeMissing.Elapsed.TotalSeconds $now $exiting
            if($probe){$probeMissing.Restart()}
        }
    }
    $uia.Refresh()
    if($uia.ExitCode -isnot [int] -or $uia.ExitCode -ne 0 -or
        !(Test-ProductUiaCompletion $Inputs.Root $RunId 'Pilot' $Inputs.Cycles $Inputs.Seconds)){throw 'TENCENT_UIA_FAILED'}
}
function Invoke-TencentDiagnostic($Inputs,[hashtable]$HeapOptions,[switch]$OnlyPrepare) {
    $run=[guid]::NewGuid().ToString('N')
    $record=[ordered]@{schema=1;run_id=$run;verdict='DIAGNOSTIC_ONLY_FAILED';exit_code=1;
        diagnostic_only=$true;release_eligible=$false;qualification_credit=0;l3_status='NOT_RUN';formal_status='NOT_RUN';
        remote_load_status='NOT_RUN';independent_media_status='NOT_RUN';phase='prepare';reason=$null;cleanup=$null;
        configuration='RelWithDebInfo';started_utc=[DateTime]::UtcNow.ToString('o')}
    $state=[pscustomobject]@{Children=[Collections.Generic.List[object]]::new();Roles=@{}}
    $lock=$null;$password=$null;$created=$false;$cleanup=$null
    try {
        Assert-TencentFrozenInputs $Inputs
        $out=Join-Path $workspace 'out';$null=[IO.Directory]::CreateDirectory($out)
        $lock=[IO.File]::Open((Join-Path $out 'product-acceptance.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
        $null=New-Item -ItemType Directory -Path $Inputs.Root -ErrorAction Stop;$created=$true
        $plan=[ordered]@{schema=1;run_id=$run;mode='DiagnosticOnly';server_provider='tencent';cycles=$Inputs.Cycles;
            seconds=$Inputs.Seconds;maximum_seconds=$Inputs.MaximumSeconds;configuration='RelWithDebInfo';
            binary_sha256=$Inputs.Binary.binary_sha256;diagnostic_only=$true;release_eligible=$false;qualification_credit=0;
            l3_status='NOT_RUN';formal_status='NOT_RUN';remote_load_status='NOT_RUN';independent_media_status='NOT_RUN';
            desktop_input_policy='diagnostic';heap_options=$HeapOptions}
        $plan | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Inputs.Root 'plan.json') -Encoding UTF8
        $plan | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Inputs.Root 'diagnostic-debugger.json') -Encoding UTF8
        $Inputs.Fingerprints | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Inputs.Root 'executed-inputs.json') -Encoding UTF8
        if($OnlyPrepare){$record.verdict='PREPARED_DIAGNOSTIC_ONLY';$record.exit_code=0;return $record}
        $record.phase='job';Initialize-TencentDiagnosticJob
        $record.phase='preflight';$password=Read-TencentDiagnosticPassword $Inputs
        Invoke-TencentMeetingPreflight $Inputs $password | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Inputs.Root 'meeting-preflight.json') -Encoding UTF8
        Assert-TencentFrozenInputs $Inputs
        $budget=Write-ProductRunBudget (Join-Path $Inputs.Root 'run-clock.json') $run $Inputs.MaximumSeconds
        $powershell=Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
        $python=(Get-Command python.exe -CommandType Application -ErrorAction Stop).Source
        $uiaRoot=Join-Path $Inputs.Root 'uia';$probe=Join-Path $Inputs.Root 'process-probe.jsonl'
        $budgetPath=Join-Path $Inputs.Root 'run-clock.json'
        $record.phase='collectors'
        $null=Start-TencentOwnedChild $state 'resource' $powershell @('-NoProfile','-ExecutionPolicy','Bypass','-File',
            (Join-Path $domain 'product_pilot_resources.ps1'),'-UiaDirectory',$uiaRoot,'-Destination',
            (Join-Path $Inputs.Root 'external-resources.jsonl'),'-RunId',$run,'-MaximumSeconds',[string]$Inputs.MaximumSeconds,'-RunBudgetPath',$budgetPath)
        $archiveLimit=[string][Math]::Min([long]2147483648,536870912*[long]$Inputs.Cycles)
        $null=Start-TencentOwnedChild $state 'archive' $python @((Join-Path $domain 'product_pilot_checkpoints.py'),
            '--probe',$probe,'--result',(Join-Path $uiaRoot 'uia-result.json'),'--output',(Join-Path $Inputs.Root 'checkpoint-archive'),
            '--run-id',$run,'--seconds',[string]$Inputs.MaximumSeconds,'--run-budget',$budgetPath,'--maximum-bytes',$archiveLimit)
        $null=Start-TencentOwnedChild $state 'diagnostic' $python @((Join-Path $domain 'product_pilot_diagnostics.py'),
            '--root',$Inputs.Root,'--watch','--seconds',[string]$Inputs.MaximumSeconds,'--run-budget',$budgetPath)
        $record.phase='uia'
        $uiaArgs=@('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $workspace 'tests/uia/product_desktop.ps1'),
            '-Executable',$Inputs.Executable,'-OutputDirectory',$uiaRoot,'-RunId',$run,'-Cycles',[string]$Inputs.Cycles,
            '-MinimumSeconds',[string]$Inputs.Seconds,'-DesktopInputPolicy','diagnostic','-Pilot')
        foreach($name in $HeapOptions.Keys){if($HeapOptions[$name]){$uiaArgs+='-'+$name}}
        $uiaEnvironment=@{LIVEKIT_UIA_ACCOUNT=$Inputs.Setup.account;LIVEKIT_UIA_PASSWORD=$password;
            LIVEKIT_UIA_MEETING_ID=$Inputs.Setup.meeting_id;LIVEKIT_UIA_SERVICE_URL=$Inputs.Setup.service_url;
            LIVEKIT_UIA_RUN_ID=$run;LIVEKIT_UIA_LOG_PAIR='1';LIVEKIT_UIA_PILOT_PROBE=$probe;LIVEKIT_UIA_GPU_BUDGET_PROBE='1'}
        try{$null=Start-TencentOwnedChild $state 'uia' $powershell $uiaArgs $uiaEnvironment}
        finally{$uiaEnvironment.Clear();$password=$null}
        Wait-TencentDiagnosticUia $Inputs $state $run $budget
        $record.phase='collector_drain'
        $cleanup=Invoke-ProductCollectorCleanup $state.Children $null '' '' ''
        if(!$cleanup.passed){throw 'TENCENT_COLLECTOR_CLEANUP_FAILED'}
        $null=Assert-ProductEvidenceCollectors $state.Roles['archive'] $state.Roles['diagnostic'] $true $Inputs.Root $run 'Pilot' $Inputs.Cycles $Inputs.Seconds
        $record.verdict='DIAGNOSTIC_ONLY_COMPLETE';$record.exit_code=0;$record.phase='complete'
    } catch {
        $record.reason=Get-TencentSafeFailure $_
        if($created){@{schema=1;run_id=$run;reason=$record.reason} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Inputs.Root 'collector-stop.json') -Encoding UTF8}
    } finally {
        $password=$null
        if($record.exit_code -ne 0){
            foreach($role in @('uia')){
                $child=$state.Roles[$role]
                if($child){try{if(!$child.HasExited){$child.Kill();$null=$child.WaitForExit(5000)}}catch{$record.reason='TENCENT_OWNED_UIA_CLEANUP_FAILED'}}
            }
            $product=$null
            try {
                $product=Get-TencentOwnedProduct $Inputs $run
                if($product -and !$product.HasExited){$product.Kill();if(!$product.WaitForExit(5000)){throw 'TENCENT_PRODUCT_CLEANUP_TIMEOUT'}}
            } catch{$record.reason='TENCENT_OWNED_PRODUCT_CLEANUP_FAILED'}
            finally{if($product){$product.Dispose()}}
        }
        if(!$cleanup -and $state.Children.Count){$cleanup=Invoke-ProductCollectorCleanup $state.Children $null '' '' ''}
        $record.cleanup=$cleanup
        if($cleanup -and !$cleanup.passed){$record.exit_code=1;$record.verdict='DIAGNOSTIC_ONLY_FAILED';if(!$record.reason){$record.reason='TENCENT_COLLECTOR_CLEANUP_FAILED'}}
        $record.finished_utc=[DateTime]::UtcNow.ToString('o')
        try {
            if($created){$record | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $Inputs.Root 'runner-exit.json') -Encoding UTF8}
        } finally {
            foreach($child in $state.Children){$child.Dispose()}
            if($lock){$lock.Dispose()}
            # The original unnamed Job handle remains open until this dedicated
            # host exits. The kernel then kills any remaining owned descendants.
        }
    }
    return [pscustomobject]$record
}

if($PSVersionTable.PSEdition -ne 'Core' -or $PSVersionTable.PSVersion.Major -lt 7){throw 'TENCENT_PWSH_7_REQUIRED'}
Assert-TencentJsonHost
if($HeapSnapshotDiagnostic -and !$HeapDiagnostic){throw 'TENCENT_SNAPSHOT_REQUIRES_HEAP_DIAGNOSTIC'}
if(($HeapDiagnosticPersistentUia -or $HeapDiagnosticNoShare -or $HeapDiagnosticNoExport) -and !$HeapDiagnostic){throw 'TENCENT_HEAP_OPTIONS_REQUIRE_DIAGNOSTIC'}
if(!$PrepareOnly){Assert-TencentDedicatedHost $PSCommandPath}
if($MinimumSeconds -eq 0){$MinimumSeconds=240*$Cycles}
try {
    $inputs=Read-TencentDiagnosticInputs $PreparedDirectory $TargetConfig $Executable $Root $MeetingId $Cycles $MinimumSeconds
    $options=@{HeapDiagnostic=[bool]$HeapDiagnostic;HeapSnapshotDiagnostic=[bool]$HeapSnapshotDiagnostic;
        HeapDiagnosticPersistentUia=[bool]$HeapDiagnosticPersistentUia;HeapDiagnosticNoShare=[bool]$HeapDiagnosticNoShare;HeapDiagnosticNoExport=[bool]$HeapDiagnosticNoExport}
    $result=Invoke-TencentDiagnostic $inputs $options -OnlyPrepare:$PrepareOnly
    $result | ConvertTo-Json -Depth 7
    exit $result.exit_code
} catch {
    [pscustomobject]@{verdict='DIAGNOSTIC_ONLY_FAILED';exit_code=1;reason=(Get-TencentSafeFailure $_);
        diagnostic_only=$true;release_eligible=$false;qualification_credit=0;l3_status='NOT_RUN';formal_status='NOT_RUN'} | ConvertTo-Json -Compress
    exit 1
}
