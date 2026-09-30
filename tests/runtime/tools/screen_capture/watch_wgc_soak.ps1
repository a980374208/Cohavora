param([Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$Host.UI.RawUI.WindowTitle = 'WGC soak - live progress and result'
$directory = (Resolve-Path -LiteralPath $OutputDirectory).Path
Add-Type -AssemblyName System.Windows.Forms
$form = New-Object System.Windows.Forms.Form
$form.Text = 'WGC soak - live progress'
$form.Width = 850
$form.Height = 280
$form.StartPosition = 'CenterScreen'
$label = New-Object System.Windows.Forms.Label
$label.Dock = 'Fill'
$label.Padding = New-Object System.Windows.Forms.Padding(20)
$label.Font = New-Object System.Drawing.Font('Segoe UI', 12)
$label.Text = "Waiting for WGC status...`n$directory"
$form.Controls.Add($label)
$form.Show()
$form.Activate()
Write-Host "WGC same-process soak: 1 hour / 100 cycles"
Write-Host "Evidence: $directory"
Write-Host 'This window only observes. Closing it does not stop the test.'
$last = ''
while ($true) {
    $statusPath = Join-Path $directory 'status.json'
    try { $state = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json }
    catch { Start-Sleep -Seconds 2; continue }
    $elapsed = [int]$state.elapsed_seconds
    $remaining = [Math]::Max(0, [int]$state.seconds + 90 - $elapsed)
    $cycle = $state.latest_sample.cycle
    $phase = $state.latest_sample.phase
    $line = '{0}  {1}  cycle {2}/{3}  elapsed {4}  remaining ~{5}  phase {6}  PID {7}' -f (Get-Date -Format 'HH:mm:ss'), $state.status, $cycle, $state.cycles, ([TimeSpan]::FromSeconds($elapsed)), ([TimeSpan]::FromSeconds($remaining)), $phase, $state.pid
    if ($line -ne $last) { Write-Host $line; $last = $line }
    $label.Text = "WGC same-process soak`nStatus: $($state.status)    Cycle: $cycle / $($state.cycles)`nElapsed: $([TimeSpan]::FromSeconds($elapsed))    Remaining: ~$([TimeSpan]::FromSeconds($remaining))`nPhase: $phase    PID: $($state.pid)`n$directory`nClosing this window does not stop the test."
    [System.Windows.Forms.Application]::DoEvents()
    if ($state.status -in @('PASS','FAIL')) {
        $color = if ($state.status -eq 'PASS') { 'Green' } else { 'Red' }
        Write-Host "FINAL RESULT: $($state.status)" -ForegroundColor $color
        if ($state.status -eq 'FAIL') {
            Get-Content -LiteralPath (Join-Path $directory 'stderr.log') -Tail 10 -ErrorAction SilentlyContinue
        }
        Write-Host "Result: $(Join-Path $directory 'result.json')"
        try {
            Add-Type -AssemblyName System.Windows.Forms
            [System.Media.SystemSounds]::Exclamation.Play()
            [System.Windows.Forms.MessageBox]::Show("WGC soak: $($state.status)`nElapsed: $elapsed seconds`nResult: $directory\result.json", 'WGC soak finished') | Out-Null
        } catch { Write-Warning $_ }
        break
    }
    $runner = Get-Process -Id $state.runner_pid -ErrorAction SilentlyContinue
    if (-not $runner) {
        Write-Host 'RUNNER MISSING: no final verdict; inspect logs. This is not PASS.' -ForegroundColor Red
        [System.Media.SystemSounds]::Exclamation.Play()
        break
    }
    for ($tick = 0; $tick -lt 100; $tick++) {
        [System.Windows.Forms.Application]::DoEvents()
        if ($form.IsDisposed) { exit }
        Start-Sleep -Milliseconds 100
    }
}
if (-not $form.IsDisposed) { [System.Windows.Forms.Application]::Run($form) }
