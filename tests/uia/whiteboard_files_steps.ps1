. "$PSScriptRoot/whiteboard_file_helpers.ps1"
if ($ProbeOnly) {
    $path = Join-Path $OutputDirectory 'input image.png'
    $dialog = Open-FileDialog 'Export' 'probe-overwrite'
    Set-FilePath $dialog $path
    (Require-Pattern (Find-Control 'whiteboardFileAccept' $button $true $dialog) ([Windows.Automation.InvokePattern]::Pattern)).Invoke()
    $box = Wait-For 'overwrite confirmation appears' { Get-OverwriteDialog }
    Save-Tree 'overwrite-probe-tree' $box
    return
}
$before = Wait-For 'initial model witness' { Read-Model }
Save-Model $before 'model-before'
$inputPath = Join-Path $OutputDirectory 'input image.png'
$outputPath = Join-Path $OutputDirectory 'export image.png'
$cancelPath = Join-Path $OutputDirectory 'cancelled image.png'
Assert-That (Test-Path -LiteralPath $inputPath) 'fixture input artifact exists'
Assert-That (!(Test-Path -LiteralPath $outputPath)) 'export destination is new'
$dialog = Open-FileDialog 'Import' 'import-cancel'
Set-FilePath $dialog $inputPath
Close-FileDialog $dialog 'import-cancel'
$cancelled = Wait-For 'import cancellation preserves model' {
    $m = Read-Model; if ($m.sample -gt $before.sample -and $m.snapshot -ceq $before.snapshot) { $m }
}
Save-Model $cancelled 'model-after-import-cancel'
$dialog = Open-FileDialog 'Import' 'import-submit'
Set-FilePath $dialog $inputPath
(Require-Pattern (Find-Control 'whiteboardFileAccept' $button $true $dialog) ([Windows.Automation.InvokePattern]::Pattern)).Invoke()
Wait-FileClosed 'import-submit'
$imported = Wait-For 'import creates decoded background page' {
    $m = Read-Model
    if ($m.sample -gt $cancelled.sample -and $m.pages -eq ($before.pages + 1) -and
        $m.pageId -ne $before.pageId -and $m.width -eq 320 -and $m.height -eq 180 -and
        $m.background -and $m.backgroundReady -and $m.objects -eq 0) { $m }
}
Save-Model $imported 'model-after-import'
$null = Find-Control 'whiteboardImportImage' $button
# IDs locate the UI. Expected messages come from the fixture's current locale,
# not fixed-language selectors or a copy of the observed status text.
$expectedErrors = Get-Content -LiteralPath "$OutputDirectory/expected-errors.json" -Raw -Encoding UTF8 | ConvertFrom-Json
$invalidEvidence = @()
foreach ($case in @(
    @{ key='unsupported'; file='invalid content.png' },
    @{ key='dimensions'; file='invalid dimensions.png' },
    @{ key='truncated'; file='truncated image.png' },
    @{ key='oversized'; file='oversized image.png' }
)) {
    $stage = "invalid-$($case.key)"
    $invalidPath = Join-Path $OutputDirectory $case.file
    $inputBytes = (Get-Item -LiteralPath $invalidPath).Length
    if ($case.key -eq 'oversized') {
        Assert-That ($inputBytes -gt (8 * 1024 * 1024)) "$stage input exceeds the 8 MiB limit"
    }
    $inputHash = (Get-FileHash -LiteralPath $invalidPath -Algorithm SHA256).Hash
    $baseline = Wait-For "$stage fresh model baseline" { Read-Model }
    Save-Model $baseline "$stage-model-before"
    $importNode = Find-Control 'whiteboardImportImage' $button
    $status = Find-Control 'whiteboardStatus' ([Windows.Automation.ControlType]::Text)
    $expectedMessage = $expectedErrors.($case.key)
    Assert-That (![string]::IsNullOrWhiteSpace($expectedMessage)) "$stage translated expected error is present"
    Assert-That ($status.Current.Name -cne $expectedMessage) "$stage error not already present before submission"
    $dialog = Open-FileDialog 'Import' $stage
    Set-FilePath $dialog $invalidPath
    (Require-Pattern (Find-Control 'whiteboardFileAccept' $button $true $dialog) ([Windows.Automation.InvokePattern]::Pattern)).Invoke()
    Wait-FileClosed $stage
    $null = Wait-For "$stage UIA reports expected localized error" { $status.Current.Name -ceq $expectedMessage }
    $null = Wait-For "$stage import button recovered" { $importNode.Current.IsEnabled }
    $observed = Wait-For "$stage complete model and undo state preserved" {
        $m = Read-Model
        if ($m.sample -gt $baseline.sample -and $m.snapshot -ceq $baseline.snapshot -and
            $m.pageId -eq $baseline.pageId -and $m.pages -eq $baseline.pages -and
            $m.background -ceq $baseline.background -and $m.backgroundReady -and
            $m.canUndo -eq $baseline.canUndo -and $m.canRedo -eq $baseline.canRedo) { $m }
    }
    Save-Model $observed "$stage-model-after"
    $null = Find-Control 'whiteboardStatus' ([Windows.Automation.ControlType]::Text)
    Save-Tree "$stage-error-tree"
    Assert-That ((Get-FileHash -LiteralPath $invalidPath -Algorithm SHA256).Hash -eq $inputHash) "$stage rejected input file unchanged"
    $invalidEvidence += [ordered]@{case=$case.key; file=$case.file; bytes=$inputBytes; sha256=$inputHash;
        expectedMessage=$expectedMessage; actualMessage=$status.Current.Name;
        beforeSample=$baseline.sample; afterSample=$observed.sample; modelUnchanged=$true; importEnabled=$importNode.Current.IsEnabled}
}
$invalidEvidence | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 "$OutputDirectory/invalid-import-report.json"
$dialog = Open-FileDialog 'Export' 'export-cancel'
Set-FilePath $dialog $cancelPath
Close-FileDialog $dialog 'export-cancel'
Assert-That (!(Test-Path -LiteralPath $cancelPath)) 'cancel does not create output'
$exportNode = Find-Control 'whiteboardExport' $button
$dialog = Open-FileDialog 'Export' 'export-submit'
Set-FilePath $dialog $outputPath
(Require-Pattern (Find-Control 'whiteboardFileAccept' $button $true $dialog) ([Windows.Automation.InvokePattern]::Pattern)).Invoke()
Wait-FileClosed 'export-submit'
$null = Wait-For 'export file committed' { Test-Path -LiteralPath $outputPath }
$null = Wait-For 'export control reenabled' { $exportNode.Current.IsEnabled }
$null = Find-Control 'whiteboardExport' $button
$after = Wait-For 'export preserves imported document' {
    $m = Read-Model; if ($m.sample -gt $imported.sample -and $m.snapshot -ceq $imported.snapshot) { $m }
}
Save-Model $after 'model-after-export'
Assert-That (!(Test-Path -LiteralPath $cancelPath)) 'cancelled destination remains absent after export'
# Independent file decoder; images are never used for UI recognition.
Add-Type -AssemblyName System.Drawing
$bytes = [IO.File]::ReadAllBytes($outputPath)
Assert-That (($bytes[0..7] -join ',') -eq '137,80,78,71,13,10,26,10') 'output has PNG signature'
$source = [Drawing.Bitmap]::FromFile($inputPath)
$exported = [Drawing.Bitmap]::FromFile($outputPath)
try {
    Assert-That ($exported.Width -eq 320 -and $exported.Height -eq 180) 'decoded output is 320x180'
    $mismatches = 0
    for ($y=0; $y -lt 180; $y++) {
        for ($x=0; $x -lt 320; $x++) {
            if ($source.GetPixel($x,$y).ToArgb() -ne $exported.GetPixel($x,$y).ToArgb()) { $mismatches++ }
        }
    }
    Assert-That ($mismatches -eq 0) 'all 57600 decoded pixels match imported source'
    [ordered]@{width=$exported.Width; height=$exported.Height; comparedPixels=57600; mismatches=$mismatches;
        files=@(Get-FileHash -LiteralPath $inputPath,$outputPath -Algorithm SHA256 | Select-Object Path,Hash)
    } | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 "$OutputDirectory/artifact-report.json"
} finally { $source.Dispose(); $exported.Dispose() }
$existingPath = Join-Path $OutputDirectory 'existing image.png'
$originalBytes = [IO.File]::ReadAllBytes($existingPath)
$originalHash = (Get-FileHash -LiteralPath $existingPath -Algorithm SHA256).Hash
$exportHash = (Get-FileHash -LiteralPath $outputPath -Algorithm SHA256).Hash
Assert-That ($originalHash -ne $exportHash) 'existing artifact differs from expected export'
[IO.File]::WriteAllBytes((Join-Path $OutputDirectory 'existing-before.png'), $originalBytes)
$dialog = Open-FileDialog 'Export' 'overwrite-cancel'
Set-FilePath $dialog $existingPath
$box = Open-OverwriteConfirmation $dialog 'overwrite-cancel'
Assert-That ((Get-FileHash -LiteralPath $existingPath).Hash -eq $originalHash) 'pending confirmation has not overwritten file'
(Require-Pattern (Find-Control 'whiteboardOverwriteCancel' $button $true $box) ([Windows.Automation.InvokePattern]::Pattern)).Invoke()
$null = Wait-For 'No dismisses only overwrite confirmation' { $null -eq (Get-OverwriteDialog) }
$null = Wait-For 'No returns to enabled save dialog' { $d = Get-FileDialog; if ($d -and $d.Current.IsEnabled) { $d } }
$value = Require-Pattern (Find-Control 'fileNameEdit' ([Windows.Automation.ControlType]::Edit) $true $dialog) ([Windows.Automation.ValuePattern]::Pattern)
Assert-That ($value.Current.Value -ceq $existingPath) 'No preserves chosen destination'
Close-FileDialog $dialog 'overwrite-cancel'
$afterCancel = Wait-For 'overwrite cancellation preserves document' {
    $m = Read-Model; if ($m.sample -gt $after.sample -and $m.snapshot -ceq $after.snapshot) { $m }
}
Save-Model $afterCancel 'model-after-overwrite-cancel'
$cancelBytes = [IO.File]::ReadAllBytes($existingPath)
Assert-That ([Convert]::ToBase64String($cancelBytes) -ceq [Convert]::ToBase64String($originalBytes)) 'No plus save cancellation preserves every original byte'
[IO.File]::WriteAllBytes((Join-Path $OutputDirectory 'existing-after-cancel.png'), $cancelBytes)
$cancelHash = (Get-FileHash -LiteralPath $existingPath).Hash
$dialog = Open-FileDialog 'Export' 'overwrite-confirm'
Set-FilePath $dialog $existingPath
$box = Open-OverwriteConfirmation $dialog 'overwrite-confirm'
(Require-Pattern (Find-Control 'whiteboardOverwriteConfirm' $button $true $box) ([Windows.Automation.InvokePattern]::Pattern)).Invoke()
$null = Wait-For 'Yes dismisses overwrite confirmation' { $null -eq (Get-OverwriteDialog) }
Wait-FileClosed 'overwrite-confirm'
$null = Wait-For 'confirmed overwrite commits expected PNG bytes' {
    (Get-FileHash -LiteralPath $existingPath -Algorithm SHA256).Hash -eq $exportHash
}
$null = Wait-For 'export reenabled after overwrite' { $exportNode.Current.IsEnabled }
$afterOverwrite = Wait-For 'confirmed overwrite preserves document' {
    $m = Read-Model; if ($m.sample -gt $afterCancel.sample -and $m.snapshot -ceq $after.snapshot) { $m }
}
Save-Model $afterOverwrite 'model-after-overwrite-confirm'
# The reference export above was independently decoded and all pixels checked.
# Full file equality transfers that content check to the overwritten artifact.
$overwrittenBytes = [IO.File]::ReadAllBytes($existingPath)
Assert-That ([Convert]::ToBase64String($overwrittenBytes) -ceq [Convert]::ToBase64String($bytes)) 'overwritten PNG byte-identical to independently verified export'
[ordered]@{ originalHash=$originalHash; cancelledHash=$cancelHash;
    confirmedHash=(Get-FileHash -LiteralPath $existingPath).Hash; expectedHash=$exportHash;
    cancellationBytesUnchanged=$true; confirmedBytesMatchVerifiedPng=$true;
    originalFile='existing-before.png'; cancelledFile='existing-after-cancel.png'; overwrittenFile='existing image.png'
} | ConvertTo-Json | Set-Content -Encoding UTF8 "$OutputDirectory/overwrite-report.json"
$script:deferred = @('JPEG, unreadable files, page-limit replacement, remote upload, slow disk and power-loss recovery NOT_RUN')
