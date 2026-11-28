# 64-bit Attila - changelog for developers

This is the technical changelog: what changed in the code, why, and where. It is written for people who build plugins
for 64-bit Attila or patch the same game (for example JGdaCaesar's Engine Performance Fix and FrameBridge add-on,
ATN's RenoDX add-on, Ludde's 8-player plugin). The player notes are in the READ ME that comes with the installer.

How to read it:
- Newest version first. Every version has "Changes" and "For plugin and mod developers".
- Addresses like `0x3473086` are addresses in the Linux 64-bit game file (`Attila`, Steam depot 325624) as the loader
  loads it, at its normal base `0x400000`.
- `ATTILA_*` names are environment switches, mostly for testing. `settings.txt` keys are in `attila64\settings.txt`.
  The full list is in [SWITCHES.md](attila64/loader/SWITCHES.md).
- Every game patch checks the original bytes first and logs "patch skipped, game code differs" when they differ.

Plugin interface: `attila64_plugin.h` and `ATTILA64_PLUGIN_API` (still **1**) have not changed from 1.3.6 to 1.3.33.
See [PLUGINS.md](attila64/loader/PLUGINS.md).

Versions before 1.3.6 are not in this file. 1.3.21 was never released.

## 1.3.33 (2026-11-28) - old 1212AD flag test pack removed

This release removes an old test copy of a 1212AD waving-flags pack. It was shipped by mistake and is now deleted on update.

### Changes
- The installer no longer ships `packs\1212\!!1212ad_flag_wave.pack`. JGdaCaesar asked for this: it was an outdated pack for internal testing only. The file is now in the installer's list of old files to remove (`g_old_files` in installer.c). The empty `packs\1212` folder is removed too (`remove_old_files`). The installer is much smaller now.
- The loader no longer loads this pack first for 1212AD lists. Before, `main` in loader.c put it at the top of `used_mods_64.txt` when the mod list had a 1212 pack and the engine fix and flags were ticked. Now, at every start, it deletes a copy left by an older install and removes the empty `packs\1212` folder. The log says "old 1212 AD waving-flags test pack in packs\1212: deleted". The player's mod list lines are now copied into `used_mods_64.txt` unchanged. Before, lines with `!!1212ad_flag_wave` were filtered out.
- Waving flags for 1212AD now come only from JGdaCaesar's own Workshop items.

### For plugin and mod developers
- The loader deletes `attila64\packs\1212\!!1212ad_flag_wave.pack` at start, and the installer deletes it on update. Do not put your own files under that name or in that folder.
- The settings `settings_flag_wave()` and `settings_perf_fix()` are no longer used for pack loading in loader.c.

## 1.3.32 (2026-11-27) - unticked add-ons leave the game folder, PC profile in the log

Unticking the engine fix or the upscaling add-on now moves their files out of the load folders. The log also gets a PC profile and the mod load order, and a missing campaign map terrain is now named before the crash.

### Changes
- Unticking "Engine performance fix" or "Upscaling and frame generation" in the mod list now moves the files into `attila64\unticked`. Ticking again moves them back. Before, unticking only skipped loading `perffix64.dll`, or wrote `addon=off` for FrameBridge, so the FrameBridge DLLs stayed in the game folder, where normal Attila's engine fix (`dinput8.dll`) could still find them. This is done by new `unticked_move`, `move_replace` and `delete_tree` in plugins.inc. `perffix_path` / `framebridge_path` in picker.c got a third place (`which == 2` = `attila64\unticked\...`), and `perffix_available` / `framebridge_available` also look there.
- `framebridge_prepare` is now called from `plugins_load` on its own. Before, it was called only from inside `perffix_prepare`, after the engine-fix tick check. Now an unticked add-on also leaves the game folder when the engine fix is unticked. The old function is now `framebridge_set`. The new `framebridge_prepare` moves the folder back first (when ticked), sets `addon=`, then moves it out (when unticked).
- PC profile in every log (new pc_profile.inc, started by `pc_profile_start` in `main` after the display scaling line). It writes "PC:" lines from a background thread: Windows version, processor, RAM and page file, each graphics card (name, maker, IDs, memory, driver version), screens (size, Hz, colour depth), battery or desktop, the game drive (SSD or HDD, bus, free space), DLLs in the game folder that are not CA's own (with version markers such as JGdaCaesar's `AttilaCPUPerfFix-version`), ReShade and FrameBridge, and the `attila64\settings.txt` lines. Programs that load into the game (overlays, recorders, ReShade) are listed 45 s after start and again at 5 minutes if there are new ones. No computer name, user name, serial numbers or monitor names are written. Folders are shown only from "steamapps" on. `ATTILA_NO_PC_PROFILE=1` turns it all off.
- The mods in load order are now in the log ("mods (load order, N packs): ..."), read from the "mod" lines of `used_mods_64.txt` (`pc_mod_list`). Only file names are shown, never folders.
- Missing campaign map terrain is named in the log (`patch_terrain_name` in crash_guards.inc). The crash was c0000005 at 32CCF10 in `TILE_DATABASE::TILE_VARIATION::tile()` on NULL. In `create_single_tile_battle` (3473060), the call at 3473086 to `TILE_DATABASE::tile_variation_from_location` (32d01a0) returns NULL when the map's terrain is not in the loaded packs, and 34730e3 uses it without a check. That call now goes through `terrain_lookup`, which logs "campaign map terrain not found: '<name>'" (up to 3 times) and flushes the log. The game still crashes after this, but the log says which map a mod did not supply. `ATTILA_NO_TERRAIN_NAME=1` turns it off.

### For plugin and mod developers
- **perffix64.dll (JGdaCaesar):** when the engine fix is unticked, `attila64\plugins\perffix64.dll` is moved to `attila64\unticked\perffix64.dll`. When ticked again, it is moved back before the Workshop version check.
- **FrameBridge (JGdaCaesar):** when upscaling is unticked, the whole `<game folder>\FrameBridge` folder is moved to `attila64\unticked\FrameBridge`. The loader looks for `helper64.exe` to see whether the folder is there. A folder is renamed, or copied and then deleted when renaming fails. `addon=off` is still written first. If your 32-bit `dinput8.dll` copies the add-on back from the Workshop when normal Attila starts, the next 64-bit start moves it out again. If the move fails (for example, files in use by normal Attila), the loader logs it and leaves the files where they are.
- New game patch site: the call at 0x3473086 (5-byte `call rel32`) is redirected to a stub in the near-game page (16 bytes from `near_game_code`). The original target 0x32d01a0 is still called.
- The PC profile thread runs `CreateDXGIFactory1` and enumerates adapters at start, and later lists loaded modules. It changes nothing in the render path.

## 1.3.31 (2026-11-26) - two crash fixes, real call stacks and crash dumps

This release fixes two crashes found through crash reports, and makes crash logs much more useful: real call chains and a crash dump file.

