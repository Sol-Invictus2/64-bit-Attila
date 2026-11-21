# Automatic test on the test copy: start the game, open the same battle with real clicks in the game's window, time the load, then the fps in battle.
# -Label names the run. -Switches 'A=1;B=2' sets switches. -Measure turns the measuring on. -BattleSeconds is how long to run the battle.
# Safety: the game is stopped if its private memory passes -MemoryLimitGB.
param([string]$Label = 'run', [string]$Switches = '', [switch]$Measure, [int]$BattleSeconds = 180, [int]$MaxLoad = 720, [int]$MemoryLimitGB = 20)
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

function Shot($name) { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tools 'window_shot.ps1') -Name attila64 -Out (Join-Path $out "$name.png") | Out-Null }
function Click($spots, $pause = 3) { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tools 'real_click.ps1') -Clicks $spots -Pause $pause | Out-Null }
function Lines { if (Test-Path $log) { @(Get-Content $log -ErrorAction SilentlyContinue) } else { @() } }
$script:peak = 0
function Guard($g) {
    $g.Refresh()
    if ($g.HasExited) { return }
    $mb = [int]($g.PrivateMemorySize64 / 1MB)
    if ($mb -gt $script:peak) { $script:peak = $mb }
    if ($mb -gt $MemoryLimitGB * 1024) { "MEMORY LIMIT: private $mb MB, stopping the game"; $g.Kill(); Start-Sleep -Seconds 2; Copy-Item $log (Join-Path $out 'log.txt') -Force; exit 2 }
}
function Finish($g) {
    Start-Sleep -Seconds 2
    Copy-Item $log (Join-Path $out 'log.txt') -Force -ErrorAction SilentlyContinue
    Move-Item (Join-Path $pkg 'attila64_profile_*.txt') $out -Force -ErrorAction SilentlyContinue
    "peak private memory $script:peak MB; results in $out"
}

$g = Start-Process (Join-Path $pkg 'attila64.exe') -PassThru -RedirectStandardError (Join-Path $out 'err.txt') -RedirectStandardOutput (Join-Path $out 'out.txt')
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt 150) {
    Start-Sleep -Seconds 3
    if ($g.HasExited) { "game ended during start, code $($g.ExitCode)"; Lines | Where-Object { $_ -match 'CRASH|FATAL' } | Select-Object -First 3; Finish $g; exit 1 }
    if (Lines | Where-Object { $_ -match '^fps' }) { break }
}
"menu after {0:n0} s" -f ((Get-Date) - $t0).TotalSeconds
Lines | Where-Object { $_ -match 'settings in use|^setting' } | Select-Object -First 3
Click '640,360;640,360' 3
Start-Sleep -Seconds 8
Click '20,303' 3
Shot 'a_menu'
$before = @(Lines | Where-Object { $_ -match '^steady for' }).Count
Click '20,303' 2
$tLoad = Get-Date
Start-Sleep -Seconds 4
Shot 'b_loading'

$loaded = $false
while (((Get-Date) - $tLoad).TotalSeconds -lt $MaxLoad) {
    Start-Sleep -Seconds 3
    if ($g.HasExited) { "game ended during load, code $($g.ExitCode)"; Lines | Where-Object { $_ -match 'CRASH|FATAL' } | Select-Object -First 3; Finish $g; exit 1 }
    Guard $g
    if (@(Lines | Where-Object { $_ -match '^steady for' }).Count -gt $before -and $g.Responding) { $loaded = $true; break }
}
$loadSecs = ((Get-Date) - $tLoad).TotalSeconds - 8
if (-not $loaded) { "NOT LOADED after {0:n0} s" -f $loadSecs; Shot 'c_stuck'; if (-not $g.HasExited) { $g.Kill() }; Finish $g; exit 1 }
"LOAD {0:n0} s" -f $loadSecs
Lines | Where-Object { $_ -match '^slow period' } | Select-Object -Last 4
Shot 'c_loaded'
Click '640,490' 5
Shot 'd_deploy'
Click '640,79' 2
$n0 = @(Lines | Where-Object { $_ -match '^fps' }).Count
$tBattle = Get-Date
$half = $false
while (((Get-Date) - $tBattle).TotalSeconds -lt $BattleSeconds) {
    Start-Sleep -Seconds 3
    if ($g.HasExited) { "game ended during battle, code $($g.ExitCode)"; break }
    Guard $g
    if (-not $half -and ((Get-Date) - $tBattle).TotalSeconds -gt $BattleSeconds / 2) { $half = $true; Shot 'e_battle1' }
}
if (-not $g.HasExited) { Shot 'f_battle2' }
$all = @(Lines)
$fps = @($all | Where-Object { $_ -match '^fps' } | Select-Object -Skip $n0 | ForEach-Object { [double](($_ -split ' ')[1]) })
"battle fps windows: " + (($fps | ForEach-Object { '{0:n1}' -f $_ }) -join ', ')
if ($fps.Count) { "BATTLE AVERAGE {0:n1} fps" -f (($fps | Measure-Object -Average).Average) }
$all | Where-Object { $_ -match 'CRASH|FATAL|WRONG' } | Select-Object -First 5
if (-not $g.HasExited) { $g.Kill() } else { "game ended by itself, code $($g.ExitCode)" }
Finish $g
