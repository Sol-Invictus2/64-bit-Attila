# 64-bit Attila loader: files and test switches

## Where things are
| File | What is in it |
|---|---|
| `loader.c` | Loading the Linux game file, connecting it to Windows, the log, start-up (`main`), the game thread, crash reports. Includes the `.inc` files below. |
| `game_fixes.inc` | Stack checks, the table of small byte patches (`g_game_patches`), music track bounds, the game's own settings, far-away sprite step. |
| `interface.inc` | Interface size that follows the screen, sharp text, version text, interface test traces. |
| `effects_and_tests.inc` | Particle pool locks, effect group check, task split, movie skip, battle step stopwatch. |
| `multiplayer.inc` | 8-player campaign, players dropping, 8 vs 8 battles, lobby join guard, battle loading screen, Steam texts with emoji, campaign messages and dilemmas for every human, script commands in direct mode on every PC (1.3.0). |
| `script_trace.inc` | Test only (`ATTILA_SCRIPT_TRACE=1`): logs campaign script commands and script errors. |
| `settings_copy.inc` | One-time copy of graphics settings from normal Attila. |
| `shim_*.c` | The Windows versions of what the Linux game expects: C library, files, threads, system and time, graphics (SDL/OpenGL), Steam, network. |
| `picker.c` | The mod list window (and the "Interface size" choice). |
| `installer.c` | `Install 64-bit Attila.exe`. |
| `profile.c` | The profiler behind `ATTILA_PROFILE`. |
| `plugins.inc`, `attila64_plugin.h`, `plugin_example\` | Plugins: other people's DLLs from the `plugins` folder (see `PLUGINS.md`). |
| `old_tests\` | Logs, pictures and benchmark folders from earlier tests. Nothing in there is needed to build. |

The `.inc` files are part of `loader.c` (one unit, in the order they are included). `build.ps1` builds `attila64.exe`.

## Switches
Set as environment variables before starting `attila64.exe`. All are OFF unless set; none is needed for normal play.

### Turn one of our changes off (to compare)
| Switch | Effect |
|---|---|
| `ATTILA_PLAIN=1` | The game exactly as shipped: no game patches, no changed settings (measuring hooks only). |
| `ATTILA_SKIP_PATCH=text` | Leaves out the byte patches whose name contains the text (e.g. `large armies`). |
| `ATTILA_NO_8V8=1` | No 8 vs 8 battle changes. |
| `ATTILA_TWO_PLAYER_CAMPAIGN=1` | No 8-player campaign changes. |
| `ATTILA_DROP_ENDS_GAME=1` | A player dropping ends the game again, as in the normal game. |
| `ATTILA_SOFT_TEXT=1` | No sharp text with the bigger interface. |
| `ATTILA_UI_GAME_SCALE=1` | Keeps the game's own second interface scale (the bug sharp text works around). |
| `ATTILA_NO_VERSION_FIT=1` | Main menu version text not widened. |
| `ATTILA_SLOW_VFX_CHECK=1` | The game's own effect group check. |
| `ATTILA_OLD_GL_BRIDGE=1` | Every OpenGL call through the long bridge again (the short one is for calls with at most 6 whole-number arguments). |
| `ATTILA_NO_PLUGINS=1` | No plugins loaded (`plugins.inc`, `PLUGINS.md`). |
| `ATTILA_NO_SETTINGS_COPY=1` | No copy of graphics settings from normal Attila. |
| `ATTILA_NO_MP_TEXT_FIX=1` | The game's own UTF-8 decoder for Steam texts (crashes on an emoji at the end of a name). |
| `ATTILA_FEED_TWO_HUMANS=1` | Campaign messages and dilemmas for the first 2 humans only, the game's limit (3rd player's timer stops at 4:59). |
| `ATTILA_SCRIPT_DIRECT_FIRST_PC=1` | Script commands in direct mode only on the first human's PC, the game's rule (1212AD desync at turn 10). |
| `ATTILA_BATTLE_CHOICE_OLD=1` | Campaign battles with 3+ players: the players not in the battle share one choice that defaults to auto-resolve, the game's 2-player rule. |
| `ATTILA_DROP_OLD_CONCEDE=1` | Concede Defeat in a multiplayer battle does not end it (the 1.2-1.3.1 rule; the AI goes on with the army). |
| `ATTILA_BATTLE_TRACE=1` | Test only: log every battle voter, vote, decision and pre-battle window. |
| `ATTILA_SPRITE_STEP=seconds` | Far-away sprite animation step (ours 0.1; `0.025` = the game's). |
| `ATTILA_VAB=0/1`, `ATTILA_UBM=0/1/2` | OpenGL settings `enable_vertex_attrib_binding` and `gl_uniform_buffer_mode` (ours 1 and 2). |

### Turn an experiment on
| Switch | Effect |
|---|---|
| `ATTILA_PARALLEL_VFX=1` | Battle effects updated on all cores. Crashed a 1212AD battle in Test 17 (shared scene nodes); since the effect gate (CHANGES_EFFECT_GATE.md) only plain effects run together. One fight tested, still off by default. |
| `ATTILA_TASK_SPLIT=n` | How finely the task system cuts parallel work (game: 20). No gain measured. |
| `ATTILA_FINE_TIMER=1` | Asks Windows for 0.5 ms timer ticks. No effect measured. |
| `ATTILA_CLOCK_REAL=1` | `clock()` returns real time instead of processor time. No effect measured. |
| `ATTILA_FPS_CAP=n` | Frame limit. |

### Measuring and logs
| Switch | Effect |
|---|---|
| `ATTILA_TICK_TRACE=1` | Every 5 s: battle steps done, average and longest step time. 10 steps a second = full speed. |
| `ATTILA_PROFILE=1` | Profiler report every 30 s next to the exe (`attila64_profile_N.txt`). | With `ATTILA_NO_GL_TIMES=1` the OpenGL calls keep their normal bridges (no per-call timing), so the profile shows the game as players run it. Summaries: `tools\prof_sum.sh`, `tools\glshare.sh` (OpenGL layer share), `tools\hot.sh` (inside one function), under WSL. |
| `ATTILA_SAMPLE=1` | Older sampler thread. |
| `ATTILA_TIME_TRACE=1` | Every 5 s: how often each clock and wait function was used. |
| `ATTILA_FPS_INTERVAL=ms` | How often the fps line is logged (default 30000). |
| `ATTILA_GFX_TRACE=1` | Logs how graphics settings are capped at start. |
| `ATTILA_GRAPHICS=dx12` / `opengl` | Graphics route, instead of the start window's choice (`graphics_choice.inc`; DirectX 12 = Mesa files in `dx12\`). |
| `ATTILA_TEXTURE_FILTER=0..4` | Texture sharpness, instead of the start window's choice (0 trilinear, 4 = 16x). The log line "texture sharpness sent to the graphics card" shows what arrives (battle only). |
| `ATTILA_AA=0..4`, `ATTILA_AA_LIMIT=n` (needs `ATTILA_GFX_TRACE=1`) | Test only: write the game's anti-aliasing setting / raise its limit. MSAA (2 to 4) draws a broken picture with the OpenGL renderer (tested 2026-11-11), so it is not offered to players. |
| `ATTILA_TEXT_TRACE=1` | Logs interface text measuring and drawing. |
| `ATTILA_LOBBY_TRACE=1` | Logs battle lobby place changes. |
| `ATTILA_SCRIPT_TRACE=1` | Logs campaign script kill_character / spawn / immortality / trait calls (and whether the script command queue is set) and script errors (`script_trace.inc`). |
| `ATTILA_COUNT_VFX=1` | Counts effects made, by name. |
| `ATTILA_SHOW=addr,...` | Logs the bytes at game addresses at start. |
| `ATTILA_TRACE=...` | Call tracing (see `loader.c`). |
| `ATTILA_DEBUG=1` | Extra C library messages. |
| `ATTILA_CHECK_UNIFORMS=1`, `ATTILA_KEEP_ZERO_UPLOADS=1`, `ATTILA_DROP_INVALIDATE=1`, `ATTILA_ORPHAN_LIMIT_MB=n` | OpenGL buffer handling checks and limits (`shim_gfx.c`). |

### For automated tests only
| Switch | Effect |
|---|---|
| `ATTILA_GAME_DIR=folder` | Game folder to use (the test copy lives outside the game folder). |
| `ATTILA_NO_MODS=1`, `ATTILA_NO_PICKER=1`, `ATTILA_PICKER_ONLY=1`, `ATTILA_PICKER_AUTO=1` | Mod list window: skip mods, skip the window, only the window, press Play by itself. |
| `ATTILA_ARGS=text` | Extra arguments handed to the game. |
| `ATTILA_TEST_SKIP_MOVIES=1` | Skips every movie at once (once caused a sound crash at the menu). |
| `ATTILA_TEST_CMD_FILE=path` | Every half second that file is read and deleted; each line is one input step ("click 640 360", "key return 0.1") run at once, or "shot" = a picture now. A step in `ATTILA_TEST_INPUT` ending in `@P` repeats every P seconds. |
| `ATTILA_TEST_SHOT_EVERY=s` | After the listed test pictures, one more every s seconds. |
| `ATTILA_TEST_NO_STEAM=1` | Pretends Steam is not running (tests the Retry box). |
| `ATTILA_TEST_BLOCK_RANGE=1/2` | Blocks the game's memory range at start (tests the error 487 fix). |
| `ATTILA_TEST_NO_VFX=name,...` | Leaves the named effects out. |
| `ATTILA_POKE=addr:value,...` | Writes bytes into game memory at start. |
| `ATTILA_MAP_COLOUR_TEST=1` | Campaign lobby map colour test. |
| `ATTILA_INSTALL_TEST=folder`, `ATTILA_INSTALL_STEAM=folder` | Installer: install into a test folder / use a test Steam folder. |

- `ATTILA_SHADOW_EVERY=n` (1-8): shadow map redrawn every nth frame (overrides the start window's choice). `ATTILA_SHADOW_STILL=metres`: how far the camera may be from the last drawn frame and still rest (0.25). See shadow_rate.inc. The old ATTILA_SHADOW_SKIP / _EVERY_LISTS / ATTILA_CAMERA_DUMP are gone (old_tests\shadow_tests_2026-11-11).
- `ATTILA_TASK_PEEK=1`: task queue looked at before locking (task_wait.inc). No gain measured.
- `ATTILA_TEST_NO_LOBBY=1`: no Steam lobby is made or joined (unattended tests; start_game.ps1 sets it).
- `ATTILA_TEST_HOST=1`: lets a test host a lobby although ATTILA_TEST_NO_LOBBY is set. `ATTILA_CAMPAIGN_TEAMS=n`: teams asked for when hosting a campaign (default 4).
- `ATTILA_UPSCALE=1`: interface at the window's size when the game's resolution is lower (render_scale.inc; test). `ATTILA_RENDER_TRACE=1`: logs the frame and window sizes. `ATTILA_VIEW_ORDER=n` (with ATTILA_PASS_STATS=1): the order of the views in frame n.
- `ATTILA_NO_DPI_AWARE=1`: the game's window is left to Windows' display scaling again (as before 1.2 test 3).
- `ATTILA_FULL_UPLOADS=1`: texture uploads send the whole level again (tex_trace.inc). `ATTILA_TEX_TRACE=1`: texels uploaded per frame + who asks. `ATTILA_TASK_TRACE=1`: whether battle steps run on a worker. `ATTILA_CORES=n`: processor count told to the game (task threads = n - 1). `ATTILA_TASK_THREADS=n`: fewer task threads.
- `ATTILA_LAZY_UNIT=1`: glActiveTexture sent only when something uses the unit (lazy_unit.inc). No gain measured in the benchmark.
- `ATTILA_BATCH_SETS=0`: soldier parts with the same shader set are drawn one by one again (the game's own rule). Default: grouped.
- settings.txt `task_waits=fast`: worker threads stay awake a moment before sleeping, and the task queue is peeked (OFF by default since 1.3.24; no line or `task_waits=game` = the game's way). `ATTILA_TASK_SPIN_US=n`: stay-awake time (default 50). `ATTILA_TASK_SPIN_OFF=1`: off.
- `ATTILA_HOME_LONG=1`: the home folder (HOME) is given by its long name even when it has letters outside English (the old way; such a folder normally gets its Windows short name so the load lists work, shim_libc.c attila_home_dir).
- `ATTILA_CARD_LINE_OF_SIGHT=1`: with DirectX 11, battle line of sight stays on the graphics card (the game's default, broken with our DirectX 11 device: no round ever finishes). Default since 2026-11-17: on the processor (dx11_game_settings in dx11_device.inc). `ATTILA_DX11_QUERY_STATS=1`: every 5 s, occlusion query answers / pixels seen / begins / ends (dx11_device.inc).
- `ATTILA_NO_DISCORD_BUTTON=1` or settings.txt `discord_button=off`: no 64-bit Attila Discord button on the main menu (menu_button.c). With DirectX 11 the button and the frame-rate number go onto the window's picture when it is 8-bit R8G8B8A8_UNORM, otherwise (an HDR mod may make it 16-bit float or 10-bit) into the game's own 8-bit picture just before it is copied there (dx_present in dx11_device.inc). `ATTILA_LOBBY_MAP_OLD=1`: the multiplayer campaign lobby map picture as in the game (no fallback to the map folder's own minimap; frontend_extras.inc).
- `ATTILA_DX11_WARP=1`: DirectX 11 drawn by Windows' own software renderer (WARP) instead of the graphics card (test; slow). `ATTILA_DX11_KEEP_DYNAMIC=1`: dynamic textures the game writes by copy/update stay dynamic (the way before the 2026-11-17 NVIDIA text fix). `ATTILA_DX11_DROP_DYNAMIC_WRITES=1`: such writes are dropped, as NVIDIA's driver does (to see the NVIDIA "no text" case on another card). All in dx11_device.inc.
| `ATTILA_NO_FILM_CLOCK_FIX=1` | Sound engine stopwatch back to processor time (films with sound stutter again). |
| `ATTILA_NO_INNER_PATH_FIX=1` | Saved key sets no longer load (full path inside a game folder path not corrected). |
| `ATTILA_FILM_TRACE=1` | Test: logs how evenly each film's frames advance (film_trace.inc). |
