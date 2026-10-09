param([string]$OutputDirectory="$PSScriptRoot/../../../out/b11-runner-selftests")
$ErrorActionPreference='Stop'
$workspace=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$runner=Join-Path $workspace 'tests/runtime/orchestration/run-render-probe.ps1'
$testRoot=Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) ('runner-'+[guid]::NewGuid().ToString('N'))
$null=[IO.Directory]::CreateDirectory($testRoot)
$tokens=$null;$parseErrors=$null
$null=[Management.Automation.Language.Parser]::ParseFile($runner,[ref]$tokens,[ref]$parseErrors)
if($parseErrors.Count){throw 'B11_RUNNER_PARSE_FAILED'}

# The actual runner executes with both external entry points shadowed. The
# product binary below is inert; no cloud command, process or native power call
# is made. Input-freeze schema/identity validation has its separate Python gate.
# A mock invoked by another .ps1 sees that script's $script scope. Use a unique
# global prefix for test state so the actual runner and all mock entry points
# share one state without depending on which script invokes the function.
Microsoft.PowerShell.Utility\Add-Type @'
using System.Collections.Generic;
public static class RenderProbePowerState {
    public static readonly List<uint> Calls = new List<uint>();
    public static uint SetThreadExecutionState(uint flags) { Calls.Add(flags); return flags; }
}
'@
function Add-Type { }
$global:B11Mockcases=[Collections.Generic.List[object]]::new()
$global:B11Mockevents=[Collections.Generic.List[string]]::new()
$global:B11MockcloudCalls=[Collections.Generic.List[object]]::new()
$global:B11MockpythonCalls=[Collections.Generic.List[object]]::new()
$global:B11MockremoteCliCalls=[Collections.Generic.List[object]]::new()
$global:B11MockcommandFiles=[Collections.Generic.List[string]]::new()
$initialLocation=(Get-Location).Path
$initialEnv=@{}
foreach($name in @('LIVEKIT_URL','LIVEKIT_SOAK_TOKEN','LIVEKIT_SOAK_ALLOW_INSECURE')){
    $initialEnv[$name]=[Environment]::GetEnvironmentVariable($name,'Process')
}
function Check([bool]$Value,[string]$Reason){if(!$Value){throw ('ASSERT_FAILED: '+$Reason)}}
function Case([string]$Name,[scriptblock]$Body){$null=& $Body;$global:B11Mockcases.Add(@{name=$Name;status='PASS'})}
function Write-Json([string]$Path,$Value){$Value|ConvertTo-Json -Depth 12 -Compress|Set-Content -LiteralPath $Path -Encoding UTF8}
function Arg-Value($Arguments,[string]$Name){
    for($index=0;$index -lt $Arguments.Count-1;$index++){
        if([string]$Arguments[$index] -ceq $Name){return [string]$Arguments[$index+1]}
    }
    return $null
}
function New-Fixture([string]$Name,[string]$Mode='success'){
    $global:B11Mockmode=$Mode;$global:B11Mockevents.Clear();$global:B11MockcloudCalls.Clear();$global:B11MockpythonCalls.Clear()
    $global:B11MockremoteCliCalls.Clear();$global:B11MockcommandFiles.Clear()
    [RenderProbePowerState]::Calls.Clear()
    $global:B11MockprobeCalls=0;$global:B11MockpublisherCleanup=0;$global:B11MocksamplerCleanup=0
    $global:B11Mockfixture=Join-Path $testRoot $Name;$null=[IO.Directory]::CreateDirectory($global:B11Mockfixture)
    $global:B11Mockprepared=Join-Path $global:B11Mockfixture 'prepared';$null=[IO.Directory]::CreateDirectory($global:B11Mockprepared)
    $global:B11Mockbinary=Join-Path $global:B11Mockfixture 'RelWithDebInfo/test_participant_window_remediation.exe'
    $null=[IO.Directory]::CreateDirectory((Split-Path $global:B11Mockbinary))
    [IO.File]::WriteAllText($global:B11Mockbinary,'inert binary; never executed')
    $global:B11MockprofilePath=Join-Path $global:B11Mockfixture 'profile.json'
    $global:B11MockmanifestPath=Join-Path $global:B11Mockfixture 'input-freeze.json'
    $global:B11MockserviceUrl='ws://203.0.113.42:18080'
    $global:B11Mockremote=[ordered]@{room='soak-render-20261008T000000Z-fixture';
        remote_directory='/tmp/soak-render-20261008T000000Z-fixture';
        publisher_pid=17017;publisher_start_ticks='1234567';publisher_kind='load_test'}
    $global:B11Mockprofile=[ordered]@{
        schema=1;scope='B11_GRID16_PREFLIGHT';probe_mode='grid16_transport';
        build_configuration='RelWithDebInfo';target=[ordered]@{
            instance='i-b11offline';service_url=$global:B11MockserviceUrl;
            local_service_url='http://127.0.0.1:18080';
            config_path='/root/b11-fixture/livekit.yaml';sfu_container='livekit-b11-fixture'};
        publisher=[ordered]@{kind='load_test';count=17;subscribers=0;resolution='low';
            codec='vp8';duration='15m';num_per_second=5;simulcast=$true};
        probe=[ordered]@{observation_seconds=300;stall_seconds=20;max_wall_seconds=420}
    }
    Write-Json $global:B11MockprofilePath $global:B11Mockprofile
    Write-Json $global:B11MockmanifestPath @{schema=1;scope='B11_GRID16_PREFLIGHT';
        status='FROZEN_PREFLIGHT_INPUTS';runtime_status='NOT_RUN';profile=$global:B11Mockprofile;
        binary=@{path=$global:B11Mockbinary;configuration='RelWithDebInfo';sha256=('a'*64)}}
    $global:B11Mockoptions=@{PreparedDirectory=$global:B11Mockprepared;Instance='i-b11offline';
        ServiceUrl=$global:B11MockserviceUrl;Python='Invoke-MockB11Python';Binary=$global:B11Mockbinary;
        Grid16Transport=$true;Profile=$global:B11MockprofilePath;InputManifest=$global:B11MockmanifestPath;
        RemoteConfigPath='/root/b11-fixture/livekit.yaml';
        RemoteServiceUrl='http://127.0.0.1:18080';SfuContainer='livekit-b11-fixture'}
    $env:LIVEKIT_URL='ws://preserved.invalid:17777'
    $env:LIVEKIT_SOAK_TOKEN='preserved-token-canary'
    $env:LIVEKIT_SOAK_ALLOW_INSECURE='preserved-insecure-canary'
    Set-Location -LiteralPath $global:B11Mockfixture
    $global:B11MockentryLocation=(Get-Location).Path
}
function Use-SshFixture {
    # Inert local fixtures are consumed only by mocks; no key parser or SSH tool runs.
    $global:B11MocktargetConfigPath=Join-Path $global:B11Mockfixture 'ssh-target.json'
    $key=Join-Path $global:B11Mockfixture 'inert-key.pem'
    $knownHosts=Join-Path $global:B11Mockfixture 'inert-known-hosts'
    [IO.File]::WriteAllText($key,'inert mock identity; never read or uploaded')
    [IO.File]::WriteAllText($knownHosts,'inert mock known hosts; never used by SSH')
    Write-Json $global:B11MocktargetConfigPath @{schema=1;transport='ssh';host='203.0.113.42';
        port=22;user='root';key_path=$key;known_hosts_path=$knownHosts}
    $global:B11Mockprofile['transport']='ssh'
    $global:B11Mockprofile['target_config']=$global:B11MocktargetConfigPath
    $global:B11Mockprofile.target.instance='ins-b11offline'
    $global:B11Mockoptions.Instance='ins-b11offline'
    $global:B11Mockoptions['TargetConfig']=$global:B11MocktargetConfigPath
    Write-Json $global:B11MockprofilePath $global:B11Mockprofile
    $manifest=Get-Content -LiteralPath $global:B11MockmanifestPath -Raw|ConvertFrom-Json
    $manifest.profile=$global:B11Mockprofile
    $manifest|Add-Member -NotePropertyName inputs -NotePropertyValue @{
        remote_transport_identity=@{transport='ssh';config_sha256=('b'*64);
            known_hosts_sha256=('c'*64);key_public_fingerprint=('SHA256:'+('A'*43))}}
    Write-Json $global:B11MockmanifestPath $manifest
}
function Assert-Restored {
    Check ((Get-Location).Path -ceq $global:B11MockentryLocation) 'caller location restored'
    Check ($env:LIVEKIT_URL -ceq 'ws://preserved.invalid:17777') 'caller URL restored'
    Check ($env:LIVEKIT_SOAK_TOKEN -ceq 'preserved-token-canary') 'caller token restored'
    Check ($env:LIVEKIT_SOAK_ALLOW_INSECURE -ceq 'preserved-insecure-canary') 'caller insecure switch restored'
}
function Invoke-Runner {
    $global:B11MockrunError=$null;$global:B11MockrunOutput=@()
    try{$global:B11MockrunOutput=@(& $runner @global:B11Mockoptions)}catch{$global:B11MockrunError=$_.Exception.Message}
}
function Assert-RejectedBeforeCloud {
    Check ($null -ne $global:B11MockrunError) 'invalid input rejected'
    Check ($global:B11MockcloudCalls.Count -eq 0 -and $global:B11MockprobeCalls -eq 0) 'rejection precedes cloud and probe'
    Check ($global:B11MockremoteCliCalls.Count -eq 0) 'rejection precedes SSH transport'
    Check ([RenderProbePowerState]::Calls.Count -eq 0) 'rejection precedes power state mutation'
    Assert-Restored
}
function Invoke-MockB11Python {
    $arguments=@($args | ForEach-Object {[string]$_})
    $global:B11MockpythonCalls.Add($arguments);$global:LASTEXITCODE=0
    if(@($arguments | Where-Object {$_ -like '*b11_input_freeze.py'}).Count -eq 1){
        $global:B11Mockevents.Add('freeze')
        Check ($arguments -contains 'verify' -and $arguments -contains '--require-remote') 'remote-bound freeze verification requested'
        Check ((Arg-Value $arguments '--manifest') -ceq $global:B11MockmanifestPath) 'manifest path forwarded exactly'
        Check ((Arg-Value $arguments '--executable') -ceq $global:B11Mockoptions.Binary) 'frozen executable binding forwarded exactly'
        Check ((Arg-Value $arguments '--profile') -ceq $global:B11MockprofilePath) 'profile path forwarded exactly'
        Check ((Arg-Value $arguments '--instance') -ceq $global:B11Mockoptions.Instance) 'instance bound during verification'
        Check ((Arg-Value $arguments '--service-url') -ceq $global:B11Mockoptions.ServiceUrl) 'service URL bound during verification'
        if($global:B11Mockoptions.ContainsKey('TargetConfig')){
            Check ((Arg-Value $arguments '--transport') -ceq 'ssh') 'SSH transport bound during verification'
            Check ((Arg-Value $arguments '--target-config') -ceq $global:B11Mockoptions.TargetConfig) 'SSH target config bound during verification'
            if($global:B11Mockprofile.target_config -cne $global:B11Mockoptions.TargetConfig){
                $global:LASTEXITCODE=2;return
            }
        }
        if($global:B11Mockmode -eq 'freeze_failure'){$global:LASTEXITCODE=2;return}
        return Get-Content -LiteralPath $global:B11MockmanifestPath -Raw
    }
    if(@($arguments | Where-Object {$_ -like '*b11_remote.py'}).Count -eq 1){
        $global:B11MockremoteCliCalls.Add($arguments)
        Check ((Arg-Value $arguments '--target-config') -ceq $global:B11Mockoptions.TargetConfig) 'every SSH operation uses exact bound target config'
        Check ((Arg-Value $arguments '--expected-config-sha256') -ceq ('b'*64)) 'SSH operation freezes config hash'
        Check ((Arg-Value $arguments '--expected-known-hosts-sha256') -ceq ('c'*64)) 'SSH operation freezes trusted host hash'
        Check ((Arg-Value $arguments '--expected-key-public-fingerprint') -ceq ('SHA256:'+('A'*43))) 'SSH operation freezes authentication identity'
        $scriptIndex=0
        while($arguments[$scriptIndex] -notlike '*b11_remote.py'){$scriptIndex++}
        $operation=$arguments[$scriptIndex+1]
        $command=$null;$source=$null;$destination=$null
        switch($operation){
            exec {
                $commandFile=Arg-Value $arguments '--command-file'
                Check ([bool]$commandFile -and (Test-Path -LiteralPath $commandFile -PathType Leaf)) 'SSH exec supplies a real command file'
                $global:B11MockcommandFiles.Add($commandFile)
                $commandBytes=[IO.File]::ReadAllBytes($commandFile)
                Check (!($commandBytes.Length -ge 3 -and $commandBytes[0] -eq 239 -and $commandBytes[1] -eq 187 -and $commandBytes[2] -eq 191)) 'SSH command has no UTF8 BOM'
                Check (!($commandBytes -contains 13)) 'SSH command uses LF line endings'
                $command=Get-Content -LiteralPath $commandFile -Raw
            }
            upload {$source=Arg-Value $arguments '--local-path';$destination=Arg-Value $arguments '--remote-path'}
            download {$source=Arg-Value $arguments '--remote-path';$destination=Arg-Value $arguments '--local-path'}
            default {throw 'UNEXPECTED_SSH_OPERATION'}
        }
        $stdout=Invoke-MockB11Transport $operation $source $destination $command
        if($global:LASTEXITCODE -ne 0){
            return @{ok=$false;operation=$operation;reason='ssh_operation_failed'}|ConvertTo-Json -Compress
        }
        return @{ok=$true;operation=$operation;returncode=0;stdout=[string]$stdout}|ConvertTo-Json -Compress
    }
    if(@($arguments | Where-Object {$_ -like '*meeting_render_probe.py'}).Count -eq 1){
        $global:B11Mockevents.Add('probe');$global:B11MockprobeCalls++
        Check ((Arg-Value $arguments '--executable') -ceq $global:B11Mockbinary) 'actual probe receives exact verified executable'
        Check ($arguments -contains '--grid16-transport') 'selected grid16 mode reaches probe'
        Check ($env:LIVEKIT_URL -ceq $global:B11MockserviceUrl) 'endpoint reaches observer credentials'
        Check ($env:LIVEKIT_SOAK_TOKEN -ceq 'observer-token-canary') 'observer credential remains in memory'
        if($global:B11Mockmode -eq 'probe_failure'){$global:LASTEXITCODE=1}
        return
    }
    throw 'UNEXPECTED_PYTHON_INVOCATION'
}
function Invoke-MockB11Transport([string]$Operation,[string]$Source,[string]$Destination,[string]$Command) {
    switch($Operation){
        upload {$global:B11Mockevents.Add('upload');return}
        download {
            $global:B11Mockevents.Add('download')
            if($Source.EndsWith('/observer.json')){
                $url=if($global:B11Mockmode -eq 'credential_mismatch'){'ws://other.invalid:18080'}else{$global:B11MockserviceUrl}
                Write-Json $Destination @{LIVEKIT_URL=$url;LIVEKIT_SOAK_TOKEN='observer-token-canary';LIVEKIT_SOAK_ALLOW_INSECURE='1'}
            }else{
                $null=[IO.Directory]::CreateDirectory((Split-Path $Destination))
                [IO.File]::WriteAllText($Destination,'inert resource evidence')
            }
            return
        }
        exec {
            if($command -match 'publisher\s*=\s*subprocess\.Popen'){
                $global:B11Mockevents.Add('setup')
                Check ($command.Contains($global:B11Mockoptions.ServiceUrl)) 'public endpoint reaches remote credential generation'
                Check ($command.Contains($global:B11Mockoptions.RemoteServiceUrl)) 'configured loopback port reaches remote publisher environment'
                Check ($command.Contains($global:B11Mockoptions.RemoteConfigPath)) 'configured SFU key path reaches remote setup'
                Check (!$command.Contains('123.56.225.164')) 'legacy cloud address absent from generated remote command'
                if($global:B11Mockoptions.ContainsKey('TargetConfig')){
                    Check ($command.Contains("['sudo', '-n', 'docker']")) 'SSH setup uses noninteractive sudo only for Docker reads'
                    Check ($command.Contains("pathlib.Path.home() / '.local/bin/lk'")) 'SSH setup resolves user-local lk installation'
                    Check ($command.Contains("lk_path, 'load-test'") -and $command.Contains("lk_path, 'token'")) 'SSH load and token use bound absolute lk path'
                }
                if($global:B11Mockmode -eq 'setup_failure'){$global:LASTEXITCODE=1;return}
                return $global:B11Mockremote | ConvertTo-Json -Compress
            }
            if($command.Contains('{{.State.Pid}}')){
                $global:B11Mockevents.Add('sampler_start')
                Check ($command.Contains($global:B11Mockoptions.SfuContainer)) 'resource sampler selects configured SFU container'
                if($global:B11Mockoptions.ContainsKey('TargetConfig')){
                    Check ($command.Contains("['sudo', '-n', 'docker']")) 'SSH sampler inspects Docker through noninteractive sudo'
                }
                $ticks=if($global:B11Mockmode -eq 'sampler_identity_invalid'){"123';raise RuntimeError('injection')#"}else{'1234568'}
                return @{pid=17018;start_ticks=$ticks;sfu_pid=17019}|ConvertTo-Json -Compress
            }
            if($command.Contains('publisher_stopped')){
                $global:B11Mockevents.Add('publisher_cleanup');$global:B11MockpublisherCleanup++
                Check ($command.Contains('time.monotonic() + grace') -and $command.Contains("result['stopped'] = current_command() is None")) 'publisher cleanup verifies exit after bounded signal waits'
                return @{publisher_stopped=$true;credential_removed=$true}|ConvertTo-Json -Compress
            }
            if($command -match "'owned'\s*:\s*False"){
                $global:B11Mockevents.Add('sampler_cleanup');$global:B11MocksamplerCleanup++
                Check ($command.Contains('time.monotonic() + grace') -and $command.Contains("result['stopped'] = current_command() is None")) 'sampler cleanup verifies exit after bounded signal waits'
                if($global:B11Mockmode -eq 'sampler_cleanup_failure'){throw 'MOCK_SAMPLER_CLEANUP_FAILURE'}
                if($global:B11Mockmode -eq 'sampler_stop_timeout'){return @{owned=$true;stopped=$false;reason='stop_timeout'}|ConvertTo-Json -Compress}
                return @{owned=$true;stopped=$true}|ConvertTo-Json -Compress
            }
            throw 'UNEXPECTED_REMOTE_COMMAND'
        }
        default {throw 'UNEXPECTED_CLOUD_OPERATION'}
    }
}
function workbench {
    $arguments=@($args | ForEach-Object {[string]$_})
    $global:B11MockcloudCalls.Add($arguments);$global:LASTEXITCODE=0
    Check ((Arg-Value $arguments '-i') -ceq $global:B11Mockoptions.Instance) 'every cloud operation targets bound instance'
    Invoke-MockB11Transport $arguments[0] $arguments[1] $arguments[2] (Arg-Value $arguments '-c')
}
$failure=$null
try {
    Case 'grid16_requires_frozen_manifest_before_side_effects' {
        New-Fixture 'missing-manifest';$global:B11Mockoptions.Remove('InputManifest');Invoke-Runner
        Assert-RejectedBeforeCloud
    }
    Case 'service_and_remote_identifiers_reject_unsafe_values' {
        foreach($entry in @(
            @{name='service';parameter='ServiceUrl';value='ws://user:secret@203.0.113.42:18080'},
            @{name='config';parameter='RemoteConfigPath';value="/root/livekit.yaml';raise RuntimeError('injection')#"},
            @{name='container';parameter='SfuContainer';value="livekit';raise RuntimeError('injection')#"},
            @{name='api';parameter='RemoteServiceUrl';value='http://user:secret@127.0.0.1:18080'})){
            New-Fixture ('unsafe-'+$entry.name);$global:B11Mockoptions[$entry.parameter]=$entry.value
            Invoke-Runner;Assert-RejectedBeforeCloud
        }
    }
    Case 'freeze_failure_never_starts_remote_load' {
        New-Fixture 'freeze-rejected' 'freeze_failure';Invoke-Runner;Assert-RejectedBeforeCloud
        Check ($global:B11Mockevents.Count -eq 1 -and $global:B11Mockevents[0] -ceq 'freeze') 'one validation attempt only'
    }
    Case 'frozen_publish_mode_cannot_be_overridden_at_launch' {
        New-Fixture 'publish-mode-mismatch';$global:B11Mockoptions.NoSimulcast=$true
        Invoke-Runner;Assert-RejectedBeforeCloud
    }
    Case 'frozen_remote_configuration_cannot_be_overridden_at_launch' {
        New-Fixture 'remote-config-mismatch';$global:B11Mockoptions.RemoteConfigPath='/root/other/livekit.yaml'
        Invoke-Runner;Assert-RejectedBeforeCloud
    }
    Case 'successful_probe_forwards_binary_and_restores_caller_state' {
        New-Fixture 'success';Invoke-Runner
        Check ($null -eq $global:B11MockrunError) ('runner succeeded: '+$global:B11MockrunError)
        Check ($global:B11Mockevents[0] -ceq 'freeze') 'freeze precedes cloud setup'
        Check ($global:B11MockprobeCalls -eq 1 -and $global:B11MockpublisherCleanup -eq 1 -and $global:B11MocksamplerCleanup -eq 1) 'one probe and owned cleanup'
        Check ([RenderProbePowerState]::Calls.Count -eq 2 -and [RenderProbePowerState]::Calls[1] -eq [Convert]::ToUInt32('80000000',16)) 'power requirement released'
        Check (@(Get-ChildItem -LiteralPath $global:B11Mockprepared -Recurse -Filter '*credential.json').Count -eq 0) 'downloaded credential deleted'
        Check (!($global:B11MockrunOutput -join "`n").Contains('observer-token-canary')) 'token absent from runner output'
        Assert-Restored
    }
    Case 'ssh_target_routes_all_remote_operations_and_removes_command_files' {
        New-Fixture 'ssh-success';Use-SshFixture;Invoke-Runner
        Check ($null -eq $global:B11MockrunError) ('SSH runner succeeded: '+$global:B11MockrunError)
        Check ($global:B11MockcloudCalls.Count -eq 0) 'SSH path never invokes workbench'
        Check ($global:B11Mockevents[0] -ceq 'freeze') 'SSH freeze precedes remote setup'
        Check ($global:B11MockprobeCalls -eq 1 -and $global:B11MockpublisherCleanup -eq 1 -and $global:B11MocksamplerCleanup -eq 1) 'SSH probe and owned cleanup each run once'
        Check ($global:B11MockremoteCliCalls.Count -eq 9) 'SSH handles four execs, one upload and four downloads'
        Check ($global:B11MockcommandFiles.Count -eq 4) 'SSH exec routes each remote script through a file'
        foreach($path in $global:B11MockcommandFiles){
            Check (!(Test-Path -LiteralPath $path)) 'SSH command tempfile deleted'
        }
        Check (@(Get-ChildItem -LiteralPath $global:B11Mockprepared -Recurse -Filter '*credential.json').Count -eq 0) 'SSH observer credential deleted'
        Check (!($global:B11MockrunOutput -join "`n").Contains('observer-token-canary')) 'SSH token absent from runner output'
        Assert-Restored
    }
    Case 'ssh_target_config_must_match_frozen_profile_before_remote_operations' {
        New-Fixture 'ssh-target-mismatch';Use-SshFixture
        $global:B11Mockprofile['target_config']=Join-Path $global:B11Mockfixture 'other-target.json'
        Write-Json $global:B11MockprofilePath $global:B11Mockprofile
        Invoke-Runner;Assert-RejectedBeforeCloud
        Check ($global:B11Mockevents.Count -eq 1 -and $global:B11Mockevents[0] -ceq 'freeze') 'SSH mismatch checked during freeze before any transport call'
    }
    Case 'ssh_cleanup_timeout_propagates_and_preserves_other_owner_cleanup' {
        New-Fixture 'ssh-cleanup-timeout' 'sampler_stop_timeout';Use-SshFixture;Invoke-Runner
        Check ($null -ne $global:B11MockrunError) 'SSH cleanup timeout remains failure'
        Check ($global:B11MocksamplerCleanup -eq 1 -and $global:B11MockpublisherCleanup -eq 1) 'SSH timeout does not skip publisher cleanup'
        Check ($global:B11MockcloudCalls.Count -eq 0) 'SSH timeout never falls back to workbench'
        foreach($path in $global:B11MockcommandFiles){Check (!(Test-Path -LiteralPath $path)) 'SSH timeout removes command tempfile'}
        Check ([RenderProbePowerState]::Calls.Count -eq 2) 'SSH timeout restores power state'
        Assert-Restored
    }
    Case 'probe_failure_still_stops_owned_load_and_restores_state' {
        New-Fixture 'probe-failure' 'probe_failure';Invoke-Runner
        Check ($null -ne $global:B11MockrunError -and $global:B11MockprobeCalls -eq 1) 'probe failure propagated'
        Check ($global:B11MockpublisherCleanup -eq 1 -and $global:B11MocksamplerCleanup -eq 1) 'failure executes all owned cleanup'
        Assert-Restored
    }
    Case 'sampler_cleanup_exception_does_not_skip_publisher_or_state_restore' {
        New-Fixture 'sampler-cleanup-failure' 'sampler_cleanup_failure';Invoke-Runner
        Check ($global:B11MocksamplerCleanup -eq 1 -and $global:B11MockpublisherCleanup -eq 1) 'cleanup failure isolated from other owner'
        Check ([RenderProbePowerState]::Calls.Count -eq 2) 'power restored despite cleanup failure'
        Assert-Restored
    }
    Case 'downloaded_endpoint_mismatch_blocks_probe_and_cleans_load' {
        New-Fixture 'credential-mismatch' 'credential_mismatch';Invoke-Runner
        Check ($null -ne $global:B11MockrunError -and $global:B11MockprobeCalls -eq 0) 'credential URL binding rejected before UI launch'
        Check ($global:B11MockpublisherCleanup -eq 1 -and $global:B11MocksamplerCleanup -eq 1) 'load cleaned after credential rejection'
        Check (@(Get-ChildItem -LiteralPath $global:B11Mockprepared -Recurse -Filter '*credential.json').Count -eq 0) 'mismatched credential removed'
        Assert-Restored
    }
    Case 'invalid_sampler_identity_never_enters_remote_cleanup_code' {
        New-Fixture 'sampler-identity-invalid' 'sampler_identity_invalid';Invoke-Runner
        Check ($null -ne $global:B11MockrunError -and $global:B11MockprobeCalls -eq 0) 'invalid sampler rejected before UI launch'
        Check ($global:B11MocksamplerCleanup -eq 0 -and $global:B11MockpublisherCleanup -eq 1) 'untrusted sampler identity discarded while owned publisher is cleaned'
        Check (@($global:B11MockcloudCalls | Where-Object {($_ -join "`n").Contains("raise RuntimeError('injection')")}).Count -eq 0) 'untrusted process metadata never inserted in a remote command'
        Assert-Restored
    }
    Case 'remote_setup_failure_never_launches_observer' {
        New-Fixture 'setup-failure' 'setup_failure';Invoke-Runner
        Check ($null -ne $global:B11MockrunError -and $global:B11MockprobeCalls -eq 0) 'remote setup failure blocks observer'
        Check ([RenderProbePowerState]::Calls.Count -eq 2) 'setup failure restores power requirement'
        Assert-Restored
    }
}catch{$failure=$_.Exception.Message; if($global:B11MockrunError){$failure += '; runner='+$global:B11MockrunError}; $failure += '; events='+($global:B11Mockevents -join ',')}finally{
    Set-Location -LiteralPath $initialLocation
    foreach($name in $initialEnv.Keys){[Environment]::SetEnvironmentVariable($name,$initialEnv[$name],'Process')}
}
$summary=[ordered]@{schema=1;kind='b11_runner_offline_contract';status=$(if($failure){'FAIL'}else{'PASS'});
    cases=$global:B11Mockcases;failure=$failure;runtime_status='NOT_RUN';
    scope='Actual runner with mocked workbench and SSH CLI, Python, native power state and inert binary; no cloud or UI launch'}
Write-Json (Join-Path $testRoot 'selftest-result.json') $summary
$summary|ConvertTo-Json -Depth 6
Get-Variable -Name 'B11Mock*' -Scope Global | Remove-Variable -Scope Global
if($failure){exit 1}
