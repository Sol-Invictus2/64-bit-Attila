# 8v8 multiplayer battles (64-bit Attila loader), layout: ui\frontend ui\battle_setup with compact player boxes (155x64 instead of
# 155x130) so the loader can put two rows of 4 per side in the old height. Portrait 48x48 on the left, name and faction button on the
# right, small icons on the portrait corners. Output: out8\attila_8v8_battle.pack (mod pack, loaded by the 64-bit engine from its packs folder) + out8\battle_setup.
param([string]$OutDir = "$PSScriptRoot\out8")
$ErrorActionPreference = 'Stop'
Add-Type -Path "$PSScriptRoot\AttilaLayout.cs"
Add-Type -Path "$PSScriptRoot\..\kit\pfh4.cs"
$data = [IO.File]::ReadAllBytes("$PSScriptRoot\vanilla_ui\battle_setup")
if (-not [AttilaLayout]::Roundtrip($data)) { throw 'vanilla layout does not round-trip' }
$root = [AttilaLayout]::Read($data)
function FindC([string]$n, $under = $root) { $c = [AttilaLayout]::Find($under, $n); if (-not $c) { throw "no $n" }; $c }
function Resize($c, [int]$w, [int]$h) {     # component size = its states and their full-size images
  foreach ($s in $c.Items('states')) {
    $ow = $s.Num('w'); $oh = $s.Num('h'); $s.SetNum('w', $w); $s.SetNum('h', $h)
    foreach ($si in $s.Items('images')) { if ($si.Num('w') -eq $ow -or $si.Num('w') -eq $ow + 1) { $si.SetNum('w', $w) }; if ($si.Num('h') -eq $oh) { $si.SetNum('h', $h) } }
  }
}
function Scale($c, [double]$f) {           # component size and all its state images scaled by f
  foreach ($s in $c.Items('states')) {
    $s.SetNum('w', [Math]::Round($s.Num('w') * $f)); $s.SetNum('h', [Math]::Round($s.Num('h') * $f))
    foreach ($si in $s.Items('images')) { foreach ($k in 'x', 'y', 'w', 'h') { $si.SetNum($k, [Math]::Round($si.Num($k) * $f)) } }
  }
}
function SetPos($c, [int]$x, [int]$y) { $c.SetNum('x', $x); $c.SetNum('y', $y) }
function Undock($c) { $c.SetNum('dock', 0); $c.SetNum('dock_x', 0); $c.SetNum('dock_y', 0) }

$W = 155; $H = 64
$t = FindC 'player_template'
Resize $t $W $H
$slot = FindC 'player_slot' $t; Resize $slot $W $H
$icon = FindC 'player_icon' $slot; Undock $icon; Scale $icon 0.75; SetPos $icon 4 8          # 48x48 portrait
# portrait children are placed relative to the portrait
foreach ($n in 'icon_ai_player', 'icon_human_player', 'steam_icon') { SetPos (FindC $n $icon) 30 28 }
$st = FindC 'status' $icon; Scale $st 0.8; SetPos $st 30 -4
$nm = FindC 'dy_name' $icon; SetPos $nm 48 -6; Resize $nm 100 22
$ht = FindC 'host_tx' $icon; Undock $ht; SetPos $ht -2 -12; Resize $ht 52 14
$alt = FindC 'faction_name_alt' $icon; SetPos $alt 48 18; Resize $alt 100 22
$fb = FindC 'button_change_faction' $icon; SetPos $fb 44 16; Scale $fb 0.72                # 107x30
$fn = FindC 'faction_name' $fb; Resize $fn 84 22; SetPos $fn 3 4
$fm = FindC 'setup_faction_menu' $fb; Resize $fm 107 30
foreach ($n in 'button_close', 'button_kick') { Scale (FindC $n $slot) 0.8 }
$sp = FindC 'sync_progress' $slot; Scale $sp 0.8; SetPos $sp 124 30
Scale (FindC 'sync_bar' $slot) 0.6
$b = FindC 'button_add_player_no_ai' $t; Scale $b 0.8; SetPos $b 57 12
$b = FindC 'button_add_player' $t; Scale $b 0.75; SetPos $b 52 8
$b = FindC 'button_add_ai' $t; Scale $b 0.75; SetPos $b 80 8

$out = [AttilaLayout]::Write($root)
if (-not [AttilaLayout]::Roundtrip($out)) { throw 'new layout does not round-trip' }
New-Item -ItemType Directory -Force $OutDir | Out-Null
[IO.File]::WriteAllBytes("$OutDir\battle_setup", $out)
$files = New-Object 'System.Collections.Generic.List[System.Collections.Generic.KeyValuePair[string,byte[]]]'
$files.Add((New-Object 'System.Collections.Generic.KeyValuePair[string,byte[]]' ('ui\frontend ui\battle_setup', $out)))
[Pfh4Pack]::Write("$OutDir\attila_8v8_battle.pack", 3, $files)
[AttilaLayout]::Tree($t, 4)
'attila_8v8_battle.pack {0}' -f (Get-FileHash "$OutDir\attila_8v8_battle.pack").Hash