### Changes
- Lua `string.sub` with a negative start (crash on the main menu or the campaign start screen with some mods). `UTILITYDLL::LUA::StringSubReplacement` (2de7e40), the game's own `string.sub` for Lua scripts, counts a negative end from the end but not a negative start. So `string.sub(s, -3)` became `substr(-4, length + 4)`. In 32-bit Attila this wraps to a wrong text. In 64-bit it reads 8 GB past the text: c0000005 at 4EA6D0 / 4EA720 in `CA::UniString::substr`. Its one call to `substr` (2de801e) now goes to `lua_sub_fix` (crash_guards.inc), which counts a negative start from the end as Lua 5.1 does. `ATTILA_NO_LUA_SUB_FIX=1` turns it off. The crash reports came with and without JGdaCaesar's engine fix, so the engine fix was not the cause.
- Text guard (crash_guards.inc). `CA::UniString::substr` (4ea5c0) is hooked. A position past the end now gives an empty text instead of a crash, and the caller is logged once per caller (up to 12). `ATTILA_NO_TEXT_GUARD=1` turns it off. Test switch: `ATTILA_TEST_SUBSTR=N` or `N,crash`.
- AI personality deal type out of range (crash when starting a new campaign with some mods). The `CAI_PERSONALITY_DEAL_GENERATION_COMPONENT` constructor (1b342a0) fills tables with room for 20 deal generator types (0..19). It uses each row's type as an index without a check. A row with type 20 wrote 4 bytes past the object, onto the saved rbx of `CAI_PERSONALITY`'s constructor (crash at 18F1334). In 32-bit Attila those bytes land on something harmless. Now a row with a type outside 0..19 is skipped and logged (`patch_deal_generation`). `ATTILA_NO_DEAL_GEN_FIX=1` turns it off. Test switch: `ATTILA_TEST_DEAL_GEN_LIMIT`.
- Real call chains in crash logs (`crash_report` in loader.c). Before, every stack value that pointed into code was listed, and most of them were data. Now only values right after a call instruction are listed (`after_call`, `code_readable`), newest first, up to 32 from 4000 stack slots. The list is headed "call chain (newest first):". A crash outside attila64.exe is now named with `loader_where`. The game's function names are not in the release. A developer tool (`tools\name_crash.ps1`) adds them to a log.
- Crash dump (`crash_write_dump` in loader.c). After a crash, `attila64_crash.dmp` is written in the attila64 folder, and the one before is kept as `attila64_crash_previous.dmp`. It is written by its own thread with `MiniDumpWriteDump` (dbghelp.dll is loaded only then): indirectly referenced memory, thread info, unloaded modules. The Linux game's stacks are not where Windows expects them, so 256 KB from the crashed thread's stack pointer are added by hand. `ATTILA_NO_CRASH_DUMP=1` turns it off.
- Crash reports can now send the dump. After the player says Yes to the report, a second question asks whether to send the dump too. The dump is a copy of game memory and cannot be cleaned of names, so it is never sent without that second Yes. The dump is offered only if it was written within 2 minutes of the log, is at most 8 MB, and log plus dump are at most 9.5 MB (`crash_dump_for_log` in crash_send.inc). The message then says "(with crash dump)".

### For plugin and mod developers
- New game patch sites (all in crash_guards.inc, applied by `patch_crash_guards` from `apply_game_patches`):
  - `hook_entry` at 0x4ea5c0 (`CA::UniString::substr`, 5 bytes: `push rbp; push r15; push r14`).
  - The call at 0x2de801e is redirected to a 16-byte stub (it jumps to `lua_sub_fix`, which then goes through the substr guard).
  - The 10 bytes at 0x1b343d8 are replaced by a `jmp` to a 64-byte stub plus 5 NOPs (type check, jumps back to 0x1b343e2, or to the next row at 0x1b3445e).
  - `hook_entry` at 0x1b342a0 (deal generation constructor, 5 bytes).
  A plugin that patches these bytes will now find them changed, and the loader's own check will skip its patch ("game code differs") if a plugin changed them first.
- Exception handling: `crash_report` now also writes the dump and ends with `ExitProcess(4)` as before. The files `attila64_crash.dmp` and `attila64_crash_previous.dmp` are new in the attila64 folder.

## 1.3.30 (2026-11-26) - Insert camera as in normal Attila, Steam ID scrubbing, read-error message

The battle context camera key now works the way normal Windows Attila does it. This release also removes Steam IDs from logs and crash reports, and explains "file could not be read" crashes.

### Changes
- Context camera key (Insert, Options > Controls "context_camera") rewritten in context_camera.inc. Normal Windows Attila handles the key in two places, and both are now put back in the Linux game:
  - A: `EMPIREBATTLE::BATTLE_CONTROLLER::on_shortcut` (9a86f0), at 9a8d24.
  - B: `EMPIREBATTLE::FIRST_PERSON_MANAGER::on_shortcut` (7ce870), at 7ce97d, for the key while first person is on.
  When `BATTLE_CAMERA_MANAGER::is_functional(0x80)` allows special cameras, the key calls the game's own `CINEMATIC_EVENT_VIEW::toggle_enabled` (e30610). This is the same call as the game's cinematic mode button and Escape, so nothing new happens in multiplayer. Soldiers get the cinematic camera, artillery gets the aiming view. Without a view, it falls back to `toggle_context_camera` (b7e360) or `toggle_first_person` (b80db0).
- Attila Cam (`plugins\attila_cam64.dll`): when a plugin has opened first person to ordinary men (the bytes at 7d0347 are no longer `74 1A`), A keeps the plain context camera and B leaves the key alone. This check is done each time the key is pressed.
- `ATTILA_CONTEXT_CAMERA_DIRECT=1` gives the 1.3.29 behaviour (always the plain context camera; B is not placed). `ATTILA_NO_CONTEXT_CAMERA_FIX=1` leaves the key as the Linux game has it (does nothing).
- Steam ID and name no longer in the log in full (`w_set_data` in shim_steam.c). For the player's own lobby advert, `steam_id` is logged as its last 4 digits and its length, and `host` (the Steam name) only as its length.
- Crash reports now replace every Steam ID (17 digits starting with 7656119) with `<steam id>` (`crash_drop_steam_ids` in crash_send.inc).
- Read-error crash message (`crash_note_unreadable_file` in loader.c). For c0000006 ("in-page error": Windows could not read part of a memory-mapped file from the drive, seen at 524920 `CA::UnbufferedFileIn::touch` reading pack data), the log now names the file (from "steamapps" on, or only the file name) with `K32GetMappedFileNameA`. It also gives Windows' reason code in words and tells the player what to do (verify game files, resubscribe the mod, check the drive, or add memory / page file). The CRASH line itself is unchanged.
- Parallel battle effects: new settings.txt key `parallel_effects=on` (`settings_parallel_effects` in picker.c, read in `loader_game_settings` in game_fixes.inc). `ATTILA_PARALLEL_VFX` still wins over it, and now `0`, `off` or an empty value mean off. Before, any value turned it on. It is still off by default.
- `inner_drive_path` (shim_stdio.c) now returns an empty path at once. Before, its loop started reading at `p + 1` past the end.

### For plugin and mod developers
- Moved patch site: 1.3.29's `hook_entry` on the entry of `BATTLE_CONTROLLER::on_shortcut` (0x9a86f0, 6 bytes) is gone. The function entry is the game's own again. Instead, two 5-byte `jmp`s replace the `mov esi,37d8d6e` at 0x9a8d24 and at 0x7ce97d (B is not placed with `ATTILA_CONTEXT_CAMERA_DIRECT=1`). The stubs (96 bytes from `near_game_code`) jump out to 0x9a8f81 / 0x7cec8c when the key was handled. The patch first checks the bytes at 9a8d24, 7ce97d, 9a8f81, 7cec8c, e30610, b7c860, b80db0, b7e360 and 7d033f, and skips itself if any of them differ.
- The loader reads the two bytes at 0x7d0347 (`FIRST_PERSON_MANAGER::valid`) on every key press to see whether a plugin opened first person to men. A plugin that changes these bytes changes how Insert behaves.
- Logs and crash reports no longer contain the Steam ID or Steam name from the lobby advert.

## 1.3.29 (2026-11-26) - frame cap before Present, battle context camera key

The frame cap now waits before the frame is shown, which gives evenly spaced frames (fix by ATN / RenoDX). The battle context camera key also works for the first time in 64-bit.

### Changes
- Frame cap moved in front of the frame hand-over. Fix by ATN / RenoDX. The cap code moved out of `SDL_GL_SwapWindow` into a new function `frame_cap_wait()` in shim_gfx.c. It is called just before Present in `dx_present` (dx11_device.inc, after `dx_gpu_frame_end`) and just before the real swap on OpenGL. Waiting after the hand-over had made a short frame follow every long one. The cap is still counted from the last hand-over with a high-resolution waitable timer: `ATTILA_FPS_CAP` (default 240, `0` = off).
- Battle context camera key (Insert). The Linux game's `BATTLE_CONTROLLER::on_shortcut` (9a86f0) never compares the shortcut name with "context_camera", so the key did nothing. New context_camera.inc hooks that function and, after the game's own handling, calls `BATTLE_CAMERA_MANAGER::toggle_context_camera` (b7e360) on the battle's camera manager (battle env `[this+0x38] + 0x64408`). It only changes the camera, so multiplayer stays in step. `ATTILA_NO_CONTEXT_CAMERA_FIX=1` turns it off. (Changed again in 1.3.30.)

