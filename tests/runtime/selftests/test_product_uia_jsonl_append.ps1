param([string]$OutputDirectory="$PSScriptRoot/../../../out/uia-jsonl-append")
$ErrorActionPreference='Stop'
$encoding=[Text.UTF8Encoding]::new($false)
$runDirectory=Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) ([guid]::NewGuid().ToString('N'))
$null=New-Item -ItemType Directory -Path $runDirectory -Force
$source=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../uia/product_desktop.ps1'))
$sourceHash=(Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
$tokens=$null;$parseErrors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($source,[ref]$tokens,[ref]$parseErrors)
if($parseErrors.Count){throw 'UIA_SOURCE_PARSE_ERROR'}
$definition=$ast.Find({param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Append-SafeJsonl'
},$true)
if(!$definition){throw 'UIA_JSONL_APPEND_HELPER_MISSING'}
# Only load the real helper; the UIA script itself must never run in this test.
Invoke-Expression $definition.Extent.Text
if(-not ('UiaJsonlAppendReadLockFixture' -as [type])){
    Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Threading;

public sealed class UiaJsonlAppendReadLockFixture : IDisposable {
    private readonly ManualResetEvent ready = new ManualResetEvent(false);
    private readonly ManualResetEvent release = new ManualResetEvent(false);
    private readonly ManualResetEvent startCountdown = new ManualResetEvent(false);
    private readonly Thread worker;
    private Exception failure;

    public UiaJsonlAppendReadLockFixture(string path, int durationMilliseconds) {
        worker = new Thread(() => {
            try {
                // Reproduce a read handle whose share flags exclude writers.
                using (var held = new FileStream(path, FileMode.Open,
                    FileAccess.Read, FileShare.Read)) {
                    ready.Set();
                    if (durationMilliseconds < 0) release.WaitOne();
                    else if (WaitHandle.WaitAny(new WaitHandle[] { release, startCountdown }) == 1)
                        release.WaitOne(durationMilliseconds);
                }
            } catch (Exception error) {
                failure = error;
            } finally {
                ready.Set();
            }
        });
        worker.IsBackground = true;
        worker.Start();
    }

    public void WaitReady() {
        if (!ready.WaitOne(5000)) throw new TimeoutException("LOCK_READY_TIMEOUT");
        if (failure != null) throw new InvalidOperationException("LOCK_SETUP_FAILED", failure);
    }

    public void BeginTimedRelease() {
        startCountdown.Set();
    }

    public void Dispose() {
        release.Set();
        if (!worker.Join(5000)) throw new TimeoutException("LOCK_RELEASE_TIMEOUT");
        ready.Dispose();
        release.Dispose();
        startCountdown.Dispose();
        if (failure != null) throw new InvalidOperationException("LOCK_WORKER_FAILED", failure);
    }
}
'@
}

# Count the real helper's retry delays. A short lock only begins its release
# countdown after File.Open has actually collided and selected a retry.
$script:appendRetryCount=0
$script:shortLockFixture=$null
function Start-Sleep([int]$Milliseconds){
    $script:appendRetryCount++
    if($null -ne $script:shortLockFixture){$script:shortLockFixture.BeginTimedRelease()}
    Microsoft.PowerShell.Utility\Start-Sleep -Milliseconds $Milliseconds
}

function Assert-JsonlBytes([string]$Path,[string[]]$Expected){
    $bytes=[IO.File]::ReadAllBytes($Path)
    if($bytes.Length -ge 3 -and $bytes[0] -eq 239 -and $bytes[1] -eq 187 -and $bytes[2] -eq 191){
        throw 'JSONL_UTF8_BOM_ADDED'
    }
    $strictEncoding=[Text.UTF8Encoding]::new($false,$true)
    $text=$strictEncoding.GetString($bytes)
    if($text -cne (($Expected -join "`n")+"`n")){throw 'JSONL_BYTES_OR_RECORD_COUNT_CHANGED'}
    foreach($line in $Expected){$null=$line | ConvertFrom-Json}
}

$results=New-Object 'System.Collections.Generic.List[object]'
$files=New-Object 'System.Collections.Generic.List[string]'
$seed='{"sequence":0,"label":"seed"}'
$unicode='{"sequence":1,"label":"'+[char]0x4E2D+[char]0x6587+'"}'
$second='{"sequence":2,"label":"second"}'
$verdict='FAIL'
$failureMessage=$null
try {
    $shortPath=Join-Path $runDirectory 'short-lock.jsonl'
    $files.Add($shortPath)
    [IO.File]::WriteAllText($shortPath,$seed+"`n",$encoding)
    $held=[UiaJsonlAppendReadLockFixture]::new($shortPath,250)
    try {
        $held.WaitReady()
        $legacyCode=$null
        try{[IO.File]::AppendAllText($shortPath,$unicode+"`n",$encoding)}
        catch{$legacyCode=$_.Exception.GetBaseException().HResult -band 65535}
        if($legacyCode -ne 32){throw 'LEGACY_APPEND_SHARING_FAILURE_NOT_REPRODUCED'}
        $script:shortLockFixture=$held
        $script:appendRetryCount=0
        $clock=[Diagnostics.Stopwatch]::StartNew()
        Append-SafeJsonl -Path $shortPath -Json $unicode
        $clock.Stop()
        $shortRetries=$script:appendRetryCount
        if($shortRetries -lt 1 -or $shortRetries -gt 20 -or $clock.ElapsedMilliseconds -gt 2500){
            throw 'SHORT_LOCK_RECOVERY_NOT_BOUNDED'
        }
    } finally {$script:shortLockFixture=$null;$held.Dispose()}
    Append-SafeJsonl -Path $shortPath -Json $second
    Assert-JsonlBytes -Path $shortPath -Expected @($seed,$unicode,$second)
    $results.Add([ordered]@{name='short_read_lock';status='PASS';legacy_win32_error=$legacyCode;elapsed_ms=$clock.ElapsedMilliseconds;retry_delays=$shortRetries;records=3})
    Write-Output 'PASS: real short read lock reproduces legacy failure; safe append recovers with exactly one record'

    $persistentPath=Join-Path $runDirectory 'persistent-lock.jsonl'
    $files.Add($persistentPath)
    [IO.File]::WriteAllText($persistentPath,$seed+"`n",$encoding)
    $before=(Get-FileHash -LiteralPath $persistentPath -Algorithm SHA256).Hash
    $held=[UiaJsonlAppendReadLockFixture]::new($persistentPath,-1)
    try {
        $held.WaitReady()
        $code=$null
        $script:appendRetryCount=0
        $clock=[Diagnostics.Stopwatch]::StartNew()
        try{Append-SafeJsonl -Path $persistentPath -Json $unicode}
        catch{$code=$_.Exception.GetBaseException().HResult -band 65535}
        $clock.Stop()
        $persistentRetries=$script:appendRetryCount
    } finally {$held.Dispose()}
    if($code -ne 32 -or $persistentRetries -lt 1 -or $persistentRetries -gt 20 -or
        $clock.ElapsedMilliseconds -lt 850 -or $clock.ElapsedMilliseconds -gt 2500){
        throw 'PERSISTENT_LOCK_FAILURE_HIDDEN_OR_UNBOUNDED'
    }
    if((Get-FileHash -LiteralPath $persistentPath -Algorithm SHA256).Hash -cne $before){
        throw 'FAILED_OPEN_CHANGED_JSONL'
    }
    Assert-JsonlBytes -Path $persistentPath -Expected @($seed)
    $results.Add([ordered]@{name='persistent_read_lock';status='PASS';win32_error=$code;elapsed_ms=$clock.ElapsedMilliseconds;retry_delays=$persistentRetries;unchanged_sha256=$before})
    Write-Output 'PASS: persistent read lock fails after about one second; original bytes remain unchanged'

    $freshPath=Join-Path $runDirectory 'fresh.jsonl'
    $files.Add($freshPath)
    Append-SafeJsonl -Path $freshPath -Json $unicode
    Append-SafeJsonl -Path $freshPath -Json $second
    Assert-JsonlBytes -Path $freshPath -Expected @($unicode,$second)
    $results.Add([ordered]@{name='fresh_utf8_once';status='PASS';records=2;bom=$false})
    Write-Output 'PASS: new UTF8 JSONL has no BOM and both records occur exactly once'

    # A byte-range lock permits File.Open but rejects the subsequent write/flush.
    # This proves error 33 after opening is not mistaken for an open-time retry.
    $writePath=Join-Path $runDirectory 'write-lock.jsonl'
    $files.Add($writePath)
    [IO.File]::WriteAllText($writePath,$seed+"`n",$encoding)
    $before=(Get-FileHash -LiteralPath $writePath -Algorithm SHA256).Hash
    $lockedOffset=[IO.File]::ReadAllBytes($writePath).Length
    $reader=[IO.FileStream]::new($writePath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
    $locked=$false
    try {
        $reader.Lock($lockedOffset,4096)
        $locked=$true
        $probe=[IO.File]::Open($writePath,[IO.FileMode]::Append,[IO.FileAccess]::Write,[IO.FileShare]::Read)
        $probe.Dispose()
        $code=$null
        $script:appendRetryCount=0
        $clock=[Diagnostics.Stopwatch]::StartNew()
        try{Append-SafeJsonl -Path $writePath -Json $unicode}
        catch{$code=$_.Exception.GetBaseException().HResult -band 65535}
        $clock.Stop()
        $writeRetries=$script:appendRetryCount
    } finally {
        if($locked){$reader.Unlock($lockedOffset,4096)}
        $reader.Dispose()
    }
    if($code -ne 33 -or $writeRetries -ne 0){throw 'WRITE_FAILURE_RETRIED_OR_HIDDEN'}
    if((Get-FileHash -LiteralPath $writePath -Algorithm SHA256).Hash -cne $before){throw 'LOCKED_WRITE_CHANGED_JSONL'}
    Append-SafeJsonl -Path $writePath -Json $unicode
    Assert-JsonlBytes -Path $writePath -Expected @($seed,$unicode)
    $results.Add([ordered]@{name='post_open_write_lock';status='PASS';win32_error=$code;elapsed_ms=$clock.ElapsedMilliseconds;retry_delays=$writeRetries;records_after_explicit_retry=2})
    Write-Output 'PASS: open succeeds but write lock fails immediately; later explicit append writes only once'
    if((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -cne $sourceHash){throw 'UIA_SOURCE_CHANGED_DURING_SELFTEST'}
    $verdict='PASS'
} catch {
    $failureMessage=$_.Exception.Message
    throw
} finally {
    $artifacts=@(foreach($path in $files){
        if([IO.File]::Exists($path)){
            [ordered]@{path=$path;sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}
        }
    })
    $hasher=[Security.Cryptography.SHA256]::Create()
    try{$helperHash=([BitConverter]::ToString($hasher.ComputeHash($encoding.GetBytes($definition.Extent.Text)))).Replace('-','').ToLowerInvariant()}
    finally{$hasher.Dispose()}
    $receipt=[ordered]@{
        status=$verdict
        source_path=$source
        source_sha256=$sourceHash
        selftest_sha256=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
        helper_sha256=$helperHash
        cases=@($results.ToArray())
        files=$artifacts
        error=$failureMessage
    }
    $receiptPath=Join-Path $runDirectory 'result.json'
    [IO.File]::WriteAllText($receiptPath,($receipt | ConvertTo-Json -Depth 6)+"`n",$encoding)
    Write-Output ('RESULT='+$receiptPath)
}
