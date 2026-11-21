# Effect gate, 2026-11-10/11: BUILT, ONE FIGHT TESTED, NOT RELEASED, OFF BY DEFAULT

Source in this folder is version 1.0's plus the effect gate. Dev engine `attila64.exe` 408ABBED (the engine before it,
268430A0, is in `..\saved_versions\before_effect_gate_2026-11-10\`). Installed game and 1.0 zip are untouched.

## What changed
- `effects_and_tests.inc`: `patch_vfx_gate` (after `patch_vfx_pool_locks`). Replaces the parallel effect task body
  (35314b0) with `vfx_task_execute`: an effect made only of plain billboard emitters with no event links, no sound and no
  particle tracking updates freely; every other effect (debris, ribbons, effects that start other effects, sound) updates
  alone under one lock. The shared spawn buffer (VFX_SCENE_NODE::spawn_particle, 32ffa60) is locked too. The comment
  above the code lists every place the effect update leaves its own data (from the call graph).
- `game_fixes.inc`: `ATTILA_PARALLEL_VFX=1` now also needs the gate. Still a test switch; default is the game's serial way.
- `loader.c`: calls `patch_vfx_gate`. `shim_gfx.c`: one log line per fps line, "effects: N updated freely, M one at a time".

## Tested
- Custom battle (1212AD list of the test copy) played to the end by the user with `ATTILA_PARALLEL_VFX=1`: no crash,
  about 95% of effect updates free, 5% one at a time. Log: `bench_gate_on\log_fight.txt`.

## Not done
- No same-battle comparison with the switch off yet (another test copy was open). Run `test_custom.ps1` twice, with and
  without `-Switches 'ATTILA_PARALLEL_VFX=1;ATTILA_FPS_INTERVAL=5000'`; its load detection misses the load, press
  Start Battle by hand (click 640,79).
- Not run in a 1212AD rain battle (where Test 17 crashed), in a siege with fire, or in multiplayer.
- Make it the default only after several long battles without a crash.
