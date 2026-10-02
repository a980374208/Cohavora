. "$PSScriptRoot/whiteboard_file_helpers.ps1"
Add-Type -AssemblyName System.Drawing

function Open-Replace([string]$Stage) {
    (Require-Pattern (Find-Control 'whiteboardImportImage' $button) ([Windows.Automation.InvokePattern]::Pattern)).Invoke()
    $box = Wait-For "$Stage confirmation" { Get-OverwriteDialog }
    Assert-That ($box.Current.AutomationId.EndsWith('.whiteboardReplaceConfirmation')) "$Stage stable confirmation identity"
    $null = Find-Control 'whiteboardReplaceConfirm' $button $true $box
    $null = Find-Control 'whiteboardReplaceCancel' $button $true $box
    Save-Tree "$Stage-confirmation-tree" $box
    return $box
}
function Import-AtLimit([string]$Path, [string]$Stage) {
    $box = Open-Replace $Stage
    (Require-Pattern (Find-Control 'whiteboardReplaceConfirm' $button $true $box) ([Windows.Automation.InvokePattern]::Pattern)).Invoke()
    $dialog = Wait-For "$Stage file dialog" { Get-FileDialog }
    Assert-That ($dialog.Current.AutomationId.EndsWith('.whiteboardImportDialog')) "$Stage stable file identity"
    Set-FilePath $dialog $Path
    (Require-Pattern (Find-Control 'whiteboardFileAccept' $button $true $dialog) ([Windows.Automation.InvokePattern]::Pattern)).Invoke()
    Wait-FileClosed $Stage
}
function Export-Verified([string]$Reference, [string]$Name) {
    $destination = Join-Path $OutputDirectory "$Name.png"
    $exportNode = Find-Control 'whiteboardExport' $button
    $dialog = Open-FileDialog 'Export' $Name
    Set-FilePath $dialog $destination
    (Require-Pattern (Find-Control 'whiteboardFileAccept' $button $true $dialog) ([Windows.Automation.InvokePattern]::Pattern)).Invoke()
    Wait-FileClosed $Name
    $null = Wait-For "$Name written and controls recovered" {
        (Test-Path -LiteralPath $destination) -and $exportNode.Current.IsEnabled
    }
    $source = [Drawing.Bitmap]::FromFile($Reference)
    $output = [Drawing.Bitmap]::FromFile($destination)
    try {
        Assert-That ($source.Width -eq $output.Width -and $source.Height -eq $output.Height) "$Name dimensions match decoded source"
        $mismatches = 0
        for ($y=0; $y -lt $source.Height; $y++) {
            for ($x=0; $x -lt $source.Width; $x++) {
                if ($source.GetPixel($x,$y).ToArgb() -ne $output.GetPixel($x,$y).ToArgb()) { $mismatches++ }
            }
        }
        Assert-That ($mismatches -eq 0) "$Name independent pixel readback matches all pixels"
        [ordered]@{case=$Name; verdict='PASS'; pixels=$source.Width*$source.Height; mismatches=$mismatches;
            sha256=(Get-FileHash -LiteralPath $destination).Hash} | ConvertTo-Json |
            Set-Content -Encoding UTF8 "$OutputDirectory/$Name-report.json"
    } finally { $source.Dispose(); $output.Dispose() }
}

$before = Wait-For 'page-limit seeded model' { Read-Model }
Assert-That ($before.pages -eq 8 -and $before.objects -eq 1) 'page-limit fixture has eight pages and one real annotation'
Save-Model $before 'limit-before'
$box = Open-Replace 'limit-cancel'
(Require-Pattern (Find-Control 'whiteboardReplaceCancel' $button $true $box) ([Windows.Automation.InvokePattern]::Pattern)).Invoke()
$null = Wait-For 'replacement cancellation dismissed' { !(Get-OverwriteDialog) -and $script:window.Current.IsEnabled }
$cancelled = Wait-For 'replacement cancellation preserves full document' {
    $m=Read-Model; if ($m.sample -gt $before.sample -and $m.snapshot -ceq $before.snapshot) { $m }
}
Save-Model $cancelled 'limit-after-cancel'
$inputPath = Join-Path $OutputDirectory 'input image.png'
Import-AtLimit $inputPath 'limit-confirm'
$replaced = Wait-For 'replacement keeps page identity and clears annotations' {
    $m=Read-Model
    if ($m.pages -eq 8 -and $m.pageId -ceq $before.pageId -and $m.epoch -gt $before.epoch -and
        $m.background -and $m.backgroundReady -and $m.objects -eq 0 -and $m.width -eq 320 -and $m.height -eq 180) { $m }
}
Save-Model $replaced 'limit-after-confirm'
Assert-That (!(Find-Control 'whiteboardAddPage' $button $false).Current.IsEnabled) 'page-limit add stays disabled'
[ordered]@{case='page-limit-replacement'; verdict='PASS'; cancelledDocumentUnchanged=$true;
    pages=$replaced.pages; pageIdentityPreserved=$true; annotationsCleared=$true; epochAdvanced=$true} |
    ConvertTo-Json | Set-Content -Encoding UTF8 "$OutputDirectory/page-limit-report.json"
