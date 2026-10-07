param([string]$EvidenceDirectory="$PSScriptRoot/../../../out/memory-release-20261007")
$ErrorActionPreference='Stop'
if ($PSVersionTable.PSVersion.Major -ne 5 -or $PSVersionTable.PSVersion.Minor -ne 1) {throw 'WINDOWS_POWERSHELL_51_REQUIRED'}
$testRoot=Join-Path ([IO.Path]::GetFullPath($EvidenceDirectory)) ('snapshot-profile-'+[guid]::NewGuid().ToString('N'))
$null=[IO.Directory]::CreateDirectory($testRoot)
$heapModule=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../tools/product_acceptance/product_heap_diagnostic.ps1'))
$invoker=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../tools/product_acceptance/invoke_product_external.ps1'))
$driver=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../uia/product_desktop.ps1'))
$worker=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../uia/product_desktop_cycle.ps1'))
$trees=@{}
foreach($path in @($heapModule,$invoker,$driver,$worker)) {
    $tokens=$null;$parseErrors=$null
    $trees[$path]=[Management.Automation.Language.Parser]::ParseFile($path,[ref]$tokens,[ref]$parseErrors)
    if($parseErrors.Count){throw ('REAL_SOURCE_PARSE_FAILED: '+$path)}
}
# Load actual functions with the optional helper disabled; all child launch and
# snapshot operations below are mocks. No product, service or debugger is run.
$HeapSnapshotDiagnostic=$false
. $heapModule
$cases=[Collections.Generic.List[object]]::new()
$script:launches=[Collections.Generic.List[object]]::new()
$script:snapshotCalls=[Collections.Generic.List[object]]::new()
$Executable=Join-Path $testRoot 'fixture-Cohavora.exe'
$script:runId='a'*32;$script:cycle=1;$script:cycleId='b'*32
$script:fixtureChild=[pscustomobject]@{Id=17171;Handle=[IntPtr]17;Path=$Executable;StartTime=[DateTime]::UtcNow;HasExited=$false}
$script:fixtureDebugger=[pscustomobject]@{Id=17272;Handle=[IntPtr]18;HasExited=$false}
$script:fixtureCapture=[pscustomobject]@{Id=17373;Handle=[IntPtr]19;HasExited=$true;ExitCode=0}
$script:fixtureCapture | Add-Member ScriptMethod WaitForExit {param($milliseconds) return $true}
$script:fixtureCapture | Add-Member ScriptMethod Kill {throw 'UNEXPECTED_CAPTURE_KILL'}
function Assert-True([bool]$Value,[string]$Label){if(!$Value){throw ('ASSERT_FAILED: '+$Label)}}
function Assert-Rejected([scriptblock]$Body,[string]$Reason){
    $caught=$null
    try{$null=& $Body}catch{$caught=$_.Exception.Message}
    Assert-True ($caught -ceq $Reason) ('expected '+$Reason+' got '+$caught)
}
function Case([string]$Name,[scriptblock]$Body){$null=& $Body;$cases.Add(@{name=$Name;status='PASS'})}
function New-Case([string]$Name){
    $script:OutputDirectory=Join-Path (Join-Path $testRoot $Name) 'uia'
    $null=[IO.Directory]::CreateDirectory($script:OutputDirectory)
    $script:HeapDiagnostic=$true;$script:HeapSnapshotDiagnostic=$true
    $script:HeapCheckOnly=$false;$script:HeapPageCheck=$false;$script:CrashDiagnostic=$false
    $script:HeapDiagnosticNoShare=$false;$script:HeapDiagnosticNoExport=$false;$script:IsolateUiaCycles=$false
    $script:launches.Clear();$script:snapshotCalls.Clear();$script:mockSnapshotOutcome='success'
    $script:child=$script:fixtureChild;$script:heapDebugger=$null
}
function Start-Process {
    param($FilePath,$ArgumentList,$WorkingDirectory,[switch]$PassThru,$RedirectStandardOutput,$RedirectStandardError,$WindowStyle)
    $script:launches.Add(@{file=$FilePath;args=@($ArgumentList);working=$WorkingDirectory;stdout=$RedirectStandardOutput;stderr=$RedirectStandardError})
    if($FilePath -eq $Executable){return $script:fixtureChild}
    if([IO.Path]::GetFileName($FilePath) -eq 'cdb.exe'){return $script:fixtureDebugger}
    if([IO.Path]::GetFileName($FilePath) -eq 'umdh.exe'){
        $destination=([string]$ArgumentList[1]).Substring(3).Trim('"')
        [IO.File]::WriteAllText($destination,'mock UMDH output')
        return $script:fixtureCapture
    }
    throw ('UNEXPECTED_LAUNCH: '+$FilePath)
}
function Get-CimInstance {param($ClassName,$Filter) return [pscustomobject]@{ExecutablePath=$Executable;ProcessId=$script:fixtureChild.Id}}
function Get-Process {param($Id) if($Id -ne $script:fixtureChild.Id){throw 'UNEXPECTED_PROCESS'};return $script:fixtureChild}
function Write-FixtureIdentity {
    [ordered]@{run_id=$script:runId;pid=$script:child.Id;start_ticks=$script:child.StartTime.ToUniversalTime().Ticks;executable=$Executable} |
        ConvertTo-Json | Set-Content (Join-Path $OutputDirectory 'product-identity.json') -Encoding UTF8
}
function Invoke-ProductNormalHeapSnapshot {
    param($Identity,$Prefix,$CdbExecutable)
    $script:snapshotCalls.Add(@{identity=$Identity;prefix=$Prefix;cdb=$CdbExecutable})
    Assert-True ($Identity.start_ticks -eq $script:child.StartTime.ToUniversalTime().Ticks) 'immutable-start-ticks-forwarded'
    switch($script:mockSnapshotOutcome){
        failure {throw 'MOCK_CAPTURE_FAILED'}
        timeout {throw 'MOCK_CAPTURE_TIMEOUT'}
        invalid {return [pscustomobject]@{status='CAPTURED_DIAGNOSTIC_ONLY'}}
    }
    return [pscustomobject]@{PSTypeName='Product.NormalHeapSnapshotReceipt';status='CAPTURED_DIAGNOSTIC_ONLY';
        identity=$Identity;dump_path=($Prefix+'.capture-private/snapshot.full.dmp');dump_size_bytes=1024;dump_sha256=('c'*64);
        started_utc=[DateTime]::UtcNow.ToString('o');finished_utc=[DateTime]::UtcNow.ToString('o');capture_exit_code=0;
        same_process_alive_after_detach=$true;normal_heap_flags_verified=$false;instrumented_diagnostic=$true;formal_credit=0}
}
$failure=$null
try {
    Case 'both_entry_points_reject_missing_parent_and_conflicting_heap_modes' {
        foreach($path in @($invoker,$driver)){
            $guard=@($trees[$path].EndBlock.Statements | Where-Object {$_ -is [Management.Automation.Language.IfStatementAst] -and $_.Extent.Text.Contains('HEAP_SNAPSHOT_REQUIRES_EXCLUSIVE_HEAP_DIAGNOSTIC')})
            Assert-True ($guard.Count -eq 1) 'one-real-mode-guard'
            foreach($bad in @('parent','check','page','crash')){
                New-Case ('guard-'+[IO.Path]::GetFileNameWithoutExtension($path)+'-'+$bad)
                switch($bad){parent {$script:HeapDiagnostic=$false};check {$script:HeapCheckOnly=$true};page {$script:HeapPageCheck=$true};crash {$script:CrashDiagnostic=$true}}
                Assert-Rejected {Invoke-Expression $guard[0].Extent.Text} 'HEAP_SNAPSHOT_REQUIRES_EXCLUSIVE_HEAP_DIAGNOSTIC'
                Assert-True ($script:launches.Count -eq 0) 'bad-mode-never-launched'
            }
            New-Case ('guard-valid-'+[IO.Path]::GetFileNameWithoutExtension($path))
            Invoke-Expression $guard[0].Extent.Text
        }
    }
    Case 'normal_start_uses_ordinary_executable_arguments_and_no_initial_debugger' {
        New-Case 'normal-start';Start-HeapDiagnosticProduct
        Assert-True ($script:launches.Count -eq 1) 'single-launch'
        $launch=$script:launches[0]
        Assert-True ($launch.file -eq $Executable -and ($launch.args -join ' ') -eq '--debug') 'ordinary-product-launch'
        Assert-True ($launch.working -eq (Split-Path $Executable) -and $launch.stdout -eq (Join-Path $OutputDirectory 'product.stdout.log') -and $launch.stderr -eq (Join-Path $OutputDirectory 'product.stderr.log')) 'ordinary-output-and-working-directory'
        Assert-True ($null -eq $script:heapDebugger -and !(Test-Path (Join-Path $script:heapDirectory 'startup.commands'))) 'no-startup-debugger-or-gflag-command'
        $marker=Get-Content (Join-Path (Split-Path $OutputDirectory) 'diagnostic-debugger.json') -Raw | ConvertFrom-Json
        Assert-True ($marker.flags -eq 'none' -and !$marker.normal_heap_flags_verified -and $marker.instrumented_diagnostic -and !$marker.release_eligible -and $marker.qualification_credit -eq 0) 'honest-diagnostic-boundary'
        Assert-True (!$marker.share_omitted -and !$marker.export_omitted -and $marker.uia_client_profile -eq 'persistent' -and @($marker.capture_phases).Count -eq 1 -and $marker.capture_phases[0] -eq 'released') 'full-workload-released-only-profile'
    }
    Case 'only_released_phase_calls_snapshot_helper_and_preserves_typed_receipt' {
        New-Case 'phases';Start-HeapDiagnosticProduct;Write-FixtureIdentity
        foreach($phase in @('active','exported','failure')){Save-HeapDiagnosticSnapshot $phase}
        Assert-True ($script:snapshotCalls.Count -eq 0) 'no-media-or-failure-dumps'
        Save-HeapDiagnosticSnapshot 'released'
        Assert-True ($script:snapshotCalls.Count -eq 1) 'one-release-dump'
        $receipt=Get-Content (Join-Path $script:heapDirectory 'released-0001.json') -Raw | ConvertFrom-Json
        Assert-True ($receipt.phase -eq 'released' -and !$receipt.allocation_stacks_only -and !$receipt.release_eligible -and $receipt.capture_receipt.dump_path.EndsWith('snapshot.full.dmp') -and !$receipt.capture_receipt.normal_heap_flags_verified) 'driver-receipt-retains-snapshot-boundary'
        Assert-Rejected {Save-HeapDiagnosticSnapshot 'released'} 'HEAP_SNAPSHOT_ALREADY_EXISTS'
        Assert-True ($script:snapshotCalls.Count -eq 1) 'duplicate-rejected-before-helper'
    }
    Case 'capture_failure_timeout_and_invalid_receipt_never_publish_driver_success' {
        foreach($outcome in @('failure','timeout','invalid')){
            New-Case ('capture-'+$outcome);Start-HeapDiagnosticProduct;Write-FixtureIdentity
            $script:mockSnapshotOutcome=$outcome
            $reason=switch($outcome){failure {'MOCK_CAPTURE_FAILED'};timeout {'MOCK_CAPTURE_TIMEOUT'};invalid {'HEAP_SNAPSHOT_CAPTURE_RECEIPT_INVALID'}}
            Assert-Rejected {Save-HeapDiagnosticSnapshot 'released'} $reason
            Assert-True (!(Test-Path (Join-Path $script:heapDirectory 'released-0001.json'))) 'no-success-after-capture-failure'
        }
    }
    Case 'driver_rejects_foreign_published_identity_before_capture' {
        foreach($field in @('run_id','pid','executable')){
            New-Case ('identity-'+$field);Start-HeapDiagnosticProduct;Write-FixtureIdentity
            $path=Join-Path $OutputDirectory 'product-identity.json';$identity=Get-Content $path -Raw | ConvertFrom-Json
            if($field -eq 'pid'){$identity.pid++}else{$identity.$field+='foreign'}
            $identity | ConvertTo-Json | Set-Content $path -Encoding UTF8
            Assert-Rejected {Save-HeapDiagnosticSnapshot 'released'} 'HEAP_SNAPSHOT_PRODUCT_IDENTITY_CHANGED'
            Assert-True ($script:snapshotCalls.Count -eq 0) 'foreign-identity-not-captured'
        }
    }
    Case 'legacy_ust_start_and_snapshot_path_are_preserved' {
        New-Case 'legacy';$script:HeapSnapshotDiagnostic=$false;Start-HeapDiagnosticProduct
        Assert-True ([IO.Path]::GetFileName($script:launches[0].file) -eq 'cdb.exe') 'legacy-starts-debugger'
        $commands=Get-Content (Join-Path $script:heapDirectory 'startup.commands') -Raw
        Assert-True ($commands.Contains('!gflag +ust')) 'legacy-ust-command'
        Save-HeapDiagnosticSnapshot 'active'
        Assert-True ($script:snapshotCalls.Count -eq 0 -and [IO.Path]::GetFileName($script:launches[1].file) -eq 'umdh.exe') 'legacy-umdh-capture'
        $receipt=Get-Content (Join-Path $script:heapDirectory 'active-0001.json') -Raw | ConvertFrom-Json
        Assert-True ($receipt.allocation_stacks_only -and $receipt.exit_code -eq 0) 'legacy-allocation-receipt'
    }
    Case 'invoker_and_worker_forward_snapshot_mode' {
        New-Case 'forwarding';$uiaArgs=@()
        $forward=@($trees[$invoker].FindAll({param($n) $n -is [Management.Automation.Language.IfStatementAst] -and $n.Extent.Text.Contains("`$uiaArgs+='-HeapSnapshotDiagnostic'")},$true))
        Assert-True ($forward.Count -eq 1) 'single-argv-forwarder'
        Invoke-Expression $forward[0].Extent.Text
        Assert-True ($uiaArgs -contains '-HeapSnapshotDiagnostic') 'invoker-argv'
        $configStatement=@($trees[$driver].FindAll({param($n) $n -is [Management.Automation.Language.AssignmentStatementAst] -and $n.Left.Extent.Text -eq '$configuration' -and $n.Extent.Text.Contains('heap_snapshot_diagnostic=')},$true))
        Assert-True ($configStatement.Count -eq 1) 'worker-config-source'
        $started=[DateTime]::UtcNow;$prefix=Join-Path $testRoot 'worker'
        Invoke-Expression $configStatement[0].Extent.Text
        $config=$configuration | ConvertTo-Json | ConvertFrom-Json
        $assignment=@($trees[$worker].EndBlock.Statements | Where-Object {$_ -is [Management.Automation.Language.AssignmentStatementAst] -and $_.Left.Extent.Text -eq '$HeapSnapshotDiagnostic'})
        Assert-True ($assignment.Count -eq 1) 'worker-mode-source'
        $HeapSnapshotDiagnostic=$false;Invoke-Expression $assignment[0].Extent.Text
        Assert-True $HeapSnapshotDiagnostic 'worker-mode-roundtrip'
    }
} catch {$failure=$_.Exception.Message}
$proof=[ordered]@{schema=1;status=$(if($failure){'FAIL'}else{'PASS'});failure=$failure;cases=@($cases.ToArray());
    powershell=$PSVersionTable.PSVersion.ToString();product_started=$false;debugger_started=$false;remote_operations=$false;
    scope='Actual profile functions and parameter/config statements with mocked launches and capture helper; capture internals have their separate selftest';
    sources=@(@($heapModule,$invoker,$driver,$worker) | ForEach-Object {@{path=$_;sha256=(Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash.ToLowerInvariant()}})}
$proof | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $testRoot 'verification.json') -Encoding UTF8
Write-Output ('EVIDENCE='+ (Join-Path $testRoot 'verification.json'))
if($failure){throw $failure}
Write-Output ('PASS '+$cases.Count+'/'+$cases.Count)
