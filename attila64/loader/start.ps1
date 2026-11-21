# Start the built loader and leave it running; wait, then capture its window. Optional game arguments.
param([int]$Wait = 30, [string]$Shot = 's0.png', [string]$GameArgs = '')
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Get-Process attila64 -ErrorAction SilentlyContinue | Stop-Process -Force
$game = 'C:\Program Files (x86)\Steam\steamapps\content\app_325610\depot_325624\Attila'
$lib = (Resolve-Path (Join-Path $root '..\lib')).Path
$work = 'C:\Program Files (x86)\Steam\steamapps\common\Total War Attila'
$argv = @("`"$game`"", "`"$lib`"")
if ($GameArgs) { $argv += "`"$GameArgs`"" }
Start-Process -FilePath (Join-Path $root 'attila64.exe') -ArgumentList $argv -WorkingDirectory $work -RedirectStandardError (Join-Path $root 'err.txt') -RedirectStandardOutput (Join-Path $root 'out.txt') | Out-Null
& (Join-Path $root 'act.ps1') -Wait $Wait -Shot $Shot
