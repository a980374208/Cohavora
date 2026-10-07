param(
    [Parameter(Mandatory=$true)][string]$Root,
    [Parameter(Mandatory=$true)][string]$TargetDirectory,
    [string]$Executable = 'E:\vsSource\WebRTC\live-kit-test\build-debug\RelWithDebInfo\directory_icon_probe.exe',
    [string]$WorkingDirectory = 'E:\vsSource\WebRTC\live-kit-test\build-debug\src\app\RelWithDebInfo'
)
$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $Root) { throw 'NEW_OUTPUT_DIRECTORY_REQUIRED' }
foreach ($path in @($TargetDirectory, $WorkingDirectory)) {
    if (![IO.Path]::IsPathRooted($path) -or !(Test-Path -LiteralPath $path -PathType Container)) {
        throw 'EXISTING_ABSOLUTE_DIRECTORY_REQUIRED'
    }
}
if (!(Test-Path -LiteralPath $Executable -PathType Leaf)) { throw 'PROBE_MISSING' }
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$null = New-Item -ItemType Directory -Path $Root
$inputs = [ordered]@{
    configuration = 'RelWithDebInfo'
    executable = $Executable
    executable_sha256 = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash
    provider_sha256 = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot '../../../../../src/ui/directory_icon_provider.h') -Algorithm SHA256).Hash
    source_sha256 = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'directory_icon_probe.cpp') -Algorithm SHA256).Hash
    runner_sha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
    working_directory = $WorkingDirectory
    target_directory = $TargetDirectory
    protocol = 'visible fresh process; empty profile; 1s baseline/5s open/2s quiet; 16 observations x 45 name reads'
    old_runner_identity = 'UNKNOWN_RECONSTRUCTED_PROTOCOL'
    started_utc = [DateTime]::UtcNow.ToString('o')
    qualification_credit = 0
    formal_credit = 0
}
$inputs | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Root 'inputs.json') -Encoding UTF8
$results = @{}
foreach ($mode in @('baseline', 'generic')) {
    $profile = Join-Path $Root ($mode + '-profile')
    $null = New-Item -ItemType Directory -Path $profile
    $stdout = Join-Path $Root ($mode + '.stdout')
    $stderr = Join-Path $Root ($mode + '.stderr')
    $process = Start-Process -FilePath $Executable -WindowStyle Normal -PassThru -WorkingDirectory $WorkingDirectory `
        -ArgumentList @($mode, ('"' + $profile + '"'), ('"' + $TargetDirectory + '"')) `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    # Keep the native handle before exit so Windows PowerShell retains ExitCode.
    $null = $process.Handle
    $startedTicks = $process.StartTime.ToUniversalTime().Ticks
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $reads = 0
    $observations = 0
    $uiaError = $null
    try {
        while (!$process.WaitForExit(100)) {
            if ($clock.Elapsed.TotalSeconds -gt 20) { throw 'PROBE_OUTER_DEADLINE' }
            if ($observations -ge 16) { continue }
            $condition = [Windows.Automation.PropertyCondition]::new(
                [Windows.Automation.AutomationElement]::ProcessIdProperty, $process.Id)
            $window = [Windows.Automation.AutomationElement]::RootElement.FindFirst(
                [Windows.Automation.TreeScope]::Children, $condition)
            if (!$window -or $window.Current.IsOffscreen) { continue }
            $children = $window.FindAll([Windows.Automation.TreeScope]::Descendants,
                [Windows.Automation.Condition]::TrueCondition)
            if (!$children.Count) { continue }
            # Exercise accessibility only in this owned window; never persist names.
            for ($i = 0; $i -lt 45; ++$i) {
                $null = $children.Item($i % $children.Count).Current.Name
                ++$reads
            }
            ++$observations
        }
    } catch {
        $uiaError = $_.Exception.GetType().FullName
    } finally {
        if (!$process.WaitForExit(1000)) {
            if ($process.StartTime.ToUniversalTime().Ticks -ne $startedTicks) { throw 'OWNER_CHANGED' }
            $process.Kill()
            $null = $process.WaitForExit(5000)
        }
        $process.Refresh()
        $exit = [ordered]@{ exit_code=$process.ExitCode; pid=$process.Id; start_ticks=$startedTicks
            uia_name_reads=$reads; uia_window_observations=$observations; uia_error=$uiaError }
        $exit | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Root ($mode + '-exit.json')) -Encoding UTF8
        $process.Dispose()
    }
    $result = Get-Content -LiteralPath $stdout | Select-Object -Last 1 | ConvertFrom-Json
    $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $Root ($mode + '.json')) -Encoding UTF8
    if ($exit.exit_code -ne 0 -or $reads -ne 720 -or $observations -ne 16 -or $uiaError -or
        $result.status -ne 'CAPTURED_DIAGNOSTIC_ONLY' -or !$result.opened_visible) {
        throw 'CONTROL_PROTOCOL_INCOMPLETE'
    }
    foreach ($phase in @('before_provider','after_provider','baseline','after_constructor','after_show','opened','after_close','closed_quiet')) {
        if (!$result.$phase.memory_query_ok -or !$result.$phase.module_query_ok) { throw 'MEASUREMENT_INCOMPLETE' }
    }
    $results[$mode] = $result
}
$baselineDelta = $results.baseline.closed_quiet.private_bytes - $results.baseline.baseline.private_bytes
$genericDelta = $results.generic.closed_quiet.private_bytes - $results.generic.baseline.private_bytes
$summary = [ordered]@{
    status = 'CAPTURED_LOCAL_CAUSAL_CONTROL_ONLY'
    baseline_delta_bytes = $baselineDelta
    generic_delta_bytes = $genericDelta
    delta_difference_bytes = $baselineDelta - $genericDelta
    baseline_yun_shell_loaded = $results.baseline.closed_quiet.yun_shell_loaded
    generic_yun_shell_loaded = $results.generic.closed_quiet.yun_shell_loaded
    configuration = 'RelWithDebInfo'
    memory_repair_closed = $false
    original_growth_fully_attributed = $false
    qualification_credit = 0
    formal_credit = 0
}
$summary | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Root 'result.json') -Encoding UTF8
$summary | ConvertTo-Json
