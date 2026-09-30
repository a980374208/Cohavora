param(
 [Parameter(Mandatory=$true)][string]$Executable,
 [Parameter(Mandatory=$true)][string]$OutputDirectory,
 [switch]$OplockOnly
)
$ErrorActionPreference='Stop'
$Executable=(Resolve-Path -LiteralPath $Executable).Path
if(Test-Path -LiteralPath $OutputDirectory){throw 'Output directory must be new'}
$evidence=[IO.Path]::GetFullPath($OutputDirectory)
if([IO.Path]::GetPathRoot($evidence) -eq 'G:\'){throw 'Evidence must be outside the test volume'}
$volume=Get-Volume -DriveLetter G
if($volume.FileSystemType -ne 'NTFS' -or $volume.Size -ge 128MB -or $volume.Size -le 64MB){throw 'Unexpected test volume'}
$unknown=@(Get-ChildItem -LiteralPath G:\ -Force | Where-Object {
 if($_.Name -eq 'System Volume Information'){return $false}
 # Windows may retain an ACL-protected, empty recycle directory on this volume.
 # Never clear it, follow a link, or admit recycled payloads to a disk-full test.
 if($_.Name -eq '$RECYCLE.BIN' -and $_.PSIsContainer -and
    -not($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -and
    @((Get-ChildItem -LiteralPath $_.FullName -Force -ErrorAction Stop)).Count -eq 0){return $false}
 return $true
})
if($unknown.Count -ne 0){throw 'Unexpected files on isolated test volume'}
$root='G:\codex-quota-io-'+[guid]::NewGuid().ToString('N')
$null=New-Item -ItemType Directory -Path $evidence
if(Test-Path -LiteralPath $root){throw 'Test root already exists'}
$before=[long]$volume.SizeRemaining
$p=$null
New-Item -ItemType Directory -Path $root | Out-Null
try {
 $identity=[Security.Principal.WindowsIdentity]::GetCurrent().User
 $acl=New-Object Security.AccessControl.DirectorySecurity
 $acl.SetOwner($identity)
 $acl.SetAccessRuleProtection($true,$false)
 $rule=New-Object Security.AccessControl.FileSystemAccessRule($identity,'FullControl','ContainerInherit,ObjectInherit','None','Allow')
 $acl.AddAccessRule($rule)
 Set-Acl -LiteralPath $root -AclObject $acl
 $resolved=(Resolve-Path -LiteralPath $root).Path
 if($resolved -ne $root -or ((Get-Item -LiteralPath $root).Attributes -band [IO.FileAttributes]::ReparsePoint)){throw 'Unsafe root'}
 $p=Start-Process -FilePath $Executable -ArgumentList $(if($OplockOnly){@($root,'oplock')}else{@($root)}) -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $evidence 'runtime.stdout.txt') -RedirectStandardError (Join-Path $evidence 'runtime.stderr.txt')
 # Retain the handle before a short redirected child exits (Windows PowerShell 5.1).
 $null=$p.Handle
 if(-not $p.WaitForExit(90000)){Stop-Process -Id $p.Id -Force; $p.WaitForExit(); throw 'Runtime watchdog timed out'}
 $code=$p.ExitCode
 Get-Content (Join-Path $evidence 'runtime.stdout.txt')
 Get-Content (Join-Path $evidence 'runtime.stderr.txt')
 if($null -eq $code){throw 'Runtime exit code unavailable'}
 if($code -ne 0){throw "Runtime exit $code"}
} finally {
 if($p -and -not $p.HasExited){Stop-Process -Id $p.Id -Force; $p.WaitForExit()}
 # Only the fixed, newly created test directory is removed; no disk/partition operations.
 if((Test-Path -LiteralPath $root) -and (Resolve-Path -LiteralPath $root).Path -eq $root) {
  if((Get-Item -LiteralPath $root -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Test root became a reparse point; cleanup stopped'}
  $links=@(Get-ChildItem -LiteralPath $root -Recurse -Force | Where-Object {$_.Attributes -band [IO.FileAttributes]::ReparsePoint})
  if($links.Count){throw 'Unexpected reparse point; cleanup stopped'}
  Remove-Item -LiteralPath $root -Recurse -Force
 }
 $after=Get-Volume -DriveLetter G
 [ordered]@{root=$root;size=$after.Size;free_before=$before;free_after=$after.SizeRemaining;test_root_removed=(-not(Test-Path -LiteralPath $root));root_entries=@(Get-ChildItem -LiteralPath G:\ -Force | Select-Object -ExpandProperty Name)} | ConvertTo-Json | Tee-Object -FilePath (Join-Path $evidence 'cleanup.json')
}
