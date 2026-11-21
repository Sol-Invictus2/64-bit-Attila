# Run 1212AD with every read step of music item 89 logged, to see where the list ends now.
param([int]$Runs = 2, [int]$Wait = 30)
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$pkg = (Resolve-Path (Join-Path $root '..\package\attila64')).Path
$env:ATTILA_GAME_DIR = 'C:\Program Files (x86)\Steam\steamapps\common\Total War Attila'
$env:ATTILA_TRACE = '3634dd0=MUSICITEM,3634f7a=M_CLIPS,3635018=M_REC,36354cc=M_ADD,3635252=M_BASE'
Remove-Item Env:ATTILA_DEBUG -ErrorAction SilentlyContinue
for ($i = 1; $i -le $Runs; $i++) {
    $p = Start-Process -FilePath (Join-Path $pkg 'attila64.exe') -PassThru -RedirectStandardError (Join-Path $root 'pkg_err.txt') -RedirectStandardOutput (Join-Path $root 'pkg_out.txt')
    if (-not $p.WaitForExit($Wait * 1000)) { $p.Kill(); Start-Sleep -Seconds 2 }
    $log = Get-Content (Join-Path $pkg 'attila64_log.txt')
    "run $i : {0} music items, crash: {1}" -f @($log | Where-Object { $_ -match '^MUSICITEM' }).Count, (@($log | Where-Object { $_ -match '^CRASH' }).Count -gt 0)
    $log | Where-Object { $_ -match '^M_|^CRASH|music track bounds' }
}
