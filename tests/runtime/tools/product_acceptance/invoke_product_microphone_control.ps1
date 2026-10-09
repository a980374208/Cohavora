param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$PreparedDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [switch]$Worker
)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../../../..')).Path
$driver=Join-Path $repo 'tests/uia/product_desktop.ps1'
$Executable=(Resolve-Path -LiteralPath $Executable).Path
$PreparedDirectory=(Resolve-Path -LiteralPath $PreparedDirectory).Path
if (!$Worker) {
    if (Test-Path -LiteralPath $OutputDirectory) {throw 'DIAGNOSTIC_DESTINATION_MUST_BE_NEW'}
    if (Get-Process -Name Cohavora -ErrorAction SilentlyContinue) {throw 'EXISTING_PRODUCT_PROCESS_PRESENT'}
    $null=New-Item -ItemType Directory -Path $OutputDirectory
    $OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
    $clock=[Diagnostics.Stopwatch]::StartNew()
    $control=$null;$owned=$null;$forced=$false;$cleanupError=$null;$workerCode=$null
    $run=[guid]::NewGuid().ToString('N')
    [ordered]@{schema=1;run_id=$run;configuration='RelWithDebInfo';
        executable=$Executable;executable_sha256=(Get-FileHash $Executable -Algorithm SHA256).Hash.ToLowerInvariant();
        driver_sha256=(Get-FileHash $driver -Algorithm SHA256).Hash.ToLowerInvariant();
        started_utc=[DateTime]::UtcNow.ToString('o');maximum_total_seconds=120;
        microphone_observation_seconds=45;independent_997hz='EXCLUDED_BY_USER';
        complete_load=$false;remote_video_paging='EXCLUDED_DIAGNOSTIC_HAS_NO_REMOTE_LOAD';formal_acceptance=$false} | ConvertTo-Json |
        Set-Content (Join-Path $OutputDirectory 'control-inputs.json') -Encoding UTF8
    try {
        $shell=Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
        $control=Start-Process -FilePath $shell -WindowStyle Hidden -PassThru -ArgumentList @(
            '-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$PSCommandPath+'"'),
            '-Executable',('"'+$Executable+'"'),'-PreparedDirectory',('"'+$PreparedDirectory+'"'),
            '-OutputDirectory',('"'+$OutputDirectory+'"'),'-Worker') -RedirectStandardOutput (Join-Path $OutputDirectory 'worker.stdout') -RedirectStandardError (Join-Path $OutputDirectory 'worker.stderr')
        $null=$control.Handle
        while (!$control.WaitForExit(250)) {
            if (!$owned -and (Test-Path (Join-Path $OutputDirectory 'product-identity.json'))) {
                $identity=Get-Content (Join-Path $OutputDirectory 'product-identity.json') -Raw | ConvertFrom-Json
                $candidate=Get-Process -Id $identity.pid -ErrorAction SilentlyContinue
                if ($identity.run_id -eq $run -and $candidate -and $candidate.Path -eq $Executable -and
                    $candidate.StartTime.ToUniversalTime().Ticks -eq [long]$identity.start_ticks) {
                    $null=$candidate.Handle;$owned=$candidate
                } else {throw 'DIAGNOSTIC_PRODUCT_IDENTITY_NOT_CONFIRMED'}
            }
            if ($clock.Elapsed.TotalSeconds -ge 105) {$forced=$true;break}
        }
    } finally {
        # Retained Process handles prevent a PID reuse from targeting another run.
        if ($control -and !$control.HasExited) {$control.Kill();$null=$control.WaitForExit(3000)}
        if (!$owned -and (Test-Path (Join-Path $OutputDirectory 'product-identity.json'))) {
            $identity=Get-Content (Join-Path $OutputDirectory 'product-identity.json') -Raw | ConvertFrom-Json
            $candidate=Get-Process -Id $identity.pid -ErrorAction SilentlyContinue
            if ($identity.run_id -eq $run -and $candidate -and $candidate.Path -eq $Executable -and
                $candidate.StartTime.ToUniversalTime().Ticks -eq [long]$identity.start_ticks) {
                $null=$candidate.Handle;$owned=$candidate
            } elseif ($candidate) {$cleanupError='PRODUCT_IDENTITY_CHANGED_NOT_TERMINATED'}
        }
        if (!$owned -and (Get-Process -Name Cohavora -ErrorAction SilentlyContinue)) {
            $cleanupError='UNCONFIRMED_PRODUCT_IDENTITY_NOT_TERMINATED'
        }
        $productForced=$false
        if ($owned -and !$owned.HasExited) {$owned.Kill();$null=$owned.WaitForExit(5000);$productForced=$true}
        $workerCode=if($control -and $control.HasExited){$control.ExitCode}else{$null}
        [ordered]@{finished_utc=[DateTime]::UtcNow.ToString('o');elapsed_seconds=$clock.Elapsed.TotalSeconds;
            worker_forced=$forced;worker_exit_code=$workerCode;
            product_forced=$productForced;product_absent=(!$cleanupError -and (!$owned -or $owned.HasExited));
            cleanup_error=$cleanupError;formal_acceptance=$false} | ConvertTo-Json |
            Set-Content (Join-Path $OutputDirectory 'supervisor-result.json') -Encoding UTF8
        if ($owned) {$owned.Dispose()};if($control){$control.Dispose()}
    }
    if ($forced -or $cleanupError -or $workerCode -ne 0 -or $productForced) {exit 1}
    $result=Get-Content (Join-Path $OutputDirectory 'microphone-result.json') -Raw | ConvertFrom-Json
    if ($result.outcome -ne 'ON_OBSERVED' -or $result.failure -or $result.cleanup_error -or $result.product_exit_code -ne 0) {exit 1}
    exit 0
}

$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
$inputRecord=Get-Content (Join-Path $OutputDirectory 'control-inputs.json') -Raw | ConvertFrom-Json
if ((Get-FileHash $Executable -Algorithm SHA256).Hash.ToLowerInvariant() -ne $inputRecord.executable_sha256) {throw 'DIAGNOSTIC_EXECUTABLE_CHANGED'}
if ((Get-FileHash $driver -Algorithm SHA256).Hash.ToLowerInvariant() -ne $inputRecord.driver_sha256) {throw 'DIAGNOSTIC_DRIVER_CHANGED'}
$env:PSModulePath=Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/Modules'
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes
. (Join-Path $repo 'tests/uia/product_desktop_evidence.ps1')
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($driver,[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'DIAGNOSTIC_DRIVER_PARSE_FAILED'}
$typeCommands=@($ast.FindAll({param($n) $n -is [Management.Automation.Language.CommandAst] -and
    $n.GetCommandName() -eq 'Add-Type' -and $n.Extent.Text.Contains('public static class ProductDesktop')},$false))
if($typeCommands.Count -ne 1){throw 'DIAGNOSTIC_DRIVER_TYPE_AMBIGUOUS'}
Invoke-Expression $typeCommands[0].Extent.Text
$definitions=@($ast.FindAll({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst]},$false))
foreach($definition in $definitions){Invoke-Expression $definition.Extent.Text}
$cycleDefinition=@($definitions | Where-Object {$_.Name -eq 'Run-Cycle'})
$joinCommand=$cycleDefinition[0].Body.EndBlock.Statements[0].PipelineElements[0]
if($joinCommand.GetCommandName() -ne 'Action' -or $joinCommand.CommandElements[1].Value -ne 'join') {throw 'DIAGNOSTIC_JOIN_BLOCK_CHANGED'}
$joinStatements=@($joinCommand.CommandElements[2].ScriptBlock.EndBlock.Statements)
if($joinStatements.Count -ne 18 -or
    !$joinStatements[11].Extent.Text.Contains("Wait-For 'remote video paging controls'") -or
    !$joinStatements[12].Extent.Text.StartsWith('$layoutPattern = Require-Pattern') -or
    !$joinStatements[13].Extent.Text.StartsWith('$script:layout = $layoutPattern.Current.Value')) {
    throw 'DIAGNOSTIC_JOIN_PAGING_BOUNDARY_CHANGED'
}
# This control has no remote load. Retain product join policy, camera and
# local startup readiness, but omit the three remote pagination statements.
$joinText=(@(for($i=0;$i -lt $joinStatements.Count;$i++) {
    if($i -notin @(11,12,13)){$joinStatements[$i].Extent.Text}
}) -join "`n")
$joinBody=[scriptblock]::Create($joinText)
$HeapDiagnostic=$false;$IsolateUiaCycles=$false;$ProbeOnly=$false
$DedicatedDesktopSessionId=(Get-Process -Id $PID).SessionId
$DesktopInputPolicy='diagnostic'
$script:runId=$inputRecord.run_id;$script:cycle=1;$script:cycleId=[guid]::NewGuid().ToString('N')
$script:operation='';$script:step='bounded-microphone';$script:child=$null
$script:rootCache=@{};$script:lastResourceSample=[DateTime]::MinValue;$script:layout='unknown'
$script:runClock=[Diagnostics.Stopwatch]::StartNew();$script:productOwnedIdentity=$null
$script:productFirstLive=$null;$script:productFirstLiveIdentityFailure=$null
$script:nativeProcessRun=$null;$script:nativeSession=$null;$script:participantHash=$null
$script:desktopBaseline=Assert-DedicatedDesktop $DedicatedDesktopSessionId -InputPolicy 'diagnostic'
$script:desktopBaseline | ConvertTo-Json | Set-Content (Join-Path $OutputDirectory 'desktop-baseline.json') -Encoding UTF8
foreach($name in @('LIVEKIT_UIA_REMOTE_CONTEXT','LIVEKIT_UIA_PILOT_PROBE','LIVEKIT_UIA_GPU_ETW_DIRECTORY','LIVEKIT_UIA_GPU_BUDGET_PROBE')) {
    [Environment]::SetEnvironmentVariable($name,$null,'Process')
}
$setup=Get-Content (Join-Path $PreparedDirectory 'setup.json') -Raw | ConvertFrom-Json
if($setup.status -ne 'PREPARED' -or $setup.meeting_id -cnotmatch '^[0-9]{9}$'){throw 'DIAGNOSTIC_PREPARED_INVALID'}
$secret=(Get-Content (Join-Path $PreparedDirectory 'password.dpapi') -Raw).Trim() | ConvertTo-SecureString
$env:LIVEKIT_UIA_ACCOUNT=$setup.account
$env:LIVEKIT_UIA_PASSWORD=[Net.NetworkCredential]::new('',$secret).Password
$env:LIVEKIT_UIA_MEETING_ID=$setup.meeting_id;$env:LIVEKIT_UIA_SERVICE_URL=$setup.service_url
$script:micOutcome='NOT_REACHED';$script:micSubmitted=$false;$script:micInitial=$null;$script:micFinal=$null
Set-Item -Path function:Toggle-To-Original -Value ${function:Toggle-To}
function Toggle-To([string]$Id,[Windows.Automation.ToggleState]$Expected) {
    if($Id -ne 'meetingMicrophone' -or $Expected -ne [Windows.Automation.ToggleState]::On) {
        Toggle-To-Original $Id $Expected;return
    }
    $node=Wait-For "$Id available" {Find-Node $Id ([Windows.Automation.ControlType]::CheckBox) -Optional} 15
    $pattern=Require-Pattern $node ([Windows.Automation.TogglePattern]::Pattern)
    $script:micInitial=[string]$pattern.Current.ToggleState
    [ordered]@{utc=[DateTime]::UtcNow.ToString('o');phase='before';state=$script:micInitial;
        enabled=$node.Current.IsEnabled;pid=$script:child.Id} | ConvertTo-Json -Compress |
        Add-Content (Join-Path $OutputDirectory 'microphone-states.jsonl') -Encoding UTF8
    if($pattern.Current.ToggleState -ne $Expected) {
        $script:micSubmitted=$true
        $pattern.Toggle() # Never resubmit an action with an uncertain result.
    }
    $observation=[Diagnostics.Stopwatch]::StartNew()
    do {
        if($script:child.HasExited){throw 'DIAGNOSTIC_PRODUCT_EXIT_DURING_MICROPHONE'}
        $control=Find-Node $Id ([Windows.Automation.ControlType]::CheckBox) -Optional
        $unavailable=Find-Node 'meetingMicrophoneUnavailable' ([Windows.Automation.ControlType]::Window) -Optional
        $state=if($control){[string](Require-Pattern $control ([Windows.Automation.TogglePattern]::Pattern)).Current.ToggleState}else{'MISSING'}
        $script:micFinal=$state
        [ordered]@{utc=[DateTime]::UtcNow.ToString('o');phase='observe';elapsed_seconds=$observation.Elapsed.TotalSeconds;
            state=$state;enabled=$(if($control){$control.Current.IsEnabled}else{$null});
            offscreen=$(if($control){$control.Current.IsOffscreen}else{$null});
            unavailable_dialog=[bool]$unavailable;pid=$script:child.Id} | ConvertTo-Json -Compress |
            Add-Content (Join-Path $OutputDirectory 'microphone-states.jsonl') -Encoding UTF8
        Sample-Resource 'active'
        if($unavailable){$script:micOutcome='PRODUCT_UNAVAILABLE';throw 'DIAGNOSTIC_PRODUCT_MICROPHONE_UNAVAILABLE'}
        Start-Sleep -Milliseconds 1000
    } while($observation.Elapsed.TotalSeconds -lt 45)
    $script:micOutcome=if($script:micFinal -eq 'On'){'ON_OBSERVED'}else{'OFF_AFTER_SINGLE_TOGGLE'}
    if($script:micOutcome -ne 'ON_OBSERVED'){throw 'DIAGNOSTIC_MICROPHONE_NOT_ON'}
}
$failure=$null;$cleanupError=$null
try {
    Start-Product
    Action 'join' $joinBody
} catch {$failure=$_.Exception.Message}
finally {
    [ordered]@{schema=1;run_id=$script:runId;finished_utc=[DateTime]::UtcNow.ToString('o');
        outcome=$script:micOutcome;toggle_submitted=$script:micSubmitted;
        initial_state=$script:micInitial;final_state=$script:micFinal;failure=$failure;
        step=$script:step;formal_acceptance=$false;complete_media_evidence=$false} | ConvertTo-Json |
        Set-Content (Join-Path $OutputDirectory 'microphone-result.json') -Encoding UTF8
    try {
        if($script:child -and !$script:child.HasExited){
            $leave=Find-Node 'meetingLeave' ([Windows.Automation.ControlType]::Button) -Optional
            if($leave){
                (Require-Pattern $leave ([Windows.Automation.InvokePattern]::Pattern)).Invoke()
                $null=Wait-For 'diagnostic leave confirmation' {Find-Node 'meetingLeaveConfirmation' ([Windows.Automation.ControlType]::Window) -Optional} 5
                Invoke 'meetingLeaveConfirm'
                $null=Wait-For 'diagnostic meeting closed' {Test-MeetingWindowClosed} 8
            }
            $main=Wait-For 'diagnostic main after leave' {Find-Node 'MeetingMainWindow' ([Windows.Automation.ControlType]::Window) -Optional} 10
            (Require-Pattern $main ([Windows.Automation.WindowPattern]::Pattern)).Close()
            if(!$script:child.WaitForExit(8000)){throw 'DIAGNOSTIC_GRACEFUL_EXIT_TIMEOUT'}
        }
    } catch {$cleanupError=$_.Exception.Message}
    $result=Get-Content (Join-Path $OutputDirectory 'microphone-result.json') -Raw | ConvertFrom-Json
    $result | Add-Member cleanup_error $cleanupError
    $result | Add-Member product_exit_code $(if($script:child -and $script:child.HasExited){[ProductDesktop]::ExitCode($script:childHandle)}else{$null})
    $result | ConvertTo-Json | Set-Content (Join-Path $OutputDirectory 'microphone-result.json') -Encoding UTF8
    $env:LIVEKIT_UIA_PASSWORD=$null
}
if($failure -or $cleanupError){exit 1}
exit 0
