# 1.3.22: "Move to top" and "Normal Attila launcher" in the mod list, on a made-up game folder in temp. Opens the window for a few seconds.
# The made-up launcher\launcher.exe is a copy of cmd.exe (waits in its own window): the test checks it started, that the
# 64-bit window waits for it, and that the game does not start after it is closed. Runs from a copy in temp (own settings).
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$t = Join-Path $env:TEMP 'attila64_launcher_test'
Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue
$game = Join-Path $t 'steamapps\common\G'
$a64 = Join-Path $t 'a64'
New-Item -ItemType Directory -Force (Join-Path $game 'data'), (Join-Path $game 'launcher'), $a64 | Out-Null
function Pack($path, $type) { [IO.File]::WriteAllBytes($path, [byte[]](0x50, 0x46, 0x48, 0x34, $type, 0, 0, 0)) }
'a', 'b', 'c' | ForEach-Object { Pack (Join-Path $game "data\mod_$_.pack") 3 }
"main.pack`t123" | Set-Content (Join-Path $game 'data\manifest.txt')
$fake = Join-Path $game 'launcher\launcher.exe'
Copy-Item (Join-Path $env:WINDIR 'System32\cmd.exe') $fake
'attila64.exe', 'SDL2.dll', 'steam_api64.dll' | ForEach-Object { Copy-Item (Join-Path $root $_) $a64 }
$env:ATTILA_GAME_DIR = $game
$env:ATTILA_PICKER_ONLY = '1'
$exe = Join-Path $a64 'attila64.exe'

Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W {
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr a, string c, string t);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, int m, IntPtr w, IntPtr l);
}
"@
# as Steam does it: our exe gets normal Attila's launcher as its argument
$p = Start-Process $exe -PassThru -ArgumentList "`"$fake`""
$w = [IntPtr]::Zero
for ($i = 0; $i -lt 200 -and $w -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 200; $w = [W]::FindWindow('Attila64Mods', [NullString]::Value) }
"window: " + ($w -ne [IntPtr]::Zero)
"move to top button: " + ([W]::FindWindowEx($w, [IntPtr]::Zero, 'Button', 'Move to top') -ne [IntPtr]::Zero)
"launcher button: " + ([W]::FindWindowEx($w, [IntPtr]::Zero, 'Button', "Normal Attila launcher`n(Mod Manager, Workshop upload)") -ne [IntPtr]::Zero)
Start-Sleep -Seconds 1
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root '..\tools\window_shot.ps1') -Name attila64 -Out (Join-Path $root 'picker_1322.png')
# press "Normal Attila launcher" (ID_LAUNCHER = 100 + 21)
[W]::PostMessage($w, 0x111, [IntPtr]121, [IntPtr]::Zero) | Out-Null
$l = $null
for ($i = 0; $i -lt 40 -and -not $l; $i++) { Start-Sleep -Milliseconds 250; $l = Get-Process -Name launcher -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $fake } }
"normal launcher started: " + [bool]$l
Start-Sleep -Seconds 2
"64-bit exe waits while it is open: " + (-not $p.HasExited)
$l | Stop-Process -Force
if (-not $p.WaitForExit(15000)) { $p.Kill(); 'TIMED OUT' }
"64-bit exe ended after it, code: $($p.ExitCode)"
"mod list saved: " + (Test-Path (Join-Path $a64 'mod_list.txt'))
"game not started (no game log): " + -not (Test-Path (Join-Path $a64 'attila64_log.txt'))
Remove-Item Env:ATTILA_GAME_DIR, Env:ATTILA_PICKER_ONLY
Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue
