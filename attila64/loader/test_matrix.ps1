# Runs several replay tests one after another and collects the headline numbers.
param([string[]]$Runs = @('new|', 'ubm2|ATTILA_UBM=2', 'ubm2vab|ATTILA_UBM=2;ATTILA_VAB=1'), [int]$Row = 302, [int]$BattleSeconds = 150, [switch]$Measure)
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
foreach ($r in $Runs) {
    $x = $r -split '\|'
    $a = @{ Label = $x[0]; Switches = $x[1]; Row = $Row; BattleSeconds = $BattleSeconds }
    if ($Measure) { $a.Measure = $true }
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'test_replay.ps1') -Label $x[0] -Switches "$($x[1])" -Row $Row -BattleSeconds $BattleSeconds $(if ($Measure) { '-Measure' }) | ForEach-Object { "$_" } | Where-Object { $_ -match '^===|^settings|^LOAD|^fps windows|^AVERAGE|^MEMORY|^peak|CRASH|FATAL|WRONG|ended|NOT LOADED|ALREADY' }
    Start-Sleep -Seconds 4
}
