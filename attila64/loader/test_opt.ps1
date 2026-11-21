# Test the optimised build from the package folder: optional debug run to count bad blocks, then close the window and read results.
param([int]$Wait = 110, [switch]$Debug)
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$pkg = (Resolve-Path (Join-Path $root '..\package\attila64')).Path
$env:ATTILA_GAME_DIR = 'C:\Program Files (x86)\Steam\steamapps\common\Total War Attila'
if ($Debug) { $env:ATTILA_DEBUG = '1' } else { Remove-Item Env:ATTILA_DEBUG -ErrorAction SilentlyContinue }
$p = Start-Process -FilePath (Join-Path $pkg 'attila64.exe') -PassThru -RedirectStandardError (Join-Path $root 'pkg_err.txt') -RedirectStandardOutput (Join-Path $root 'pkg_out.txt')
Start-Sleep -Seconds $Wait
$p.Refresh()
if ($p.HasExited) { "ended early, code $($p.ExitCode)" } else {
    "running: mem $([int]($p.WorkingSet64 / 1MB)) MB, peak $([int]($p.PeakWorkingSet64 / 1MB)) MB"
    [void]$p.CloseMainWindow()
    if (-not $p.WaitForExit(30000)) { 'did not close in 30 s; stopping it'; $p.Kill() }
}
$log = Get-Content (Join-Path $pkg 'attila64_log.txt')
"log lines: $($log.Count)"
"bad blocks: " + ($log | Where-Object { $_ -match 'bad block' }).Count
"sound blocks logged: " + ($log | Where-Object { $_ -match 'sound block' }).Count
$log | Where-Object { $_ -match 'patch skipped|CRASH|FATAL|^rax|in attila64|main returned|settings saved|settings not|^fps' } | Select-Object -Last 8 | ForEach-Object { if ($_.Length -gt 110) { $_.Substring(0, 110) } else { $_ } }