Export-Verified $inputPath 'limit-export'

$jpegPath=Join-Path $OutputDirectory 'input image.jpg'
Import-AtLimit $jpegPath 'jpeg-import'
$jpeg = Wait-For 'JPEG is decoded and replaces current background' {
    $m=Read-Model
    if ($m.pageId -ceq $replaced.pageId -and $m.pages -eq 8 -and $m.epoch -gt $replaced.epoch -and
        $m.background -and $m.background -cne $replaced.background -and $m.backgroundReady -and $m.objects -eq 0) { $m }
}
Save-Model $jpeg 'jpeg-imported'
Export-Verified (Join-Path $OutputDirectory 'jpeg decoded reference.png') 'jpeg-readback'

$deniedPath=Join-Path $OutputDirectory 'denied image.png'
$deniedHash=(Get-FileHash -LiteralPath $deniedPath).Hash
$acl=Get-Acl -LiteralPath $deniedPath
$sddl=$acl.GetSecurityDescriptorSddlForm([Security.AccessControl.AccessControlSections]::Access)
$sid=[Security.Principal.WindowsIdentity]::GetCurrent().User
$deny=New-Object Security.AccessControl.FileSystemAccessRule($sid,
    [Security.AccessControl.FileSystemRights]::ReadData,[Security.AccessControl.AccessControlType]::Deny)
$baseline = Wait-For 'access-denied baseline' { Read-Model }
$statusNode=Find-Control 'whiteboardStatus' ([Windows.Automation.ControlType]::Text)
$importNode=Find-Control 'whiteboardImportImage' $button
try {
    $acl.AddAccessRule($deny)
    Set-Acl -LiteralPath $deniedPath -AclObject $acl
    $accessDenied=$false
    try { $null=[IO.File]::ReadAllBytes($deniedPath) } catch {
        $cause=$_.Exception
        while ($cause.InnerException) { $cause=$cause.InnerException }
        $accessDenied=($cause -is [UnauthorizedAccessException]) -and (($cause.HResult -band 65535) -eq 5)
    }
    Assert-That $accessDenied 'input read is denied with actual Windows ERROR_ACCESS_DENIED'
    Import-AtLimit $deniedPath 'access-denied'
    $expected=Get-Content -LiteralPath "$OutputDirectory/expected-errors.json" -Raw -Encoding UTF8 | ConvertFrom-Json
    $null=Wait-For 'access-denied localized error and recovered import' {
        $statusNode.Current.Name -ceq $expected.unreadable -and $importNode.Current.IsEnabled
    }
    $preserved=Wait-For 'access-denied preserves model and history' {
        $m=Read-Model
        if ($m.sample -gt $baseline.sample -and $m.snapshot -ceq $baseline.snapshot -and
            $m.canUndo -eq $baseline.canUndo -and $m.canRedo -eq $baseline.canRedo) { $m }
    }
    Save-Model $preserved 'denied-model-preserved'
    Save-Tree 'denied-error-tree'
} finally {
    $restore=New-Object Security.AccessControl.FileSecurity
    $restore.SetSecurityDescriptorSddlForm($sddl,[Security.AccessControl.AccessControlSections]::Access)
    Set-Acl -LiteralPath $deniedPath -AclObject $restore
}
Assert-That ((Get-FileHash -LiteralPath $deniedPath).Hash -ceq $deniedHash) 'restored input content unchanged'
Import-AtLimit $deniedPath 'access-restored-retry'
$recovered=Wait-For 'same file retries successfully after access restoration' {
    $m=Read-Model
    if ($m.pageId -ceq $baseline.pageId -and $m.pages -eq 8 -and $m.epoch -gt $baseline.epoch -and
        $m.background -ceq $replaced.background -and $m.backgroundReady -and $m.objects -eq 0) { $m }
}
Save-Model $recovered 'access-restored-model'
Export-Verified $inputPath 'access-restored-readback'
[ordered]@{case='access-denied-retry'; verdict='PASS'; nativeError=5; modelPreserved=$true;
    historyPreserved=$true; aclRestored=$true; inputSha256=$deniedHash; retrySucceeded=$true} |
    ConvertTo-Json | Set-Content -Encoding UTF8 "$OutputDirectory/access-denied-report.json"
$script:deferred=@('Physical power loss and slow physical disk require independent acceptance; remote upload is a separate live-service case')
