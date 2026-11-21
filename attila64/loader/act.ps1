# Act on the running game: optional clicks (x,y in the 1280-wide picture, repeated), wait, then capture its window and report state.
param([string]$Clicks = '', [int]$Wait = 5, [string]$Shot = 'act.png')
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$tools = Join-Path $root '..\tools'
if ($Clicks) {
    foreach ($pair in $Clicks -split ';') {
        $xy = $pair -split ','
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tools 'post_key.ps1') attila64 click ([int]$xy[0] * 2) ([int]$xy[1] * 2) | Out-Null
        Start-Sleep -Seconds 2
    }
}
Start-Sleep -Seconds $Wait
$q = Get-Process attila64 -ErrorAction SilentlyContinue
if ($q) { "alive mem {0} MB cpu {1}s" -f [int]($q.WorkingSet64 / 1MB), [int]$q.CPU } else { 'GAME IS GONE' }
if ($q) { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tools 'window_shot.ps1') -Name attila64 -Out (Join-Path $root $Shot) }
Get-Content (Join-Path $root 'err.txt') | Where-Object { $_ -match 'CRASH|FATAL|^rax|in attila64|refused: t|bad block' } | Select-Object -Last 5 | ForEach-Object { if ($_.Length -gt 150) { $_.Substring(0, 150) } else { $_ } }
