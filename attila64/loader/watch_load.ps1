# Times a load that was just started in the running test copy, then records fps for a while. Stops the game if its memory passes the limit.
# -StartClick is clicked once loaded, such as the Start Battle button of a custom battle.
# -LoadMark is the log line that means loaded: '^steady for' (25 fps for 8 s), or '^slow period ended' for battles that run under 25 fps.
param([string]$Label = 'run', [int]$BattleSeconds = 180, [int]$MaxLoad = 720, [int]$MemoryLimitGB = 20, [string]$StartClick = '', [string]$LoadMark = '^steady for')
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$tools = Join-Path $root '..\tools'
$pkg = (Resolve-Path (Join-Path $root '..\package\attila64')).Path
$log = Join-Path $pkg 'attila64_log.txt'
$out = Join-Path $root "bench_$Label"
New-Item -ItemType Directory -Force $out | Out-Null
function Shot($name) { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tools 'window_shot.ps1') -Name attila64 -Out (Join-Path $out "$name.png") | Out-Null }
function Lines { if (Test-Path $log) { @(Get-Content $log -ErrorAction SilentlyContinue) } else { @() } }
$g = Get-Process attila64 -ErrorAction Stop | Select-Object -First 1
$peak = 0
$before = @(Lines | Where-Object { $_ -match $LoadMark }).Count
$tLoad = Get-Date
$loaded = $false
while (((Get-Date) - $tLoad).TotalSeconds -lt $MaxLoad) {
    Start-Sleep -Seconds 3
    $g.Refresh()
    if ($g.HasExited) { "game ended during load"; Lines | Where-Object { $_ -match 'CRASH|FATAL' } | Select-Object -First 3; exit 1 }
    $mb = [int]($g.PrivateMemorySize64 / 1MB); if ($mb -gt $peak) { $peak = $mb }
    if ($mb -gt $MemoryLimitGB * 1024) { "MEMORY LIMIT: private $mb MB after {0:n0} s, stopping the game" -f ((Get-Date) - $tLoad).TotalSeconds; $g.Kill(); Start-Sleep -Seconds 2; Copy-Item $log (Join-Path $out 'log.txt') -Force; exit 2 }
    if (@(Lines | Where-Object { $_ -match $LoadMark }).Count -gt $before -and $g.Responding) { $loaded = $true; break }
}
$loadSecs = ((Get-Date) - $tLoad).TotalSeconds - $(if ($LoadMark -eq '^steady for') { 8 } else { 0 })
if (-not $loaded) { "NOT LOADED after {0:n0} s, peak private $peak MB" -f $loadSecs; Shot 'c_stuck'; exit 1 }
"LOAD {0:n0} s, peak private $peak MB" -f $loadSecs
Lines | Where-Object { $_ -match '^slow period' } | Select-Object -Last 3
Shot 'c_loaded'
if ($StartClick) { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tools 'real_click.ps1') -Clicks $StartClick -Pause 2 | Out-Null }
$n0 = @(Lines | Where-Object { $_ -match '^fps' }).Count
$t = Get-Date
$half = $false
while (((Get-Date) - $t).TotalSeconds -lt $BattleSeconds) {
    Start-Sleep -Seconds 3
    $g.Refresh()
    if ($g.HasExited) { "game ended during battle, code $($g.ExitCode)"; break }
    $mb = [int]($g.PrivateMemorySize64 / 1MB); if ($mb -gt $peak) { $peak = $mb }
    if ($mb -gt $MemoryLimitGB * 1024) { "MEMORY LIMIT in battle: private $mb MB, stopping the game"; $g.Kill(); break }
    if (-not $half -and ((Get-Date) - $t).TotalSeconds -gt $BattleSeconds / 2) { $half = $true; Shot 'e_battle1' }
}
$g.Refresh()
if (-not $g.HasExited) { Shot 'f_battle2' }
$all = @(Lines)
$fps = @($all | Where-Object { $_ -match '^fps' } | Select-Object -Skip $n0 | ForEach-Object { [double](($_ -split ' ')[1]) })
"fps windows: " + (($fps | ForEach-Object { '{0:n1}' -f $_ }) -join ', ')
if ($fps.Count) { "AVERAGE {0:n1} fps" -f (($fps | Measure-Object -Average).Average) }
$all | Where-Object { $_ -match 'CRASH|FATAL|WRONG' } | Select-Object -First 5
Copy-Item $log (Join-Path $out 'log.txt') -Force
"peak private $peak MB; results in $out"
