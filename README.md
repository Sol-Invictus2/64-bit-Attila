# 64-bit Attila

Bugs, crashes and logs: post them on Discord, https://discord.gg/3TUEGTMshP

Runs **Total War: ATTILA** as a 64-bit game on Windows, using the 64-bit game file Steam offers for the Linux
version. You need Total War: ATTILA on Steam. Your normal Attila is not changed: Steam's Play button still starts it.

This is the source code of **version 1.3.20**, the same version as on the Steam Workshop.

## For players

Get the installer from the Steam Workshop page:
https://steamcommunity.com/sharedfiles/filedetails/?id=3834958414

How to install, play together and report problems: [READ ME.txt](attila64/friend2/READ%20ME.txt)

## What you get

- No 4 GB memory limit.
- The game draws with DirectX 11, with the game's own DirectX 11 shaders (OpenGL is the fallback).
- Faster loading and higher fps in big battles.
- 8-player multiplayer campaign.
- 8 vs 8 multiplayer battles, with players or AI armies.
- When a player drops out, the AI takes over their armies and the game goes on.
- Every player autosaves at the end of each campaign round.
- A mod list before the game starts. Normal `.pack` mods should work; tested with Medieval Kingdoms 1212AD and
  The Dawnless Days.

## What is in here

| Folder | What it is |
|---|---|
| `attila64/loader` | The loader (`attila64.exe`): loads the Linux game file on Windows and gives it the Windows versions of what it expects (C library, files, threads, graphics, Steam, network), plus the game fixes, the DirectX 11 renderer, the mod list window and the installer. File map and every test switch: [SWITCHES.md](attila64/loader/SWITCHES.md). |
| `attila64/loader/plugin_example` | A small example plugin. See [PLUGINS.md](attila64/loader/PLUGINS.md). |
| `attila64/friend2` | The READ ME for players, and the notices for the other people's parts the installer carries. |
| `attila64/art` | The picture the icon is made from (public domain). |
| `attila_8player` | The scripts that make the two changed game screens: the 8-player campaign lobby and the 8 vs 8 battle set-up. |
| `kit` | The pack file reader and writer those scripts use. |

## What is not in here

- **No game files.** The 64-bit game file (`Attila`, Steam depot 325624) is downloaded by each player from Steam with
  their own account. The game's own screen files that the 8-player screens are made from are not here either, and
  neither are the finished packs.
- No built programs. Players get the installer from the Workshop.
- Other people's files that the installer carries: `SDL2.dll`, `steam_api64.dll`, `libstdc++.so.6` and `libgcc_s.so.1`.
  They are listed in [THIRD_PARTY_NOTICES.txt](attila64/friend2/THIRD_PARTY_NOTICES.txt).

## Building

On Windows, with PowerShell. The scripts expect the folders as they are here.

1. Put the Zig compiler, version 0.17.0 for 64-bit Windows, in `tools\zig\zig-x86_64-windows-0.17.0\` in this folder
   (so that `tools\zig\zig-x86_64-windows-0.17.0\zig.exe` exists).
2. Build the loader:

   ```powershell
   powershell -ExecutionPolicy Bypass -File attila64\loader\build.ps1
   ```

   This makes `attila64\loader\attila64.exe`.

The installer (`attila64\loader\build_installer.ps1`) packs the loader together with files that are not in this
repository, so it also needs:

- `SDL2.dll` (2.24.1) and `steam_api64.dll` in `attila64\loader`,
- `libstdc++.so.6` and `libgcc_s.so.1` (GCC 4.9, Debian build) in `attila64\lib`,
- the two packs in `attila_8player\out8`. `build_lobby_8p.ps1` and `build_8v8_battle.ps1` make them from the game's
  own `mp_grand_campaign` and `battle_setup` screen files (from `ui\frontend ui` in your own game's packs), which
  go in `attila_8player\vanilla_ui`. Run `build_lobby_8p.ps1` with `-OutDir` set to the `out8` folder.

## Credits and licence

64-bit Attila is a loader by Sol Invictus, built with AI assistance.

MIT, see [LICENSE.txt](LICENSE.txt). It covers 64-bit Attila's own files (the loader, the installer and their source
code) in every version, including the test versions shared before the licence was added. It covers no game files:
none are included, and you need your own copy of the game. The game screens edited for 8 players are not covered
either. The other parts keep their own licences, listed in
[THIRD_PARTY_NOTICES.txt](attila64/friend2/THIRD_PARTY_NOTICES.txt).

Not affiliated with or endorsed by Creative Assembly, Feral Interactive or SEGA.
Total War and Total War: ATTILA are trademarks of their owners.
