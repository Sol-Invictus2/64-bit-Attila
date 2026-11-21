# Attila 8-player campaign (64-bit Attila), lobby layout: ui\frontend ui\mp_grand_campaign with 8 compact player panels (4 per side)
# instead of 2 tall ones. Panels 3/5/7 are copies of panel 1, 4/6/8 copies of panel 2 (without its victory_conditions);
# copies get fresh ids. Each panel: portrait, name, ready status, faction dropdown (+ kick on the right side); the big
# flag and the effect icons are parked off screen. An open-slot box behind every panel spot; title "Eight Player Campaign". Output: out\attila_8player_lobby.pack (mod pack, loaded by the 64-bit engine from its packs folder; was movie pack, loads without the
# launcher's mod list) + out\mp_grand_campaign.
param([string]$OutDir = "$PSScriptRoot\out")
$ErrorActionPreference = 'Stop'
Add-Type -Path "$PSScriptRoot\AttilaLayout.cs"
Add-Type -Path "$PSScriptRoot\..\kit\pfh4.cs"
$src = "$PSScriptRoot\vanilla_ui\mp_grand_campaign"
$data = [IO.File]::ReadAllBytes($src)
if (-not [AttilaLayout]::Roundtrip($data)) { throw 'vanilla layout does not round-trip' }
$root = [AttilaLayout]::Read($data)
function FindC([string]$n, $under = $root) { $c = [AttilaLayout]::Find($under, $n); if (-not $c) { throw "no $n" }; $c }

# every id the file defines (components, images, states), to hand out fresh ones
function AllNodes($n) { $n; foreach ($v in $n.Values) { if ($v.Data -is [AtNode]) { AllNodes $v.Data } elseif ($v.Data -is [System.Collections.Generic.List[AtNode]]) { foreach ($i in $v.Data) { AllNodes $i } } } }
function DefIds($n) { foreach ($x in (AllNodes $n)) { if ($x.Kind -in 'component', 'image', 'state') { [uint32]$x.Num('uid') } } }
$script:nextId = [uint32](((DefIds $root) | Measure-Object -Maximum).Maximum) + 0x100
function Refresh($n) {                       # new ids for a copied subtree; every u32 that named an old id follows
  $map = @{}; foreach ($id in (DefIds $n)) { if ($id -ne 0 -and -not $map.ContainsKey($id)) { $map[$id] = $script:nextId; $script:nextId += 0x20 } }
  foreach ($x in (AllNodes $n)) { foreach ($v in $x.Values) { if ($v.Type -eq 'u32' -and $map.ContainsKey([uint32]$v.Data)) { $v.Data = [uint32]$map[[uint32]$v.Data] } } }
}
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
function Font($c, [int]$size) { foreach ($s in $c.Items('states')) { $s.SetNum('s9', $size) } }   # state font size
function SetPos($c, [int]$x, [int]$y) { $c.SetNum('x', $x); $c.SetNum('y', $y) }

$W = 256; $H = 100
function Compact($p) {
  Resize $p $W $H
  SetPos (FindC 'icon_frame' $p) 5 5
  $pl = FindC 'player' $p; SetPos $pl 8 8                          # portrait; the rest is placed relative to it
  $nm = FindC 'dy_player_name' $pl; SetPos $nm 72 2; Resize $nm 144 24
  $st = FindC 'status' $pl; SetPos $st 72 32; Resize $st 168 26; Font $st 12      # "Waiting for player" on one line
  SetPos (FindC 'faction_dropdown' $pl) 6 66
  $vc = FindC 'icon_voicechat' $pl; Scale $vc 0.48; SetPos $vc 44 44              # small headset on the portrait corner
  SetPos (FindC 'flag' $pl) -3000 0
  SetPos (FindC 'subpanel_effect_bundles' $pl) -3000 0
  $k = [AttilaLayout]::Find($p, 'button_kick'); if ($k) { Scale $k 0.56; SetPos $k 224 6 }
  # team button (64-bit Attila 1.2): a small copy of the Purchase button named button_team; the engine writes "Team N" on it and
  # a click by the panel's own player moves him to the next team with a free place. The status text gives up the room for it.
  Resize $st 92 26
  $tb = [AttilaLayout]::Copy((FindC 'button_purchase')); $tb.SetText('name', 'button_team'); Resize $tb 74 24; SetPos $tb 176 40; $tb.SetNum('dock', 0); $tb.SetNum('dock_x', 0); $tb.SetNum('dock_y', 0)
  $tt = FindC 'button_txt' $tb; Resize $tt 74 24; Font $tt 12; SetPos $tt 0 0
  foreach ($s in $tt.Items('states')) { $s.SetText('text', 'Team'); $s.SetText('s6', '') }
  $tb.SetText('tip0', 'Your team. Click to move to the next team with a free place.'); $tb.SetText('tip1', '')
  Refresh $tb; $p.Children.Add($tb)
}

