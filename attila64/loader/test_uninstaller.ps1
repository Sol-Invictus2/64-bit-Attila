# 1.3.22 tests (temp folder only; real Steam, game, desktop and registry untouched):
# A. install puts "Uninstall 64-bit Attila.exe" in the folder; B. Play button still on a deleted folder -> answer 2 clears it;
# C. the uninstaller copy removes the whole folder, itself included.
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $root 'Install 64-bit Attila.exe'
$t = Join-Path $env:TEMP 'attila64_uninst_test'
Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$t\depot", "$t\steam\userdata\111\config" | Out-Null
Copy-Item (Resolve-Path (Join-Path $root '..\package\attila64\Attila')).Path "$t\depot\Attila"
$vdf = "$t\steam\userdata\111\config\localconfig.vdf"
$env:ATTILA_INSTALL_TEST = $t
function Run { $p = Start-Process $exe -PassThru -NoNewWindow -RedirectStandardOutput "$t\out.txt"; if (-not $p.WaitForExit(90000)) { $p.Kill(); 'TIMED OUT' } }

'--- B. deleted folder, Play button still ours -> 2 clears it, installs nothing'
@"
"UserLocalConfigStore"
{
	"Software"
	{
		"Valve"
		{
			"Steam"
			{
				"apps"
				{
					"325610"
					{
						"LaunchOptions"		"\"D:\\Games\\Total War Attila\\attila64\\attila64.exe\" %command%"
					}
				}
			}
		}
	}
}
"@ | Set-Content $vdf
$env:ATTILA_INSTALL_ANSWER = '2'
Run
Get-Content "$t\out.txt" | Select-String 'folder is gone|back to normal|starts normal'
"launch option cleared: " + -not ((Get-Content $vdf -Raw) -match 'attila64')
"nothing installed: " + -not (Test-Path "$t\game\attila64")
Remove-Item Env:ATTILA_INSTALL_ANSWER

'--- A. install puts the uninstaller in the folder'
Run
$un = "$t\game\attila64\Uninstall 64-bit Attila.exe"
"uninstaller there: " + (Test-Path $un)
"same as installer: " + ((Get-FileHash $un -Algorithm MD5).Hash -eq (Get-FileHash $exe -Algorithm MD5).Hash)
"play button set: " + ((Get-Content $vdf -Raw) -match 'attila64\.exe')

'--- C. the uninstaller in the folder removes everything, itself too'
$p = Start-Process $un -PassThru -NoNewWindow -RedirectStandardOutput "$t\un.txt"
$p.WaitForExit(30000) | Out-Null
for ($i = 0; $i -lt 30 -and (Test-Path "$t\game\attila64"); $i++) { Start-Sleep -Seconds 1 }
Start-Sleep -Seconds 4
"attila64 folder gone: " + (-not (Test-Path "$t\game\attila64"))
"play button back to normal: " + -not ((Get-Content $vdf -Raw) -match 'attila64')
"temp copies left: " + @(Get-ChildItem $env:TEMP -Filter 'attila64_uninstall_*.exe' -ErrorAction SilentlyContinue).Count
Remove-Item Env:ATTILA_INSTALL_TEST
Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue
