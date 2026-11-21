# Build "Install 64-bit Attila.exe": one file holding the loader and its libraries, never the Linux game file.
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$zig = (Resolve-Path (Join-Path $root '..\..\tools\zig\zig-x86_64-windows-0.17.0\zig.exe')).Path
Push-Location $root
# The compiler's cache does not notice changes to embedded files, so their fingerprint goes on the command line.
$stamp = (@('attila64.exe', 'SDL2.dll', 'steam_api64.dll', '..\lib\libstdc++.so.6', '..\lib\libgcc_s.so.1', '..\friend2\THIRD_PARTY_NOTICES.txt', '..\LICENSE.txt', 'attila64.ico', '..\..\attila_8player\out8\attila_8player_lobby.pack', '..\..\attila_8player\out8\attila_8v8_battle.pack') | ForEach-Object { (Get-FileHash -LiteralPath $_ -Algorithm MD5).Hash }) -join ''
& $zig cc -target x86_64-windows-gnu -O1 -fno-sanitize=undefined "-DEMBEDDED_FILES_STAMP=$stamp" 'installer.c' 'attila64.rc' -o 'Install 64-bit Attila.exe' -luser32 -lshell32 -lole32 -ladvapi32 -luuid
$code = $LASTEXITCODE
Pop-Location
if ($code -ne 0) { 'BUILD FAILED'; exit 1 }
Remove-Item (Join-Path $root 'Install 64-bit Attila.pdb') -ErrorAction SilentlyContinue
"{0:n1} MB" -f ((Get-Item (Join-Path $root 'Install 64-bit Attila.exe')).Length / 1MB)
