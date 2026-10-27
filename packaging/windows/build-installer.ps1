#requires -Version 5.1
<#
Build with ISS-managed Authenticode signing. With no certificate thumbprint,
create/reuse a local, non-exportable self-signed code-signing certificate.
The certificate is not installed into any trusted root store.
#>
[CmdletBinding(DefaultParameterSetName = 'Build')]
param(
    [Parameter(Mandatory = $true)][string]$Iscc,
    [Parameter(Mandatory = $true)][string]$SignTool,
    [Parameter(Mandatory = $true, ParameterSetName = 'Build')][string]$AppBuildDir,
    [Parameter(Mandatory = $true, ParameterSetName = 'Build')][string]$OutputDir,
    [Parameter(Mandatory = $true, ParameterSetName = 'ConfigureIde')][switch]$ConfigureIdeSignToolOnly,
    [string]$CertificateThumbprint,
    [string]$TimestampUrl,
    [string]$SelfSignedConfig = (Join-Path $env:LOCALAPPDATA 'Cohavora\signing\authenticode-selfsigned.json')
)

$ErrorActionPreference = 'Stop'
$Iscc = (Resolve-Path -LiteralPath $Iscc).Path
$SignTool = (Resolve-Path -LiteralPath $SignTool).Path
$InstallerScript = Join-Path $PSScriptRoot 'CohavoraInstaller.iss'
if (-not $ConfigureIdeSignToolOnly) {
    $AppBuildDir = (Resolve-Path -LiteralPath $AppBuildDir).Path
    $OutputDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDir)
    foreach ($relative in @('Cohavora.exe', 'renderers\cohavora-render-dx11.dll', 'renderers\cohavora-render-opengl.dll')) {
        if (-not (Test-Path -LiteralPath (Join-Path $AppBuildDir $relative) -PathType Leaf)) {
            throw "Missing packaging input: $relative"
        }
    }
    $AppVersion = (Get-Item -LiteralPath (Join-Path $AppBuildDir 'Cohavora.exe')).VersionInfo.ProductVersion
    if (-not $AppVersion -or $AppVersion -notmatch '^\d+\.\d+\.\d+(\.\d+)?$') {
        throw 'Application ProductVersion must be a numeric version.'
    }
    if ((Test-Path -LiteralPath $OutputDir) -and (Get-ChildItem -LiteralPath $OutputDir -Force | Select-Object -First 1)) {
        throw 'Use a new or empty output directory to avoid reusing signatures from another certificate.'
    }
}

$SelfSigned = [string]::IsNullOrWhiteSpace($CertificateThumbprint)
if ($SelfSigned) {
    if ($TimestampUrl) { throw 'Self-signed local builds do not use a public timestamp service.' }
    if (Test-Path -LiteralPath $SelfSignedConfig -PathType Leaf) {
        $Settings = Get-Content -LiteralPath $SelfSignedConfig -Raw | ConvertFrom-Json
        $CertificateThumbprint = [string]$Settings.thumbprint
    } else {
        $SigningDirectory = Split-Path -Parent $SelfSignedConfig
        [void](New-Item -ItemType Directory -Path $SigningDirectory -Force)
        $Certificate = New-SelfSignedCertificate -Type CodeSigningCert `
            -Subject 'CN=Cohavora Self-Signed' -FriendlyName 'Cohavora local Authenticode' `
            -CertStoreLocation 'Cert:\CurrentUser\My' -KeyAlgorithm RSA -KeyLength 3072 `
            -Provider 'Microsoft Software Key Storage Provider' -HashAlgorithm SHA256 `
            -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddYears(2)
        $CertificateThumbprint = $Certificate.Thumbprint
        $PublicCertificate = Join-Path $SigningDirectory 'authenticode-selfsigned.cer'
        [void](Export-Certificate -Cert $Certificate -FilePath $PublicCertificate -Type CERT)
        [ordered]@{
            schema = 'cohavora-authenticode-selfsigned-v1'
            thumbprint = $CertificateThumbprint
            subject = $Certificate.Subject
            public_certificate = $PublicCertificate
        } | ConvertTo-Json | Set-Content -LiteralPath $SelfSignedConfig -Encoding UTF8
    }
} elseif (-not $TimestampUrl) {
    throw 'A trusted certificate build requires an RFC 3161 TimestampUrl.'
}
if ($CertificateThumbprint -notmatch '^[a-fA-F0-9]{40}$') {
    throw 'CertificateThumbprint must contain exactly 40 hexadecimal characters.'
}
$Certificate = Get-Item -LiteralPath "Cert:\CurrentUser\My\$CertificateThumbprint"
if (-not $Certificate.HasPrivateKey -or $Certificate.NotBefore -gt (Get-Date) -or $Certificate.NotAfter -le (Get-Date)) {
    throw 'The selected certificate needs a private key and must be within its validity period.'
}
$CodeSigningUsage = $Certificate.Extensions | Where-Object { $_.Oid.Value -eq '2.5.29.37' }
if (-not ($CodeSigningUsage.EnhancedKeyUsages | Where-Object { $_.Value -eq '1.3.6.1.5.5.7.3.3' })) {
    throw 'The selected certificate must allow code signing.'
}
if ($SelfSigned -and $Certificate.Subject -ne $Certificate.Issuer) {
    throw 'The saved local certificate is not self-signed.'
}

