# Start window test (2026-11-22): mod pictures and dragging a mod. A made-up game folder in temp with three Workshop mods
# (two with a picture next to the pack, one without) and one data-folder mod with a picture. The window is shown, a screenshot
# is taken (picker_before.png), the mouse drags row 3 to row 1 (the real mouse moves for a moment), a second screenshot
# (picker_after.png), then Play is pressed by a message and mod_list.txt is printed. The real game folder is not touched.
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class W {
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
'@
[W]::SetProcessDPIAware() | Out-Null
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$t = Join-Path $env:TEMP 'attila64_picker_test'
Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue
$game = "$t\steamapps\common\G"
$ws = "$t\steamapps\workshop\content\325610"
New-Item -ItemType Directory -Force "$game\data", "$game\attila64", "$ws\111", "$ws\222", "$ws\333" | Out-Null
function Pack($path, $type) { [IO.File]::WriteAllBytes($path, [byte[]](0x50, 0x46, 0x48, 0x34, $type, 0, 0, 0)) }
function Pic($path, $color, $text, $w, $h) {
    $b = New-Object Drawing.Bitmap $w, $h; $g = [Drawing.Graphics]::FromImage($b)
    $g.Clear([Drawing.Color]::FromName($color))
    $g.DrawString($text, (New-Object Drawing.Font 'Arial', 28), [Drawing.Brushes]::White, 10, 10)
    $g.Dispose(); $b.Save($path, [Drawing.Imaging.ImageFormat]::Png); $b.Dispose()
}
Pack "$game\data\main.pack" 1
Pack "$game\data\mydata.pack" 3; Pic "$game\data\mydata.png" 'DarkGreen' 'mydata (data)' 256 256
Pack "$ws\111\one.pack" 3; Pic "$ws\111\one.png" 'DarkRed' 'one' 256 256
Pack "$ws\222\two.pack" 3; Pic "$ws\222\preview.png" 'Navy' 'two (other name, wide)' 512 200
Pack "$ws\333\three.pack" 3
"main.pack`t123" | Set-Content "$game\data\manifest.txt"
foreach ($f in 'attila64.exe', 'SDL2.dll', 'steam_api64.dll') { Copy-Item (Join-Path $root $f) "$game\attila64" }
@('mod "one.pack";', 'mod "two.pack";', 'mod "three.pack";', 'mod "mydata.pack";') | Set-Content "$game\attila64\mod_list.txt"
"   (three.pack is in Workshop 333, picked up by the Workshop scan)"
$env:ATTILA_GAME_DIR = $game; $env:ATTILA_PICKER_ONLY = '1'
$p = Start-Process "$game\attila64\attila64.exe" -PassThru
$h = [IntPtr]::Zero
for ($i = 0; $i -lt 50 -and $h -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 200; $h = [W]::FindWindow('Attila64Mods', [NullString]::Value) }
if ($h -eq [IntPtr]::Zero) { 'NO WINDOW'; $p.Kill(); exit 1 }
Start-Sleep -Milliseconds 600
function Shot($name) {
    $r = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$r) | Out-Null
    $b = New-Object Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T); $g = [Drawing.Graphics]::FromImage($b)
    $dc = $g.GetHdc(); [W]::PrintWindow($h, $dc, 2) | Out-Null; $g.ReleaseHdc($dc); $g.Dispose()
    $b.Save((Join-Path $t $name), [Drawing.Imaging.ImageFormat]::Png); $b.Dispose(); "screenshot: " + (Join-Path $t $name)
}
Shot 'picker_before.png'
# drag: rows are about 17-19 px; the list starts at client y 48 + header. Grab row 3 (three.pack) and drop on row 1.
$r = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$r) | Out-Null
[W]::SetForegroundWindow($h) | Out-Null
$cx = $r.L + 150                                  # inside the Mod column, not on the tick box
function RowY($k) { $r.T + 107 + 17 * ($k - 1) }  # row k's middle (window coordinates, 100% screen scale)
function Click($k) {
    [W]::SetCursorPos($cx, (RowY $k)); Start-Sleep -Milliseconds 100
    [W]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); [W]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 300
}
$y3 = RowY 3; $y1 = (RowY 1) - 4
[W]::SetCursorPos($cx, $y3); Start-Sleep -Milliseconds 150
[W]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 100
for ($y = $y3; $y -ge $y1; $y -= 3) { [W]::SetCursorPos($cx, $y); Start-Sleep -Milliseconds 25 }
Start-Sleep -Milliseconds 150
[W]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 400
Shot 'picker_after.png'      # three.pack now on top: "This mod has no picture"
Click 3; Shot 'picker_two.png'   # two.pack: its folder's preview.png (other name), wide picture fitted
[W]::PostMessage($h, 0x111, [IntPtr]105, [IntPtr]::Zero) | Out-Null   # Play
if (-not $p.WaitForExit(15000)) { $p.Kill(); 'TIMED OUT' }
'--- mod_list.txt after the drag + Play'
Get-Content "$game\attila64\mod_list.txt" | ForEach-Object { "   $_" }
foreach ($k in 'ATTILA_GAME_DIR', 'ATTILA_PICKER_ONLY') { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
