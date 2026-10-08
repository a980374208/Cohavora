param([string]$OutputDirectory="$PSScriptRoot/../../../out/resource-job-short-test")
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$null=New-Item -ItemType Directory -Path $OutputDirectory -Force
$fixtureRoot=Join-Path $OutputDirectory ([guid]::NewGuid().ToString('N'))
$null=New-Item -ItemType Directory -Path $fixtureRoot
$source=(Resolve-Path (Join-Path $PSScriptRoot '../tools/product_acceptance/product_pilot_resources.ps1')).Path
. (Join-Path $PSScriptRoot '../tools/product_acceptance/product_pilot_probe_tail.ps1')
$stub=Join-Path $fixtureRoot 'resource-audio-stub.exe'
Add-Type -OutputAssembly $stub -OutputType ConsoleApplication -TypeDefinition @'
using System;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading;
public static class ResourceAudioChildStub {
    public static int Main(string[] args) {
        if (args.Length != 4) return 2;
        Process self = Process.GetCurrentProcess();
        string common = "\"schema\":1,\"run_id\":\"" + args[1] + "\",\"pid\":" + self.Id +
            ",\"start_ticks\":" + self.StartTime.ToUniversalTime().Ticks;
        File.AppendAllText(args[2], "{" + common + ",\"event\":\"started\"}\n", new UTF8Encoding(false));
        bool forced = Path.GetDirectoryName(args[2]).EndsWith("forced", StringComparison.Ordinal);
        Thread.Sleep(forced ? 20000 : 500);
        File.AppendAllText(args[2], "{" + common + ",\"event\":\"complete\"}\n", new UTF8Encoding(false));
        return 0;
    }
}
'@
$fixtureProcess=Get-Process -Id $PID
$encoding=[Text.UTF8Encoding]::new($false)
$cases=New-Object 'System.Collections.Generic.List[object]'
foreach($mode in @('normal','forced')){
    $caseDirectory=Join-Path $fixtureRoot $mode
    $null=New-Item -ItemType Directory -Path $caseDirectory
    $runId=[guid]::NewGuid().ToString('N')
    $identity=[ordered]@{run_id=$runId;pid=$fixtureProcess.Id;start_ticks=$fixtureProcess.StartTime.ToUniversalTime().Ticks;executable=$fixtureProcess.Path}
    [IO.File]::WriteAllText((Join-Path $caseDirectory 'product-identity.json'),($identity|ConvertTo-Json -Compress),$encoding)
    $action=[ordered]@{schema=1;run_id=$runId;pid=$fixtureProcess.Id;cycle=1;operation_id=1;action='fixture';phase='completed';utc=[DateTime]::UtcNow.ToString('o')}
    [IO.File]::WriteAllText((Join-Path $caseDirectory 'uia-actions.jsonl'),($action|ConvertTo-Json -Compress)+"`n",$encoding)
    $destination=Join-Path $caseDirectory 'external-resources.jsonl'
    $audioPath=Join-Path $caseDirectory 'product-audio.jsonl'
    $maximum=if($mode -eq 'normal'){4}else{20}
    $owner=$null;$child=$null
    try{
        $owner=Start-Process -FilePath 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe' -WindowStyle Hidden -PassThru -ArgumentList @(
            '-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',('"'+$source+'"'),
            '-UiaDirectory',('"'+$caseDirectory+'"'),'-Destination',('"'+$destination+'"'),
            '-RunId',$runId,'-MaximumSeconds',[string]$maximum,'-AudioCollector',('"'+$stub+'"')) -RedirectStandardOutput (Join-Path $caseDirectory 'owner.stdout') -RedirectStandardError (Join-Path $caseDirectory 'owner.stderr')
        $null=$owner.Handle
        $readyClock=[Diagnostics.Stopwatch]::StartNew()
        $started=$null;$resourceRow=$null
        while(!$started -or !$resourceRow){
            if($readyClock.Elapsed.TotalSeconds -gt 10){throw ('RESOURCE_JOB_FIXTURE_NOT_READY: '+$mode)}
            if(Test-Path -LiteralPath $audioPath){
                $rows=@(Read-ProductPilotCompleteJsonlTail -Path $audioPath -Count 2 | ForEach-Object {$_|ConvertFrom-Json})
                $started=$rows|Where-Object {$_.event -eq 'started'}|Select-Object -First 1
            }
            if((Test-Path -LiteralPath $destination) -and (Get-Item -LiteralPath $destination).Length -gt 0){
                $resourceRows=@(Read-ProductPilotCompleteJsonlTail -Path $destination | ForEach-Object {$_|ConvertFrom-Json})
                if($resourceRows.Count){$resourceRow=$resourceRows[0]}
            }
            if(!$started -or !$resourceRow){
                if($owner.HasExited){throw ('RESOURCE_JOB_FIXTURE_OWNER_EXITED_EARLY: '+$mode)}
                Start-Sleep -Milliseconds 20
            }
        }
        if($started.run_id -cne $runId){throw 'RESOURCE_JOB_CHILD_RECEIPT_WRONG_RUN'}
        $candidate=Get-Process -Id $started.pid -ErrorAction SilentlyContinue
        if($candidate){
            if($candidate.HasExited){$candidate.Dispose();$candidate=$null}
            elseif($candidate.StartTime.ToUniversalTime().Ticks -ne $started.start_ticks -or $candidate.Path -cne $stub){
                $candidate.Dispose();throw 'RESOURCE_JOB_CHILD_IDENTITY_NOT_OWNED'
            }
        }
        $child=$candidate
        if($mode -eq 'forced'){
            if(!$child -or $child.StartTime.ToUniversalTime().Ticks -ne $started.start_ticks -or
                $child.Path -cne $stub){throw 'RESOURCE_JOB_CHILD_IDENTITY_NOT_OWNED'}
            $null=$child.Handle
            $killClock=[Diagnostics.Stopwatch]::StartNew()
            $owner.Kill()
            if(!$owner.WaitForExit(5000) -or !$child.WaitForExit(5000)){throw 'RESOURCE_JOB_CHILD_SURVIVED_OWNER_KILL'}
            $killClock.Stop()
            $final=@(Read-ProductPilotCompleteJsonlTail -Path $audioPath -Count 2 | ForEach-Object {$_|ConvertFrom-Json})
            if(@($final|Where-Object {$_.event -eq 'complete'}).Count){throw 'RESOURCE_JOB_FORCE_CASE_FINISHED_BEFORE_KILL'}
            $cases.Add([ordered]@{mode=$mode;status='PASS';owner_pid=$owner.Id;child_pid=$started.pid;child_start_ticks=$started.start_ticks;
                child_exited=$child.HasExited;kernel_cleanup_elapsed_ms=$killClock.Elapsed.TotalMilliseconds;child_natural_completion=$false})
        }else{
            if(!$owner.WaitForExit(10000)){throw 'RESOURCE_JOB_NORMAL_OWNER_TIMEOUT'}
            $owner.Refresh()
            if($owner.ExitCode -ne 0){throw ('RESOURCE_JOB_NORMAL_OWNER_EXIT: '+$owner.ExitCode)}
            $final=@(Read-ProductPilotCompleteJsonlTail -Path $audioPath -Count 2 | ForEach-Object {$_|ConvertFrom-Json})
            if(@($final|Where-Object {$_.event -eq 'complete'}).Count -ne 1){throw 'RESOURCE_JOB_NORMAL_CHILD_NOT_COMPLETE'}
            if($child -and !$child.WaitForExit(5000)){throw 'RESOURCE_JOB_NORMAL_CHILD_ALIVE'}
            $cases.Add([ordered]@{mode=$mode;status='PASS';owner_pid=$owner.Id;owner_exit_code=$owner.ExitCode;
                child_pid=$started.pid;child_start_ticks=$started.start_ticks;child_natural_completion=$true})
        }
    }finally{
        # Only Process objects for children created by this fixture are touched.
        if($owner -and !$owner.HasExited){$owner.Kill();$null=$owner.WaitForExit(5000)}
        if($child -and !$child.HasExited){$child.Kill();$null=$child.WaitForExit(5000)}
        if($owner){$owner.Dispose()};if($child){$child.Dispose()}
    }
}
$result=[ordered]@{schema=1;status='PASS_SCOPED';powershell_version=$PSVersionTable.PSVersion.ToString();cases=@($cases.ToArray());
    resources_sha256=(Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant();
    test_sha256=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant();
    fixture_root=$fixtureRoot;stub_sha256=(Get-FileHash -LiteralPath $stub -Algorithm SHA256).Hash.ToLowerInvariant();
    boundary='Local stub child/job lifetime only; no product, WASAPI, SDK, service, PILOT, Formal or build of product';formal_credit=0}
[IO.File]::WriteAllText((Join-Path $fixtureRoot 'result.json'),($result|ConvertTo-Json -Depth 7),$encoding)
$result|ConvertTo-Json -Depth 7
