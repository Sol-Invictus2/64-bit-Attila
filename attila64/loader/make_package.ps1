# Put together the folder that goes inside the game's folder: attila64\package\attila64. The Linux game file is the player's own
# Steam depot copy; it is copied here for this PC only and must not be sent to anyone.
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$pkg = Join-Path $root '..\package\attila64'
New-Item -ItemType Directory -Force $pkg | Out-Null
Copy-Item (Join-Path $root 'attila64.exe'), (Join-Path $root 'SDL2.dll'), (Join-Path $root 'steam_api64.dll') $pkg -Force
Copy-Item (Join-Path $root '..\lib\libstdc++.so.6'), (Join-Path $root '..\lib\libgcc_s.so.1') $pkg -Force
# The DirectX 12 route (start window's Graphics choice): Mesa's OpenGL-on-DirectX-12 files, loaded only when the player picks it.
New-Item -ItemType Directory -Force (Join-Path $pkg 'dx12') | Out-Null
Copy-Item (Join-Path $root '..\lib\dx12\*.dll') (Join-Path $pkg 'dx12') -Force
$elf = Join-Path $pkg 'Attila'
if (-not (Test-Path $elf)) { Copy-Item 'C:\Program Files (x86)\Steam\steamapps\content\app_325610\depot_325624\Attila' $elf }
Get-ChildItem $pkg | Select-Object Name, @{n = 'MB'; e = { [math]::Round($_.Length / 1MB, 1) } } | Format-Table -AutoSize
