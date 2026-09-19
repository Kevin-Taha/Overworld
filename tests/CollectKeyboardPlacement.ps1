[CmdletBinding()]
param(
    [string]$ProbePath = (Join-Path $PSScriptRoot 'gamepad_keyboard_placement_probe.exe'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'results')
)
$ErrorActionPreference = 'Stop'
if (!(Test-Path -LiteralPath $ProbePath -PathType Leaf)) {
    throw 'Keyboard placement probe executable not found. Build the explicit probe target first.'
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$report = Join-Path $OutputDirectory "keyboard-placement-$stamp.txt"
$errors = Join-Path $OutputDirectory "keyboard-placement-$stamp-errors.txt"
Write-Host 'No application or Start menu will be opened automatically.'
Write-Host 'Starting the diagnostic: after 5 seconds, open Start and press Y to reproduce.'
Write-Host 'Leave the invisible keyboard attempt active for about 10 seconds.'
Write-Host 'Then close Start yourself so the app status can be captured.'
Write-Host 'Do not switch to private documents during this diagnostic.'
Write-Host 'Recording window metadata for 20 seconds after the countdown. No input or screenshots.'
$watch = [Diagnostics.Stopwatch]::StartNew()
$process = Start-Process -FilePath (Resolve-Path -LiteralPath $ProbePath).Path `
    -ArgumentList '--metadata-only' -NoNewWindow -PassThru `
    -RedirectStandardOutput $report -RedirectStandardError $errors
for ($remaining = 5; $remaining -gt 0; --$remaining) {
    Write-Host "Starting in $remaining..."
    Start-Sleep -Seconds 1
}
Write-Host 'NOW: open Start and press Y. Leave the attempt active for about 10 seconds, then close Start.'
$waitMilliseconds = [int][Math]::Max(0, 35000 - $watch.ElapsedMilliseconds)
if (!$process.WaitForExit($waitMilliseconds)) {
    Stop-Process -Id $process.Id -Force
    throw "Diagnostic exceeded its 35-second watchdog and was stopped. Partial report: $report"
}
$process.WaitForExit()
Write-Host "Report: $report"
if ($process.ExitCode) {
    throw "Diagnostic exited with code $($process.ExitCode). See $report and $errors."
}
Write-Host 'Finished. Return to the assistant and say the diagnostic is ready.'
