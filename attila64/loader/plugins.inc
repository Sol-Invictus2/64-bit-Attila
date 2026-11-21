// Plugins: other people's DLLs in the "plugins" folder next to attila64.exe, given a small interface to the game
// (attila64_plugin.h, PLUGINS.md). Part of loader.c (included there, not built by itself; the order of the includes matters).
// ATTILA_NO_PLUGINS=1 loads none. ATTILA_PLAIN=1 (the game as shipped) loads none either.
#include "attila64_plugin.h"

#define MAX_PLUGINS 32
typedef int (*PluginInitFn)(const Attila64Api *);
typedef void (*PluginStartedFn)(const Attila64Api *);
static Attila64Api g_plugin_api;
static char g_plugin_dir[MAX_PATH];
static PluginStartedFn g_plugin_started[MAX_PLUGINS];
static int g_nplugins;

static int plugin_patch(uintptr_t at, const uint8_t *expect, const uint8_t *bytes, size_t len) {
    if (!expect || !bytes || !len || at < g_plugin_api.game_lo || at + len > g_plugin_api.game_hi) return 0;
    if (memcmp((const void *)at, expect, len)) return 0;
    memcpy((void *)at, bytes, len);
    FlushInstructionCache(GetCurrentProcess(), (const void *)at, len);
    return 1;
}

static void *plugin_hook(uintptr_t at, const uint8_t *expect, int len, void *fn) {
    if (!expect || !fn || len < 5 || len > 15 || at < g_plugin_api.game_lo || at + (uintptr_t)len > g_plugin_api.game_hi) return NULL;
    void *orig = hook_entry(at, expect, len, fn);
    if (orig) FlushInstructionCache(GetCurrentProcess(), (const void *)at, (SIZE_T)len);
    return orig;
}

static void *plugin_code_near_game(size_t n) { return near_game_code(n); }

static int plugin_name_order(const void *a, const void *b) { return _stricmp((const char *)a, (const char *)b); }

static void plugins_load(const Module *game) {
    if (getenv("ATTILA_NO_PLUGINS") || getenv("ATTILA_PLAIN")) return;
    GetModuleFileNameA(NULL, g_plugin_dir, sizeof g_plugin_dir - 16);
    char *s = strrchr(g_plugin_dir, '\\');
    strcpy(s ? s + 1 : g_plugin_dir, "plugins");
    char pattern[MAX_PATH + 8], names[MAX_PLUGINS][MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s\\*.dll", g_plugin_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;   // no folder or nothing in it: the normal case
    int n = 0;
    do if (n < MAX_PLUGINS) snprintf(names[n++], MAX_PATH, "%s", fd.cFileName); while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(names, (size_t)n, MAX_PATH, plugin_name_order);

    g_plugin_api.api_version = ATTILA64_PLUGIN_API;
    g_plugin_api.engine_version = "1.2";   // keep in step with attila64.rc
    g_plugin_api.game_lo = (uintptr_t)game->lo;
    g_plugin_api.game_hi = (uintptr_t)game->hi;
    g_plugin_api.plugin_dir = g_plugin_dir;
    g_plugin_api.log = log_line;
    g_plugin_api.patch = plugin_patch;
    g_plugin_api.hook = plugin_hook;
    g_plugin_api.code_near_game = plugin_code_near_game;

    for (int i = 0; i < n; i++) {
        char path[MAX_PATH * 2];
        snprintf(path, sizeof path, "%s\\%s", g_plugin_dir, names[i]);
        HMODULE dll = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);   // its own helper DLLs may sit beside it
        if (!dll) { log_line("plugin %s: could not be loaded (error %lu)", names[i], GetLastError()); continue; }
        PluginInitFn init = (PluginInitFn)(void *)GetProcAddress(dll, "attila64_plugin_init");
        if (!init) { log_line("plugin %s: not a 64-bit Attila plugin (no attila64_plugin_init), left out", names[i]); FreeLibrary(dll); continue; }
        int r = init(&g_plugin_api);
        if (r) { log_line("plugin %s: says it is not usable here (%d), left out", names[i], r); continue; }
        g_plugin_started[g_nplugins++] = (PluginStartedFn)(void *)GetProcAddress(dll, "attila64_plugin_game_started");
        log_line("plugin in use: %s", names[i]);
    }
}

// Called with the game's own settings in place (from loader_game_settings).
void plugins_game_started(void) {
    for (int i = 0; i < g_nplugins; i++) if (g_plugin_started[i]) g_plugin_started[i](&g_plugin_api);
}
