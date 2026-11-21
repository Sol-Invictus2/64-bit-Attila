# How long does a first start from a new folder take, and is the second start quicker? Reports when the menu's fps line reaches 30+.
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$pkg = (Resolve-Path (Join-Path $root '..\package\attila64')).Path
$base = Join-Path $env:TEMP 'attila64_fresh'
$t = Join-Path $base 'game\attila64'
Remove-Item $base -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $t | Out-Null
Copy-Item "$root\attila64.exe", "$pkg\SDL2.dll", "$pkg\steam_api64.dll", "$pkg\libstdc++.so.6", "$pkg\libgcc_s.so.1", "$pkg\Attila" $t
$env:ATTILA_GAME_DIR = 'C:\Program Files (x86)\Steam\steamapps\common\Total War Attila'
$env:ATTILA_NO_PICKER = '1'
foreach ($run in 'first start', 'second start') {
    "=== $run"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $g = Start-Process "$t\attila64.exe" -PassThru -RedirectStandardError "$env:TEMP\f_err.txt" -RedirectStandardOutput "$env:TEMP\f_out.txt"
    $ready = $false
    while ($sw.Elapsed.TotalSeconds -lt 300 -and -not $g.HasExited) {
        Start-Sleep -Seconds 5
        $lines = @(Get-Content "$t\attila64_log.txt" -ErrorAction SilentlyContinue)
        $fps = $lines | Where-Object { $_ -match '^fps ([\d.]+)' } | Select-Object -Last 1
        if ($fps -and [double]($fps -split ' ')[1] -ge 30) { $ready = $true; break }
    }
    "reached normal speed: $ready after $([int]$sw.Elapsed.TotalSeconds) s; log lines $($lines.Count); last fps: $fps"
    $lines | Where-Object { $_ -match 'CRASH' } | Select-Object -First 2
    if (-not $g.HasExited) { $g.Kill(); Start-Sleep -Seconds 3 } else { "ended by itself, code $($g.ExitCode)" }
}
Remove-Item $base -Recurse -Force -ErrorAction SilentlyContinue