# Inno Setup substitutes $q and $f. Escape literal dollars in supplied paths.
$SignCommand = '$q{0}$q sign /sha1 {1} /s My /fd SHA256' -f $SignTool.Replace('$', '$$'), $CertificateThumbprint
if ($TimestampUrl) {
    $TimestampUri = [Uri]$TimestampUrl
    if (-not $TimestampUri.IsAbsoluteUri -or $TimestampUri.Scheme -notin @('http', 'https') -or
            $TimestampUri.UserInfo -or $TimestampUrl -match '["\s]') {
        throw 'TimestampUrl must be an absolute HTTP(S) URL without credentials, quotes or whitespace.'
    }
    $SignCommand += ' /tr $q{0}$q /td SHA256' -f $TimestampUrl.Replace('$', '$$')
}
$SignCommand += ' $f'
if ($ConfigureIdeSignToolOnly) {
    # Match Inno Setup's contiguous SignTool0, SignTool1, ... configuration.
    # The IDE reads this list at startup; an already open IDE must be restarted.
    $ConfigMutex = [System.Threading.Mutex]::new($false, 'Inno-Setup-IDE-Config-Mutex')
    $ConfigLocked = $false
    $SignToolsKey = $null
    try {
        try { $ConfigLocked = $ConfigMutex.WaitOne(5000) }
        catch [System.Threading.AbandonedMutexException] { $ConfigLocked = $true }
        if (-not $ConfigLocked) { throw 'Inno Setup configuration is busy. Close its settings dialog and retry.' }
        $SignToolsKey = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('Software\Jordan Russell\Inno Setup\SignTools')
        $ToolIndex = 0
        while ($true) {
            $ToolValue = [string]$SignToolsKey.GetValue("SignTool$ToolIndex", '')
            if (-not $ToolValue -or $ToolValue.StartsWith('cohavora=', [StringComparison]::OrdinalIgnoreCase)) { break }
            ++$ToolIndex
        }
        $SignToolsKey.SetValue("SignTool$ToolIndex", "cohavora=$SignCommand", [Microsoft.Win32.RegistryValueKind]::String)
    } finally {
        if ($SignToolsKey) { $SignToolsKey.Dispose() }
        if ($ConfigLocked) { $ConfigMutex.ReleaseMutex() }
        $ConfigMutex.Dispose()
    }
    [ordered]@{
        configured_ide_sign_tool = 'cohavora'
        certificate_thumbprint = $CertificateThumbprint
        self_signed = $SelfSigned
        reopen_ide_before_compiling = $true
    } | ConvertTo-Json
    return
}
& $Iscc "/Scohavora=$SignCommand" "/DAppBuildDir=$AppBuildDir" "/DOutputDir=$OutputDir" $InstallerScript
if ($LASTEXITCODE -ne 0) { throw 'ISS packaging or Authenticode signing failed.' }

$Installer = Join-Path $OutputDir "Cohavora-$AppVersion-x64-Setup.exe"
$Signature = Get-AuthenticodeSignature -LiteralPath $Installer
if (-not $Signature.SignerCertificate -or $Signature.SignerCertificate.Thumbprint -ne $CertificateThumbprint -or
        $Signature.Status -in @('NotSigned', 'HashMismatch', 'NotSupported', 'Incompatible')) {
    throw 'The generated installer has no matching intact Authenticode signature.'
}
if (-not $SelfSigned) {
    & $SignTool verify /pa /all /tw $Installer
    if ($LASTEXITCODE -ne 0) { throw 'Authenticode verification failed or reported a warning.' }
}
[ordered]@{
    installer = $Installer
    certificate_thumbprint = $CertificateThumbprint
    self_signed = $SelfSigned
    windows_signature_status = [string]$Signature.Status
    public_trust = $(if ($SelfSigned) { 'NOT_TRUSTED_BY_DEFAULT' } else { 'VERIFIED' })
} | ConvertTo-Json
