# Own mod list test (mod_list.txt next to the exe, since 2026-11-14): on a made-up game folder in temp, with a copy of the exe in
# <game>\attila64 like an installed game. Hidden (the start window clicks by itself). The real game folder is not touched.
#  1. first start: mod_list.txt is made from the game folder's used_mods.txt, the start window's change is saved there,
#     used_mods.txt (normal Attila's list) stays exactly as it was
#  2. normal Attila's launcher rewrites used_mods.txt in another order: 64-bit Attila keeps its own order
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$t = Join-Path $env:TEMP 'attila64_modlist_test'
Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue
$game = "$t\steamapps\common\G"
$ws = "$t\steamapps\workshop\content\325610"
New-Item -ItemType Directory -Force "$game\data", "$game\attila64", "$ws\111", "$ws\222" | Out-Null
function Pack($path, $type) { [IO.File]::WriteAllBytes($path, [byte[]](0x50, 0x46, 0x48, 0x34, $type, 0, 0, 0)) }
Pack "$game\data\main.pack" 1
Pack "$game\data\mydata.pack" 3
Pack "$ws\111\one.pack" 3
Pack "$ws\222\two.pack" 3
"main.pack`t123" | Set-Content "$game\data\manifest.txt"
foreach ($f in 'attila64.exe', 'SDL2.dll', 'steam_api64.dll') { Copy-Item (Join-Path $root $f) "$game\attila64" }
$w111 = ("$ws\111" -replace '\\', '/'); $w222 = ("$ws\222" -replace '\\', '/')
@("add_working_directory `"$w111`";", "add_working_directory `"$w222`";", 'mod "one.pack";', 'mod "two.pack";', 'mod "mydata.pack";') | Set-Content "$game\used_mods.txt"
$normalBefore = (Get-FileHash "$game\used_mods.txt").Hash
$env:ATTILA_GAME_DIR = $game; $env:ATTILA_PICKER_ONLY = '1'; $env:ATTILA_PICKER_AUTO = '1'; $env:ATTILA_TEST_HIDDEN = '1'
function Run { $p = Start-Process "$game\attila64\attila64.exe" -PassThru -WindowStyle Hidden; if (-not $p.WaitForExit(20000)) { $p.Kill(); 'TIMED OUT' } }
'--- 1. first start (start window: untick the first, move the last to the top, Play)'
Run
"own list made: " + (Test-Path "$game\attila64\mod_list.txt")
Get-Content "$game\attila64\mod_list.txt" | ForEach-Object { "   $_" }
"normal Attila's used_mods.txt unchanged: " + ($normalBefore -eq (Get-FileHash "$game\used_mods.txt").Hash)
'--- 2. the normal launcher writes its own order into used_mods.txt; 64-bit Attila starts again'
$own = Get-Content "$game\attila64\mod_list.txt" -Raw
@("add_working_directory `"$w111`";", "add_working_directory `"$w222`";", 'mod "mydata.pack";', 'mod "one.pack";', 'mod "two.pack";') | Set-Content "$game\used_mods.txt"
Remove-Item Env:ATTILA_PICKER_AUTO   # this time the window is just closed with Play by the test switch below
$env:ATTILA_PICKER_AUTO = '2'
Run
$after = Get-Content "$game\attila64\mod_list.txt" -Raw
"own list kept its order (not the launcher's): " + (($after -replace '\s', '') -like "*$((($own -split "`n" | Where-Object { $_ -match '^mod ' } | Select-Object -First 1) -replace '\s', ''))*")
Get-Content "$game\attila64\mod_list.txt" | ForEach-Object { "   $_" }
foreach ($k in 'ATTILA_GAME_DIR', 'ATTILA_PICKER_ONLY', 'ATTILA_PICKER_AUTO', 'ATTILA_TEST_HIDDEN') { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue
