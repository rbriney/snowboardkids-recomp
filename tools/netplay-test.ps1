# Unattended netplay test on one PC: several copies of the game play a recording against each other over 127.0.0.1.
# Each copy takes its own players' inputs from the recording (SK1_NET_SCRIPT) and the determinism harness compares
# every copy's game state against the recording on every tick.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\netplay-test.ps1 [-Recording path]
#        [-HostPlayers 1] [-Joiners 1] [-Loss 0] [-Latency 0] [-Jitter 0] [-Delay 3 (0: adapt to ping)] [-TimeoutMinutes 12]
#        [-KillJoinerAfterSeconds 0]
#   -Joiners takes the local player count of each joining copy, e.g. -Joiners 1,1,1 for four copies of one player,
#   or -HostPlayers 2 -Joiners 2 for two copies with two players each.
#   -SnapshotTicks 100,200 saves raw RDRAM from every copy after those ticks (for byte-level comparison).
#   -OwnView shows each copy only its own player, full screen, in races.
#   -RecordedPlayers keeps the game's player count at the recording's (e.g. 2 players with four copies), so the
#   recorded race plays out exactly and the extra copies follow along; without it extra players repeat the recorded
#   players' inputs.
#   -KillJoinerAfterSeconds closes the last joiner part-way through, to test the host continuing without it.
param(
    [string]$Recording = "$PSScriptRoot\..\det\run1.bin",
    [int]$HostPlayers = 1,
    [string]$Joiners = "1",
    [int]$Loss = 0,
    [int]$Latency = 0,
    [int]$Jitter = 0,
    [int]$Delay = 3,
    [int]$Port = 7777,
    [int]$TimeoutMinutes = 12,
    [int]$KillJoinerAfterSeconds = 0,
    [switch]$OwnView,
    [switch]$RecordedPlayers,
    [string]$SnapshotTicks = ""
)
$ErrorActionPreference = "Stop"
# -File passes "1,1,1" as one string, so split it here.
$JoinerList = @($Joiners -split "," | ForEach-Object { [int]$_ })
$repo = Split-Path $PSScriptRoot -Parent
$build = "$repo\build-windows"
$exe = "$build\SnowboardKidsRecompiled.exe"
$work = "$repo\det\nettest"
New-Item -ItemType Directory -Force $work | Out-Null

Get-Process SnowboardKidsRecompiled -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

$total = $HostPlayers + ($JoinerList | Measure-Object -Sum).Sum
$roles = @("host") + (1..$JoinerList.Count | ForEach-Object { "join$_" })
foreach ($role in $roles) {
    Copy-Item $Recording "$work\$role.bin" -Force
    Copy-Item "$Recording.save" "$work\$role.bin.save" -Force
    Remove-Item "$work\$role.bin.report.txt", "$work\$role.bin.tick*.rdram", "$work\$role.bin.snapshots" -ErrorAction SilentlyContinue
    if ($SnapshotTicks) { ($SnapshotTicks -split ",") | Set-Content "$work\$role.bin.snapshots" -Encoding ascii }
}
Remove-Item "$build\netplay-*.log" -ErrorAction SilentlyContinue

function Start-Copy([string]$role, [int]$players, [string]$instance) {
    $env:SK1_AUTOSTART = "1"
    $env:SK1_NET_SCRIPT = "1"
    $env:SK1_NET_BIND = "127.0.0.1"
    $env:SK1_NET_INSTANCE = $instance
    $env:SK1_NET_LOCAL_PLAYERS = "$players"
    $env:SK1_DET_REPLAY = "$work\$role.bin"
    $env:SK1_DET_SPEED = ""
    $env:SK1_NET_SIM_LOSS = "$Loss"
    $env:SK1_NET_SIM_LATENCY = "$Latency"
    $env:SK1_NET_SIM_JITTER = "$Jitter"
    $env:SK1_NET_OWN_VIEW = $(if ($OwnView) { "1" } else { "" })
    $env:SK1_NET_RECORDED_PLAYERS = $(if ($RecordedPlayers) { "1" } else { "" })
    if ($role -eq "host") {
        $env:SK1_NET_HOST = "$Port"; $env:SK1_NET_JOIN = ""; $env:SK1_NET_DELAY = $(if ($Delay -gt 0) { "$Delay" } else { "" }); $env:SK1_NET_PLAYERS = "$total"
    } else {
        $env:SK1_NET_HOST = ""; $env:SK1_NET_JOIN = "127.0.0.1:$Port"
    }
    return Start-Process -FilePath $exe -WorkingDirectory $build -PassThru
}

$started = Get-Date
$procs = [ordered]@{}
$procs["host"] = Start-Copy "host" $HostPlayers ""
Start-Sleep -Seconds 2
for ($i = 0; $i -lt $JoinerList.Count; $i++) {
    $role = "join$($i + 1)"
    $procs[$role] = Start-Copy $role $JoinerList[$i] "$($i + 1)"
    Start-Sleep -Seconds 2
}
Write-Host ("Started {0} copies ({1} players: host {2}, joiners {3}); loss {4}%, latency {5} ms, jitter {6} ms, delay {7}" -f `
    $roles.Count, $total, $HostPlayers, ($JoinerList -join ","), $Loss, $Latency, $Jitter, $Delay)

$killed = $null
$deadline = $started.AddMinutes($TimeoutMinutes)
$finished = @{}
while ((Get-Date) -lt $deadline -and $finished.Count -lt $roles.Count) {
    Start-Sleep -Seconds 5
    if ($KillJoinerAfterSeconds -gt 0 -and $null -eq $killed -and ((Get-Date) - $started).TotalSeconds -ge $KillJoinerAfterSeconds) {
        $killed = $roles[-1]
        Stop-Process -Id $procs[$killed].Id -Force
        $finished[$killed] = $true
        Write-Host "Closed $killed after $KillJoinerAfterSeconds s"
    }
    foreach ($role in $roles) {
        $report = "$work\$role.bin.report.txt"
        if (-not $finished.ContainsKey($role) -and (Test-Path $report) -and (Select-String -Path $report -Pattern "replay finished" -Quiet)) {
            $finished[$role] = $true
        }
        if (-not $finished.ContainsKey($role) -and $procs[$role].HasExited) {
            $finished[$role] = $true
            Write-Host "$role exited early (code $($procs[$role].ExitCode))"
        }
    }
}
$elapsed = (Get-Date) - $started
foreach ($role in $roles) {
    if (-not $procs[$role].HasExited) { Stop-Process -Id $procs[$role].Id -Force }
}
Write-Host ("Elapsed {0:mm\:ss}" -f $elapsed)
foreach ($role in $roles) {
    Write-Host "===== $role report"
    if (Test-Path "$work\$role.bin.report.txt") { Get-Content "$work\$role.bin.report.txt" -TotalCount 6 | Select-Object -Skip 2 } else { Write-Host "(no report)" }
}
Get-ChildItem "$build\netplay-*.log" | ForEach-Object {
    Write-Host "===== $($_.Name)"
    Get-Content $_.FullName | Where-Object { $_ -notmatch "^tick " }
    Get-Content $_.FullName | Where-Object { $_ -match "^tick " } | Select-Object -Last 1
}
