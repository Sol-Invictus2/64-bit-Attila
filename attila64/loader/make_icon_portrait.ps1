# The icon in use: the Yuan album portrait of Genghis Khan (14th century, public domain, National Palace Museum Taipei, from Wikimedia Commons),
# cropped round inside a gold ring. Small sizes crop closer so the face stays readable. Writes attila64.ico and icon_preview.png.
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Add-Type -AssemblyName System.Drawing
$src = [Drawing.Image]::FromFile((Resolve-Path (Join-Path $root '..\art\genghis_portrait_960.jpg')).Path)
function Draw($s) {
    $b = New-Object Drawing.Bitmap $s, $s, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [Drawing.Graphics]::FromImage($b)
    $g.SmoothingMode = 'AntiAlias'; $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
    $crop = if ($s -le 32) { New-Object Drawing.RectangleF 225, 300, 520, 520 } else { New-Object Drawing.RectangleF 150, 225, 680, 680 }
    $ring = [math]::Max(1.5, $s * 0.035)
    $inset = $ring / 2 + 0.5
    $path = New-Object Drawing.Drawing2D.GraphicsPath
    $path.AddEllipse($inset, $inset, $s - 2 * $inset, $s - 2 * $inset)
    $g.SetClip($path)
    $g.DrawImage($src, (New-Object Drawing.RectangleF 0, 0, $s, $s), $crop, [Drawing.GraphicsUnit]::Pixel)
    $g.ResetClip()
    $g.DrawEllipse((New-Object Drawing.Pen ([Drawing.Color]::FromArgb(14, 26, 48)), ($ring + [math]::Max(1, $s * 0.012))), $inset, $inset, $s - 2 * $inset, $s - 2 * $inset)
    $g.DrawEllipse((New-Object Drawing.Pen ([Drawing.Color]::FromArgb(232, 184, 74)), $ring), $inset, $inset, $s - 2 * $inset, $s - 2 * $inset)
    $g.Dispose()
    $b
}
$sizes = 256, 64, 48, 32, 24, 16
$pngs = foreach ($s in $sizes) { $m = New-Object IO.MemoryStream; $bmp = Draw $s; $bmp.Save($m, [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose(); , $m.ToArray() }
$f = New-Object IO.MemoryStream
$w = New-Object IO.BinaryWriter $f
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$off = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $n = $pngs[$i].Length
    $w.Write([byte]($(if ($s -ge 256) { 0 } else { $s }))); $w.Write([byte]($(if ($s -ge 256) { 0 } else { $s })))
    $w.Write([byte]0); $w.Write([byte]0); $w.Write([uint16]1); $w.Write([uint16]32); $w.Write([uint32]$n); $w.Write([uint32]$off)
    $off += $n
}
foreach ($p in $pngs) { $w.Write([byte[]]$p) }
[IO.File]::WriteAllBytes((Join-Path $root 'attila64.ico'), $f.ToArray())
$sheet = New-Object Drawing.Bitmap 400, 280
$g = [Drawing.Graphics]::FromImage($sheet)
$g.Clear([Drawing.Color]::FromArgb(70, 60, 50))
$g.DrawImage((Draw 256), 12, 12, 256, 256)
$y = 12
foreach ($s in 64, 48, 32, 16) { $g.DrawImage((Draw $s), 290, $y, $s, $s); $y += $s + 10 }
$sheet.Save((Join-Path $root 'icon_preview.png'))
$src.Dispose()
"icon {0} KB" -f [int]((Get-Item (Join-Path $root 'attila64.ico')).Length / 1KB)
