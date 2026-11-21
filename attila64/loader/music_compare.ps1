# Run 1212AD several times logging a fingerprint of each music item, then compare crashed runs against good ones.
param([int]$Runs = 5, [int]$Wait = 30)
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$pkg = (Resolve-Path (Join-Path $root '..\package\attila64')).Path
$env:ATTILA_GAME_DIR = 'C:\Program Files (x86)\Steam\steamapps\common\Total War Attila'
$env:ATTILA_TRACE = '3634dd0=MUSICITEM'
Remove-Item Env:ATTILA_DEBUG -ErrorAction SilentlyContinue
$sets = @()
for ($i = 1; $i -le $Runs; $i++) {
    $p = Start-Process -FilePath (Join-Path $pkg 'attila64.exe') -PassThru -RedirectStandardError (Join-Path $root 'pkg_err.txt') -RedirectStandardOutput (Join-Path $root 'pkg_out.txt')
    $ended = $p.WaitForExit($Wait * 1000)
    if (-not $ended) { $p.Kill(); Start-Sleep -Seconds 2 }
    $log = Get-Content (Join-Path $pkg 'attila64_log.txt')
    $items = @($log | Where-Object { $_ -match '^MUSICITEM' } | ForEach-Object { $x = $_ -split ' '; "$($x[3]) $($x[5])" })
    $crash = @($log | Where-Object { $_ -match '^CRASH' }).Count -gt 0
    Copy-Item (Join-Path $pkg 'attila64_log.txt') (Join-Path $root "music_$i.txt") -Force
    "run $i : {0}, {1} music items" -f $(if ($crash) { 'CRASHED' } else { 'ok' }), $items.Count
    $sets += , @{ crash = $crash; items = $items }
}
$good = $sets | Where-Object { -not $_.crash } | Select-Object -First 1
if (-not $good) { 'no good run to compare with'; exit }
foreach ($s in $sets) {
    if ($s -eq $good) { continue }
    $n = [Math]::Min($s.items.Count, $good.items.Count)
    $first = -1
    for ($k = 0; $k -lt $n; $k++) { if ($s.items[$k] -ne $good.items[$k]) { $first = $k; break } }
    $inGood = @{}; $good.items | ForEach-Object { $inGood[$_] = 1 }
    $unknown = @($s.items | Where-Object { -not $inGood[$_] })
    "crash={0}: {1} items, first difference in order at {2}, items never seen in the good run: {3}" -f $s.crash, $s.items.Count, $first, $unknown.Count
    if ($s.crash) { "  last item parsed: " + $s.items[-1] + $(if ($inGood[$s.items[-1]]) { ' (same bytes as in the good run)' } else { ' (NOT seen in the good run)' }) }
    $unknown | Select-Object -First 4 | ForEach-Object { "  different: $_" }
}
