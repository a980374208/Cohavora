param([string]$OutputDirectory="$PSScriptRoot/../../out/uia-cycle-ownership")
$ErrorActionPreference='Stop'
$OutputDirectory=Join-Path $OutputDirectory ([guid]::NewGuid().ToString('N'))
$null=New-Item -ItemType Directory -Path $OutputDirectory
$OutputDirectory=(Resolve-Path $OutputDirectory).Path
$self=Get-Process -Id $PID
$configuration=@{output_directory=$OutputDirectory;executable='C:\not-the-requested-product.exe';
    started_utc=[DateTime]::UtcNow.ToString('o');cycles=2;minimum_seconds=480;
    product_pid=$PID;product_start_ticks=$self.StartTime.ToUniversalTime().Ticks;
    run_id=('a'*32);cycle=1;cycle_id=('b'*32);result_path="$OutputDirectory/result.json"}
$configuration | ConvertTo-Json | Set-Content "$OutputDirectory/config.json" -Encoding UTF8
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot/product_desktop_cycle.ps1" -Configuration "$OutputDirectory/config.json"
if ($LASTEXITCODE -ne 1) {throw 'Worker accepted an unrelated product process'}
$result=Get-Content "$OutputDirectory/result.json" -Raw | ConvertFrom-Json
if ($result.status -ne 'FAIL' -or $result.reason -ne 'ISOLATED_CYCLE_PRODUCT_IDENTITY_CHANGED') {throw 'Worker did not fail at identity boundary'}
if (!(Get-Process -Id $PID -ErrorAction SilentlyContinue)) {throw 'Worker terminated the unrelated owner'}
Write-Output 'PASS: mismatched process identity rejected before UIA; unrelated process retained'
