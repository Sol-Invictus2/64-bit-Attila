# Build and run the loader with a time limit from the Windows game folder; show how far it got. Nothing in that folder is changed.
param([int]$Seconds = 60, [int]$Lines = 45, [string]$Filter = '', [switch]$NoBuild, [string]$GameArgs = '')
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $root 'attila64.exe'
if (-not $NoBuild) {
    Remove-Item $exe, (Join-Path $root 'err.txt') -ErrorAction SilentlyContinue
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'build.ps1')
    if (-not (Test-Path $exe)) { exit 1 }
}
$sdl = Join-Path $root 'SDL2.dll'
if (-not (Test-Path $sdl)) { Copy-Item 'C:\Program Files (x86)\Steam\steamapps\common\Scourge Of War - Remastered\sdk\MyGUI\SDL2.dll' $sdl }
$home64 = Join-Path $env:USERPROFILE 'attila64_home'
New-Item -ItemType Directory -Force $home64 | Out-Null
$game = 'C:\Program Files (x86)\Steam\steamapps\content\app_325610\depot_325624\Attila'
$lib = (Resolve-Path (Join-Path $root '..\lib')).Path
$work = 'C:\Program Files (x86)\Steam\steamapps\common\Total War Attila'
$argv = @("`"$game`"", "`"$lib`"")
if ($GameArgs) { $argv += $GameArgs }
$err = Join-Path $root 'err.txt'
$p = Start-Process -FilePath $exe -ArgumentList $argv -WorkingDirectory $work -NoNewWindow -PassThru -RedirectStandardError $err -RedirectStandardOutput (Join-Path $root 'out.txt')
if (-not $p.WaitForExit($Seconds * 1000)) { $p.Kill(); "still running after $Seconds s" }
"exit: $($p.ExitCode)"
$log = Get-Content $err
if ($Filter) { $log | Where-Object { $_ -match $Filter } } else { $log | Where-Object { $_ -notmatch '^not supplied yet' } | Select-Object -Last $Lines }
