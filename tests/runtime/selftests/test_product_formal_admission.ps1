param([string]$OutputDirectory="$PSScriptRoot/../../../out/b14-repair-20261007")
$ErrorActionPreference='Stop'
$domain=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../tools/product_acceptance'))
. "$domain/product_pilot_admission.ps1"
$invoker=Join-Path $domain 'invoke_product_external.ps1'
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($invoker,[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'PARSE_FAILED'}
$formal=@($ast.EndBlock.Statements | Where-Object {$_ -is [Management.Automation.Language.IfStatementAst] -and $_.Clauses[0].Item1.Extent.Text -eq "$"+"Mode -eq 'Formal'"})
if($formal.Count -ne 1){throw 'FORMAL_BRANCH_NOT_UNIQUE'}
$branch=[scriptblock]::Create($formal[0].Extent.Text)
$directory=[IO.Path]::GetFullPath((Join-Path $OutputDirectory ('admission-'+[guid]::NewGuid().ToString('N'))))
$null=[IO.Directory]::CreateDirectory($directory)
$oldTools=Join-Path $directory 'qualified/RelWithDebInfo'
$newTools=Join-Path $directory 'changed/RelWithDebInfo'
$null=[IO.Directory]::CreateDirectory($oldTools);$null=[IO.Directory]::CreateDirectory($newTools)
$Executable=Join-Path $oldTools 'Cohavora.exe'
$oldAudio=Join-Path $oldTools 'product_audio_loopback.exe'
$oldGpu=Join-Path $oldTools 'product_gpu_trace.exe'
$newAudio=Join-Path $newTools 'product_audio_loopback.exe'
$newGpu=Join-Path $newTools 'product_gpu_trace.exe'
foreach($path in @($Executable,$oldAudio,$oldGpu)){
    [IO.File]::WriteAllText($path,('inert fixture: '+[IO.Path]::GetFileName($path)))
}
Copy-Item -LiteralPath $oldAudio -Destination $newAudio
Copy-Item -LiteralPath $oldGpu -Destination $newGpu
$inputs=[ordered]@{}
foreach($path in @($Executable,$oldAudio,$oldGpu)){$inputs[$path]=(Get-FileHash $path).Hash.ToLowerInvariant()}
$ReleaseGate=Join-Path $directory 'release-gate.json'
$schedulerPolicy=@{sha256=('a'*64)};$DesktopInputPolicy='diagnostic'
$gate=[ordered]@{verdict='READY';historical_crash_regression_closed=$true;full_media_gpu_ready=$true;
    desktop_input_policy=$DesktopInputPolicy;collector_scheduler_policy=$schedulerPolicy;
    product_sha256=$inputs[$Executable];inputs=$inputs}
$Mode='Formal';$evidenceDrive=[IO.Path]::GetPathRoot($directory).Substring(0,1)
$limits=@{minimum_free_disk_bytes=42949672960}
# Only disk availability is stubbed. Execute the complete actual Formal AST;
# all identity, canonical path and SHA checks use real inert files.
function Get-PSDrive {param([string]$Name) [pscustomobject]@{Free=1TB}}
$cases=[Collections.Generic.List[object]]::new()
function Check([string]$Name,[string]$Expected=''){
    $gate|ConvertTo-Json -Depth 6|Set-Content $ReleaseGate -Encoding UTF8
    $failure='';try{& $branch}catch{$failure=$_.Exception.Message}
    if($failure -cne $Expected){throw "CASE_FAILED: $Name expected=$Expected actual=$failure"}
    $cases.Add(@{name=$Name;status='PASS';rejection=$failure})
}
$AudioCollector=$oldAudio;$GpuTraceTool=$oldGpu
Check 'qualified_actual_tools'
$AudioCollector=$newAudio;Check 'same_audio_bytes_new_path' 'FORMAL_TOOL_PATH_CHANGED:audio'
[IO.File]::WriteAllText($newAudio,'unqualified audio')
Check 'changed_audio_path_and_bytes' 'FORMAL_TOOL_PATH_CHANGED:audio'
$AudioCollector=$oldAudio;$GpuTraceTool=$newGpu
Check 'same_gpu_bytes_new_path' 'FORMAL_TOOL_PATH_CHANGED:gpu'
[IO.File]::WriteAllText($newGpu,'unqualified GPU')
Check 'changed_gpu_path_and_bytes' 'FORMAL_TOOL_PATH_CHANGED:gpu'
$GpuTraceTool=$oldGpu;$AudioCollector=$Executable
Check 'listed_product_cannot_be_audio' 'FORMAL_TOOL_PATH_CHANGED:audio'
$AudioCollector=$oldAudio
foreach($role in @(@{name='audio';path=$oldAudio;duplicate=$newAudio},@{name='gpu';path=$oldGpu;duplicate=$newGpu})){
    $saved=$inputs[$role.path];$inputs.Remove($role.path)
    Check ($role.name+'_missing_identity') ('FORMAL_TOOL_IDENTITY_MISSING_OR_AMBIGUOUS:'+$role.name)
    $inputs[$role.path]=$saved;$inputs[$role.duplicate]=(Get-FileHash $role.duplicate).Hash.ToLowerInvariant()
    Check ($role.name+'_ambiguous_identity') ('FORMAL_TOOL_IDENTITY_MISSING_OR_AMBIGUOUS:'+$role.name)
    $inputs.Remove($role.duplicate)
    $original=[IO.File]::ReadAllBytes($role.path)
    [IO.File]::WriteAllText($role.path,'changed in place')
    Check ($role.name+'_changed_in_place') ('FORMAL_TOOL_BYTES_CHANGED:'+$role.name)
    [IO.File]::WriteAllBytes($role.path,$original)
}
Check 'qualified_after_all_negative_cases'
$report=@{schema=1;status='PASS';cases=@($cases.ToArray());runtime_started=$false;hashes=@{}}
foreach($path in @($invoker,"$domain/product_pilot_admission.ps1",$PSCommandPath)){$report.hashes[$path]=(Get-FileHash $path).Hash.ToLowerInvariant()}
$report|ConvertTo-Json -Depth 8|Set-Content (Join-Path $OutputDirectory 'formal-tool-binding-verification.json') -Encoding UTF8
Write-Output ("FORMAL_TOOL_BINDING_PASS: "+$cases.Count)
