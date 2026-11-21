// 64-bit Attila plugin interface, version 1. MIT licence, like the loader.
//
// A plugin is a 64-bit Windows DLL in the "plugins" folder next to attila64.exe. The loader loads every *.dll there (in name
// order) after the game is in memory and its own fixes are applied, before the game starts running, and calls
//
//     int attila64_plugin_init(const Attila64Api *api);        // required; return 0 for "ready", anything else = "not used"
//
// and, if the plugin has it, once the game has set up its own settings:
//
//     void attila64_plugin_game_started(const Attila64Api *api);   // optional
//
// The game is the Linux build of Total War: ATTILA, loaded at its own fixed addresses (the addresses a disassembler shows for
// the Linux file "Attila" are the addresses in memory). Its functions use the Linux calling convention: a function a plugin
// hands to hook(), and any game function a plugin calls, must be declared with ATTILA64_GAME_CALL.
//
// Multiplayer: every player needs the same plugins if they change anything the simulation uses. Plugins that only change
// what is drawn or shown are safe to differ. The log lists the plugins in use.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define ATTILA64_PLUGIN_API 1
#define ATTILA64_GAME_CALL __attribute__((sysv_abi))

typedef struct Attila64Api {
    uint32_t api_version;        // ATTILA64_PLUGIN_API of the loader; check it is at least the version the plugin was written for
    const char *engine_version;  // the loader's version, such as "1.1"
    uintptr_t game_lo, game_hi;  // the game's code and data in memory
    const char *plugin_dir;      // the plugins folder, no trailing slash

    // Writes a line to attila64_log.txt (printf style, no newline needed).
    void (*log)(const char *fmt, ...);

    // Changes len bytes of the game at address at, but only if they are exactly `expect` now (so a plugin made for another
    // game version, or meeting another plugin's change, does nothing instead of breaking the game). 1 = changed, 0 = left alone.
    int (*patch)(uintptr_t at, const uint8_t *expect, const uint8_t *bytes, size_t len);

    // Makes the game function at `at` call fn instead. `expect` is the function's first len bytes (5 or more, ending on a
    // whole instruction, with nothing position-dependent in them). Returns what to call for the original function, or NULL
    // if the bytes differ. fn and the returned function are ATTILA64_GAME_CALL.
    void *(*hook)(uintptr_t at, const uint8_t *expect, int len, void *fn);

    // n bytes of memory within reach of the game's 32-bit jumps, for hand-made code. NULL when there is none left.
    void *(*code_near_game)(size_t n);
} Attila64Api;
