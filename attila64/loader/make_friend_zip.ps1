# Zip for friends: the one-file installer and a read-me. The Linux game file is never included; each player downloads their own from Steam.
param([string]$Name = '64-bit Attila Test.zip')
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$stage = Join-Path $env:TEMP 'attila64_friend'
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item (Join-Path $root 'Install 64-bit Attila.exe'), (Join-Path $root '..\friend2\READ ME.txt'), (Join-Path $root '..\LICENSE.txt') $stage
$out = Join-Path (Resolve-Path (Join-Path $root '..\..\send_to_friends')) $Name
Remove-Item $out -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $out
Remove-Item $stage -Recurse -Force
Add-Type -AssemblyName System.IO.Compression.FileSystem
$z = [IO.Compression.ZipFile]::OpenRead($out)
$z.Entries | ForEach-Object { "{0,-34} {1,10}" -f $_.FullName, $_.Length }
$z.Dispose()
"zip: $out, $([math]::Round((Get-Item $out).Length / 1MB, 1)) MB"
