# SUPERSEDED 2026-11-09: the Workshop folder (Attila64_Release\workshop) and upload\item.vdf are now kept by hand (Test 14,
# 64-bit Attila name, MIT licence). Running this would rebuild a dev engine and overwrite them, so it stops here.
'make_release.ps1 is superseded: the Workshop files are kept by hand now. Nothing done.'; exit 1
# Builds the public release: attila64\release\workshop (the Workshop folder), preview.png, the Nexus zip and the upload files. Uploads nothing.
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$a64 = (Resolve-Path (Join-Path $root '..')).Path
$rel = (Join-Path $env:USERPROFILE 'Documents\Attila64_Release')
Remove-Item (Join-Path $rel 'workshop') -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $rel 'workshop'), (Join-Path $rel 'upload') | Out-Null

# licence texts come from the Linux side, where they are installed
$env:WSL_UTF8 = 1
$head = Get-Content (Join-Path $a64 'friend2\notices_head.txt') -Raw
$exc = wsl -d Ubuntu -u sol --cd ~ -- awk '/^GCC RUNTIME LIBRARY EXCEPTION/{p=1} p{print} p&&w&&/of GCC\.$/{exit} /No Weakening/{w=1}' /usr/share/doc/libstdc++6/copyright
$gpl = wsl -d Ubuntu -u sol --cd ~ -- cat /usr/share/common-licenses/GPL-3
if (@($exc).Count -lt 20 -or @($gpl).Count -lt 500) { 'LICENCE TEXTS MISSING'; exit 1 }
$sep = '=' * 90
$text = $head + "$sep`r`nGCC RUNTIME LIBRARY EXCEPTION (for the two GCC files above)`r`n$sep`r`n" + ($exc -join "`r`n") + "`r`n`r`n$sep`r`nGNU GENERAL PUBLIC LICENSE version 3`r`n$sep`r`n" + ($gpl -join "`r`n") + "`r`n"
$noticePath = Join-Path $a64 'friend2\THIRD_PARTY_NOTICES.txt'
[IO.File]::WriteAllText($noticePath, $text, (New-Object Text.ASCIIEncoding))

# programs
$b = powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'build.ps1') | ForEach-Object { "$_" }
$b | Where-Object { $_ -match 'error|FAILED|undefined' } | Select-Object -First 6
powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'build_installer.ps1') | ForEach-Object { "$_" } | Select-Object -Last 3
$wsf = Join-Path $rel 'workshop'
Copy-Item (Join-Path $root 'Install Attila 64-bit.exe'), (Join-Path $a64 'friend2\READ ME.txt'), $noticePath $wsf

# Workshop preview, square and under 1 MB
Add-Type -AssemblyName System.Drawing
$src = [Drawing.Image]::FromFile((Join-Path $a64 'art\genghis_portrait_960.jpg'))
$bmp = New-Object Drawing.Bitmap 512, 512
$g = [Drawing.Graphics]::FromImage($bmp)
$g.InterpolationMode = 'HighQualityBicubic'; $g.SmoothingMode = 'AntiAlias'; $g.TextRenderingHint = 'AntiAlias'
$g.DrawImage($src, (New-Object Drawing.RectangleF 0, 0, 512, 512), (New-Object Drawing.RectangleF 90, 190, 780, 780), [Drawing.GraphicsUnit]::Pixel)
$g.FillRectangle((New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(205, 12, 20, 38))), 0, 424, 512, 88)
$g.FillRectangle((New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(232, 184, 74))), 0, 420, 512, 4)
$fmt = New-Object Drawing.StringFormat; $fmt.Alignment = 'Center'
$g.DrawString('ATTILA 64-BIT', (New-Object Drawing.Font 'Georgia', 30, ([Drawing.FontStyle]::Bold)), (New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(245, 236, 210))), 256, 428, $fmt)
$g.DrawString('no 4 GB limit', (New-Object Drawing.Font 'Georgia', 15), (New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(232, 184, 74))), 256, 474, $fmt)
$g.Dispose(); $src.Dispose()
$preview = Join-Path $rel 'preview.png'
$bmp.Save($preview, [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()

# Nexus zip
$zip = Join-Path $rel 'Attila 64-bit Beta 0.1.zip'
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $wsf '*') -DestinationPath $zip

# upload files for SteamCMD; private on purpose, so the page can be checked before it goes public
$desc = (Get-Content (Join-Path $a64 'release_pages\workshop_page.txt') | Select-Object -Skip 2) -join "`n"
$desc = $desc -replace '\\', '\\' -replace '"', '\"'
$vdf = @"
"workshopitem"
{
    "appid"           "325610"
    "publishedfileid" "3834958414"
    "contentfolder"   "$($wsf -replace '\\', '\\')"
    "previewfile"     "$($preview -replace '\\', '\\')"
    "visibility"      "2"
    "title"           "Attila 64-bit (Beta)"
    "description"     "$desc"
    "changenote"      "Beta 0.1"
}
"@
[IO.File]::WriteAllText((Join-Path $rel 'upload\item.vdf'), $vdf, (New-Object Text.UTF8Encoding $false))
Get-ChildItem $wsf, $preview, $zip | ForEach-Object { "{0,-34} {1,10:n0} KB" -f $_.Name, ($_.Length / 1KB) }
