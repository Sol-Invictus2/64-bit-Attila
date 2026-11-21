# Run the package with no paths given, from outside the game folder (ATTILA_GAME_DIR points at the game), then ask the window to close.
param([int]$Wait = 150)
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$pkg = (Resolve-Path (Join-Path $root '..\package\attila64')).Path
$env:ATTILA_GAME_DIR = 'C:\Program Files (x86)\Steam\steamapps\common\Total War Attila'
$p = Start-Process -FilePath (Join-Path $pkg 'attila64.exe') -ArgumentList '"C:\Program Files (x86)\Steam\steamapps\common\Total War Attila\Attila.exe"' -PassThru -RedirectStandardError (Join-Path $root 'pkg_err.txt') -RedirectStandardOutput (Join-Path $root 'pkg_out.txt')
Start-Sleep -Seconds $Wait
$p.Refresh()
if ($p.HasExited) { "ended early, code $($p.ExitCode)" } else {
    "running: mem $([int]($p.WorkingSet64 / 1MB)) MB, window '$($p.MainWindowTitle)'"
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '..\tools\window_shot.ps1') -Name attila64 -Out (Join-Path $root 'pkg.png')
    [void]$p.CloseMainWindow()
    if ($p.WaitForExit(30000)) { "closed by itself, code $($p.ExitCode)" } else { 'did not close in 30 s; stopping it'; $p.Kill() }
}
$log = Get-Content (Join-Path $pkg 'attila64_log.txt')
$log | Where-Object { $_ -match 'cloud write|CRASH|FATAL|main returned|exit\(|rename failed|^fps' } | Select-Object -Last 8 | ForEach-Object { if ($_.Length -gt 110) { $_.Substring(0, 110) } else { $_ } }
"1212 packs seen in the log: " + ($log | Where-Object { $_ -match '1212' }).Count
$pf = "$env:USERPROFILE\attila64_home\.Creative Assembly\Attila\scripts\preferences.script.txt"
"settings file: " + (Get-Item $pf -ErrorAction SilentlyContinue).LastWriteTime
Get-Date -Format HH:mm:ss
