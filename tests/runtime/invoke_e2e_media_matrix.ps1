param(
    [string]$Url = $env:LIVEKIT_URL,
    [string]$PublisherIdentity = 's8c-publisher',
    [string]$ReceiverIdentity = 's8c-receiver',
    [string]$PublisherTokenEnv = 'LIVEKIT_L3_TOKEN_S8C_PUBLISHER',
    [string]$ReceiverTokenEnv = 'LIVEKIT_L3_TOKEN_S8C_RECEIVER',
    [ValidateSet('vp8', 'h264', 'vp9', 'av1')]
    [string]$ReceiverPublishCodec,
    [ValidateRange(1, 64)][int]$Probes = 10,
    [ValidateRange(0.0, 1.0)][double]$MinimumMarkerSuccessRate = 0.95,
    [ValidateRange(1, 1000000)][long]$MaximumClockUncertaintyUs = 10000,
    [ValidateSet('auto-camera-720', 'vp8-720-high', 'h264-720-high',
        'vp9-svc-camera', 'av1-camera', 'av1-backup-screen',
        'vp8-source-360', 'vp8-simulcast-medium', 'vp8-simulcast-low')]
    [string]$CaseId,
    [string]$RunId = (Get-Date -AsUTC -Format 'yyyyMMddTHHmmssZ')
)

$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$executable = Join-Path $workspace 'build-debug/Debug/test_e2e_media_runtime.exe'
$evidence = Join-Path $workspace "build-debug/evidence/e2e-media-s8c/$RunId"
$processes = @()

function Get-RequiredEnvironmentValue([string]$Name) {
    $value = [Environment]::GetEnvironmentVariable($Name)
    if ([string]::IsNullOrWhiteSpace($value)) {
        throw "Required environment variable is missing: $Name"
    }
    return $value
}

function Convert-KeyValueLine([string]$Line) {
    $values = @{}
    foreach ($match in [regex]::Matches($Line, '(?<key>[A-Za-z0-9_]+)=(?<value>[^\s]+)')) {
        $values[$match.Groups['key'].Value] = $match.Groups['value'].Value
    }
    return $values
}

function Get-LongValue($Values, [string]$Name) {
    if (-not $Values.ContainsKey($Name) -or $Values[$Name] -eq 'UNAVAILABLE') {
        throw "Summary field unavailable: $Name"
    }
    return [long]::Parse($Values[$Name], [Globalization.CultureInfo]::InvariantCulture)
}

function Get-DoubleValue($Values, [string]$Name) {
    if (-not $Values.ContainsKey($Name) -or $Values[$Name] -eq 'UNAVAILABLE') {
        throw "Summary field unavailable: $Name"
    }
    return [double]::Parse($Values[$Name], [Globalization.CultureInfo]::InvariantCulture)
}

function Start-Peer([string]$Name, [string[]]$Arguments, [string]$Token) {
    $environment = @{
        COHAVORA_E2E_RUNTIME_TOKEN = $Token
        RUST_LOG = 'off'
    }
    $process = Start-Process -FilePath $executable -ArgumentList $Arguments `
        -PassThru -WorkingDirectory $workspace -WindowStyle Hidden `
        -Environment $environment `
        -RedirectStandardOutput (Join-Path $evidence "$Name.stdout.log") `
        -RedirectStandardError (Join-Path $evidence "$Name.stderr.log")
    $script:processes += $process
    return $process
}

function Wait-Pair($Publisher, $Receiver, [int]$TimeoutSeconds) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while (-not $Publisher.HasExited -or -not $Receiver.HasExited) {
        if ([DateTime]::UtcNow -ge $deadline) {
            throw 'Peer process deadline expired'
        }
        Start-Sleep -Milliseconds 100
    }
    $Publisher.WaitForExit()
    $Receiver.WaitForExit()
}

if ([string]::IsNullOrWhiteSpace($Url)) {
    throw 'LIVEKIT_URL or -Url is required'
}
if (-not (Test-Path -LiteralPath $executable)) {
    throw "Runtime harness not found: $executable"
}
if ($RunId -notmatch '^[A-Za-z0-9_.-]+$') {
    throw 'RunId must contain only letters, digits, dot, underscore, or dash'
}
if (Test-Path -LiteralPath $evidence) {
    throw "Evidence run already exists: $RunId"
}
$null = New-Item -ItemType Directory -Path $evidence

