[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$InputManifest,
    [Parameter(Mandatory)][string]$Profile,
    [Parameter(Mandatory)][string]$Output,
    [string]$PreparedDirectory,
    [switch]$Plan
)
$ErrorActionPreference = 'Stop'
$runner = Join-Path $PSScriptRoot 'b11_hd_remote_run.py'
$arguments = @('-B', $runner, '--executable', [IO.Path]::GetFullPath($Executable),
    '--input-manifest', [IO.Path]::GetFullPath($InputManifest),
    '--profile', [IO.Path]::GetFullPath($Profile), '--output', [IO.Path]::GetFullPath($Output))
if ($PreparedDirectory) { $arguments += @('--prepared-directory', [IO.Path]::GetFullPath($PreparedDirectory)) }
if ($Plan) { $arguments += '--plan' }
& python @arguments
if ($LASTEXITCODE -ne 0) { throw 'Independent B11 HD diagnostic did not pass; review runner-summary.json.' }
