param([string]$OutputDirectory="$PSScriptRoot/../../../out/gpu-heartbeat-file-sharing")
$ErrorActionPreference='Stop'
$null=New-Item -ItemType Directory -Path $OutputDirectory -Force
$path=Join-Path $OutputDirectory (([guid]::NewGuid().ToString('N'))+'.json')
$encoding=[Text.UTF8Encoding]::new($false)
$tokens=$null;$errors=$null
$source=Join-Path $PSScriptRoot '../tools/product_acceptance/invoke_product_external.ps1'
$ast=[Management.Automation.Language.Parser]::ParseFile($source,[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'FORMAL_ENTRY_PARSE_ERROR'}
$definition=$ast.Find({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Read-GpuTraceHeartbeat'},$true)
if(!$definition){throw 'GPU_HEARTBEAT_READER_MISSING'}
Invoke-Expression $definition.Extent.Text
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class HeartbeatFileSharingFixture {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern SafeFileHandle CreateFileW(string path, uint access, uint share,
        IntPtr security, uint disposition, uint flags, IntPtr template);
}
'@
$payload='{"utc_ms":1,"events_lost":7,"realtime_buffers_lost":9,"query_error":11}'
[IO.File]::WriteAllText($path,$payload,$encoding)
# A real DELETE handle represents the replacement overlap. The old provider
# reader omits FILE_SHARE_DELETE and must reproduce the Windows sharing error.
$delete= [HeartbeatFileSharingFixture]::CreateFileW($path,0x10000,7,[IntPtr]::Zero,3,0x80,[IntPtr]::Zero)
if($delete.IsInvalid){throw 'DELETE_HANDLE_SETUP_FAILED'}
try {
    $legacyCode=$null
    try {$unused=Get-Content -LiteralPath $path -Raw -ErrorAction Stop}
    catch {$legacyCode=$_.Exception.GetBaseException().HResult -band 65535}
    if($legacyCode -ne 32){throw 'LEGACY_SHARING_FAILURE_NOT_REPRODUCED'}
    $value=Read-GpuTraceHeartbeat $path
    if($value.utc_ms -ne 1 -or $value.events_lost -ne 7 -or
        $value.realtime_buffers_lost -ne 9 -or $value.query_error -ne 11){throw 'HEARTBEAT_VALUES_CHANGED'}
} finally {$delete.Dispose()}
Write-Output 'PASS: legacy Get-Content sharing violation32 reproduced; replacement-compatible reader preserves stale/loss/error values'

$ready=[Threading.ManualResetEventSlim]::new($false)
$worker=[PowerShell]::Create()
$null=$worker.AddScript({param($Path,$Ready)
    $held=[IO.FileStream]::new($Path,[IO.FileMode]::Open,[IO.FileAccess]::Write,[IO.FileShare]::None)
    try {$Ready.Set();[Threading.Thread]::Sleep(120)}finally{$held.Dispose()}
}).AddArgument($path).AddArgument($ready)
$pending=$worker.BeginInvoke()
try {
    if(!$ready.Wait(5000)){throw 'TRANSIENT_LOCK_READY_TIMEOUT'}
    $value=Read-GpuTraceHeartbeat $path
    if($value.events_lost -ne 7){throw 'TRANSIENT_LOCK_READ_CHANGED'}
    $null=$worker.EndInvoke($pending)
    if($worker.HadErrors){throw 'TRANSIENT_LOCK_WORKER_FAILED'}
} finally {$worker.Dispose();$ready.Dispose()}
Write-Output 'PASS: real transient exclusive writer lock recovers within bounded reader retry'

$held=[IO.FileStream]::new($path,[IO.FileMode]::Open,[IO.FileAccess]::Write,[IO.FileShare]::None)
$clock=[Diagnostics.Stopwatch]::StartNew();$code=$null
try {
    try{$unused=Read-GpuTraceHeartbeat $path}catch{$code=$_.Exception.GetBaseException().HResult -band 65535}
} finally {$held.Dispose()}
if($code -ne 32 -or $clock.ElapsedMilliseconds -lt 500 -or $clock.ElapsedMilliseconds -gt 2000){throw 'PERSISTENT_SHARING_FAILURE_HIDDEN_OR_UNBOUNDED'}
Write-Output 'PASS: persistent exclusive lock remains a sharing failure after the 500ms bound'

if([IO.File]::ReadAllText($path,$encoding) -cne $payload){throw 'EVIDENCE_BYTES_CHANGED'}
[IO.File]::WriteAllText($path,'{invalid',$encoding)
$failed=$false
try{$unused=Read-GpuTraceHeartbeat $path}catch{$failed=$true}
if(!$failed){throw 'MALFORMED_HEARTBEAT_HIDDEN'}
$code=$null
try{$unused=Read-GpuTraceHeartbeat ($path+'.missing')}catch{$code=$_.Exception.GetBaseException().HResult -band 65535}
if($code -ne 2){throw 'MISSING_HEARTBEAT_HIDDEN'}
Write-Output 'PASS: raw evidence unchanged; malformed JSON and missing file still fail'
$code=$null;$clock=[Diagnostics.Stopwatch]::StartNew()
try{$unused=Read-GpuTraceHeartbeat ([IO.Path]::GetFullPath($OutputDirectory))}
catch{$code=$_.Exception.GetBaseException().HResult -band 65535}
if($code -ne 5 -or $clock.ElapsedMilliseconds -ge 500){throw 'ACCESS_OR_PATH_FAILURE_RETRIED_OR_HIDDEN'}
Write-Output 'PASS: real access/path error5 remains an immediate failure'
