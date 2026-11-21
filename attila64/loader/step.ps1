# One development step: build + run, show the end of the log, name crash addresses through the Linux tools, show the game's picture state.
param([int]$Seconds = 90, [int]$Lines = 14, [int]$Syms = 14, [switch]$NoBuild)
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$args2 = @{ Seconds = $Seconds; Lines = $Lines }
if ($NoBuild) { $args2.NoBuild = $true }
& (Join-Path $root 'run.ps1') @args2 2>&1 | ForEach-Object { $s = "$_"; if ($s.Length -gt 170) { $s.Substring(0, 170) } else { $s } } | Where-Object { $_ -notmatch 'bridges generated|^$|^Name|^----|attila64.exe' }
$err = Join-Path $root 'err.txt'
if (Select-String -Path $err -Pattern 'stack\+' -Quiet) {
    $env:WSL_UTF8 = 1
    $t = (wsl -d Ubuntu -u root -- wslpath -a ($root.Replace('\', '/') + '/../tools/sym.sh')).Trim()
    $e = (wsl -d Ubuntu -u root -- wslpath -a ($err.Replace('\', '/'))).Trim()
    wsl -d Ubuntu -u sol --cd ~ -- bash $t $e $Syms
}
