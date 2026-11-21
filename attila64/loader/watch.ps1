# Run the built loader visibly, watch it, and capture only the game's own window. No build.
param([int]$Seconds = 75, [string]$Shot = 'win1.png', [switch]$Sample, [string]$GameArgs = '')
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$game = 'C:\Program Files (x86)\Steam\steamapps\content\app_325610\depot_325624\Attila'
$lib = (Resolve-Path (Join-Path $root '..\lib')).Path
$work = 'C:\Program Files (x86)\Steam\steamapps\common\Total War Attila'
if ($Sample) { $env:ATTILA_SAMPLE = '1' }
$argv = @("`"$game`"", "`"$lib`"")
if ($GameArgs) { $argv += $GameArgs }
$p = Start-Process -FilePath (Join-Path $root 'attila64.exe') -ArgumentList $argv -WorkingDirectory $work -PassThru -RedirectStandardError (Join-Path $root 'err.txt') -RedirectStandardOutput (Join-Path $root 'out.txt')
$step = 15
for ($t = $step; $t -le $Seconds; $t += $step) {
    Start-Sleep -Seconds $step
    $q = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
    if (-not $q) { "gone at $t s, code $($p.ExitCode)"; break }
    "{0,4}s mem {1,5} MB cpu {2,5}s threads {3} window '{4}'" -f $t, [int]($q.WorkingSet64 / 1MB), [int]$q.CPU, $q.Threads.Count, $q.MainWindowTitle
}
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '..\tools\window_shot.ps1') -Name attila64 -Out (Join-Path $root $Shot)
if (-not $p.HasExited) { $p.Kill() }
