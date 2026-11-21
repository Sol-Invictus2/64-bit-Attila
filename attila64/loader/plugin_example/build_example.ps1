# Builds example_plugin.dll with the Zig C compiler in tools\zig (any compiler that makes a 64-bit Windows DLL and knows
# __attribute__((sysv_abi)) will do: clang, gcc/MinGW).
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$zig = Join-Path $root '..\..\..\tools\zig\zig-x86_64-windows-0.17.0\zig.exe'
& $zig cc -target x86_64-windows-gnu -O1 -shared (Join-Path $root 'example_plugin.c') -o (Join-Path $root 'example_plugin.dll')
if ($LASTEXITCODE -ne 0) { 'BUILD FAILED'; exit 1 }
Remove-Item (Join-Path $root 'example_plugin.pdb'), (Join-Path $root 'example_plugin.lib') -ErrorAction SilentlyContinue
Get-Item (Join-Path $root 'example_plugin.dll') | Select-Object Name, Length