$publisherToken = Get-RequiredEnvironmentValue $PublisherTokenEnv
$receiverToken = Get-RequiredEnvironmentValue $ReceiverTokenEnv
$matrix = @(
    [pscustomobject]@{ Id = 'auto-camera-720'; Codec = 'auto'; ExpectedCodec = 'vp8'; Source = 'camera'; Width = 1280; Height = 720; Simulcast = $true; Scalability = ''; BackupCodec = 'none'; BackupPolicy = 'prefer-regression'; AutoBackup = $false; Quality = 'high'; ExpectedWidth = 1280; ExpectedHeight = 720 },
    [pscustomobject]@{ Id = 'vp8-720-high'; Codec = 'vp8'; ExpectedCodec = 'vp8'; Source = 'camera'; Width = 1280; Height = 720; Simulcast = $true; Scalability = ''; BackupCodec = 'none'; BackupPolicy = 'prefer-regression'; AutoBackup = $false; Quality = 'high'; ExpectedWidth = 1280; ExpectedHeight = 720 },
    [pscustomobject]@{ Id = 'h264-720-high'; Codec = 'h264'; ExpectedCodec = 'h264'; Source = 'camera'; Width = 1280; Height = 720; Simulcast = $false; Scalability = ''; BackupCodec = 'none'; BackupPolicy = 'prefer-regression'; AutoBackup = $false; Quality = 'high'; ExpectedWidth = 1280; ExpectedHeight = 720 },
    [pscustomobject]@{ Id = 'vp9-svc-camera'; Codec = 'vp9'; ExpectedCodec = 'vp9'; Source = 'camera'; Width = 1280; Height = 720; Simulcast = $false; Scalability = 'L3T3_KEY'; BackupCodec = 'none'; BackupPolicy = 'prefer-regression'; AutoBackup = $false; Quality = 'high'; ExpectedWidth = 1280; ExpectedHeight = 720 },
    [pscustomobject]@{ Id = 'av1-camera'; Codec = 'av1'; ExpectedCodec = 'av1'; Source = 'camera'; Width = 1280; Height = 720; Simulcast = $false; Scalability = 'L1T1'; BackupCodec = 'none'; BackupPolicy = 'prefer-regression'; AutoBackup = $false; Quality = 'high'; ExpectedWidth = 1280; ExpectedHeight = 720 },
    [pscustomobject]@{ Id = 'av1-backup-screen'; Codec = 'av1'; ExpectedCodec = 'av1'; Source = 'screen'; Width = 1280; Height = 720; Simulcast = $true; Scalability = ''; BackupCodec = 'vp8'; BackupPolicy = 'prefer-regression'; AutoBackup = $false; Quality = 'high'; ExpectedWidth = 1280; ExpectedHeight = 720 },
    [pscustomobject]@{ Id = 'vp8-source-360'; Codec = 'vp8'; ExpectedCodec = 'vp8'; Source = 'camera'; Width = 640; Height = 360; Simulcast = $false; Scalability = ''; BackupCodec = 'none'; BackupPolicy = 'prefer-regression'; AutoBackup = $false; Quality = 'high'; ExpectedWidth = 640; ExpectedHeight = 360 },
    [pscustomobject]@{ Id = 'vp8-simulcast-medium'; Codec = 'vp8'; ExpectedCodec = 'vp8'; Source = 'camera'; Width = 1280; Height = 720; Simulcast = $true; Scalability = ''; BackupCodec = 'none'; BackupPolicy = 'prefer-regression'; AutoBackup = $false; Quality = 'medium'; ExpectedWidth = 640; ExpectedHeight = 360 },
    [pscustomobject]@{ Id = 'vp8-simulcast-low'; Codec = 'vp8'; ExpectedCodec = 'vp8'; Source = 'camera'; Width = 1280; Height = 720; Simulcast = $true; Scalability = ''; BackupCodec = 'none'; BackupPolicy = 'prefer-regression'; AutoBackup = $false; Quality = 'low'; ExpectedWidth = 320; ExpectedHeight = 180 }
)
if (-not [string]::IsNullOrWhiteSpace($CaseId)) {
    $matrix = @($matrix | Where-Object Id -eq $CaseId)
}
$results = @()

