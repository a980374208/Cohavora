param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$ExpectedExecutableName,
    [ValidateSet('RelWithDebInfo')][string]$Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'
$binaryPath = (Resolve-Path -LiteralPath $Executable).Path
if (-not [string]::Equals([System.IO.Path]::GetFileName($binaryPath), $ExpectedExecutableName,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Unexpected runtime harness name (expected $ExpectedExecutableName)"
}
$stream = [System.IO.File]::OpenRead($binaryPath)
try {
    $reader = [System.Reflection.PortableExecutable.PEReader]::new($stream)
    $codeViewEntries = @($reader.ReadDebugDirectory() | Where-Object {
        $_.Type -eq [System.Reflection.PortableExecutable.DebugDirectoryEntryType]::CodeView
    })
    if ($codeViewEntries.Count -ne 1) {
        throw 'Executable must contain exactly one CodeView debug entry'
    }
    $codeView = $reader.ReadCodeViewDebugDirectoryData($codeViewEntries[0])
    $configurationSegment = '(^|[\\/])' + [regex]::Escape($Configuration) + '([\\/]|$)'
    if (-not [regex]::IsMatch($codeView.Path, $configurationSegment,
            [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)) {
        throw "Embedded CodeView path does not identify a $Configuration build"
    }
    $expectedPdbName = [System.IO.Path]::ChangeExtension($ExpectedExecutableName, '.pdb')
    if (-not [string]::Equals([System.IO.Path]::GetFileName($codeView.Path), $expectedPdbName,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Embedded CodeView path does not identify $expectedPdbName"
    }

    [pscustomobject]@{
        configuration = $Configuration
        binary_path = $binaryPath
        binary_sha256 = (Get-FileHash -LiteralPath $binaryPath -Algorithm SHA256).Hash.ToLowerInvariant()
        pdb_path = $codeView.Path
        pdb_guid = $codeView.Guid.ToString()
        pdb_age = $codeView.Age
    } | ConvertTo-Json -Compress
} finally {
    if ($reader) { $reader.Dispose() }
    $stream.Dispose()
}
