# Start the game, wait, send Escape a few times to pass the movies, then report and capture the game window; leaves it running if asked.
param([int]$Wait = 35, [int]$After = 80, [string]$Shot = 'win2.png', [switch]$Keep)
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$tools = Join-Path $root '..\tools'
$game = 'C:\Program Files (x86)\Steam\steamapps\content\app_325610\depot_325624\Attila'
$lib = (Resolve-Path (Join-Path $root '..\lib')).Path
$work = 'C:\Program Files (x86)\Steam\steamapps\common\Total War Attila'
$p = Start-Process -FilePath (Join-Path $root 'attila64.exe') -ArgumentList "`"$game`"", "`"$lib`"" -WorkingDirectory $work -PassThru -RedirectStandardError (Join-Path $root 'err.txt') -RedirectStandardOutput (Join-Path $root 'out.txt')
Start-Sleep -Seconds $Wait
foreach ($i in 1..3) { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tools 'post_key.ps1') attila64 key 27 | Out-Null; Start-Sleep -Seconds 6 }
for ($t = 0; $t -lt $After; $t += 15) {
    Start-Sleep -Seconds 15
    $q = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
    if (-not $q) { "gone, code $($p.ExitCode)"; break }
    "{0,4}s mem {1,5} MB cpu {2,5}s" -f ($Wait + 18 + $t + 15), [int]($q.WorkingSet64 / 1MB), [int]$q.CPU
}
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tools 'window_shot.ps1') -Name attila64 -Out (Join-Path $root $Shot)
if (-not $Keep -and -not $p.HasExited) { $p.Kill() }
Get-Content (Join-Path $root 'err.txt') | Where-Object { $_ -match 'CRASH|FATAL|^rax|in attila64' } | ForEach-Object { if ($_.Length -gt 150) { $_.Substring(0, 150) } else { $_ } }
