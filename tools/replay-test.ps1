# Unattended determinism replay on the PC: replays a recording (optionally fast-forwarded) and prints the report.
# Usage: powershell -ExecutionPolicy Bypass -File tools\replay-test.ps1 [-Recording path] [-Speed 1]
#        [-TimeoutMinutes 12]
param(
    [string]$Recording = "$PSScriptRoot\..\det\run1.bin",
    [int]$Speed = 1,
    [int]$TimeoutMinutes = 12
)
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
$build = "$repo\build-windows"
$work = "$repo\det\replaytest"
New-Item -ItemType Directory -Force $work | Out-Null
Get-Process SnowboardKidsRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

Copy-Item $Recording "$work\replay.bin" -Force
Copy-Item "$Recording.save" "$work\replay.bin.save" -Force
Remove-Item "$work\replay.bin.report.txt" -ErrorAction SilentlyContinue

$env:SK1_AUTOSTART = "1"
$env:SK1_DET_REPLAY = "$work\replay.bin"
$env:SK1_DET_SPEED = "$Speed"
$env:SK1_NET_HOST = ""; $env:SK1_NET_JOIN = ""
$started = Get-Date
$proc = Start-Process -FilePath "$build\SnowboardKidsRecompiled.exe" -WorkingDirectory $build -PassThru
$report = "$work\replay.bin.report.txt"
$deadline = $started.AddMinutes($TimeoutMinutes)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 5
    if ((Test-Path $report) -and (Select-String -Path $report -Pattern "replay finished" -Quiet)) { break }
    if ($proc.HasExited) { Write-Host "game exited early (code $($proc.ExitCode))"; break }
}
$elapsed = (Get-Date) - $started
if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
Write-Host ("Speed {0}x, elapsed {1:mm\:ss}" -f $Speed, $elapsed)
if (Test-Path $report) { Get-Content $report -TotalCount 12 } else { Write-Host "(no report)" }