### For plugin and mod developers
- **Frame timing:** on DirectX 11, the cap wait now happens inside `dx_present` before `IDXGISwapChain::Present`. Before, it happened after, in `SDL_GL_SwapWindow`, which on the DirectX 11 route is now called only for the frame count. On OpenGL, it now happens before the real `SDL_GL_SwapWindow`. Present / frame-generation / upscaling hooks (FrameBridge, RenoDX) now see the wait before Present, not after.
- `frame_cap_wait()` is a non-static function in shim_gfx.c. It is not part of the plugin API.
- New game patch site: `hook_entry` at 0x9a86f0 (`BATTLE_CONTROLLER::on_shortcut`, 6 bytes `push rbp; push r15; push r14; push rbx`). This was removed again in 1.3.30.

## 1.3.28 (2026-11-26) - crash reports

After a crash, the mod list can now send the crash log to the developer, but only if the player says Yes.

### Changes
- New crash_send.inc (included by picker.c). When the mod list window opens and the last game's log has a CRASH line, the player is asked once: "Send the crash report?". The question is posted as `WM_CRASH_OFFER` after the window is made. Nothing is sent without Yes. Each crash is asked about only once: the log's size and time are kept in `crash_report_asked.txt` next to attila64.exe.
- What is sent (an HTTPS POST to the developer's Discord channel, with WinHTTP):
  - A message text: "Crash report: 64-bit Attila <version>" and the first CRASH line of the log (at most 150 characters). Discord mentions are switched off.
  - The last game's log as the file `attila64_log.txt` (up to 9 MB). The player's profile folder is replaced with `<profile>`, and any other `Users\<name>\` path becomes `Users\<user>\`. From 1.3.30, Steam IDs are replaced with `<steam id>`. From 1.3.31, an optional crash dump can be sent after a second question.
  - The log holds what the game did, the graphics card, the Windows version and the mod list. From 1.3.32 it also holds the PC profile.
- The player sees a message saying whether it was sent. If not sent, it suggests "Copy last log".
- settings.txt `crash_reports=off`: never ask. In hidden test runs (`ATTILA_TEST_HIDDEN`), it is not asked unless `ATTILA_CRASH_SEND_TEST` is set (`=auto` is for tests: Yes is taken and the answer goes to `crash_send_test.txt`).
- build.ps1 now links `winhttp`.

### For plugin and mod developers
- Nothing in the game process changes: the question and the upload happen in the mod list window, before the game starts.
- If your plugin writes into the 64-bit log, the player may now send those lines to the developer.

## 1.3.27 (2026-11-26) - three fixes by ATN / RenoDX: crash filter, hook layout, overlays with HDR

This release has three fixes by ATN / RenoDX. Exceptions that a Windows DLL handles itself no longer end the game, plugin hooks of 12-15 bytes work, and the frame-rate number and Discord button work with HDR swap chains.

### Changes
- The crash filter now treats only faults in the game or loader code as crashes (`crash_filter` in loader.c, by ATN / RenoDX). Before, any exception the filter saw ended the game, even one that a driver, an overlay such as ReShade, or a system DLL (for example RPC error 6ba) catches itself a moment later. Now an exception whose address is not in our own code (`in_own_code`: attila64.exe, the near-game page, or the Linux modules) is left to the DLL's own handler and logged (up to 5 times, `log_left_alone`). Stack overflow (C00000FD) and heap damage (C0000374) are still reported at once. The report itself moved into `crash_report`.
- Uncaught exceptions are still reported. The Linux code has no Windows unwind data, so Windows' search for a handler got lost in its frames, and an uncaught DLL exception ended the game with no report. New `cover_frames` registers one function table entry (`RtlAddFunctionTable`) per Linux module, covering its whole range, with `game_frame_handler` as the handler. The search now stops at the first game frame and reports the crash there. `unhandled_note` now also calls `crash_report`.
- Plugin hooks of 12-15 bytes work (`hook_entry` in interface.inc, by ATN / RenoDX). The 32-byte block from `near_game_code` used to hold the trampoline first and the 12-byte way in at +16. With 12-15 copied bytes, the way in overwrote the trampoline's jump back. The way in is now first (+0) and the trampoline follows at +12.
- Frame-rate number and Discord button with HDR swap chains (`dx_present` in dx11_device.inc, by ATN / RenoDX). Both are drawn as 8-bit R8G8B8A8 pixels. When another program (an HDR mod) has made the window's buffer 16-bit float or 10-bit, they showed as a patch of garbage. Now, if the swap chain buffer is `DXGI_FORMAT_R8G8B8A8_UNORM`, they go onto it after the copy as before. If not, they go into the game's own 8-bit picture just before it is copied to the window, when that picture is 8-bit and the same size. If neither works, they are left out and a log line says so once. Test pictures are taken before this, so they stay clean (new `dx_overlays`).
- New settings.txt key `discord_button=off` (`settings_discord_button_off` in picker.c, read in `menu_button_visible`): hides the 64-bit Attila Discord button on the main menu, like `ATTILA_NO_DISCORD_BUTTON=1`.

### For plugin and mod developers
- **Plugin hooks:** `plugin_hook` / `hook_entry` with a length of 12 to 15 bytes now works. Before 1.3.27, such a hook had its jump back overwritten. The layout of the 32-byte block changed: way in at +0, trampoline (copied bytes + 5-byte `jmp` back) at +12. `ATTILA64_PLUGIN_API` stays 1.
- **Exception handling:** an exception raised in a Windows DLL (this includes plugin DLLs such as perffix64.dll, because they are not in attila64.exe, the near-game page or the Linux modules) is no longer reported at first chance. It is reported only if nobody catches it and it reaches a game frame or the unhandled-exception filter. Each Linux module now has a function table entry from `RtlAddFunctionTable` over its whole range. Its unwind info and a jump to the handler are in a page allocated above the module, within 4 GB.
- `near_game_code`'s page is now a file-level variable (`near_page` in game_fixes.inc), so the crash filter can count code there (trampolines, patches) as our own.
- **Present path:** with a non-8-bit swap chain (RenoDX HDR), the frame-rate number and Discord button are now written into the game's back buffer just before `CopyResource` to the swap chain buffer, not onto the swap chain buffer. With an 8-bit swap chain, nothing changes.

## 1.3.26 (2026-11-26) - campaign and battle film panels fit the screen
Full-screen campaign cutscenes (for example the 1212AD crusade and faction intro films) and the battle film panel were too big on screens above 1080 pixels high. This release makes them fit.

### Changes
- Campaign full-screen film panel cut off. The panel (`UIDLL::MOVIE_EVENT_PANEL`, "event_movie_panel", started by the script `cm:play_movie_in_ui`) moves its film box to 0,0 and sizes it to the unscaled screen size. With the interface zoom (any screen above 1080 high) the film was zoom times too big: pushed down, black band on top, cut off at the right and bottom. This was worst on 3440x1440. The battle film panel (`UIDLL::BattleMoviePanel::OnCreate`) works out its rectangle the same way. Fix in `interface.inc` `ui_restricted_size`: calls that return to `0x2aeffc5`, `0x2aeffe9` (campaign) and `0x2c745a0` (battle) now get the laid-out size, the same as the five film callers fixed in 1.3.10. 1.3.10 missed these three because they load the function into a register before the call.
- New log line (first 4 times): "film panel size: laid-out size given to the campaign / battle film panel".

### For plugin and mod developers
- `ui_restricted_size` (the hook on the engine's screen size query) now returns the laid-out size, not the unscaled size, to the three return addresses above. This matters if your plugin also hooks or reads that query, or draws over film panels.
- Plugin API unchanged (`ATTILA64_PLUGIN_API` 1).

## 1.3.25 (2026-11-26) - saved keys load, smooth films, Graphics choice
This release fixes saved key sets that did not load and stutter in the intro films. The main menu background film now plays at full speed. The mod list also gets a DirectX 11 / OpenGL choice again.

### Changes
- Saved key sets did not load. Saving always worked. But at the next start (and on Load Keys), `SHORTCUT_HANDLER::load_keys` (`0x1cccd00`) opens `<home>/custom_keys/<name>.keys.xml` through the pack file system (`CA::FileIn::open` `0x4e60a0`). That code only takes a path as a full path when it starts with `/`. So a Windows path `c:/users/...` was glued behind each game folder (`<data folder>/c:/users/...`). The file was never found and the default keys came back. Fix in `shim_stdio.c` `inner_drive_path`: a path with `/<letter>:/` after its start is cut to the part from the drive letter on. Used by the `open` and `__xstat` shims. Log line "full path inside a game folder path: the full path used" (first 4). `ATTILA_NO_INNER_PATH_FIX=1` turns it off.
- Films with sound stuttered (logos, intro). The film frame comes from the Wwise play position (`CAkPositionRepository::GetCurrPosition` `0x368d760`), and Wwise adds "`clock()` since the last sound buffer" to it. `clock()` is processor time, which Windows counts in about 15.6 ms steps. So the film position jumped 15-20 ms at a time and went back about 10 ms at each new buffer. Fix in `shim_sys.c` `clock()`: callers inside the Wwise code range (`0x363a000`-`0x3740000`, all 17 of its `clock()` calls) get real time (QueryPerformanceCounter). The game's other `clock()` callers (Lua `os.clock`, debug menu) still get processor time. Changing only the position repository's calls gave a half-speed film, because Wwise passes these time stamps between its parts. `ATTILA_NO_FILM_CLOCK_FIX=1` turns it off. Log line "film clock: the sound engine counts real time".
- Looping films without sound (the main menu background film) ran at half speed. This is a bug in the game's own code, so probably also on Linux. In `VP8::MOVIE::calculate_target_frame` (`0x342c4a0`), a negative "wanted minus decoded" frame count on a looping film is taken as a wrap-around, and the film length is added. The one-thread decoder (`get_frame_st`) is always one frame ahead, so every other call saw about 1649 "frames behind" and stopped the film clock. Fix: two new entries in `game_fixes.inc` `g_game_patches`, "looping films at full speed", at `0x342c71c` (no sound) and `0x342c5ff` (with sound). Both change `add eax,[rbx+0x14]` to a 3-byte nop. A real lag of more than 15 frames still stops the clock. The with-sound site was not exercised in tests.
- Mod list: a "Graphics" box (DirectX 11 / OpenGL) is always shown (`picker.c`, `ID_GFX`). It replaces the "Use DirectX 11 again" button. DirectX 11 writes `graphics=dx11` and OpenGL writes the new value `graphics=opengl_chosen` in settings.txt. `settings_graphics_dx11()` now returns false for `opengl_fallback` and for `opengl_chosen`. The old plain `graphics=opengl` is still not read (it means DirectX 11). The "DirectX 11 did not start last time" message now says to set "Graphics" back to DirectX 11.
- Test only: `film_trace.inc` (new, `ATTILA_FILM_TRACE=1`) hooks `0x342c4a0` and logs how evenly each film's frames advance. `ATTILA_TEST_SKIP_MOVIES=0` now means films play (`effects_and_tests.inc`).

### For plugin and mod developers
- New game patch sites: `0x342c71c` and `0x342c5ff` (3 bytes each, in `VP8::MOVIE::calculate_target_frame`). Test-only hook on the entry of `0x342c4a0` when `ATTILA_FILM_TRACE` is set.
- Frame timing: `clock()` now returns real time (not processor time) when it is called from the Wwise range `0x363a000`-`0x3740000`. Other callers are unchanged.
- The `open` and `__xstat` shims now fix paths with a drive letter inside them (see above). This affects any file the game opens that way, not only key files.
- settings.txt key `graphics` has a new value, `opengl_chosen`. If your plugin or add-on reads `graphics` to detect the OpenGL route, check for it as well as `opengl_fallback`.
- Plugin API unchanged (`ATTILA64_PLUGIN_API` 1).

## 1.3.24 (2026-11-26) - worker threads sleep the game's way by default
A one-setting change: the "fast task waits" option is now off unless you turn it on.

### Changes
- Worker threads no longer stay awake before sleeping by default, and the task queue is no longer peeked without its lock by default (`task_spin.inc` and the queue peek in `task_wait.inc`). Reason, from measurements on 2026-11-11: no fps gain (133.4 off, 132.8 / 133.5 on), but processor use went from 338% to 535%. That can slow PCs with few cores and laptops. `picker.c` `settings_fast_task_waits()` now returns true only for `task_waits=fast`. No line, or `task_waits=game`, now means the game's own way. Before 1.3.24 it was on unless `task_waits=game`. `ATTILA_TASK_SPIN_US=n` and `ATTILA_TASK_SPIN_OFF=1` are unchanged. SWITCHES.md is updated.

### For plugin and mod developers
- Threading: game worker threads now sleep at once by default, as in the unpatched game. If you measured CPU use or frame times on 1.3.23 or older, they may differ now. Set `task_waits=fast` in settings.txt to get the old behaviour.
- Plugin API unchanged (`ATTILA64_PLUGIN_API` 1).

## 1.3.23 (2026-11-26) - no freeze when changing faction or army
In the multiplayer battle lobby and in Custom Battle, every faction or army change froze the game for about 0.4-0.6 s, more with many Workshop mods. This release removes the freeze.

### Changes
- Cause 1: when a file is missing, `CA::open_file` and `CA::FileInf::stat` search again with a case-blind pattern (`Filesystem::globsearch`). This is the Linux game's way around case-sensitive disks. On Windows names already match in any case, so this search never finds anything. `VFS_IMPL::latest_entry` asks for each interface picture in every mod folder, so one faction change made about 20,000 such searches. Fix: two new entries in `game_fixes.inc` `g_game_patches`, "no case-blind second search for missing files", at `0x4ee20a` (open) and `0x4ece5a` (stat). Each replaces a 5-byte call with `xor eax,eax` + 3-byte nop ("not found").
- Cause 2: the game looks for every interface picture as a loose file in every subscribed Workshop item's folder (`steamapps/workshop/content/325610/<item>/ui/...`). Measured: about 41,600 stat/open/glob calls for one faction change with 208 items. Fix in `shim_stdio.c` `ws_dir_missing`: the first folder level under a Workshop item is checked once and remembered (hash table, up to 7000 entries). A file under a folder that is not there gets "not there" at once. This applies only to read-only opens and stat, and only to Workshop item folders. A "not there" answer is believed for 5 seconds, then checked again, so items Steam downloads while the game runs are still found. Used in the `fopen`/`fopen64` (read modes), `open` (read-only, no create) and `__xstat` shims. Log line "Workshop folder check: ..." (first 3). `ATTILA_NO_WS_DIR_CACHE=1` = the old way.
- Test only: `ATTILA_FILE_TRACE=1` logs every stat / open / fopen / glob / scandir with its time in microseconds (`shim_stdio.c`).

### For plugin and mod developers
- New game patch sites: `0x4ee20a` and `0x4ece5a` (5 bytes each). A missing file is no longer searched for again case-blind. If a mod relied on a wrong-case file name being found by that second search, note that Windows file names are not case-sensitive anyway.
- File access: reads of loose files under a Workshop item's first-level folder that does not exist now fail at once, without asking the disk. If your tool creates such a folder while the game runs, it is seen within 5 seconds.
- Plugin API unchanged (`ATTILA64_PLUGIN_API` 1).

## 1.3.22 (2026-11-25) - uninstaller and a button for normal Attila's launcher
1.3.21 was never released. 1.3.22 is built on 1.3.20, so this section lists the changes against 1.3.20. It adds a real uninstaller, a way to reach CA's own launcher (Mod Manager, Workshop upload), and a "Move to top" button in the mod list.

### Changes
- Uninstaller (`installer.c`). Install and self-update now copy the installer into the folder as "Uninstall 64-bit Attila.exe" (`put_uninstaller`). They also list 64-bit Attila in Windows Settings > Apps, for the current user only (`HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\64-bit Attila`). Uninstall removes that key. Started under that name, the exe copies itself to %TEMP% and runs from there with the new `uninstall-temp` argument (`uninstall_from_temp`), so the whole folder can be deleted. The temp copy deletes itself afterwards through a hidden `cmd` that retries for up to two minutes. Uninstall now asks before it removes anything. Reason: a player deleted the folder by hand, and Steam's Play button then failed with "cannot find the path specified".
- Deleted folder, Play button still set (`installer.c`, `play_button_is_ours`). If attila64.exe is missing but a Steam account's Launch Options (`localconfig.vdf`) still name it, the installer asks: 1 = install again, 2 = put Steam's Play button back to normal Attila (it also removes the Apps entry and the desktop icon). `ATTILA_INSTALL_ANSWER` gives the answer in installer test mode.
- "Normal Attila launcher (Mod Manager, Workshop upload)" button in the mod list (`picker.c`, `start_ca_launcher`, `ID_LAUNCHER`). With our Launch Options, Steam's Play starts us, so CA's Mod Manager (needed to upload Attila mods to the Workshop) could not be reached. The button saves the mod list and starts CA's launcher with `SteamAppId`/`SteamGameId` 325610. attila64.exe then waits for the launcher to close, so Steam still shows Attila as running, and the 64-bit game is not started. The launcher path comes from Steam's `%command%` argument (`loader.c` keeps any argument ending in `launcher.exe` in `g_ca_launcher`). If there is none, `launcher\launcher.exe` in the game folder is used.
- "Move to top" button in the mod list (`picker.c`, `move_selected_to_top`, `ID_TOP`). The other buttons in that column are a little smaller to make room.
- The installer's closing text now explains how to reach the normal launcher and how to uninstall, and says not to just delete the folder.
- New test scripts: `test_uninstaller.ps1`, `test_launcher_button.ps1`.

### For plugin and mod developers
- Files: the installer now writes "Uninstall 64-bit Attila.exe" into the attila64 folder, at install and at every self-update. Uninstall deletes it too. Uninstall still removes only an empty `plugins` folder, so other people's plugins stay.
- Load path: when the "Normal Attila launcher" button is used, attila64.exe exits without loading the game, so no plugins are loaded in that run.
- Plugin API unchanged (`ATTILA64_PLUGIN_API` 1).

## 1.3.20 (2026-11-24) - crash with a game controller connected
This release fixes a crash at the main menu when a game controller, gamepad or joystick (anything Steam Input sees as a controller) is connected. It is the first version delivered to players on 1.3.19 through the self-update.

### Changes
- Two Steam controller calls crashed: `SteamController003` slot 9 (`GetDigitalActionData`, 2-byte result) and slot 12 (`GetAnalogActionData`, 13-byte result). Both return a struct. Windows (MSVC) member functions return every struct through a hidden pointer. Linux returns structs of up to 16 bytes in rax/rdx. Without a bridge, Steam took the controller handle for the result address and crashed. Fix in `shim_steam.c`: both slots added to `g_rules` as `K_RETCLASS`. The slot order was checked against the game's own calls.
- `sysv_to_ms_retclass` (the bridge for methods that return a class) now reads back 16 bytes: offset 0x30 into rax and 0x38 into rdx. Before, it read only 8 bytes into rax.

### For plugin and mod developers
- `sysv_to_ms_retclass` now also sets rdx. This affects every `K_RETCLASS` Steam slot (matchmaking, game server and the rest). Results of up to 16 bytes now come back whole.
- Plugin API unchanged (`ATTILA64_PLUGIN_API` 1).

## 1.3.19 (2026-11-24) - updates itself from the Workshop
64-bit Attila now updates itself from its own Workshop item. It uses the same rules as JGdaCaesar's perffix64.dll update.

### Changes
- New file `self_update.inc`, called from `main()` in `loader.c` before the start window opens. It looks at `Install 64-bit Attila.exe` in the 64-bit Attila Workshop item folder (`steamapps\workshop\content\325610\<item>\`). When that installer's file version is higher than this build's `APP_VERSION_NUM`, the loader runs it as `"Install 64-bit Attila.exe" self-update "<attila64 folder>"`, with no console window. While it runs, a small "Updating 64-bit Attila from X to Y..." box shows. The loader waits up to 120 s.
- Rules: only a newer version is used, so a newer dev or hand-installed build is never replaced by an older one. Only a finished download is used (the same `pe_complete()` check as for perffix64.dll: the file must be as long as its PE header says). Nothing happens when `attila64\update_off.txt` exists (the same switch that stops the perffix64.dll update). If anything fails, the current version starts as it is.
- After a good update, the loader checks that `attila64.exe` on disk is a different file (file ID), then starts the new `attila64.exe` with the same command line and `ATTILA64_UPDATED=<old version>`. The old process waits for it and returns its exit code, so Steam still sees the game running. If the new exe cannot be started, a message box asks the player to start the game again.
- The installer (`installer.c`) has a new `self-update <folder>` mode (`self_update_files`). It asks no questions and only touches 64-bit Attila's own files. The game file, desktop icon, Steam Play button, `settings.txt` and `mod_list.txt` are not touched. Only files that differ are written, first as `<name>.new`. Then they are moved in, with `attila64.exe` last. A file in use (the running `attila64.exe`) is renamed to `<name>.old` first. Exit code 0 means updated.
- The result goes into the log once it is open (a `self-update: ...` line after the version line). The installer's own output goes to `self_update_log.txt`.
- New switches: `ATTILA_NO_SELF_UPDATE=1` turns it off. `ATTILA_TEST_SELF_UPDATE=1` turns it on in a development build. Only release builds update by default (`APP_SELF_UPDATE` in `version.h`). `ATTILA_TEST_UPDATE_FROM=<installer>` uses another installer file. `ATTILA_TEST_SELF_UPDATE_ONLY` writes `self_update_test.txt` and stops. The last three are for tests (`test_self_update.ps1`).
- Uninstall now also removes `self_update_log.txt`, `attila64.exe.old`, `SDL2.dll.old`, `steam_api64.dll.old` and `update_off.txt`.

### For plugin and mod developers
- Start-up order changed: the self-update check runs first in `main()`, before the start window, the log and plugin loading. After an update, the game runs in a second `attila64.exe` process (child of the first). The first process only waits.
- New files the loader writes or deletes in the attila64 folder: `self_update_log.txt`, and `attila64.exe.old` / `SDL2.dll.old` / `steam_api64.dll.old` (deleted at every start).
- The self-update can create the folders `packs`, `plugins`, `packs\1212` and `themes`. It only writes the installer's own files there. It does not touch other DLLs in `plugins`.
- `update_off.txt` now stops both the loader's self-update and the perffix64.dll update.
- `ATTILA64_UPDATED` is cleared before the game starts, so plugins will not see it.
- Plugin interface unchanged (`ATTILA64_PLUGIN_API` 1).

## 1.3.18 (2026-11-24) - unit stats in two columns
The unit stats panel now shows the stats in two columns. Melee stats are on the left.

### Changes
- `multiplayer.inc`: new `unit_stats_two_columns()`. It runs inside the existing unit stats fix (`unit_stats_one_column`). That fix is a hook on `UnitInformationPanelCallback::InitStatistics` (0x2a2b6f0), redirected at its two call sites 0x2a2a4b4 (InitialisePanel) and 0x2a2d5c7 (RefreshStats). It also runs after the panel's OnUpdatePulse (vtable hook at 0x3b0c2b0, original 0x2a2dcd0).
- How it works: each stat label is read (`stat_name` state text). Labels with "melee", "charge" or "cavalry" go to the left column, the others to the right. Each stat becomes a cell with the name and value on one line and the bar stretched under them. The two columns use 1.3 times the list's width. When the right column is 2 or more stats longer, its last stats move to the bottom of the left column.
- The cells stay above the next visible part under the list, such as the ability icons (`us_room_below`).
- The game sets bar fill widths again on hover and compare, in its own 90-wide scale. The code remembers every width it wrote (`g_us_mark`, 1024 entries) and only scales the widths the game wrote.
- It falls back to the old single column when a label cannot be read, when the text is not English (no match), when all stats are on one side, or when a cell would be under 28 units high. Display only.
- New switch: `ATTILA_STATS_ONE_COLUMN=1` keeps the old single column.
- `PLUGINS.md`, `SWITCHES.md` and `attila64.manifest` (UTF-8 active code page) are now saved with the release source. They are not new files in the loader.

### For plugin and mod developers
- If your plugin or UI mod changes the unit information panel (`dynamic_stats` list at +308h, rows `stat_name` / `dy_value` / `bar_frame`), this release now resizes and moves those rows and the children of their bars (`frame`, `bar_border`, `icon_compare`, `bar_base`, `bar_mod`). Use `ATTILA_STATS_ONE_COLUMN=1` to compare against the old layout.
- No new patch sites. The hooks at 0x2a2a4b4 / 0x2a2d5c7 / vtable 0x3b0c2b0 were already there before.
- Plugin interface unchanged (`ATTILA64_PLUGIN_API` 1).

## 1.3.17 (2026-11-24) - changing the resolution in the game (DirectX 11)
Fixes a white screen with DirectX 11 after changing the resolution or window mode in the game's options.

### Changes
- Cause: the game applies a new mode with `WS_ENGINE_IMP::set_graphics_options` -> `res_destroy(2)` -> `pr_mode_change` -> `WS_ENGINE_SCREEN::mode_change` -> `destroy_device`, then `pr_create_device` -> `create_device`. On OpenGL the context stays, so all textures, buffers and shaders stay valid. The DirectX 11 route released the whole device at `destroy_device` and made a new one. Every resource the game still held then pointed into the old device: white screen, and a crash in one test.
- Fix in `dx11_device.inc`: device vtable slot +0x100 (`destroy_device`) now calls the new `dx_destroy_screen()`. It releases only the game's back-buffer objects (device fields +0x1618 / +0x1620 / +0x1628, and clears +0x1630) and our back-buffer view. It then clears all bound state (`dx_unbind_all`).
- `dx_create_device()` keeps the existing device (new `dx_resize_screen()`). For the same window it calls `ResizeBuffers` on the swap chain. For a new window it makes a new flip-discard swap chain. Then it makes the game's back-buffer objects again at the new size and resets the scissor rectangle.
- Slot +0x128 (`destroy`, at game end) still releases everything.
- New log lines: "DirectX 11: screen let go for a new mode (device kept)" and "DirectX 11: screen now WxH (device kept)".
- Known limit: the sharp-interface on/off choice is made only once per run. If you start at a small size and switch to 4K, the sharp interface needs a restart.

### For plugin and mod developers
- Render/present path: the D3D11 device, the context and all game resources now live through a resolution or window-mode change. Only the swap chain buffers (and maybe the swap chain itself, if the window changed) and the back-buffer view are replaced. If you hold the swap chain's back buffer or RTV (RenoDX, FrameBridge, overlays), let it go on a resize and get it again. `ClearState` is called on the immediate context at that point.
- Plugin interface unchanged (`ATTILA64_PLUGIN_API` 1).

## 1.3.16 (2026-11-24) - sharp interface from a Workshop add-on
The sharp 4K interface pictures now come from a separate Workshop item, "64-bit Attila: Sharp 4K Interface" (about 1 GB). The loader finds it by itself.

### Changes
- `ui_x2.inc`: new `x2_find_dir()`. It looks for `<exe folder>\ui4k\` first (hand-installed, tests). If that is not there, it scans every subscribed Attila Workshop item (`steamapps\workshop\content\325610\*`) for `ui4k\attila64_sharp_ui.txt`, so the item number is not built in. The pictures are read where Steam puts them. Nothing is copied into the game folder.
- On/off rules are unchanged: DirectX 11 only, on when the interface zoom is 1.5 or more (for example "Auto" on a 3840x2160 screen). `ATTILA_UI_X2=0/1` forces it off or on. Without the pictures it stays off.
- The log line now says where the pictures came from, or that neither `ui4k` nor the Workshop item was found.
- New `item_sharp_ui.vdf` for uploading the add-on (10,080 pictures).

### For plugin and mod developers
- The sharp interface keeps the interface upload texture and the cache pages at twice their size on the card. A mod's own interface picture under the same name is kept as it is (only pictures that match the game's loaded picture are swapped). This is not new, but more players will now have it on.
- Plugin interface unchanged (`ATTILA64_PLUGIN_API` 1).

## 1.3.15 (2026-11-24) - engine performance fix from the Workshop, themes for the mod list window
JGdaCaesar's engine fix is no longer bundled. The loader now takes it from his Workshop item and keeps it up to date. The mod list window also gets themes. This is the first saved release that includes the source.

### Changes
- The installer no longer ships `perffix64.dll` (the comment in `picker.c` says its licence does not allow bundling). Players subscribe to JGdaCaesar's "Attila Engine Performance Fix" on the Workshop. Uninstall still removes `plugins\perffix64.dll`, `perffix64.log`, `flagwave.log`, `flagwave_off.txt`, `plugins\perffix64.dll.old` and `plugins\perffix64.dll.new`.
- `plugins.inc`, `perffix_prepare()` runs at the start of `plugins_load()` when the "Engine performance fix" tick is on (`perf_fix=1`, the default). It compares `<attila64>\plugins\perffix64.dll` with the Workshop copy `steamapps\workshop\content\325610\<his item>\perffix64.dll` (`perffix_path()` in `picker.c`).
- Update rules (`perffix_take_workshop()`), the same as his own `update.c` / `update64.c`, so the two never fight:
  - The version is the number after `AttilaCPUPerfFix-version:` in the first 2 MB of the DLL (`perffix_version()`, 7.5 = 750). A file without it is never taken.
  - Only a higher number is taken.
  - Only a finished download is taken (`pe_complete()`: the file is at least as long as the end of its last PE section). Steam writes the version mark early.
  - Nothing is copied when `attila64\update_off.txt` exists.
  - The copy goes to `perffix64.dll.new`, is checked again, then moved over `perffix64.dll`. Any failure keeps the DLL that is already there.
- Not subscribed: a `perffix64.dll` already in `plugins` (for example the one 1.3.14 installed) is used as it is. The log suggests subscribing.
- The waving-flags tick (`flag_wave=` in settings.txt) still becomes `attila64\flagwave_off.txt`, which the DLL reads.
- The FrameBridge add-on (`framebridge_prepare()`) is unchanged from 1.3.14. Its folder is copied from his Workshop item to `<game>\FrameBridge` when it is missing or its `version.txt` differs. The `addon=` line of its `settings.txt` follows the `upscaling=` tick.
- Themes in the mod list window (`picker.c`): a "Theme of this window" box with Plain grey, Dark, Stained glass, Islamic tiles and Mongol painting. It is saved as `theme=plain|dark|glass|islamic|mongol` in settings.txt. The pictures are installed to `<attila64>\themes\` (`glass.jpg`, `islamic.jpg`, `mongol.jpg`, `CREDITS.txt`) and drawn with GDI+. If a file is missing, glass and tiles are drawn by code and Mongol looks like Dark. Push buttons, the list and its header are owner-drawn or custom-drawn in a theme.

### For plugin and mod developers
- JGdaCaesar: the loader copies your Workshop `perffix64.dll` into `attila64\plugins` before plugins load. It follows your version mark and `update_off.txt` rules. It never replaces a newer DLL in `plugins` with an older Workshop one. It writes `perffix64.dll.new` only for a moment. Your DLL's own in-game update still works as before. If "Engine performance fix" is unticked, `perffix64.dll` is not copied and not loaded (log line "engine performance fix unticked in the start window, left out").
- Load order is unchanged: `plugins_load()` runs after the loader's own patches (`loader.c`). DLLs load in name order, up to 32. `attila64_plugin_game_started` is called from `game_fixes.inc` once the game settings have their start values.
- New files the installer writes: the `themes\` folder.
- Plugin interface: `ATTILA64_PLUGIN_API` is 1 (`attila64_plugin.h`). Note that `api->engine_version` is still the fixed text "1.2" in `plugins.inc`. It does not give the real loader version, so do not rely on it.

## 1.3.14 (2026-11-24) - engine performance fix bundled, upscaling add-on, waving flags for 1212AD
This release bundled JGdaCaesar's Engine Performance Fix. It also added start-window ticks for the fix, for his upscaling/frame-generation add-on and for waving 1212AD flags. Only the binaries of this release were saved, so these notes come from the player notes, the binaries' strings and the 1.3.15 source.

### Changes
- The installer shipped `plugins\perffix64.dll` (version mark 750 = 7.5) inside 64-bit Attila. Unlike 1.3.15, the loader of this release had no version or download checks for the Workshop copy. When the player was subscribed, it copied his Workshop `perffix64.dll` into `plugins`.
- New mod list ticks: "Engine performance fix" (`perf_fix=1|0` in settings.txt; unticked means `perffix64.dll` is not loaded) and "Waving flags (only for 1212AD)" (`flag_wave=1|0`; unticked means the loader writes `attila64\flagwave_off.txt` for the DLL, ticked means it deletes that file).
- Upscaling and frame generation: JGdaCaesar's "Upscaling and Framegen for Attila" (FrameBridge, run by perffix64.dll; NVIDIA DLSS or AMD FSR, F8 for its settings). The loader copies the `FrameBridge` folder from his Workshop item to `<game>\FrameBridge` when it is missing or its `version.txt` differs. The new tick "Upscaling and frame generation" (`upscaling=1|0`) sets the `addon=on|off` line in `FrameBridge\settings.txt`, so it also switches the add-on for normal Attila. The player's other FrameBridge settings are kept.
- Waving flags for 1212AD: the installer ships `packs\1212\!!1212ad_flag_wave.pack`, which holds 1212AD's banner and pennant models with the cloth data of JGdaCaesar's Flag Wave. The loader puts it first in `used_mods_64.txt` (its own `add_working_directory` and `mod` lines), so it wins over 1212's own copies. It only does this when the mod list has a 1212 pack and both the engine fix and flags ticks are on. The DLL waves flags only when a loaded pack carries `flagwave\flag_wave.txt`.

### For plugin and mod developers
- JGdaCaesar: `perffix64.dll` is loaded through the normal plugin path (`attila64\plugins`, `attila64_plugin_init`). The loader can leave it out by name when the tick is off. The loader writes and deletes `flagwave_off.txt` and edits only the `addon=` line of `FrameBridge\settings.txt`.
- Mod makers: when the flags are on, `!!1212ad_flag_wave.pack` is loaded before every other pack, so it overrides 1212AD's banner and pennant models.
- Plugin interface: `ATTILA64_PLUGIN_API` 1 (as in the 1.3.15 header; the header of this release was not saved).

## 1.3.13 (2026-11-22) - mod list: drag mods, mod pictures
Two mod list window features. Only the binaries of this release were saved, so the details come from the player notes and the matching comments in the 1.3.15 `picker.c`.

### Changes
- Drag and drop in the mod list (`picker.c`): hold the mouse button on a mod and move it up or down. Above or below the list it scrolls (timer). Move up and Move down still work. A move rewrites only the changed rows, so the list keeps its scroll position and no longer jumps to the top.
- Mod picture: the window shows the picture of the selected mod, as the normal Attila launcher does. It uses `<pack name>.png` / `.jpg` next to the pack, in the Workshop item folder or in `data`. A Workshop folder without one shows its first `.png` / `.jpg`. It is drawn with GDI+ (`gdiplus.dll`, loaded when the window opens).

### For plugin and mod developers
- Mod authors: name a picture like your pack (`mymod.pack` -> `mymod.png`) and it shows in the 64-bit Attila mod list.
- Nothing else. No change to the game patches or the plugin interface.

## 1.3.12 (2026-11-22) - 8 vs 8 battle screens, 1212AD unit stats

This release makes the battle loading screen and the results screen show all 8 armies a side. It also fixes 1212AD's unit stats panel, where the two columns lay on top of each other. All changes are in `multiplayer.inc`, and they only change the interface.

### Changes
- Battle loading screen, 8 vs 8: the second row of faction icons (`mon4`..`mon7`, from `patch_loading_screen_eight`) was never made. It used `TWUI::ComponentTemplateLibrary::m_LibraryInstance` (`0x5163480`), which is empty on this screen. The copies are now made with the screen's own `LoadingScreenUI::CreateFromComponent` (`0x2b58f10`, instance `s_loading_screen_ui` at `0x5172fc8`). The old library is only used when that instance is missing.
- The `mon` icons are docked (dock point `+0x1a0`, offset `+0x1a4/+0x1a8`), so a plain move did not stay. The engine put them back each time it docked them. Rows are now placed with `TWUI::Component::SetDockOffset` (`0x29235b0`): row 1 moves up and row 2 moves down by half an icon. Icons are resized to 44 high (`TWUI::Component::Resize`, `0x2916c40`) so two rows fit in the header (`mon_place`, `mon_size`).
- Results screen after a multiplayer battle (`PostBattleLoadingScreenPanel`): each side's `team_clip_box` held only 4 rows of 46, so armies 5-8 were cut off or drawn over the other side. The new `results_screen_create` runs after the game's own OnCreate. It makes each clip box taller and moves everything under it down. The frames around it grow too (a "room walk", the same idea as the 64-bit Rome 2 engine). If 8 + 8 rows would go past the Continue button, the rows are scaled shorter (at least 26 layout units). `bottom_parent` (battle tip, loading bar, Continue) stays where the game put it. The battle tip (`panel_parchment`, `quote`) is hidden where the larger panel covers it. With 4 armies a side or fewer, nothing changes.
- New switch `ATTILA_NO_8V8_SCREENS=1`: the results screen keeps the game's own layout. The existing `ATTILA_NO_8V8=1` also turns off both new screen hooks.
- 1212AD unit stats panel (`UnitInformationPanelCallback`): 1212AD's layout puts the stat rows in two columns 214 apart, but each row is 294 wide, so the columns overlapped. After `InitStatistics` (`0x2a2b6f0`), if two visible rows overlap, all rows go into one column at the first row's x, like vanilla. Layouts whose rows do not overlap are not changed. The check also runs after the panel's OnUpdatePulse, because hovering a unit card placed the rows in two columns again.
- New test tools: `ATTILA_UI_DUMP=1` writes both screens' component trees to the log. `ATTILA_TEST_SHOT_SCREENS=1` takes a test picture as each screen appears. A test command `uidump <component name>` (through `ATTILA_TEST_CMD_FILE`, `shim_gfx.c`) writes that component's tree, starting from `TWUI::Component::m_Root` (`0x51632d8`).

### For plugin and mod developers
- New vtable hooks (the vtable entry is replaced, and the game's function is called first):
  - Battle loading screen OnCreate: vtable `0x3b36f60`, entry `0x2b5a740`.
  - Results screen OnCreate: vtable `0x3b37460`, entry `0x2b5c6e0`.
  - Unit info panel OnUpdatePulse: vtable `0x3b0c2b0`, entry `0x2a2dcd0`.
- The hook looks for the entry by its original value. If a plugin has already replaced that entry, the hook logs "patch skipped, game code differs" and does nothing.
- The two calls to `InitStatistics` at `0x2a2a4b4` (InitialisePanel) and `0x2a2d5c7` (RefreshStats) are redirected to the loader's own code next to the game.
- Ludde (attila_8p64.dll): these screen changes overlap with 8-player work. `ATTILA_NO_8V8=1` turns off the loading/results screen hooks. The unit stats fix is not controlled by that switch.
- The plugin interface is not changed (`ATTILA64_PLUGIN_API` is still 1).

## 1.3.11 (2026-11-22) - "Use DirectX 11 again" button

This release gives PCs that were moved to the OpenGL safety net a way back to DirectX 11 from the mod list window.

### Changes
- Up to 1.3.7, `dx11_starting.txt` was written before the loader connected to Steam. If the game was started without Steam running, the file was left behind. The next start window then wrote `graphics=opengl_fallback` to settings.txt, and that setting stayed. Since 1.3.8, Steam is connected first, so this should not happen again, but PCs already on the fallback stayed on it.
- `picker.c`: when `settings_graphics_dx11()` is false, the mod list window shows a "Use DirectX 11 again" button (`ID_DX11_AGAIN`). Clicking it writes `graphics=dx11` to settings.txt and removes the button. The "DirectX 11 did not start last time" message now mentions the button.

### For plugin and mod developers
- Nothing.

## 1.3.10 (2026-11-22) - films, cursor lock, surround sound

This release fixes three problems: off-centre films with interface zoom, the mouse leaving the screen in full screen, and broken sound with 5.1/7.1 speaker settings.

### Changes
- Films: with any interface size above 100% (including Auto on screens taller than 1080), the film rectangle came from the unscaled screen size. The film was then drawn through the zoomed interface (`VP8::VIDEO_CONTROLLER::draw`). So it was too big, pushed right and down, and cut off. In `interface.inc` `ui_restricted_size`, five film callers now get the laid-out size. They are found by return address:
  - `0x571a40` `MVC_MANAGER::update_movies`
  - `0x29a5ce8` `FRONT_END_UI::set_movie_position_and_dimension`
  - `0x29a5e15` `FRONT_END_UI::draw`
  - `0x29a60dd` `FRONT_END_UI::play_movie_in_component`
  - `0x2c11d62` `BATTLE_UI::notify(PLAY_MOVIE_MSG)`
- Cursor lock: the Linux game never confines the cursor (no `SDL_SetWindowGrab`), and our full screen is a borderless window the size of the screen. With a second monitor on the right or below, the cursor moved onto it, so edge-scrolling did not work on those edges. The new `cursor_lock_tick` in `shim_gfx.c` runs from `SDL_PollEvent`. While the game window is full screen and in front, it calls `ClipCursor` on the window, renewed every 0.5 s. It releases the cursor when another window is in front. New settings.txt key: `cursor_lock=off` turns this off (`picker.c` `settings_cursor_lock_off`). It is always off in hidden tests.
- Surround sound: with a speaker setting chosen in the options (5.1, 7.1, maybe headphones), Wwise `CAkSinkSDL2::_Init` (`0x37004d0`) opens the SDL audio device with 0 channels and no changes allowed. SDL then opens a stereo device, while Wwise mixes 6 or 8 channels into it. Music and close sounds were lost. The `SDL_OpenAudioDevice` shim (`shim_gfx.c`) now uses the sink's own channel count (userdata `+0xc`), and Windows mixes it down to the real speakers. Stereo is not changed. New switch: `ATTILA_NO_SOUND_CHANNEL_FIX=1` turns this off.

### For plugin and mod developers
- In full screen, the loader now calls `ClipCursor` every 0.5 s from the game's event pump (main thread). A plugin that shows its own window or overlay over the game may find the cursor kept inside the game window.
- New hook-by-return-address cases in `ui_restricted_size` (listed above).
- The audio device can now open with 6 or 8 channels instead of 2.

## 1.3.9 (2026-11-22) - big panels fit on screen

This release changes the interface zoom so larger panels are never laid out on a screen lower than 1080.

### Changes
- With a large interface size (for example 150% at 3440x1440), the zoom allowed a laid-out height down to 900. Larger panels, such as 1212AD's new-campaign screen, need 1080 and went off the top of the screen. In `interface.inc` `ui_restricted_size` (the hook on `WS_ENGINE_IMP::get_ui_restricted_screen_size`, `0x30ff560`), the highest factor is now limited by `w / 1440` and `h / 1080` (before: `h / 900`). On a 1440-high screen, 150% and more now give 133%. On a 2160-high screen, the most is 200%. The log says once when the size was limited.
- `ui_x2.inc` `x2_active` uses the same limit. Side effect: the DirectX 11 sharp interface pictures switch on only at a zoom of 1.5 or more, so on a 1440-high screen they are now always off.

### For plugin and mod developers
- Plugins that read the interface zoom or the laid-out screen size will see smaller zoom values on screens lower than 2160 at large interface sizes.

## 1.3.8 (2026-11-22) - Steam overlay

This release makes the Steam overlay (Shift+Tab) work by connecting to Steam before the game's graphics start.

### Changes
- The Steam overlay (`gameoverlayrenderer64.dll`) is loaded by `SteamAPI_Init`. It only hooks swap chains made after it is loaded, because it wraps `CreateDXGIFactory` / `CreateSwapChainForHwnd`. The game called `SteamAPI_Init` only after it made its DirectX 11 swap chain, so the overlay never attached. The new `steam_connect_early()` in `shim_steam.c` is called in `loader.c` `main` right before `setup_graphics_route()`. It does the same `connect_steam()` the game uses. The game's own later `SteamAPI_Init` gets the stored result.
- The "cannot reach Steam" box now comes before the game window instead of later.
- Because Steam is now connected before `dx11_starting.txt` is written, a start without Steam no longer moves a PC to the OpenGL fallback (see 1.3.11).
- New test switch: `ATTILA_STEAM_LATE=1` uses the old order (Steam is left to the game).

### For plugin and mod developers
- Load order changed: Steam (and with it Steam's overlay DLL) is now loaded in `main`, before the game thread starts. That means before `plugins_load` and before the DirectX 11 device and swap chain are made.
- The Steam overlay now hooks the game's DXGI factory and swap chain, and it draws in Present. Plugins that hook Present or the swap chain (JGdaCaesar's FrameBridge, ATN's RenoDX add-on) now share it with Steam's overlay hook. `ATTILA_STEAM_LATE=1` gives the old order for comparison.

## 1.3.7 (2026-11-22) - Steam's Play button

This release lets the installer set Steam's Play button to start 64-bit Attila, and it adds a way for mod scripts to detect 64-bit Attila.

### Changes
- `installer.c`: new `steam_play_button()`. For each Steam account it reads `userdata\<account>\config\localconfig.vdf`, finds `UserLocalConfigStore > Software > Valve > Steam > apps > 325610`, and sets `LaunchOptions` to `"<path>\attila64.exe" %command%`. It uses a small VDF reader and writer (`vdf_find_attila`, `vdf_escape` / `vdf_unescape`, `launch_option_file`). Steam writes this file when it closes, so the installer closes Steam (`steam.exe -shutdown`, waits up to 90 s), changes the file, and starts Steam again through Explorer so Steam does not run as administrator. The installer asks first. Launch Options that belong to someone else (not naming `attila64.exe`) are not changed.
- Steam then starts our exe with the path of Attila's own launcher as an argument. The exe ignores that argument (`loader.c` `main`; no change needed there).
- Uninstall now sets the Play button back first (it clears only our own line), and the old "clear the Launch Option yourself" text is removed.
- New installer arguments:
  - `play-on` / `play-off` change only the Play button.
  - `from-game` installs or updates without questions and sets the Play button. It first waits up to 60 s for `Attila.exe` to close. It is meant for a mod's own "switch to 64-bit" button (1212AD's script).
- With `ATTILA_INSTALL_TEST`, the installer uses the test folder's own `steam\userdata\...` instead of the real Steam folder.
- `shim_libc.c` `shim_environ`: the game's environment now contains `ATTILA64=1`.

### For plugin and mod developers
- Mod scripts can detect 64-bit Attila: `os.getenv("ATTILA64")` returns `"1"` in 64-bit Attila and `nil` in normal Attila.
- The installer now writes the user's Steam `localconfig.vdf` (Attila's `LaunchOptions` only), and it may close and restart Steam.
- If the game is started through Steam's Play button, the process gets the launcher path as an extra argument.

## 1.3.6 (2026-11-17) - dynamic textures written by the card (NVIDIA text attempt)

This release tried to fix missing text (every text a solid bar) on an NVIDIA card with DirectX 11. Note: a later tester log showed that 1.3.6 did not fix it on that card. The later investigation found that normal 32-bit Attila does the same copies, so the cause in this release's notes was not confirmed.

### Changes
- Every font's letter page is a dynamic texture. `SPRITE_FONT::render_kerned` fills it by a copy from another dynamic texture (the copy returns to `0x30a1f44`). DirectX 11 does not allow card writes (CopyResource, CopySubresourceRegion, UpdateSubresource) into dynamic textures. The new `dx_tex_writable_by_card()` in `dx11_device.inc` runs the first time the game writes a dynamic 2D texture by copy, update or resolve. It makes the texture again as a plain (DEFAULT) texture, copies in what it held, drops our views (`dx_tex_release`, `dx_bound_forget_textures`), and sets a new flag `DXTF_PLAIN`. After that, the game's map/unmap of that texture use the plain-texture path (memory copy + UpdateSubresource). It is called from `dx_tex_update`, `dx_tex_copy`, `dx_tex_copy_region`, `dx_tex_resolve` and the sharp-interface `dx_x2_write`. In test runs, only font pages were converted.
- New test switches (`SWITCHES.md`):
  - `ATTILA_DX11_WARP=1` creates the device with Windows' software renderer (`D3D_DRIVER_TYPE_WARP`).
  - `ATTILA_DX11_KEEP_DYNAMIC=1` turns the conversion off.
  - `ATTILA_DX11_DROP_DYNAMIC_WRITES=1` drops card writes into dynamic textures, to copy the reported NVIDIA behaviour on another card.

### For plugin and mod developers
- The loader's DirectX 11 texture object grew by 8 bytes: new field `DXT_FLAGS` at `+0x128`, and `DXT_SIZE` is now `0x130`. Code that reads these objects by offset must use the new layout.
- A dynamic texture's `ID3D11Resource` (`DXT_RES`) can be replaced by a new DEFAULT texture while the game runs, so a stored pointer to the old resource or its views becomes stale. With `ATTILA_DX11_WARP=1`, the device is not on the graphics card.
- The plugin interface is not changed (`ATTILA64_PLUGIN_API` is still 1).

