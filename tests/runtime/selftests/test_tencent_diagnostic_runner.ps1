param([string]$OutputDirectory="$PSScriptRoot/../../../out/tencent-runner-selftests")
$ErrorActionPreference='Stop'
$domain=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../tools/product_acceptance'))
$workspace=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$runnerSource=Join-Path $domain 'invoke_tencent_pilot.ps1'
. (Join-Path $domain 'product_pilot_watchdog.ps1')
. (Join-Path $domain 'product_pilot_probe_tail.ps1')
. (Join-Path $domain 'product_pilot_observer.ps1')
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($runnerSource,[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'RUNNER_PARSE_FAILED'}
foreach($node in $ast.EndBlock.Statements | Where-Object {$_ -is [Management.Automation.Language.FunctionDefinitionAst]}){
    . ([scriptblock]::Create($node.Extent.Text))
}
$testRoot=Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) ('runner-'+[guid]::NewGuid().ToString('N'))
$null=[IO.Directory]::CreateDirectory($testRoot)
$script:cases=[Collections.Generic.List[object]]::new()
$script:startInfos=[Collections.Generic.List[object]]::new()
$script:fakeProcesses=[Collections.Generic.List[object]]::new()
$script:events=[Collections.Generic.List[string]]::new()
$script:actualElapsed=(Get-Item Function:Get-ProductRunBudgetElapsed).ScriptBlock
function Check([bool]$Value,[string]$Reason){if(!$Value){throw ('ASSERT_FAILED:'+ $Reason)}}
function Rejected([scriptblock]$Body,[string]$Code){
    $caught=$null;try{$null=& $Body}catch{$caught=$_.Exception.Message}
    Check ($caught -ceq $Code) ('expected '+$Code+' received '+$caught)
}
function Case([string]$Name,[scriptblock]$Body){& $Body;$script:cases.Add(@{name=$Name;status='PASS'})}
function Write-Json([string]$Path,$Value){$Value|ConvertTo-Json -Depth 7 -Compress|Set-Content -LiteralPath $Path -Encoding UTF8}
function Get-TencentBinaryIdentity([string]$Path){
    @{configuration='RelWithDebInfo';binary_sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
}
function New-Fixture([string]$Name,[string]$Mode='normal'){
    $script:mode=$Mode;$script:passwordReads=0;$script:jobCalls=0;$script:apiCalls=0
    $script:startInfos.Clear();$script:fakeProcesses.Clear();$script:events.Clear()
    $script:fixture=Join-Path $testRoot $Name;$null=[IO.Directory]::CreateDirectory($script:fixture)
    $script:prepared=Join-Path $script:fixture 'prepared';$null=[IO.Directory]::CreateDirectory($script:prepared)
    $script:product=Join-Path $script:fixture 'RelWithDebInfo/Cohavora.exe'
    $null=[IO.Directory]::CreateDirectory((Split-Path $script:product))
    [IO.File]::WriteAllText($script:product,'inert binary; PE validation has its independent gate')
    [IO.File]::WriteAllText((Join-Path $script:prepared 'password.dpapi'),'inert placeholder; never decrypted')
    $script:target=Join-Path $script:fixture 'target.json'
    Write-Json $script:target @{schema=1;provider='tencent';service_url='https://fixture.invalid/api'}
    Write-Json (Join-Path $script:prepared 'setup.json') @{status='PREPARED';account='account-canary';meeting_id='111111111';service_url='https://fixture.invalid/api'}
    $script:inputs=Read-TencentDiagnosticInputs $script:prepared $script:target $script:product (Join-Path $script:fixture 'evidence') '' 2 480
}
function Read-TencentDiagnosticPassword($Inputs){$script:passwordReads++;return 'password-canary'}
function Initialize-TencentDiagnosticJob {$script:jobCalls++;$script:events.Add('job')}
function Test-TencentJobMembership($Process){return $script:mode -ne 'membership_failure'}
function Get-TencentOwnedProduct($Inputs,[string]$RunId){return $null}
function Get-ProductRunBudgetElapsed($Budget){
    if($script:mode -eq 'deadline'){return $script:inputs.MaximumSeconds+1}
    & $script:actualElapsed $Budget
}
function Invoke-RestMethod {
    param($Uri,$Method,$Headers,$Body,$ContentType,$TimeoutSec,$MaximumRedirection)
    $script:apiCalls++
    Check ($MaximumRedirection -ceq 0 -and $TimeoutSec -eq 20) 'API redirects disabled with bounded timeout'
    if($script:mode -eq 'api_transport_failure'){throw 'RAW_TRANSPORT: https://fixture.invalid/api password-canary token-canary'}
    if($Uri.EndsWith('/user/login')){
        $loginBody=$Body|ConvertFrom-Json
        Check ($loginBody.account -ceq 'account-canary' -and $loginBody.password -ceq 'password-canary') 'credential memory reaches intended API'
        return [pscustomobject]@{errCode=0;data=@{token='token-canary';userID='fixture-owner'}}
    }
    Check ($Uri.EndsWith('/meeting/get_meeting') -and $Headers.token -ceq 'token-canary') 'only read-only meeting query'
    return [pscustomobject]@{errCode=0;data=@{meetingDetail=@{info=@{
        creatorDefinedMeeting=@{scheduledTime=[DateTimeOffset]::UtcNow.ToUnixTimeSeconds();meetingDuration=259200}
        systemGenerated=@{meetingID='111111111';creatorUserID='fixture-owner'}
    }}}}
}
function Start-TencentProcess($StartInfo) {
    $script:startInfos.Add($StartInfo)
    $role=if($StartInfo.Arguments.Contains('product_pilot_resources.ps1')){'resource'}
        elseif($StartInfo.Arguments.Contains('product_pilot_checkpoints.py')){'archive'}
        elseif($StartInfo.Arguments.Contains('product_pilot_diagnostics.py')){'diagnostic'}
        else{'uia'}
    $script:events.Add('start-'+$role)
    Check ($script:jobCalls -eq 1) 'owner job precedes every spawn'
    if($script:mode -eq 'partial_start' -and $role -eq 'archive'){throw 'MOCK_START_FAILURE'}
    $process=[pscustomobject]@{Role=$role;Id=77000+$script:fakeProcesses.Count;Handle=[IntPtr]17;
        HasExited=$false;ExitCode=$(if($script:mode -eq 'uia_failure' -and $role -eq 'uia'){1}else{0});
        Killed=$false;Disposed=$false;WaitCalls=[Collections.Generic.List[int]]::new();Root=$script:inputs.Root}
    $process|Add-Member ScriptMethod Refresh {}
    $process|Add-Member ScriptMethod Kill {$this.Killed=$true;$this.HasExited=$true;$this.ExitCode=-1}
    $process|Add-Member ScriptMethod Dispose {$this.Disposed=$true}
    $process|Add-Member ScriptMethod WaitForExit {
        param([int]$Milliseconds)
        $this.WaitCalls.Add($Milliseconds)
        if($this.Role -eq 'uia' -and $script:mode -eq 'deadline' -and !$this.Killed){return $false}
        if($this.Role -eq 'archive'){
            Check ($Milliseconds -ge 3000) 'archive receives its terminal settle interval'
            Write-Json (Join-Path $this.Root 'checkpoint-archive/collector.jsonl') @{event='collector.stopped';run_id=$script:fakeRun;status='COMPLETE'}
        }
        if($this.Role -eq 'diagnostic'){
            Write-Json (Join-Path $this.Root 'diagnostic-watcher-result.json') @{run_id=$script:fakeRun;status='COMPLETE'}
        }
        if($script:mode -eq 'collector_failure' -and $this.Role -eq 'archive'){$this.ExitCode=1}
        $this.HasExited=$true
        return $true
    }
    $script:fakeProcesses.Add($process)
    if($role -eq 'uia'){
        $script:fakeRun=[regex]::Match($StartInfo.Arguments,'"-RunId" "([0-9a-f]{32})"').Groups[1].Value
        Check ($script:fakeRun.Length -eq 32) 'run id passed as non-secret argv'
        $uiaRoot=Join-Path $script:inputs.Root 'uia';$null=[IO.Directory]::CreateDirectory($uiaRoot)
        if($script:mode -ne 'deadline'){
            Write-Json (Join-Path $uiaRoot 'uia-result.json') @{
                run_id=$script:fakeRun;verdict=$(if($script:mode -eq 'uia_failure'){'FAIL'}else{'PILOT_COMPLETE'});
                cycles_requested=2;cycles_completed=2;minimum_seconds=480;
                product_first_live=@{pid=77010;start_ticks=123456789;executable=$script:inputs.Executable}
            }
            Write-Json (Join-Path $uiaRoot 'product-identity.json') @{run_id=$script:fakeRun;pid=77010;start_ticks=123456789;executable=$script:inputs.Executable}
            Write-Json (Join-Path $uiaRoot 'process-exit.json') @{run_id=$script:fakeRun;pid=77010;exit_code=0}
        }
    }
    if($role -eq 'archive'){$null=[IO.Directory]::CreateDirectory((Join-Path $script:inputs.Root 'checkpoint-archive'))}
    return $process
}
$options=@{HeapDiagnostic=$false;HeapSnapshotDiagnostic=$false;HeapDiagnosticPersistentUia=$false;HeapDiagnosticNoShare=$false;HeapDiagnosticNoExport=$false}
$failure=$null
try {
    Case 'pwsh_json_preserves_shared_integer_and_utc_contracts' {
        Rejected {Assert-TencentJsonHost @{}} 'TENCENT_PWSH_DATEKIND_STRING_REQUIRED'
        $value=ConvertFrom-Json '{"schema":1,"pid":77010,"start_ticks":638000000000000001,"utc":"2026-10-07T00:00:00.0000000Z","float":1.0,"text":"1","flag":true}'
        Check ($value.schema -is [int] -and $value.pid -is [int] -and $value.start_ticks -is [long]) 'only bounded integers become Int32'
        Check ($value.utc -is [string] -and $value.utc -ceq '2026-10-07T00:00:00.0000000Z') 'UTC stays exact string'
        Check ($value.float -is [double] -and $value.text -is [string] -and $value.flag -is [bool]) 'float string and bool remain distinct'
    }
    Case 'target_and_meeting_bindings_fail_before_credentials_or_spawn' {
        foreach($bad in @('provider','endpoint','meeting','userinfo','query','fragment')){
            New-Fixture ('binding-'+$bad)
            $target=Get-Content $script:target -Raw|ConvertFrom-Json
            $meeting=''
            switch($bad){
                provider {$target.provider='other'}
                endpoint {$target.service_url='https://other.invalid/api'}
                meeting {$meeting='222222222'}
                userinfo {$target.service_url='https://user:pass@fixture.invalid/api'}
                query {$target.service_url='https://fixture.invalid/api?token=canary'}
                fragment {$target.service_url='https://fixture.invalid/api#canary'}
            }
            Write-Json $script:target $target
            $code=switch($bad){provider {'TENCENT_TARGET_CONFIG_INVALID'};endpoint {'TENCENT_TARGET_BINDING_MISMATCH'};meeting {'TENCENT_MEETING_BINDING_MISMATCH'};default {'TENCENT_SERVICE_URL_INVALID'}}
            Rejected {Read-TencentDiagnosticInputs $script:prepared $script:target $script:product $script:inputs.Root $meeting 2 480} $code
            Check ($script:passwordReads -eq 0 -and $script:startInfos.Count -eq 0 -and $script:apiCalls -eq 0) 'binding rejection has zero side effects'
        }
    }
    Case 'dedicated_file_host_guard_rejects_interactive_and_other_script_hosts' {
        Assert-TencentDedicatedHost $runnerSource @('pwsh.dll','-File',$runnerSource) 'C:/fixture/pwsh.exe'
        foreach($args in @(@('pwsh.dll','-Command','& runner.ps1'),@('pwsh.dll','-File','other.ps1'))){
            Rejected {Assert-TencentDedicatedHost $runnerSource $args 'C:/fixture/pwsh.exe'} 'TENCENT_DEDICATED_PWSH_FILE_HOST_REQUIRED'
        }
        Rejected {Assert-TencentDedicatedHost $runnerSource @('powershell.exe','-File',$runnerSource) 'C:/fixture/powershell.exe'} 'TENCENT_DEDICATED_PWSH_FILE_HOST_REQUIRED'
    }
    Case 'prepare_only_does_not_decode_call_api_initialize_job_or_spawn' {
        New-Fixture 'prepare'
        $receipt=Invoke-TencentDiagnostic $script:inputs $options -OnlyPrepare
        Check ($receipt.verdict -ceq 'PREPARED_DIAGNOSTIC_ONLY' -and $receipt.exit_code -eq 0) 'prepare status'
        Check ($script:passwordReads -eq 0 -and $script:apiCalls -eq 0 -and $script:jobCalls -eq 0 -and $script:startInfos.Count -eq 0) 'strict zero runtime prepare'
        $marker=Read-ProductObserverSnapshot (Join-Path $script:inputs.Root 'diagnostic-debugger.json')
        Check ($marker.diagnostic_only -and !$marker.release_eligible -and $marker.formal_status -ceq 'NOT_RUN') 'prepare marker blocks formal credit'
    }
    Case 'API_redirects_are_disabled_and_raw_transport_error_is_suppressed' {
        New-Fixture 'api-failure' 'api_transport_failure'
        $receipt=Invoke-TencentDiagnostic $script:inputs $options
        Check ($receipt.exit_code -eq 1 -and $receipt.reason -ceq 'MEETING_API_TRANSPORT_FAILED') 'typed API failure propagates'
        $evidence=Get-Content (Join-Path $script:inputs.Root 'runner-exit.json') -Raw
        Check (!$evidence.Contains('RAW_TRANSPORT') -and !$evidence.Contains('password-canary') -and !$evidence.Contains('fixture.invalid')) 'raw transport values excluded'
        Check ($script:startInfos.Count -eq 0) 'preflight failure never spawns collectors'
    }
    Case 'normal_completion_binds_budget_drains_archive_and_keeps_secrets_local' {
        New-Fixture 'normal'
        $env:LIVEKIT_UIA_PASSWORD='parent-password-canary';$env:LIVEKIT_UIA_REMOTE_CONTEXT='1';$env:LIVEKIT_UIA_GPU_ETW_DIRECTORY='old-run'
        $receipt=Invoke-TencentDiagnostic $script:inputs $options
        Check ($receipt.exit_code -eq 0 -and $receipt.verdict -ceq 'DIAGNOSTIC_ONLY_COMPLETE') 'scoped completion'
        Check ($env:LIVEKIT_UIA_PASSWORD -ceq 'parent-password-canary' -and $env:LIVEKIT_UIA_REMOTE_CONTEXT -ceq '1') 'parent environment not modified'
        Check ($script:startInfos.Count -eq 4) 'only three collectors and UIA start'
        foreach($info in $script:startInfos){
            Check (!$info.UseShellExecute -and $info.CreateNoWindow -and !$info.Arguments.Contains('password-canary')) 'direct hidden launch, no password argv'
            Check (!$info.EnvironmentVariables.ContainsKey('LIVEKIT_UIA_REMOTE_CONTEXT') -and !$info.EnvironmentVariables.ContainsKey('LIVEKIT_UIA_GPU_ETW_DIRECTORY')) 'foreign observers removed'
        }
        foreach($info in $script:startInfos | Select-Object -First 3){
            Check (!$info.EnvironmentVariables.ContainsKey('LIVEKIT_UIA_PASSWORD') -and !$info.EnvironmentVariables.ContainsKey('LIVEKIT_UIA_ACCOUNT')) 'collectors inherit no UIA credentials'
            Check ($info.Arguments.Contains((Join-Path $script:inputs.Root 'run-clock.json'))) 'one shared budget passed to each collector'
        }
        Check ($script:startInfos[3].EnvironmentVariables['LIVEKIT_UIA_PASSWORD'] -ceq 'password-canary') 'only UIA receives child-local credential'
        foreach($file in Get-ChildItem -LiteralPath $script:inputs.Root -File -Recurse){
            $text=Get-Content -LiteralPath $file.FullName -Raw
            foreach($secret in @('account-canary','password-canary','token-canary','fixture.invalid','111111111')){
                Check (!$text.Contains($secret)) 'raw business and credential values absent from runner evidence'
            }
        }
        Check (@($script:fakeProcesses | Where-Object {!$_.HasExited -or !$_.Disposed}).Count -eq 0) 'every owned process disposed after bounded cleanup'
    }
    Case 'UIA_failure_collector_failure_partial_launch_and_deadline_return_failure' {
        foreach($mode in @('uia_failure','collector_failure','partial_start','deadline','membership_failure')){
            New-Fixture $mode $mode
            $receipt=Invoke-TencentDiagnostic $script:inputs $options
            Check ($receipt.exit_code -eq 1 -and $receipt.verdict -ceq 'DIAGNOSTIC_ONLY_FAILED') 'failure code never swallowed'
            Check (Test-Path -LiteralPath (Join-Path $script:inputs.Root 'collector-stop.json')) 'failed runs request collector interruption'
            Check (@($script:fakeProcesses | Where-Object {!$_.HasExited -or !$_.Disposed}).Count -eq 0) 'partial launch and timeout clean all registered processes'
            if($mode -eq 'deadline'){Check ($receipt.reason -ceq 'TENCENT_RUN_WATCHDOG_TIMEOUT' -and $script:fakeProcesses[-1].Killed) 'deadline kills UIA and records reason'}
            if($mode -eq 'membership_failure'){Check ($script:fakeProcesses.Count -eq 1) 'membership failure still registers child for cleanup'}
        }
    }
    Case 'formal_release_evaluator_rejects_runner_marker' {
        New-Fixture 'release-rejection'
        $null=Invoke-TencentDiagnostic $script:inputs $options -OnlyPrepare
        $checkScript=Join-Path $script:fixture 'reject_formal.py'
        [IO.File]::WriteAllText($checkScript,@'
import sys
from pathlib import Path
sys.path.insert(0,sys.argv[1])
import release_product_acceptance as release
root=Path(sys.argv[2])
try:
    release.evaluate([root,root.parent/"other1",root.parent/"other2"],{})
except ValueError as error:
    if str(error).startswith("diagnostic_run_not_release_eligible:"):
        sys.exit(0)
    raise
raise AssertionError("diagnostic accepted for formal release")
'@)
        & python.exe $checkScript $domain $script:inputs.Root
        Check ($LASTEXITCODE -eq 0) 'real release evaluator refuses diagnostic marker'
    }
} catch{$failure=$_.Exception.Message}
$proof=[ordered]@{schema=1;status=$(if($failure){'FAIL'}else{'PASS_SCOPED'});failure=$failure;cases=@($script:cases.ToArray());
    runtime_started=$false;product_started=$false;cloud_called=$false;real_credentials_read=$false;formal_status='NOT_RUN';qualification_credit=0;
    scope='Actual runner functions, existing meeting/observer/cleanup functions; inert API/process/PE-validation fixtures. Job inheritance has a separate process test.';
    sources=@(@($runnerSource,$PSCommandPath,(Join-Path $domain 'product_meeting_fixture.ps1'),(Join-Path $domain 'product_pilot_resources.ps1'),
        (Join-Path $domain 'product_pilot_watchdog.ps1'),(Join-Path $domain 'product_pilot_observer.ps1')) |
        ForEach-Object {@{path=$_;sha256=(Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash.ToLowerInvariant()}})}
Write-Json (Join-Path $testRoot 'result.json') $proof
Write-Output ('EVIDENCE='+ (Join-Path $testRoot 'result.json'))
if($failure){throw $failure}
Write-Output ('PASS '+$script:cases.Count+'/'+$script:cases.Count)
