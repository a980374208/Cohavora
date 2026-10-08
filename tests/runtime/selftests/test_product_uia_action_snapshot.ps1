param([switch]$LegacyTail,[string]$OutputDirectory="$PSScriptRoot/../../../out/uia-action-snapshot")
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../tools/product_acceptance/product_pilot_probe_tail.ps1')
$null=New-Item -ItemType Directory -Path $OutputDirectory -Force
$path=Join-Path $OutputDirectory (([guid]::NewGuid().ToString('N'))+'.jsonl')
$encoding=[Text.UTF8Encoding]::new($false)
$first='{"sequence":1,"utc":"2026-10-03T23:54:00Z"}'
$second='{"sequence":2,"utc":"2026-10-03T23:54:01Z"}'
[IO.File]::WriteAllText($path,$first+"`n",$encoding)
if($LegacyTail){
    $emitted=0
    $last=Get-Content -LiteralPath $path -Tail 1 | ForEach-Object {
        $emitted++
        if($emitted -eq 1){[IO.File]::AppendAllText($path,$second+"`n",$encoding)}
        $_
    } | ConvertFrom-Json
    Write-Output ('LEGACY_RECORDS='+@($last).Count+' UTC_TYPE='+$last.utc.GetType().FullName)
    # This must reproduce the real Formal41 DateTime conversion failure.
    $unused=[DateTime]$last.utc
    throw 'LEGACY_RACE_NOT_REPRODUCED'
}
$tokens=$null;$errors=$null
$source=Join-Path $PSScriptRoot '../tools/product_acceptance/invoke_product_external.ps1'
$ast=[Management.Automation.Language.Parser]::ParseFile($source,[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'FORMAL_ENTRY_PARSE_ERROR'}
$definition=$ast.Find({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Read-LatestCompleteUiaAction'},$true)
if(!$definition){throw 'UIA_SNAPSHOT_READER_MISSING'}
Invoke-Expression $definition.Extent.Text
# A real append handle remains open while the watchdog reads.
$writer=[IO.FileStream]::new($path,[IO.FileMode]::Append,[IO.FileAccess]::Write,[IO.FileShare]::Read)
try {
    $bytes=$encoding.GetBytes($second+"`n"+'{"sequence":3,"utc":')
    $writer.Write($bytes,0,$bytes.Length);$writer.Flush()
    $rows=@(Read-LatestCompleteUiaAction $path)
    if($rows.Count -ne 1 -or $rows[0].sequence -ne 2 -or $rows[0].utc -ne '2026-10-03T23:54:01Z'){throw 'COMPLETE_SCALAR_SNAPSHOT_FAILED'}
    $unused=[DateTime]$rows[0].utc
    $tail=$encoding.GetBytes('"2026-10-03T23:54:02Z"}'+"`n")
    $writer.Write($tail,0,$tail.Length);$writer.Flush()
    $last=Read-LatestCompleteUiaAction $path
    if($last.sequence -ne 3){throw 'COMPLETED_APPEND_NOT_OBSERVED'}
} finally {$writer.Dispose()}
if([IO.File]::ReadAllText($path,$encoding) -cne ($first+"`n"+$second+"`n"+'{"sequence":3,"utc":"2026-10-03T23:54:02Z"}'+"`n")){throw 'EVIDENCE_BYTES_CHANGED'}
[IO.File]::WriteAllText($path,'{"sequence":4', $encoding)
if($null -ne (Read-LatestCompleteUiaAction $path)){throw 'FIRST_PARTIAL_LINE_ACCEPTED'}
foreach($bad in @('[{"utc":"2026-10-03T23:54:00Z"}]', '{"utc":["2026-10-03T23:54:00Z","2026-10-03T23:54:01Z"]}', '{invalid')){
    [IO.File]::WriteAllText($path,$bad+"`n",$encoding)
    $failed=$false
    try{$unused=Read-LatestCompleteUiaAction $path}catch{$failed=$true}
    if(!$failed){throw 'MALFORMED_COMPLETE_RECORD_HIDDEN'}
}
$failed=$false
try{$unused=Read-LatestCompleteUiaAction ($path+'.missing')}catch{$failed=$true}
if(!$failed){throw 'MISSING_EVIDENCE_HIDDEN'}

# Exercise the complete-record reader independently of the watchdog wrapper.
$bom=[byte[]]@(239,187,191)
$unicode='{"sequence":7,"utc":"2026-10-03T23:54:07Z","label":"'+[char]0x4E2D+[char]0x6587+'"}'
[IO.File]::WriteAllBytes($path,($bom+$encoding.GetBytes($unicode+"`r`n"+$second+"`r`n"+'{"sequence":8}')))
$complete=@(Read-ProductPilotCompleteJsonlTail -Path $path -Count 2)
if($complete.Count -ne 2 -or $complete[0] -cne $unicode -or $complete[1] -cne $second){throw 'BOM_CRLF_OR_COMPLETE_COUNT_CHANGED'}
[IO.File]::WriteAllBytes($path,($encoding.GetBytes($first+"`n")+[byte[]]@(0xF0,0x9F)))
$complete=@(Read-ProductPilotCompleteJsonlTail -Path $path)
if($complete.Count -ne 1 -or $complete[0] -cne $first){throw 'INCOMPLETE_UTF8_EOF_NOT_EXCLUDED'}
[IO.File]::WriteAllBytes($path,($encoding.GetBytes($first+"`n")+[byte[]]@(0xFF,10)))
$failed=$false
$leaked=New-Object 'System.Collections.Generic.List[string]'
try{Read-ProductPilotCompleteJsonlTail -Path $path -Count 2 | ForEach-Object {$leaked.Add($_)}}catch{$failed=$true}
if(!$failed -or $leaked.Count -ne 0){throw 'INVALID_COMPLETE_UTF8_NOT_ATOMICALLY_REJECTED'}
[IO.File]::WriteAllText($path,$first+"`n"+$second+"`n`n",$encoding)
$complete=@(Read-ProductPilotCompleteJsonlTail -Path $path -Count 2)
if($complete.Count -ne 2 -or $complete[0] -cne $second -or $complete[1] -cne ''){throw 'EMPTY_COMPLETE_PHYSICAL_RECORD_LOST'}
[IO.File]::WriteAllText($path,$first+"`n"+$second+"`n"+('x'*129),$encoding)
$failed=$false
try{$unused=Read-ProductPilotCompleteJsonlTail -Path $path -MaximumBytes 128}catch{$failed=$true}
if(!$failed){throw 'UNBOUNDED_PARTIAL_SUFFIX_FALLBACK'}

# Malformed UTF-8 far before the suffix must neither be read/decoded nor cause
# fallback to a whole-file scan. A small explicit window covers both tail rows.
$large=New-Object byte[] (2MB)
for($i=0;$i -lt $large.Length;$i++){$large[$i]=255}
[IO.File]::WriteAllBytes($path,($large+$encoding.GetBytes("`n"+$first+"`n"+$second+"`n")))
$complete=@(Read-ProductPilotCompleteJsonlTail -Path $path -MaximumBytes 128)
if($complete.Count -ne 1 -or $complete[0] -cne $second){throw 'BOUNDED_COMPLETE_SUFFIX_CHANGED'}
Write-Output 'PASS: real append coexistence; scalar latest complete action; EOF partial/completion; strict selected JSON; missing path; BOM/UTF8/CRLF; strict complete UTF8 with atomic output; empty physical row; bounded suffix without full fallback'
