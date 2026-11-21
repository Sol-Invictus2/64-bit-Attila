param([switch]$Sym)   # -Sym: also attila64_sym.exe + attila64_sym.pdb (same options plus -g) for profiles with our own function names; never ship those
# Build attila64.exe with the Zig C compiler in tools\zig. Windows 64-bit, placed high so the game's fixed low address stays free.
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$zig = Join-Path $root '..\..\tools\zig\zig-x86_64-windows-0.17.0\zig.exe'
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'gen_gl.ps1')
$src = @('loader.c', 'shim_libc.c', 'shim_stdio.c', 'shim_thread.c', 'shim_sys.c', 'shim_gfx.c', 'shim_gl_gen.c', 'shim_standin.c', 'shim_steam.c', 'steam_hub.c', 'shim_net.c', 'profile.c', 'picker.c', 'test_auto.c', 'fps_overlay.c', 'menu_button.c', 'attila64.rc') | ForEach-Object { Join-Path $root $_ }
$out = Join-Path $root 'attila64.exe'
& $zig cc -target x86_64-windows-gnu -O1 -fno-sanitize=undefined -Wno-unused-parameter `
    '-Wl,--image-base=0x7f0000000' $src -o $out -lkernel32 -luser32 -lws2_32 -lcomctl32 -lgdi32 -ld3d11 -ldxgi -ldxguid -lole32
if ($LASTEXITCODE -ne 0) { Write-Output "BUILD FAILED"; exit 1 }
Get-Item $out | Select-Object Name, Length
if ($Sym) {
    $symOut = Join-Path $root 'attila64_sym.exe'
    & $zig cc -target x86_64-windows-gnu -O1 -g -fno-sanitize=undefined -Wno-unused-parameter `
        '-Wl,--image-base=0x7f0000000' $src -o $symOut -lkernel32 -luser32 -lws2_32 -lcomctl32 -lgdi32 -ld3d11 -ldxgi -ldxguid -lole32
    if ($LASTEXITCODE -ne 0) { Write-Output 'SYM BUILD FAILED'; exit 1 }
    Get-Item $symOut | Select-Object Name, Length
}