$main = FindC 'main_panel'
$p1 = FindC 'panel_player1'; $p2 = FindC 'panel_player2'
Compact $p1; Compact $p2
SetPos $p1 30 92; SetPos $p2 817 92
$bg = FindC 'bg'; SetPos $bg 817 92; Resize $bg 257 $H

$kids = $main.Children
$at = $kids.IndexOf($p2) + 1
$rows = @(198, 304, 410)
for ($i = 3; $i -le 8; $i++) {
  $right = ($i % 2) -eq 0
  $c = [AttilaLayout]::Copy($(if ($right) { $p2 } else { $p1 }))
  if ($right) { $vc = [AttilaLayout]::Find($c, 'victory_conditions'); if ($vc) { [void]$c.Children.Remove($vc) } }
  $c.SetText('name', "panel_player$i")
  SetPos $c $(if ($right) { 817 } else { 30 }) $rows[[int][Math]::Floor(($i - 3) / 2)]
  Refresh $c
  $kids.Insert($at, $c); $at++
}

# an open-slot box (copy of bg + its invite button) behind every other panel spot; a joined player's panel covers
# it. The copies go after the original bg, so the game's search for "button_invite" still finds the original first.
$at = $kids.IndexOf($bg) + 1
foreach ($pos in @(@(30, 92), @(30, 198), @(817, 198), @(30, 304), @(817, 304), @(30, 410), @(817, 410))) {
  $c = [AttilaLayout]::Copy($bg)
  $c.SetText('name', ('bg_{0}_{1}' -f $pos[0], $pos[1]))
  # empty frame only: the copies' invite icon showed through a joined player's panel, and the game only drives the first one
  foreach ($b in @($c.Children | Where-Object { $_.Text('name') -eq 'button_invite' })) { [void]$c.Children.Remove($b) }
  SetPos $c $pos[0] $pos[1]
  Refresh $c
  $kids.Insert($at, $c); $at++
}

# title: "Eight Player Campaign" (layout text; the localisation key would put "Two Player Campaign" back)
$hd = FindC 'panel_heading' (FindC 'title_plaque')
$st = $hd.Items('states')[0]; $st.SetText('text', 'Eight Player Campaign'); $st.SetText('s6', '')
$hd.SetText('tip0', 'This campaign can be played by up to eight players, against each other or together against the AI. ')
$hd.SetText('tip1', '')

$out = [AttilaLayout]::Write($root)
if (-not [AttilaLayout]::Roundtrip($out)) { throw 'new layout does not round-trip' }
New-Item -ItemType Directory -Force $OutDir | Out-Null
[IO.File]::WriteAllBytes("$OutDir\mp_grand_campaign", $out)
$files = New-Object 'System.Collections.Generic.List[System.Collections.Generic.KeyValuePair[string,byte[]]]'
$files.Add((New-Object 'System.Collections.Generic.KeyValuePair[string,byte[]]' ('ui\frontend ui\mp_grand_campaign', $out)))
[Pfh4Pack]::Write("$OutDir\attila_8player_lobby.pack", 3, $files)
[AttilaLayout]::Tree((FindC 'main_panel'), 1)
'attila_8player_lobby.pack {0}' -f (Get-FileHash "$OutDir\attila_8player_lobby.pack").Hash



