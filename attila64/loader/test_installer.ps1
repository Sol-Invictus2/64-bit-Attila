# Installer tests, all inside a temporary folder. The real game folder and desktop are not touched.
# -NoGame skips starting the game, for when the player has theirs open.
param([switch]$NoGame)
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $root 'Install 64-bit Attila.exe'
$t = Join-Path $env:TEMP 'attila64_install_test'
Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$t\depot" | Out-Null
$real = (Resolve-Path (Join-Path $root '..\package\attila64\Attila')).Path

'--- 1. real search, changes nothing'
Remove-Item Env:ATTILA_INSTALL_TEST -ErrorAction SilentlyContinue
$p = Start-Process $exe -ArgumentList 'find' -PassThru -NoNewWindow -RedirectStandardOutput "$t\find.txt"
if (-not $p.WaitForExit(15000)) { $p.Kill() }
Get-Content "$t\find.txt"

$env:ATTILA_INSTALL_TEST = $t
'--- 2. waits for a download that arrives late, in two pieces'
$p = Start-Process $exe -PassThru -NoNewWindow -RedirectStandardOutput "$t\wait.txt"
Start-Sleep -Seconds 6
"still waiting while nothing is there: " + (-not $p.HasExited)
$bytes = New-Object byte[] (200MB)
[IO.File]::WriteAllBytes("$t\depot\Attila", $bytes)
Start-Sleep -Seconds 6
"still waiting on a part file: " + (-not $p.HasExited)
Copy-Item $real "$t\depot\Attila" -Force
if (-not $p.WaitForExit(90000)) { $p.Kill(); 'TIMED OUT' }
Get-Content "$t\wait.txt"

'--- 3. files match'
$map = @{ 'attila64.exe' = "$root\attila64.exe"; 'SDL2.dll' = "$root\SDL2.dll"; 'steam_api64.dll' = "$root\steam_api64.dll"; 'libstdc++.so.6' = "$root\..\lib\libstdc++.so.6"; 'libgcc_s.so.1' = "$root\..\lib\libgcc_s.so.1"; 'Attila' = $real; 'packs\attila_8player_lobby.pack' = "$root\..\..\attila_8player\out8\attila_8player_lobby.pack"; 'packs\attila_8v8_battle.pack' = "$root\..\..\attila_8player\out8\attila_8v8_battle.pack"; 'packs\1212\!!1212ad_flag_wave.pack' = "$root\..\flagpack\!!1212ad_flag_wave.pack"; 'themes\mongol.jpg' = "$root\themes\mongol.jpg"; 'themes\glass.jpg' = "$root\themes\glass.jpg"; 'themes\islamic.jpg' = "$root\themes\islamic.jpg" }
foreach ($k in $map.Keys) {
    $a = (Get-FileHash -LiteralPath "$t\game\attila64\$k" -Algorithm MD5).Hash; $b = (Get-FileHash -LiteralPath $map[$k] -Algorithm MD5).Hash
    "{0}: {1}" -f $k, $(if ($a -eq $b) { 'same' } else { 'DIFFERENT' })
}
$lnk = (New-Object -ComObject WScript.Shell).CreateShortcut("$t\icon.lnk")
"icon target: $($lnk.TargetPath)"
"icon folder: $($lnk.WorkingDirectory)"
"no dx12 folder (not shipped since 1.2 Test 6): " + (-not (Test-Path "$t\game\attila64\dx12"))

'--- 3b. update over an older install: its DirectX 12 files are removed, settings kept'
New-Item -ItemType Directory -Force "$t\game\attila64\dx12" | Out-Null
'opengl32.dll', 'libgallium_wgl.dll', 'dxil.dll' | ForEach-Object { Set-Content "$t\game\attila64\dx12\$_" 'old' }
Set-Content "$t\game\attila64\settings.txt" 'interface_size=125'
$p = Start-Process $exe -PassThru -NoNewWindow -RedirectStandardOutput "$t\update.txt"
if (-not $p.WaitForExit(60000)) { $p.Kill(); 'TIMED OUT' }
Get-Content "$t\update.txt" | Select-Object -Last 2
"dx12 folder gone after update: " + (-not (Test-Path "$t\game\attila64\dx12"))
"settings kept: " + ((Get-Content "$t\game\attila64\settings.txt" -Raw) -match 'interface_size=125')
"loader updated: " + ((Get-FileHash "$t\game\attila64\attila64.exe" -Algorithm MD5).Hash -eq (Get-FileHash "$root\attila64.exe" -Algorithm MD5).Hash)

'--- 4. game starts from this install'
Remove-Item Env:ATTILA_INSTALL_TEST
$env:ATTILA_GAME_DIR = 'C:\Program Files (x86)\Steam\steamapps\common\Total War Attila'
$env:ATTILA_NO_PICKER = '1'
if ($NoGame) { 'skipped' } else {
    $g = Start-Process "$t\game\attila64\attila64.exe" -PassThru -WorkingDirectory "$t\game\attila64" -RedirectStandardError "$t\err.txt" -RedirectStandardOutput "$t\out.txt"
    if (-not $g.WaitForExit(35000)) { $g.Kill(); Start-Sleep -Seconds 2; 'ran 35 s, stopped by the test' } else { "ended by itself, code $($g.ExitCode)" }
    "log lines: " + @(Get-Content "$t\game\attila64\attila64_log.txt").Count
    Get-Content "$t\game\attila64\attila64_log.txt" | Where-Object { $_ -match 'CRASH|FATAL|music track|online thread|Steam: |^fps' } | Select-Object -First 10
    Get-Content "$t\err.txt" -TotalCount 5
    "last lines:"; Get-Content "$t\game\attila64\attila64_log.txt" -Tail 3 | ForEach-Object { if ($_.Length -gt 140) { $_.Substring(0, 140) } else { $_ } }
}
Remove-Item Env:ATTILA_GAME_DIR

'--- 5. uninstall leaves nothing of ours, keeps other files'
$env:ATTILA_INSTALL_TEST = $t
Set-Content "$t\game\keep.txt" 'x'
Set-Content "$t\game\attila64\mod_list.txt" 'mod "x.pack";'   # 64-bit Attila's own mod list (since 1.2 Test 7): goes with it
'perffix64.log', 'flagwave.log', 'flagwave_off.txt', 'plugins\perffix64.dll.old' | ForEach-Object { Set-Content "$t\game\attila64\$_" 'x' }   # left by perffix64.dll: go with it
$p = Start-Process $exe -ArgumentList 'uninstall' -PassThru -NoNewWindow -RedirectStandardOutput "$t\un.txt"
if (-not $p.WaitForExit(30000)) { $p.Kill(); 'TIMED OUT' }
Get-Content "$t\un.txt" | Select-Object -Last 2
"attila64 folder gone: " + (-not (Test-Path "$t\game\attila64"))
"icon gone: " + (-not (Test-Path "$t\icon.lnk"))
"other files kept: " + (Test-Path "$t\game\keep.txt")
"download kept: " + (Test-Path "$t\depot\Attila")
Remove-Item Env:ATTILA_INSTALL_TEST
Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue
