# Copy the package into the Windows Attila folder, as one new folder. No existing file is changed. -Uninstall removes that folder only.
param([switch]$Uninstall)
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$game = 'C:\Program Files (x86)\Steam\steamapps\common\Total War Attila'
$dest = Join-Path $game 'attila64'
if ($Uninstall) {
    if (Test-Path (Join-Path $dest 'attila64.exe')) { Remove-Item $dest -Recurse -Force; 'Removed the attila64 folder. Clear the Steam launch option as well.' } else { 'Not installed.' }
    exit
}
$pkg = Join-Path $root '..\package\attila64'
if (-not (Test-Path (Join-Path $pkg 'Attila'))) { 'Run make_package.ps1 first.'; exit 1 }
Copy-Item $pkg $game -Recurse -Force
"Installed in $dest"
''
'In Steam: right-click Total War: ATTILA > Properties > General > Launch Options, and paste this line:'
"`"$dest\attila64.exe`" %command%"
''
'To go back to normal 32-bit Attila, clear that line. Your original files were not changed.'
