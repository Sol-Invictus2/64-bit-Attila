# Self-update tests (self_update.inc + installer "self-update"), all in a made-up Steam folder in Temp:
#   <t>\steamapps\common\Total War Attila\attila64   = an install of THIS build (1.3.20)
#   <t>\steamapps\workshop\content\325610\3834958414  = the pretend Workshop item
# -New: folder with a newer build (default ..\test_selfupd_1321, same source with version 1.3.21).
# -Game: also start the updated game for 35 s (needs the real game installed; refuses while any Attila runs).
param([string]$New = (Join-Path $PSScriptRoot '..\test_selfupd_1321'), [switch]$Game)
$ErrorActionPreference = 'Continue'
$root = $PSScriptRoot
$old_inst = Join-Path $root 'Install 64-bit Attila.exe'
$new_inst = Join-Path $New 'Install 64-bit Attila.exe'
$t = Join-Path $env:TEMP 'attila64_selfupd'
$a64 = "$t\steamapps\common\Total War Attila\attila64"
$ws = "$t\steamapps\workshop\content\325610\3834958414"
$wsi = "$ws\Install 64-bit Attila.exe"
$env:ATTILA_TEST_SELF_UPDATE_ONLY = '1'
Remove-Item Env:ATTILA_NO_SELF_UPDATE, Env:ATTILA_TEST_UPDATE_FROM, Env:ATTILA_INSTALL_TEST -ErrorAction SilentlyContinue
$pass = 0; $fail = 0
function Check($what, $ok) { if ($ok) { $script:pass++; "  ok    $what" } else { $script:fail++; "  FAIL  $what" } }
function Ver($f) { (Get-Item -LiteralPath $f).VersionInfo.FileVersion }
function Md5($f) { (Get-FileHash -LiteralPath $f -Algorithm MD5).Hash }
function Fresh {   # a clean 1.3.20 install with player files
    Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $a64, $ws | Out-Null
    Copy-Item (Join-Path $root 'attila64.exe') $a64
    $p = Start-Process $old_inst -ArgumentList 'self-update', "`"$a64`"" -PassThru -NoNewWindow -RedirectStandardOutput "$t\fresh.txt"
    if (-not $p.WaitForExit(60000)) { $p.Kill() }
    Set-Content "$a64\settings.txt" 'interface_size=125'
    Set-Content "$a64\mod_list.txt" 'mod "my_mod.pack";'
    New-Item -ItemType Directory -Force "$a64\plugins" | Out-Null
    Set-Content "$a64\plugins\perffix64.dll" 'his dll'
}
function Start64 {   # start attila64.exe the way Steam would; returns what it wrote
    Remove-Item "$a64\self_update_test.txt" -ErrorAction SilentlyContinue
    $p = Start-Process "$a64\attila64.exe" -ArgumentList '-steam_test_arg' -PassThru -WorkingDirectory $a64
    if (-not $p.WaitForExit(120000)) { $p.Kill(); 'TIMED OUT' }
    "  exit code $($p.ExitCode)"
    if (Test-Path "$a64\self_update_test.txt") { (Get-Content "$a64\self_update_test.txt") | ForEach-Object { "  > $_" } } else { '  (no test file written)' }
}
function Said($text) { (Test-Path "$a64\self_update_test.txt") -and ((Get-Content "$a64\self_update_test.txt" -Raw) -match [regex]::Escape($text)) }

"built: this $(Ver $old_inst), newer $(Ver $new_inst)"

'--- 1. fresh 1.3.20 install made with "self-update" on an empty folder'
Fresh
Get-Content "$t\fresh.txt" | Select-Object -Last 1
Check 'all files there' ((Test-Path "$a64\SDL2.dll") -and (Test-Path "$a64\packs\attila_8v8_battle.pack") -and (Test-Path "$a64\themes\mongol.jpg"))
Check 'exe is 1.3.20' ((Ver "$a64\attila64.exe") -eq '1.3.20')

'--- 2. not subscribed (no Workshop file)'
Start64
Check 'says not subscribed, stays 1.3.20' ((Said 'no Workshop installer') -and (Said '1.3.20'))

'--- 3. Workshop has the same version'
Copy-Item $old_inst $wsi
Start64
Check 'up to date' (Said 'up to date with the Workshop item (1.3.20)')

'--- 4. Workshop still downloading a newer one (file cut short)'
$bytes = [IO.File]::ReadAllBytes($new_inst)
[IO.File]::WriteAllBytes($wsi, $bytes[0..([int]($bytes.Length * 0.6))])
Start64
Check 'waits for the download' ((Said 'still downloading') -and (Ver "$a64\attila64.exe") -eq '1.3.20')

'--- 5. update_off.txt with a newer one there'
Copy-Item $new_inst $wsi -Force
Set-Content "$a64\update_off.txt" 'x'
Start64
Check 'stays on 1.3.20' ((Said 'update_off.txt found') -and (Ver "$a64\attila64.exe") -eq '1.3.20')
Remove-Item "$a64\update_off.txt"

'--- 6. ATTILA_NO_SELF_UPDATE'
$env:ATTILA_NO_SELF_UPDATE = '1'
Start64
Check 'stays on 1.3.20' ((Said 'off (ATTILA_NO_SELF_UPDATE)') -and (Ver "$a64\attila64.exe") -eq '1.3.20')
Remove-Item Env:ATTILA_NO_SELF_UPDATE

'--- 7. Workshop has a NEWER version: updates and starts the new one'
Start64
Check 'new exe ran and logged it' ((Said '1.3.21') -and (Said 'updated from 1.3.20'))
$map = @{ 'attila64.exe' = "$New\attila64.exe"; 'SDL2.dll' = "$New\SDL2.dll"; 'packs\attila_8v8_battle.pack' = "$New\..\..\attila_8player\out8\attila_8v8_battle.pack"; 'themes\mongol.jpg' = "$New\themes\mongol.jpg" }
foreach ($k in $map.Keys) { Check "$k = the new build's" ((Md5 "$a64\$k") -eq (Md5 $map[$k])) }
Check 'settings.txt kept' ((Get-Content "$a64\settings.txt" -Raw) -match 'interface_size=125')
Check 'mod_list.txt kept' ((Get-Content "$a64\mod_list.txt" -Raw) -match 'my_mod')
Check 'his perffix64.dll kept' ((Get-Content "$a64\plugins\perffix64.dll" -Raw) -match 'his dll')
Check 'no .new files left' (-not (Get-ChildItem $a64 -Recurse -Filter *.new))
Check 'game file / icon untouched (no Attila file made)' (-not (Test-Path "$a64\Attila"))
"  self_update_log.txt:"; Get-Content "$a64\self_update_log.txt" | ForEach-Object { "    $_" }

'--- 8. next start: up to date, attila64.exe.old cleaned up'
Start64
Check 'up to date at 1.3.21' (Said 'up to date with the Workshop item (1.3.21)')
Check 'attila64.exe.old gone' (-not (Test-Path "$a64\attila64.exe.old"))

'--- 9. Workshop OLDER than the install (dev / hand-installed newer build)'
Copy-Item $old_inst $wsi -Force
Start64
Check 'newer one kept' ((Said 'is newer than the Workshop item') -and (Ver "$a64\attila64.exe") -eq '1.3.21')

'--- 10. update fails part-way (SDL2.dll locked by another program): old exe stays, tries again next start'
Fresh
Copy-Item $new_inst $wsi -Force
$lock = [IO.File]::Open("$a64\SDL2.dll", 'Open', 'Read', 'None')
Start64
$lock.Close()
Check 'says FAILED, old version starts' ((Said 'FAILED') -and (Said '1.3.20'))
Check 'exe still 1.3.20' ((Ver "$a64\attila64.exe") -eq '1.3.20')
Check 'no .new files left' (-not (Get-ChildItem $a64 -Recurse -Filter *.new))
Start64
Check 'next start updates' ((Said 'updated from 1.3.20') -and (Ver "$a64\attila64.exe") -eq '1.3.21' -and (Md5 "$a64\SDL2.dll") -eq (Md5 "$New\SDL2.dll"))

'--- 11. uninstall removes the update leftovers too'
Set-Content "$a64\attila64.exe.old" 'x'; Set-Content "$a64\update_off.txt" 'x'
$env:ATTILA_INSTALL_TEST = "$t\un"
New-Item -ItemType Directory -Force "$t\un\game" | Out-Null
Copy-Item $a64 "$t\un\game\attila64" -Recurse
Remove-Item "$t\un\game\attila64\self_update_test.txt", "$t\un\game\attila64\plugins\perffix64.dll" -ErrorAction SilentlyContinue
$p = Start-Process $new_inst -ArgumentList 'uninstall' -PassThru -NoNewWindow -RedirectStandardOutput "$t\un.txt"
if (-not $p.WaitForExit(30000)) { $p.Kill() }
Remove-Item Env:ATTILA_INSTALL_TEST
Check 'attila64 folder gone' (-not (Test-Path "$t\un\game\attila64"))
if (Test-Path "$t\un\game\attila64") { Get-ChildItem "$t\un\game\attila64" -Recurse | ForEach-Object { "    left: $($_.FullName)" } }

Remove-Item Env:ATTILA_TEST_SELF_UPDATE_ONLY
if ($Game) {
    '--- 12. the updated game really starts (35 s, no start window)'
    if (Get-Process attila, attila64 -ErrorAction SilentlyContinue) { '  SKIPPED: an Attila is running' } else {
        Fresh
        Copy-Item $new_inst $wsi -Force
        # Fresh's stand-ins would break a real start: a text file as perffix64.dll (Bad Image box), a mod that does not exist (VFS error)
        Remove-Item "$a64\plugins\perffix64.dll", "$a64\mod_list.txt"
        $env:ATTILA_TEST_HIDDEN = '1'   # the game window stays off the player's screen
        $real =(Resolve-Path (Join-Path $root '..\package\attila64\Attila')).Path
        cmd /c mklink /H "$a64\Attila" "$real" | Out-Null
        $env:ATTILA_GAME_DIR = 'C:\Program Files (x86)\Steam\steamapps\common\Total War Attila'
        $env:ATTILA_NO_PICKER = '1'
        $g = Start-Process "$a64\attila64.exe" -PassThru -WorkingDirectory $a64
        if (-not $g.WaitForExit(35000)) { Get-Process attila64 -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$a64*" } | Stop-Process -Force; Start-Sleep 2; '  ran 35 s, stopped by the test' } else { "  ended by itself, code $($g.ExitCode)" }
        Remove-Item Env:ATTILA_GAME_DIR, Env:ATTILA_NO_PICKER, Env:ATTILA_TEST_HIDDEN
        Copy-Item "$a64\attila64_log.txt" (Join-Path $root 'test_self_update_game_log.txt') -Force   # kept for a look afterwards
        $log = Get-Content "$a64\attila64_log.txt"
        $log | Select-Object -First 3 | ForEach-Object { "  log: $_" }
        Check 'log: 1.3.21, updated from 1.3.20' (($log -match '64-bit Attila 1\.3\.21') -and ($log -match 'updated from 1\.3\.20'))
        Check 'no crash line' (-not ($log -match '^CRASH|FATAL'))
        "  lines: $($log.Count)"
    }
}
"=== $pass passed, $fail failed"
Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue
