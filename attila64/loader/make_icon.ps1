# Draws our own icon: a Mongol sulde (spear banner with a horsehair tassel) over a recurve bow, on Tengri sky blue. Writes attila64.ico and a preview.
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Add-Type -AssemblyName System.Drawing
function Draw($s) {
    $b = New-Object Drawing.Bitmap $s, $s, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [Drawing.Graphics]::FromImage($b)
    $g.SmoothingMode = 'AntiAlias'
    $g.ScaleTransform($s / 256.0, $s / 256.0)
    $gold = [Drawing.Color]::FromArgb(232, 184, 74)
    $dark = [Drawing.Color]::FromArgb(14, 26, 48)
    # disc
    $rect = New-Object Drawing.RectangleF 6, 6, 244, 244
    $path = New-Object Drawing.Drawing2D.GraphicsPath
    $path.AddEllipse($rect)
    $sky = New-Object Drawing.Drawing2D.PathGradientBrush $path
    $sky.CenterColor = [Drawing.Color]::FromArgb(44, 118, 196)
    $sky.SurroundColors = @([Drawing.Color]::FromArgb(12, 40, 92))
    $g.FillEllipse($sky, $rect)
    $g.DrawEllipse((New-Object Drawing.Pen $gold, 9), 10.5, 10.5, 235, 235)
    # recurve bow, across the disc
    $bow = New-Object Drawing.Drawing2D.GraphicsPath
    # tips upper left and lower right, the belly swings out to the lower left, the tips flick back the other way: a recurve
    $bow.AddBezier(62, 38, 40, 44, 42, 62, 50, 76)
    $bow.AddBezier(50, 76, 58, 128, 30, 150, 66, 192)
    $bow.AddBezier(66, 192, 104, 226, 132, 200, 182, 208)
    $bow.AddBezier(182, 208, 198, 216, 214, 214, 220, 194)
    $g.DrawLine((New-Object Drawing.Pen ([Drawing.Color]::FromArgb(235, 235, 225)), 3), 62, 38, 220, 194)
    $bowPen = New-Object Drawing.Pen $dark, 18
    $bowPen.StartCap = 'Round'; $bowPen.EndCap = 'Round'; $bowPen.LineJoin = 'Round'
    $g.DrawPath($bowPen, $bow)
    $goldPen = New-Object Drawing.Pen $gold, 10
    $goldPen.StartCap = 'Round'; $goldPen.EndCap = 'Round'; $goldPen.LineJoin = 'Round'
    $g.DrawPath($goldPen, $bow)
    # shaft
    $g.FillRectangle((New-Object Drawing.SolidBrush $dark), 120, 92, 16, 150)
    $g.FillRectangle((New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(120, 76, 40))), 124, 92, 8, 148)
    # spear head with side blades
    $gb = New-Object Drawing.SolidBrush $gold
    $head = [Drawing.PointF[]]@((New-Object Drawing.PointF 128, 18), (New-Object Drawing.PointF 146, 62), (New-Object Drawing.PointF 134, 78), (New-Object Drawing.PointF 122, 78), (New-Object Drawing.PointF 110, 62))
    $g.FillPolygon((New-Object Drawing.SolidBrush $dark), $head)
    $g.DrawPolygon((New-Object Drawing.Pen $dark, 6), $head)
    $g.FillPolygon($gb, $head)
    # crescent guard
    $g.DrawArc((New-Object Drawing.Pen $dark, 15), 88, 44, 80, 46, 20, 140)
    $g.DrawArc((New-Object Drawing.Pen $gold, 8), 88, 44, 80, 46, 20, 140)
    # horsehair tassel
    # separate long strands of horsehair falling from the binding, swinging a little in the wind
    $ends = @(@(-58, 118), @(-49, 146), @(-38, 136), @(-28, 160), @(-17, 146), @(-7, 170), @(5, 150), @(15, 172), @(26, 146), @(37, 160), @(47, 134), @(56, 146), @(64, 112))
    foreach ($pass in 0, 1) {
        foreach ($raw in $ends) {
            $e = @(($raw[0] * 0.72), ($raw[1] * 0.56))
            $x0 = 128 + $e[0] * 0.38; $x1 = 128 + $e[0]; $y1 = 92 + $e[1]
            $pen = if ($pass -eq 0) { New-Object Drawing.Pen $dark, 8 } else { New-Object Drawing.Pen ([Drawing.Color]::FromArgb(240, 236, 224)), 4 }
            $pen.StartCap = 'Round'; $pen.EndCap = 'Triangle'
            $g.DrawBezier($pen, $x0, 92, $x0 + $e[0] * 0.5, 92 + $e[1] * 0.3, $x1 + 6, 92 + $e[1] * 0.65, $x1, $y1)
        }
    }
    # binding
    $g.FillRectangle((New-Object Drawing.SolidBrush $dark), 102, 84, 52, 16)
    $g.FillRectangle($gb, 106, 87, 44, 10)
    $g.Dispose()
    $b
}
$sizes = 256, 64, 48, 32, 24, 16
$pngs = foreach ($s in $sizes) { $m = New-Object IO.MemoryStream; $bmp = Draw $s; $bmp.Save($m, [Drawing.Imaging.ImageFormat]::Png); if ($s -eq 256) { $bmp.Save((Join-Path $root 'icon_preview.png')) }; $bmp.Dispose(); , $m.ToArray() }
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
# sheet: big, then the small sizes people actually see
$sheet = New-Object Drawing.Bitmap 400, 280
$g = [Drawing.Graphics]::FromImage($sheet)
$g.Clear([Drawing.Color]::FromArgb(70, 60, 50))
$g.DrawImage((Draw 256), 12, 12, 256, 256)
$y = 12
foreach ($s in 64, 48, 32, 16) { $g.DrawImage((Draw $s), 290, $y, $s, $s); $y += $s + 10 }
$sheet.Save((Join-Path $root 'icon_preview.png'))
"icon {0} KB" -f [int]((Get-Item (Join-Path $root 'attila64.ico')).Length / 1KB)
