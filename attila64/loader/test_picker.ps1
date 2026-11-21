# Mod picker test on a made-up game folder in temp; the real used_mods.txt is not touched. Run only when the player's game is closed, it opens a window.
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$t = Join-Path $env:TEMP 'attila64_picker_test'
Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue
$game = "$t\steamapps\common\G"
$ws = "$t\steamapps\workshop\content\325610"
New-Item -ItemType Directory -Force "$game\data", "$ws\111", "$ws\222" | Out-Null
function Pack($path, $type) { [IO.File]::WriteAllBytes($path, [byte[]](0x50, 0x46, 0x48, 0x34, $type, 0, 0, 0)) }
Pack "$game\data\main.pack" 1          # the game's own, in the manifest
Pack "$game\data\blood.pack" 1         # DLC, not in the manifest, not a mod type
Pack "$game\data\mydata.pack" 3        # a mod in data
Pack "$ws\111\one.pack" 3
Pack "$ws\222\two.pack" 3
"main.pack`t123" | Set-Content "$game\data\manifest.txt"
$w111 = ("$ws\111" -replace '\\', '/')
@("add_working_directory `"$w111`";", 'mod "one.pack";', 'mod "mydata.pack";', 'mod "gone.pack";', 'some_other_line 1;') | Set-Content "$game\used_mods.txt"
$before = Get-Content "$game\used_mods.txt" -Raw
$env:ATTILA_GAME_DIR = $game
$env:ATTILA_PICKER_ONLY = '1'
$exe = Join-Path $root 'attila64.exe'

'--- auto: untick first, move last to top, Play'
$env:ATTILA_PICKER_AUTO = '1'
$p = Start-Process $exe -PassThru
if (-not $p.WaitForExit(15000)) { $p.Kill(); 'TIMED OUT' }
Get-Content "$game\used_mods.txt"
"copy of the old list kept: " + ((Get-Content "$game\used_mods_before_picker.txt" -Raw) -eq $before)
Remove-Item Env:ATTILA_PICKER_AUTO

'--- picture of the real list; the window is closed without pressing Play, so nothing is saved'
$real = 'C:\Program Files (x86)\Steam\steamapps\common\Total War Attila'
$h1 = (Get-FileHash "$real\used_mods.txt").Hash
$env:ATTILA_GAME_DIR = $real
$p = Start-Process $exe -PassThru
Start-Sleep -Seconds 3
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '..\tools\window_shot.ps1') -Name attila64 -Out (Join-Path $root 'picker.png')
$p.CloseMainWindow() | Out-Null
if (-not $p.WaitForExit(8000)) { $p.Kill() }
"real list unchanged: " + ($h1 -eq (Get-FileHash "$real\used_mods.txt").Hash)
Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue
