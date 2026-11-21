# Repeatable test on the test copy: start the game, watch a saved replay, time the load and record fps. Real mouse clicks in the game's window.
# -Row is the replay's line on the Replays screen (sorted by name, 15 apart): 287 auto_save, 302 bench_big, 317 bench_last,
# 332 bench_murcia (the user's rainy historic Murcia battle, 2026-11-07), 347 bench_test.
param([string]$Label = 'run', [string]$Switches = '', [switch]$Measure, [int]$Row = 302, [int]$BattleSeconds = 150, [int]$MemoryLimitGB = 20)
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$tools = Join-Path $root '..\tools'
$pkg = (Resolve-Path (Join-Path $root '..\package\attila64')).Path
$log = Join-Path $pkg 'attila64_log.txt'
$out = Join-Path $root "bench_$Label"
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out | Out-Null
if (Get-Process attila64 -ErrorAction SilentlyContinue) { 'GAME ALREADY OPEN, not testing'; exit 1 }
Copy-Item (Join-Path $root 'attila64.exe') (Join-Path $pkg 'attila64.exe') -Force
Remove-Item (Join-Path $pkg 'attila64_profile_*.txt') -ErrorAction SilentlyContinue
$env:ATTILA_GAME_DIR = 'C:\Program Files (x86)\Steam\steamapps\common\Total War Attila'
$env:ATTILA_NO_PICKER = '1'
Remove-Item Env:ATTILA_PROFILE, Env:ATTILA_ARGS, Env:ATTILA_VAB, Env:ATTILA_UBM, Env:ATTILA_DROP_INVALIDATE, Env:ATTILA_ORPHAN_LIMIT_MB -ErrorAction SilentlyContinue
if ($Measure) { $env:ATTILA_PROFILE = '1' }
foreach ($kv in ($Switches -split ';' | Where-Object { $_ })) { $x = $kv -split '='; Set-Item "Env:$($x[0])" $x[1] }
function Move2($spots, $pause = 2) { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tools 'real_click.ps1') -Clicks $spots -Pause $pause -Hover | Out-Null }
function Click($spots, $pause = 2) { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tools 'real_click.ps1') -Clicks $spots -Pause $pause | Out-Null }
function Lines { if (Test-Path $log) { @(Get-Content $log -ErrorAction SilentlyContinue) } else { @() } }
$g = Start-Process (Join-Path $pkg 'attila64.exe') -PassThru -RedirectStandardError (Join-Path $out 'err.txt') -RedirectStandardOutput (Join-Path $out 'out.txt')
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt 150) {
    Start-Sleep -Seconds 3
    if ($g.HasExited) { "game ended during start, code $($g.ExitCode)"; Lines | Where-Object { $_ -match 'CRASH|FATAL' } | Select-Object -First 3; exit 1 }
    if (Lines | Where-Object { $_ -match '^fps' }) { break }
}
"=== $Label [$Switches]: menu after {0:n0} s" -f ((Get-Date) - $t0).TotalSeconds
Lines | Where-Object { $_ -match 'settings in use' } | Select-Object -First 1
Click '640,360;640,360' 3
Start-Sleep -Seconds 8
Move2 '640,600' 1
Move2 '20,303' 2
Click '113,426' 5
# the row must really be selected: without a selection Watch Replay plays the first row (auto_save, the last battle fought)
Click "468,$Row" 2
Click "468,$Row" 2
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tools 'window_shot.ps1') -Name attila64 -Out (Join-Path $out 'a_replays.png') | Out-Null
Click '511,474' 1
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'watch_load.ps1') -Label $Label -BattleSeconds $BattleSeconds -MemoryLimitGB $MemoryLimitGB | ForEach-Object { "$_" }
$g.Refresh()
if (-not $g.HasExited) { $g.Kill() }
Start-Sleep -Seconds 3
Move-Item (Join-Path $pkg 'attila64_profile_*.txt') $out -Force -ErrorAction SilentlyContinue