try {
    foreach ($case in $matrix) {
        $session = "$RunId-$($case.Id)"
        $common = @(
            '--url', $Url,
            '--token-env', 'COHAVORA_E2E_RUNTIME_TOKEN',
            '--session', $session,
            '--phase-id', $case.Id,
            '--codec', $case.Codec,
            '--expected-codec', $case.ExpectedCodec,
            '--source', $case.Source,
            '--backup-codec', $case.BackupCodec,
            '--backup-policy', $case.BackupPolicy,
            '--auto-backup', $case.AutoBackup.ToString().ToLowerInvariant(),
            '--width', [string]$case.Width,
            '--height', [string]$case.Height,
            '--simulcast', $case.Simulcast.ToString().ToLowerInvariant(),
            '--quality', $case.Quality,
            '--probes', [string]$Probes,
            '--shared-clock-ground-truth', 'true'
        )
        if (-not [string]::IsNullOrWhiteSpace($case.Scalability)) {
            $common += @('--scalability-mode', $case.Scalability)
        }
        $receiverArgs = @('--role', 'receiver', '--local-peer', $ReceiverIdentity,
            '--remote-peer', $PublisherIdentity) + $common
        if (-not [string]::IsNullOrWhiteSpace($ReceiverPublishCodec)) {
            $receiverArgs += @('--receiver-publish-codec', $ReceiverPublishCodec)
        }
        $publisherArgs = @('--role', 'publisher', '--local-peer', $PublisherIdentity,
            '--remote-peer', $ReceiverIdentity) + $common

        $receiver = Start-Peer "$($case.Id)-receiver" $receiverArgs $receiverToken
        Start-Sleep -Milliseconds 750
        $publisher = Start-Peer "$($case.Id)-publisher" $publisherArgs $publisherToken
        Wait-Pair $publisher $receiver (45 + $Probes * 5)

        $publisherLog = Join-Path $evidence "$($case.Id)-publisher.stdout.log"
        $summaryLine = Get-Content -LiteralPath $publisherLog |
            Where-Object { $_ -like 'E2E_SUMMARY role=publisher *' } |
            Select-Object -Last 1
        if ([string]::IsNullOrWhiteSpace($summaryLine)) {
            throw "Publisher summary missing for $($case.Id)"
        }
        $summary = Convert-KeyValueLine $summaryLine
        $receiverLog = Join-Path $evidence "$($case.Id)-receiver.stdout.log"
        $receiverSummaryLine = Get-Content -LiteralPath $receiverLog |
            Where-Object { $_ -like 'E2E_SUMMARY role=receiver *' } |
            Select-Object -Last 1
        if ([string]::IsNullOrWhiteSpace($receiverSummaryLine)) {
            throw "Receiver summary missing for $($case.Id)"
        }
        $receiverSummary = Convert-KeyValueLine $receiverSummaryLine
        $markerRate = Get-DoubleValue $summary 'marker_success_rate'
        $clockP95 = Get-LongValue $summary 'clock_uncertainty_p95_us'
        $clockMax = Get-LongValue $summary 'clock_uncertainty_max_us'
        $e2e02ErrorP95 = Get-LongValue $summary 'e2e02_error_p95_us'
        $e2e03ErrorP95 = Get-LongValue $summary 'e2e03_error_p95_us'
        $receivedWidthMin = Get-LongValue $summary 'received_width_min'
        $receivedWidthMax = Get-LongValue $summary 'received_width_max'
        $receivedHeightMin = Get-LongValue $summary 'received_height_min'
        $receivedHeightMax = Get-LongValue $summary 'received_height_max'
        $encodedFrames = Get-LongValue $summary 'encoded_frames'
        $sentPackets = Get-LongValue $summary 'sent_packets'
        $decodedFrames = Get-LongValue $receiverSummary 'decoded_frames'
        $observedCodecs = @($summary['observed_codecs'] -split ',')

        $processPass = $publisher.ExitCode -eq 0 -and $receiver.ExitCode -eq 0
        $markerPass = $markerRate -ge $MinimumMarkerSuccessRate
        $clockPass = $clockP95 -le $MaximumClockUncertaintyUs
        $errorPass = $e2e02ErrorP95 -le $clockMax -and
            $e2e03ErrorP95 -le $clockMax
        $dimensionPass = $receivedWidthMin -eq $case.ExpectedWidth -and
            $receivedWidthMax -eq $case.ExpectedWidth -and
            $receivedHeightMin -eq $case.ExpectedHeight -and
            $receivedHeightMax -eq $case.ExpectedHeight
        $codecPass = $summary['effective_codec'] -eq $case.ExpectedCodec -and
            $observedCodecs -contains $case.ExpectedCodec
        $counterPass = $encodedFrames -gt 0 -and $sentPackets -gt 0 -and
            $decodedFrames -gt 0 -and $receiverSummary['first_frame_detected'] -eq 'true'
        $uplinkPass = [string]::IsNullOrWhiteSpace($ReceiverPublishCodec) -or
            ($receiverSummary['local_publish_codec'] -eq $ReceiverPublishCodec -and
             $receiverSummary['local_uplink_verified'] -eq 'true')
        $passed = $processPass -and $markerPass -and $clockPass -and
            $errorPass -and $dimensionPass -and $codecPass -and $counterPass -and $uplinkPass

        $results += [pscustomobject]@{
            Phase = $case.Id
            Codec = $case.Codec
            EffectiveCodec = $summary['effective_codec']
            ObservedCodecs = $summary['observed_codecs']
            ReceiverPublishCodec = $ReceiverPublishCodec
            ReceiverUplinkVerified = $receiverSummary['local_uplink_verified']
            SourceKind = $case.Source
            Mode = $summary['mode']
            Scalability = $summary['scalability']
            BackupPolicy = $case.BackupPolicy
            Source = "$($case.Width)x$($case.Height)"
            Simulcast = $case.Simulcast
            Quality = $case.Quality
            Received = "${receivedWidthMin}x${receivedHeightMin}"
            MarkerSuccessRate = $markerRate
            ClockUncertaintyP95Us = $clockP95
            E2E02ErrorP95Us = $e2e02ErrorP95
            E2E03ErrorP95Us = $e2e03ErrorP95
            EncodedFrames = $encodedFrames
            SentPackets = $sentPackets
            DecodedFrames = $decodedFrames
            Passed = $passed
        }
        Write-Output ("[S8C_CASE] phase={0} codec={1} quality={2} received={3}x{4} marker_rate={5:F3} clock_p95_us={6} e2e02_error_p95_us={7} e2e03_error_p95_us={8} passed={9}" -f
            $case.Id, $case.Codec, $case.Quality, $receivedWidthMin,
            $receivedHeightMin, $markerRate, $clockP95, $e2e02ErrorP95,
            $e2e03ErrorP95, $passed.ToString().ToLowerInvariant())
    }

    $csv = Join-Path $evidence 'matrix-summary.csv'
    $results | Export-Csv -LiteralPath $csv -NoTypeInformation -Encoding utf8
    $matrixPassed = @($results | Where-Object { -not $_.Passed }).Count -eq 0
    Write-Output "[S8C_MATRIX] passed=$($matrixPassed.ToString().ToLowerInvariant()) evidence=$evidence"
    $results | Format-Table -AutoSize | Out-String | Write-Output
    if (-not $matrixPassed) { exit 1 }
} catch {
    # Do not echo exception details: process setup errors can contain credential
    # sources. Peer logs contain only sanitized runtime output.
    Write-Output "[S8C_MATRIX] INCONCLUSIVE evidence=$evidence"
    exit 2
} finally {
    foreach ($process in $processes) {
        if (-not $process.HasExited) {
            $process.Kill($true)
            $process.WaitForExit()
        }
    }
}
