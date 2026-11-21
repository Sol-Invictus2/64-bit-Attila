// Example plugin for 64-bit Attila: shows the three things a plugin can do, and changes nothing in the game.
//   1. write to the log
//   2. check game bytes before touching them (here: only checks, patches nothing)
//   3. hook a game function and call the original (here: counts calls of the game's texture set-up function)
// Build: build_example.ps1 (needs the Zig compiler in tools\zig). Put example_plugin.dll into <game>\attila64\plugins.
#include "../attila64_plugin.h"

static const Attila64Api *g_api;

// OPENGL_SHADER::set_shader_resources in the Linux game file Steam offers (the one 64-bit Attila 1.1 was made for): its
// address and first bytes. If a game update moves it, the bytes no longer match and hook() returns NULL.
#define SET_RESOURCES_AT 0x33ddd50
static const uint8_t k_set_resources_start[5] = {0x55, 0x41, 0x57, 0x41, 0x56};   // push rbp; push r15; push r14

typedef uint32_t (ATTILA64_GAME_CALL *SetResourcesFn)(void *shader, uint32_t first, uint32_t count, void **resources);
static SetResourcesFn g_original;
static volatile long g_calls;

static ATTILA64_GAME_CALL uint32_t counted_set_resources(void *shader, uint32_t first, uint32_t count, void **resources) {
    long n = __sync_add_and_fetch(&g_calls, 1);
    if (n == 1 || n == 1000000) g_api->log("example plugin: the game's texture set-up has run %ld time%s", n, n == 1 ? "" : "s");
    return g_original(shader, first, count, resources);
}

__declspec(dllexport) int attila64_plugin_init(const Attila64Api *api) {
    if (api->api_version < 1) return 1;
    g_api = api;
    api->log("example plugin: loaded by 64-bit Attila %s, game in memory at %p to %p", api->engine_version, (void *)api->game_lo, (void *)api->game_hi);
    g_original = (SetResourcesFn)api->hook(SET_RESOURCES_AT, k_set_resources_start, 5, (void *)counted_set_resources);
    api->log(g_original ? "example plugin: counting texture set-up calls" : "example plugin: this game version differs, nothing hooked");
    return 0;
}

__declspec(dllexport) void attila64_plugin_game_started(const Attila64Api *api) {
    api->log("example plugin: the game has set up its own settings");
}
