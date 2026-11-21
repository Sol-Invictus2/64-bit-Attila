# Start the game several times and see how often the sound bank crash happens, with sound bank loads logged. Each run is stopped afterwards.
param([int]$Runs = 3, [int]$Wait = 75)
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$pkg = (Resolve-Path (Join-Path $root '..\package\attila64')).Path
$env:ATTILA_GAME_DIR = 'C:\Program Files (x86)\Steam\steamapps\common\Total War Attila'
$env:ATTILA_NO_PICKER = '1'
Remove-Item Env:ATTILA_TRACE -ErrorAction SilentlyContinue
Remove-Item Env:ATTILA_DEBUG -ErrorAction SilentlyContinue
for ($i = 1; $i -le $Runs; $i++) {
    $p = Start-Process -FilePath (Join-Path $pkg 'attila64.exe') -PassThru -RedirectStandardError (Join-Path $root 'pkg_err.txt') -RedirectStandardOutput (Join-Path $root 'pkg_out.txt')
    $t0 = Get-Date
    $ended = $p.WaitForExit($Wait * 1000)
    $secs = [int]((Get-Date) - $t0).TotalSeconds
    if (-not $ended) { $p.Kill(); Start-Sleep -Seconds 2 }
    $log = Get-Content (Join-Path $pkg 'attila64_log.txt')
    $crash = $log | Where-Object { $_ -match '^CRASH' } | Select-Object -First 1
    $banks = $log | Where-Object { $_ -match '^\[\d+\] LoadBank' }
    "run $i : {0} after {1} s, {2} bank load lines" -f $(if ($crash) { 'CRASHED' } else { 'ok' }), $secs, $banks.Count
    Copy-Item (Join-Path $pkg 'attila64_log.txt') (Join-Path $root "rate_$i.txt") -Force
    if ($crash) {
        $crash.Substring(0, [Math]::Min(100, $crash.Length))
        $banks | Select-Object -Last 3 | ForEach-Object { '  ' + $_.Substring(0, [Math]::Min(100, $_.Length)) }
    }
}
