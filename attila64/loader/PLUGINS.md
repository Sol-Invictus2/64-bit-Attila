# 64-bit Attila plugins (first version, 2026-11-11: in the dev engine only, not in a release yet)

A plugin is a 64-bit Windows DLL that the loader loads next to the game, so that other people can add engine fixes without
changing the loader itself. Everything a plugin author needs is in `attila64_plugin.h` (interface version 1) and the working
example in `plugin_example\`.

## For players
- Put the plugin's `.dll` into `<your Attila folder>\attila64\plugins` (make the folder if it is not there).
- The log (`attila64_log.txt`, or "Copy last log" in the mod list window) has a line `plugin in use: name.dll` for each one,
  and says why when one is left out.
- A plugin is a program: it can do anything on your PC that the game can. Only use plugins from people you trust.
- Multiplayer: if a plugin changes how battles or the campaign are calculated, every player needs it. Plugins that only change
  what is drawn or shown can differ between players.
- To switch all plugins off without deleting them, start with `ATTILA_NO_PLUGINS=1` set, or move the DLLs out of the folder.

## For plugin authors
- The game is the **Linux** build of Total War: ATTILA, loaded at its own fixed addresses. Open the Linux file `Attila`
  (Steam depot 325624) in Ghidra or IDA: it has function names, and the addresses shown there are the addresses in memory.
- Game functions use the Linux calling convention. Declare every function you hook, and every game function you call, with
  `ATTILA64_GAME_CALL`. The interface functions themselves (`api->log` and so on) are ordinary Windows functions.
- The loader calls `attila64_plugin_init(api)` after the game is in memory and the loader's own fixes are applied, before any
  game code has run. Patch and hook code here. The game's data has not been set up yet at this point.
- If you export `attila64_plugin_game_started(api)`, it is called once the game has given its settings their start values:
  the place to change settings.
- `api->patch(address, expected, new, length)` changes game bytes only if they are exactly the expected ones, and
  `api->hook(address, expected, length, your_function)` does the same check before redirecting a function. Always give the
  real original bytes: this is what keeps a plugin harmless when the game is updated or another plugin got there first.
- Plugins load in name order. Up to 32 are loaded.
- Build with any compiler that makes a 64-bit Windows DLL and knows `__attribute__((sysv_abi))` (clang, gcc/MinGW, Zig). See
  `plugin_example\build_example.ps1`. MSVC's own compiler does not know that attribute.

## What the interface gives (version 1)
| Member | What it is |
|---|---|
| `api_version`, `engine_version` | Interface version (1) and the loader's version text. |
| `game_lo`, `game_hi` | The game's code and data in memory. |
| `plugin_dir` | The plugins folder, for a plugin's own settings files. |
| `log(fmt, ...)` | A line in the log. |
| `patch(at, expect, bytes, len)` | Checked change of game bytes. 1 = done, 0 = left alone. |
| `hook(at, expect, len, fn)` | Checked redirect of a game function; returns what to call for the original, or NULL. |
| `code_near_game(n)` | Memory the game's 32-bit jumps can reach, for hand-made code. |

## Not in this version
- No way to look game functions up by name (the Linux file has names, but the loader does not carry the table).
- No helper for Lua or the game's scripting.
- No ordering or dependency rules between plugins beyond name order.
- Not yet in the installer; the plugins folder is not created or removed by it.
