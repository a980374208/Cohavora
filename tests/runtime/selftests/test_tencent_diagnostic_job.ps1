param([string]$OutputDirectory="$PSScriptRoot/../../../out/tencent-runner-selftests")
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
Set-StrictMode -Version Latest
$outputRoot=[IO.Path]::GetFullPath($OutputDirectory)
$null=New-Item -ItemType Directory -Path $outputRoot -Force
$fixtureRoot=Join-Path $outputRoot ('job-'+[guid]::NewGuid().ToString('N'))
$null=New-Item -ItemType Directory -Path $fixtureRoot
$resourcesSource=(Resolve-Path (Join-Path $PSScriptRoot '../tools/product_acceptance/product_pilot_resources.ps1')).Path
$shellPath=(Get-Command powershell.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
$ownerShellPath=(Get-Command pwsh.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
$sourceHash=(Get-FileHash -LiteralPath $resourcesSource -Algorithm SHA256).Hash.ToLowerInvariant()
$tokens=$null;$parseErrors=$null
$sourceAst=[Management.Automation.Language.Parser]::ParseFile($resourcesSource,[ref]$tokens,[ref]$parseErrors)
if($parseErrors.Count){throw 'TENCENT_JOB_SOURCE_PARSE_FAILED'}
$jobFunctions=@($sourceAst.EndBlock.Statements | Where-Object {
    $_ -is [Management.Automation.Language.FunctionDefinitionAst] -and
    $_.Name -ceq 'Initialize-ProductPilotResourceJob'
})
if($jobFunctions.Count -ne 1){throw 'TENCENT_JOB_SOURCE_FUNCTION_NOT_UNIQUE'}
$encoding=[Text.UTF8Encoding]::new($false)
$commonPath=Join-Path $fixtureRoot 'fixture-common.ps1'
# Inert fixture helpers: all launches are direct Process.Start calls. No product,
# service, account, password, UI Automation or cloud tooling is used.
$common=@'
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
if (-not ('FixtureProcessIdentityNative' -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
public static class FixtureProcessIdentityNative {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool QueryFullProcessImageName(IntPtr handle, uint flags,
        StringBuilder path, ref uint length);
    public static string ImagePath(IntPtr handle) {
        var path = new StringBuilder(32768);
        uint length = (uint)path.Capacity;
        if (!QueryFullProcessImageName(handle, 0, path, ref length))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "TENCENT_JOB_FIXTURE_PATH_READ_FAILED");
        if (String.IsNullOrWhiteSpace(path.ToString()))
            throw new InvalidOperationException("TENCENT_JOB_FIXTURE_PATH_EMPTY");
        return path.ToString();
    }
}
"@
}
function Write-FixtureJson([string]$Path,$Value) {
    $temporary=$Path+'.'+[guid]::NewGuid().ToString('N')+'.tmp'
    [IO.File]::WriteAllText($temporary,($Value|ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
    [IO.File]::Move($temporary,$Path)
}
function Read-FixtureJson([string]$Path) {
    $file=Get-Item -LiteralPath $Path
    if($file.Length -le 0 -or $file.Length -gt 65536){throw 'TENCENT_JOB_FIXTURE_RECEIPT_SIZE_INVALID'}
    return ([IO.File]::ReadAllText($Path,[Text.UTF8Encoding]::new($false,$true))|ConvertFrom-Json)
}
function Get-FixtureTextHash([string]$Text) {
    $sha=[Security.Cryptography.SHA256]::Create()
    try{return ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text)))).Replace('-','').ToLowerInvariant()}
    finally{$sha.Dispose()}
}
function Get-FixtureIdentity([Diagnostics.Process]$Process,[string]$RunId) {
    $null=$Process.Handle
    $executable=[FixtureProcessIdentityNative]::ImagePath($Process.Handle)
    $ticks=$Process.StartTime.ToUniversalTime().Ticks
    if([string]::IsNullOrWhiteSpace($RunId) -or $Process.Id -le 0 -or $ticks -le 0){throw 'TENCENT_JOB_FIXTURE_IDENTITY_INCOMPLETE'}
    return [ordered]@{run_id=$RunId;pid=$Process.Id;start_ticks=$ticks;
        executable=$executable;powershell_major=$PSVersionTable.PSVersion.Major;
        powershell_version=$PSVersionTable.PSVersion.ToString()}
}
function Assert-FixtureProcessIdentity([Diagnostics.Process]$Process,$Identity,[string]$RunId) {
    $actualTicks=$Process.StartTime.ToUniversalTime().Ticks
    $actualExecutable=[FixtureProcessIdentityNative]::ImagePath($Process.Handle)
    if(!$Identity -or [string]::IsNullOrWhiteSpace([string]$Identity.executable)){throw 'TENCENT_JOB_FIXTURE_IDENTITY_INCOMPLETE'}
    if($Identity.run_id -cne $RunId -or $Process.Id -ne $Identity.pid -or
        $actualTicks -ne $Identity.start_ticks -or
        ![string]::Equals($actualExecutable,$Identity.executable,[StringComparison]::OrdinalIgnoreCase)){
        throw ('TENCENT_JOB_FIXTURE_PROCESS_IDENTITY_MISMATCH: pid='+$Process.Id+
            ' expected_pid='+$Identity.pid+' actual_start_ticks='+$actualTicks+' expected_start_ticks='+$Identity.start_ticks+
            ' actual_executable='+$actualExecutable+' expected_executable='+$Identity.executable+
            ' actual_run_id='+$RunId+' expected_run_id='+$Identity.run_id)
    }
}
function Get-FixtureAttachedProcess($Identity,[string]$RunId) {
    $process=[Diagnostics.Process]::GetProcessById([int]$Identity.pid)
    try{
        $null=$process.Handle
        if($process.HasExited){throw 'TENCENT_JOB_FIXTURE_PROCESS_ALREADY_EXITED'}
        Assert-FixtureProcessIdentity -Process $process -Identity $Identity -RunId $RunId
        return $process
    }catch{$process.Dispose();throw}
}
function Start-FixturePowerShell([string]$Executable,[string[]]$Arguments,[string]$RunId) {
    $info=[Diagnostics.ProcessStartInfo]::new()
    $info.FileName=$Executable
    $info.UseShellExecute=$false
    $info.CreateNoWindow=$true
    $quoted=@($Arguments|ForEach-Object {
        if($_.Contains('"') -or $_.Contains("`r") -or $_.Contains("`n")){throw 'TENCENT_JOB_FIXTURE_ARGUMENT_INVALID'}
        '"'+[regex]::Replace($_,'(\\+)$','$1$1')+'"'
    })
    $info.Arguments=$quoted -join ' '
    foreach($key in @($info.EnvironmentVariables.Keys)){
        if(([string]$key).StartsWith('LIVEKIT_',[StringComparison]::OrdinalIgnoreCase)){
            $info.EnvironmentVariables.Remove([string]$key)
        }
    }
    $process=[Diagnostics.Process]::new()
    $process.StartInfo=$info
    $started=$false
    try{
        if(!$process.Start()){throw 'TENCENT_JOB_FIXTURE_PROCESS_START_FAILED'}
        $started=$true
        $null=$process.Handle
        $identity=Get-FixtureIdentity -Process $process -RunId $RunId
        if(![string]::Equals($identity.executable,[IO.Path]::GetFullPath($Executable),[StringComparison]::OrdinalIgnoreCase)){
            throw 'TENCENT_JOB_FIXTURE_LAUNCH_PATH_MISMATCH'
        }
        $process|Add-Member -MemberType NoteProperty -Name FixtureIdentity -Value $identity
        return $process
    }catch{
        $launchFailure=$_
        # Before publishing an identity, this exact Process object and retained
        # handle belong to the process just created here. Never look up a PID.
        try{
            if($started -and !$process.HasExited){
                $process.Kill()
                if(!$process.WaitForExit(5000)){throw 'TENCENT_JOB_FIXTURE_START_CLEANUP_TIMEOUT'}
            }
        }catch{$launchFailure.Exception.Data['fixture_cleanup_error']=$_.Exception.Message}
        finally{
            try{$process.Dispose()}
            catch{$launchFailure.Exception.Data['fixture_dispose_error']=$_.Exception.Message}
        }
        throw $launchFailure
    }
}
function Stop-FixtureOwnedProcess([Diagnostics.Process]$Process,$Identity,[string]$RunId) {
    if(!$Process){return}
    $Process.Refresh()
    if(!$Process.HasExited){
        Assert-FixtureProcessIdentity -Process $Process -Identity $Identity -RunId $RunId
        $Process.Kill()
        if(!$Process.WaitForExit(5000)){throw 'TENCENT_JOB_FIXTURE_OWNED_PROCESS_SURVIVED_CLEANUP'}
    }
}
function Invoke-FixtureCleanup([object[]]$Entries,[string]$RunId) {
    $rows=New-Object 'System.Collections.Generic.List[object]'
    foreach($entry in $Entries){
        if(!$entry.Process){continue}
        $errorMessage=$null;$disposeError=$null
        try{Stop-FixtureOwnedProcess -Process $entry.Process -Identity $entry.Identity -RunId $RunId}
        catch{$errorMessage=$_.Exception.Message}
        finally{
            try{$entry.Process.Dispose()}catch{$disposeError=$_.Exception.Message}
        }
        $rows.Add([ordered]@{role=$entry.Role;error=$errorMessage;dispose_error=$disposeError})
    }
    return [pscustomobject]@{passed=(@($rows|Where-Object {$_.error -or $_.dispose_error}).Count -eq 0);processes=@($rows.ToArray())}
}
'@
[IO.File]::WriteAllText($commonPath,$common,$encoding)
. $commonPath
$functionHash=Get-FixtureTextHash -Text $jobFunctions[0].Extent.Text
$productPath=Join-Path $fixtureRoot 'fixture-product.ps1'
[IO.File]::WriteAllText($productPath,@'
param([string]$Root,[string]$RunId)
. (Join-Path (Split-Path $Root -Parent) 'fixture-common.ps1')
$self=[Diagnostics.Process]::GetCurrentProcess()
try{Write-FixtureJson -Path (Join-Path $Root 'product-identity.json') -Value (Get-FixtureIdentity $self $RunId)}
finally{$self.Dispose()}
# A deadline prevents a fixture leak even if an unexpected Job failure occurs.
$clock=[Diagnostics.Stopwatch]::StartNew()
while($clock.Elapsed.TotalSeconds -lt 60){Start-Sleep -Milliseconds 100}
Write-FixtureJson -Path (Join-Path $Root 'product-natural-expiry.json') -Value @{run_id=$RunId;unexpected_natural_expiry=$true}
'@,$encoding)
$uiaPath=Join-Path $fixtureRoot 'fixture-uia.ps1'
[IO.File]::WriteAllText($uiaPath,@'
param([string]$Root,[string]$RunId,[string]$ShellPath)
. (Join-Path (Split-Path $Root -Parent) 'fixture-common.ps1')
$self=[Diagnostics.Process]::GetCurrentProcess()
try{Write-FixtureJson -Path (Join-Path $Root 'uia-identity.json') -Value (Get-FixtureIdentity $self $RunId)}
finally{$self.Dispose()}
$product=$null;$productIdentity=$null;$primaryFailure=$null;$cleanup=$null
try{
    $product=Start-FixturePowerShell -Executable $ShellPath -Arguments @('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass',
        '-File',(Join-Path (Split-Path $Root -Parent) 'fixture-product.ps1'),'-Root',$Root,'-RunId',$RunId) -RunId $RunId
    $productIdentity=$product.FixtureIdentity
    $clock=[Diagnostics.Stopwatch]::StartNew()
    while($clock.Elapsed.TotalSeconds -lt 60){Start-Sleep -Milliseconds 100}
    throw 'TENCENT_JOB_FIXTURE_UIA_UNEXPECTED_DEADLINE'
}catch{$primaryFailure=$_}
finally{
    $cleanup=Invoke-FixtureCleanup @(@{Role='product';Process=$product;Identity=$productIdentity}) $RunId
}
$receiptFailure=$null
try{Write-FixtureJson (Join-Path $Root 'uia-cleanup.json') $cleanup}catch{$receiptFailure=$_}
if($primaryFailure){
    if($receiptFailure){$primaryFailure.Exception.Data['fixture_receipt_error']=$receiptFailure.Exception.Message}
    throw $primaryFailure
}
if($receiptFailure){throw $receiptFailure}
if(!$cleanup.passed){throw 'TENCENT_JOB_FIXTURE_UIA_CLEANUP_FAILED'}
'@,$encoding)
$ownerPath=Join-Path $fixtureRoot 'fixture-owner.ps1'
[IO.File]::WriteAllText($ownerPath,@'
param([string]$Root,[string]$RunId,[string]$ShellPath,[string]$ResourcesSource)
. (Join-Path (Split-Path $Root -Parent) 'fixture-common.ps1')
$tokens=$null;$parseErrors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($ResourcesSource,[ref]$tokens,[ref]$parseErrors)
if($parseErrors.Count){throw 'TENCENT_JOB_OWNER_SOURCE_PARSE_FAILED'}
$definitions=@($ast.EndBlock.Statements|Where-Object {
    $_ -is [Management.Automation.Language.FunctionDefinitionAst] -and
    $_.Name -ceq 'Initialize-ProductPilotResourceJob'
})
if($definitions.Count -ne 1){throw 'TENCENT_JOB_OWNER_FUNCTION_NOT_UNIQUE'}
. ([scriptblock]::Create($definitions[0].Extent.Text))
# The raw non-inheritable Job handle remains open until this owner exits. Closing
# it in finally would kill the owner itself and destroy its normal exit status.
Initialize-ProductPilotResourceJob
$self=[Diagnostics.Process]::GetCurrentProcess()
try{
    $ownerIdentity=Get-FixtureIdentity -Process $self -RunId $RunId
    if(![ProductPilotResourceOwnerJob]::Contains($self.Handle)){throw 'TENCENT_JOB_OWNER_NOT_ASSIGNED'}
}finally{$self.Dispose()}
$uia=$null;$product=$null;$uiaIdentity=$null;$productIdentity=$null;$primaryFailure=$null;$cleanup=$null
try{
    $uia=Start-FixturePowerShell -Executable $ShellPath -Arguments @('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass',
        '-File',(Join-Path (Split-Path $Root -Parent) 'fixture-uia.ps1'),'-Root',$Root,'-RunId',$RunId,'-ShellPath',$ShellPath) -RunId $RunId
    $uiaIdentity=$uia.FixtureIdentity
    if(![ProductPilotResourceOwnerJob]::Contains($uia.Handle)){throw 'TENCENT_JOB_UIA_NOT_INHERITED'}
    $clock=[Diagnostics.Stopwatch]::StartNew()
    $productReceipt=Join-Path $Root 'product-identity.json'
    while(!(Test-Path -LiteralPath $productReceipt)){
        if($clock.Elapsed.TotalSeconds -gt 15 -or $uia.HasExited){throw 'TENCENT_JOB_PRODUCT_NOT_READY'}
        Start-Sleep -Milliseconds 20
    }
    $productIdentity=Read-FixtureJson $productReceipt
    $product=Get-FixtureAttachedProcess -Identity $productIdentity -RunId $RunId
    if(![ProductPilotResourceOwnerJob]::Contains($product.Handle)){throw 'TENCENT_JOB_PRODUCT_NOT_INHERITED'}
    $uiaReceipt=Read-FixtureJson (Join-Path $Root 'uia-identity.json')
    Assert-FixtureProcessIdentity -Process $uia -Identity $uiaReceipt -RunId $RunId
    Write-FixtureJson -Path (Join-Path $Root 'owner-ready.json') -Value ([ordered]@{
        schema=1;run_id=$RunId;owner=$ownerIdentity;uia=$uiaReceipt;product=$productIdentity;
        owner_in_job=$true;uia_in_job=$true;product_in_job=$true;use_shell_execute=$false;
        resources_sha256=(Get-FileHash -LiteralPath $ResourcesSource -Algorithm SHA256).Hash.ToLowerInvariant();
        extracted_function_sha256=(Get-FixtureTextHash $definitions[0].Extent.Text)
    })
    $stopPath=Join-Path $Root 'normal-stop.json'
    while(!(Test-Path -LiteralPath $stopPath)){
        if($clock.Elapsed.TotalSeconds -gt 30 -or $uia.HasExited -or $product.HasExited){throw 'TENCENT_JOB_OWNER_UNEXPECTED_EXIT_OR_DEADLINE'}
        Start-Sleep -Milliseconds 20
    }
    $stop=Read-FixtureJson $stopPath
    if($stop.run_id -cne $RunId){throw 'TENCENT_JOB_NORMAL_STOP_WRONG_RUN'}
    Stop-FixtureOwnedProcess -Process $product -Identity $productIdentity -RunId $RunId
    Stop-FixtureOwnedProcess -Process $uia -Identity $uiaIdentity -RunId $RunId
    Write-FixtureJson -Path (Join-Path $Root 'normal-cleanup.json') -Value ([ordered]@{
        schema=1;run_id=$RunId;cleanup='explicit_owner_cleanup';uia_exited=$uia.HasExited;product_exited=$product.HasExited
    })
}catch{$primaryFailure=$_}
finally{
    $cleanup=Invoke-FixtureCleanup @(@{Role='product';Process=$product;Identity=$productIdentity},
        @{Role='uia';Process=$uia;Identity=$uiaIdentity}) $RunId
}
$receiptFailure=$null
try{Write-FixtureJson (Join-Path $Root 'owner-cleanup.json') $cleanup}catch{$receiptFailure=$_}
if($primaryFailure){
    if($receiptFailure){$primaryFailure.Exception.Data['fixture_receipt_error']=$receiptFailure.Exception.Message}
    throw $primaryFailure
}
if($receiptFailure){throw $receiptFailure}
if(!$cleanup.passed){throw 'TENCENT_JOB_FIXTURE_OWNER_CLEANUP_FAILED'}
'@,$encoding)
$cases=New-Object 'System.Collections.Generic.List[object]'
$failure=$null
$observedOwnerVersion=$null
$cleanupReceipts=New-Object 'System.Collections.Generic.List[object]'
try{
    foreach($mode in @('normal','forced')){
        $caseDirectory=Join-Path $fixtureRoot $mode
        $null=New-Item -ItemType Directory -Path $caseDirectory
        $runId=[guid]::NewGuid().ToString('N')
        $owner=$null;$uia=$null;$product=$null;$ownerIdentity=$null;$uiaIdentity=$null;$productIdentity=$null
        $caseFailure=$null;$caseResult=$null;$caseCleanup=$null
        try{
            $owner=Start-FixturePowerShell -Executable $ownerShellPath -Arguments @('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass',
                '-File',$ownerPath,'-Root',$caseDirectory,'-RunId',$runId,'-ShellPath',$shellPath,'-ResourcesSource',$resourcesSource) -RunId $runId
            $ownerIdentity=$owner.FixtureIdentity
            $readyPath=Join-Path $caseDirectory 'owner-ready.json'
            $readyClock=[Diagnostics.Stopwatch]::StartNew()
            while(!(Test-Path -LiteralPath $readyPath)){
                if($readyClock.Elapsed.TotalSeconds -gt 20 -or $owner.HasExited){throw ('TENCENT_JOB_OWNER_NOT_READY: '+$mode)}
                Start-Sleep -Milliseconds 20
            }
            $ready=Read-FixtureJson $readyPath
            if($ready.run_id -cne $runId -or !$ready.owner_in_job -or !$ready.uia_in_job -or !$ready.product_in_job -or
                $ready.use_shell_execute -or $ready.resources_sha256 -cne $sourceHash -or
                $ready.extracted_function_sha256 -cne $functionHash){throw 'TENCENT_JOB_READY_CONTRACT_INVALID'}
            if($ready.owner.powershell_major -ne 7 -or
                ![string]::Equals($ready.owner.executable,$ownerShellPath,[StringComparison]::OrdinalIgnoreCase)){
                throw 'TENCENT_JOB_FIXTURE_OWNER_NOT_POWERSHELL_7'
            }
            $observedOwnerVersion=$ready.owner.powershell_version
            foreach($identity in @($ready.uia,$ready.product)){
                if($identity.powershell_major -ne 5 -or
                    ![string]::Equals($identity.executable,$shellPath,[StringComparison]::OrdinalIgnoreCase)){
                    throw 'TENCENT_JOB_FIXTURE_DESCENDANT_NOT_WINDOWS_POWERSHELL_5'
                }
            }
            Assert-FixtureProcessIdentity -Process $owner -Identity $ready.owner -RunId $runId
            $uiaIdentity=$ready.uia;$productIdentity=$ready.product
            $uia=Get-FixtureAttachedProcess -Identity $uiaIdentity -RunId $runId
            $product=Get-FixtureAttachedProcess -Identity $productIdentity -RunId $runId
            $cleanupClock=[Diagnostics.Stopwatch]::StartNew()
            if($mode -eq 'forced'){
                Stop-FixtureOwnedProcess -Process $owner -Identity $ownerIdentity -RunId $runId
                if(!$uia.WaitForExit(5000) -or !$product.WaitForExit(5000)){throw 'TENCENT_JOB_DESCENDANT_SURVIVED_OWNER_KILL'}
                if(Test-Path -LiteralPath (Join-Path $caseDirectory 'normal-cleanup.json')){throw 'TENCENT_JOB_FORCED_CASE_USED_NORMAL_CLEANUP'}
            }else{
                Write-FixtureJson -Path (Join-Path $caseDirectory 'normal-stop.json') -Value @{run_id=$runId}
                if(!$owner.WaitForExit(10000)){throw 'TENCENT_JOB_NORMAL_OWNER_TIMEOUT'}
                if($owner.ExitCode -ne 0){throw ('TENCENT_JOB_NORMAL_OWNER_EXIT: '+$owner.ExitCode)}
                $cleanup=Read-FixtureJson (Join-Path $caseDirectory 'normal-cleanup.json')
                if($cleanup.run_id -cne $runId -or $cleanup.cleanup -cne 'explicit_owner_cleanup' -or
                    !$cleanup.uia_exited -or !$cleanup.product_exited){throw 'TENCENT_JOB_NORMAL_CLEANUP_NOT_PROVEN'}
                if(!$uia.WaitForExit(5000) -or !$product.WaitForExit(5000)){throw 'TENCENT_JOB_NORMAL_DESCENDANT_ALIVE'}
            }
            $cleanupClock.Stop()
            if(Test-Path -LiteralPath (Join-Path $caseDirectory 'product-natural-expiry.json')){throw 'TENCENT_JOB_PRODUCT_EXPIRED_NATURALLY'}
            $caseResult=[ordered]@{mode=$mode;status='PASS';owner=$ready.owner;uia=$uiaIdentity;product=$productIdentity;
                owner_exit_code=$owner.ExitCode;uia_exited=$uia.HasExited;product_exited=$product.HasExited;
                owner_in_job=$ready.owner_in_job;uia_in_job=$ready.uia_in_job;product_in_job=$ready.product_in_job;
                cleanup_elapsed_ms=$cleanupClock.Elapsed.TotalMilliseconds;natural_completion=$false}
        }catch{$caseFailure=$_}
        finally{
            # Handles and pid/start_ticks/executable identities belong only to this
            # fixture. The owner Job remains the fallback for partial startup.
            $caseCleanup=Invoke-FixtureCleanup @(@{Role='owner';Process=$owner;Identity=$ownerIdentity},
                @{Role='uia';Process=$uia;Identity=$uiaIdentity},@{Role='product';Process=$product;Identity=$productIdentity}) $runId
            $cleanupReceipts.Add([ordered]@{mode=$mode;run_id=$runId;cleanup=$caseCleanup})
        }
        if($caseFailure){throw $caseFailure}
        if(!$caseCleanup.passed){throw 'TENCENT_JOB_FIXTURE_CONTROLLER_CLEANUP_FAILED'}
        $cases.Add($caseResult)
    }
    if((Get-FileHash -LiteralPath $resourcesSource -Algorithm SHA256).Hash.ToLowerInvariant() -cne $sourceHash){throw 'TENCENT_JOB_SOURCE_CHANGED_DURING_TEST'}
}catch{$failure=$_}
$status=if($failure){'FAIL_LOCAL_PROCESS_OWNERSHIP'}else{'PASS_LOCAL_PROCESS_OWNERSHIP'}
$result=[ordered]@{schema=1;status=$status;cases=@($cases.ToArray());cleanup=@($cleanupReceipts.ToArray());
    identity_path_source='retained_handle_QueryFullProcessImageName';powershell_version=$PSVersionTable.PSVersion.ToString();
    owner_host_configuration=[ordered]@{executable=$ownerShellPath;expected_powershell_major=7;observed_powershell_version=$observedOwnerVersion;
        descendant_executable=$shellPath;descendant_expected_powershell_major=5;launch='direct Process.Start with UseShellExecute=false'};
    resources_sha256=$sourceHash;extracted_function_sha256=$functionHash;
    test_sha256=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant();
    fixture_root=$fixtureRoot;fixture_hashes=@([ordered]@{
        common=(Get-FileHash -LiteralPath $commonPath -Algorithm SHA256).Hash.ToLowerInvariant();
        owner=(Get-FileHash -LiteralPath $ownerPath -Algorithm SHA256).Hash.ToLowerInvariant();
        uia=(Get-FileHash -LiteralPath $uiaPath -Algorithm SHA256).Hash.ToLowerInvariant();
        product=(Get-FileHash -LiteralPath $productPath -Algorithm SHA256).Hash.ToLowerInvariant()
    });diagnostic_only=$true;formal_status='NOT_RUN';formal_credit=0;runtime_credit=0;
    boundary='Local synthetic pwsh owner -> Windows PowerShell 5.1 UIA -> Windows PowerShell 5.1 product ownership only; no product, UIA actions, credentials, SDK, service, cloud, media or qualification'}
if($failure){
    $result.failure=$failure.Exception.Message;$result.failure_stack=$failure.ScriptStackTrace
    foreach($key in @('fixture_cleanup_error','fixture_dispose_error','fixture_receipt_error')){
        if($failure.Exception.Data.Contains($key)){$result[$key]=[string]$failure.Exception.Data[$key]}
    }
}
[IO.File]::WriteAllText((Join-Path $fixtureRoot 'result.json'),($result|ConvertTo-Json -Depth 9),$encoding)
$result|ConvertTo-Json -Depth 9
if($failure){throw $failure}
