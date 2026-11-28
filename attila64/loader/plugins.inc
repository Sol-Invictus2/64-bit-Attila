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

// JGdaCaesar's engine fix (perffix64.dll) and its Flag Wave banners: the two start-window ticks (picker.c, perf_fix= and
// flag_wave= in settings.txt). Not shipped with this mod: taken from his Workshop item when the player has it.
extern int settings_perf_fix(void), settings_flag_wave(void);
extern void perffix_path(int which, char *out, size_t size);
extern int settings_upscaling(void);
extern void framebridge_path(int which, char *out, size_t size);

// Copies a folder tree (the add-on's FrameBridge); files already there are overwritten. 0 = something failed.
static int copy_tree(const char *from, const char *to) {
    char pat[MAX_PATH * 2], s[MAX_PATH * 2], d[MAX_PATH * 2];
    WIN32_FIND_DATAA fd;
    int ok = 1;
    CreateDirectoryA(to, NULL);
    snprintf(pat, sizeof pat, "%s\\*", from);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        snprintf(s, sizeof s, "%s\\%s", from, fd.cFileName);
        snprintf(d, sizeof d, "%s\\%s", to, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok &= copy_tree(s, d);
        else if (!CopyFileA(s, d, FALSE)) ok = 0;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return ok;
}

// Deletes a folder tree. 0 = something is left (a file in use).
static int delete_tree(const char *dir) {
    char pat[MAX_PATH * 2], s[MAX_PATH * 2];
    WIN32_FIND_DATAA fd;
    int ok = 1;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            snprintf(s, sizeof s, "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok &= delete_tree(s);
            else {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesA(s, FILE_ATTRIBUTE_NORMAL);
                if (!DeleteFileA(s)) ok = 0;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryA(dir) && ok;
}

// Moves a file or folder to "to", replacing what is there. A folder is renamed (same drive), or else copied and deleted.
// 0 = failed; then "from" is still where it was (a failed copy is cleaned up again).
static int move_replace(const char *from, const char *to, int is_dir) {
    char parent[MAX_PATH * 2];
    snprintf(parent, sizeof parent, "%s", to);
    char *s = strrchr(parent, '\\');
    if (s) { *s = 0; CreateDirectoryA(parent, NULL); }
    if (!is_dir) return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0;
    if (GetFileAttributesA(to) != INVALID_FILE_ATTRIBUTES && !delete_tree(to)) return 0;
    if (MoveFileExA(from, to, 0)) return 1;
    if (copy_tree(from, to) && delete_tree(from)) return 1;
    delete_tree(to);
    return 0;
}

// Unticked in the start window = out of the folder where it would be loaded, into attila64\unticked; ticked again = back.
// Before, an untick only left the DLL unloaded (engine fix) or wrote addon=off (upscaling), so the files stayed: the
// FrameBridge DLLs in the GAME folder, where normal Attila's engine fix (dinput8.dll) finds them too. A tester asked for
// the files to be gone when unticked. path(0) = where it is used, path(2) = attila64\unticked (picker.c).
static void unticked_move(const char *what, void (*path)(int, char *, size_t), const char *probe, int on, int is_dir) {
    char used[MAX_PATH * 2], parked[MAX_PATH * 2], a[MAX_PATH * 2 + 32], b[MAX_PATH * 2 + 32];
    path(0, used, sizeof used);
    path(2, parked, sizeof parked);
    snprintf(a, sizeof a, "%s%s", used, probe);     // the folder's helper64.exe, or the DLL itself
    snprintf(b, sizeof b, "%s%s", parked, probe);
    int in_use_place = GetFileAttributesA(a) != INVALID_FILE_ATTRIBUTES, in_parked = GetFileAttributesA(b) != INVALID_FILE_ATTRIBUTES;
    if (!on && in_use_place) {
        if (move_replace(used, parked, is_dir)) log_line("%s: unticked in the start window, moved out to attila64\\unticked", what);
        else log_line("%s: unticked, but could not be moved out to attila64\\unticked (error %lu; is normal Attila running?)", what, GetLastError());
    } else if (on && !in_use_place && in_parked) {
        if (move_replace(parked, used, is_dir)) log_line("%s: ticked again, moved back from attila64\\unticked", what);
        else log_line("%s: ticked, but could not be moved back from attila64\\unticked (error %lu)", what, GetLastError());
    }
}

static void read_small(const char *path, char *out, size_t size) {
    out[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t n = fread(out, 1, size - 1, f);
    out[n] = 0;
    fclose(f);
}

// JGdaCaesar's Upscaling and Framegen add-on (<game>\FrameBridge), run by perffix64.dll: the start window's tick.
// Copied in from his Workshop item when it is missing or the Workshop has another version (his own settings.txt is not
// in the package, so the player's choices stay); then its addon= line is set to the tick.
static void framebridge_set(void) {
    char mine[MAX_PATH * 2], ws[MAX_PATH * 2], a[MAX_PATH * 2 + 32], b[MAX_PATH * 2 + 32];
    int on = settings_upscaling();
    framebridge_path(0, mine, sizeof mine);
    framebridge_path(1, ws, sizeof ws);
    snprintf(a, sizeof a, "%s\\helper64.exe", ws);
    if (on && GetFileAttributesA(a) != INVALID_FILE_ATTRIBUTES) {
        char v1[64], v2[64];
        snprintf(a, sizeof a, "%s\\version.txt", ws);
        snprintf(b, sizeof b, "%s\\version.txt", mine);
        read_small(a, v1, sizeof v1);
        read_small(b, v2, sizeof v2);
        snprintf(b, sizeof b, "%s\\helper64.exe", mine);
        if (GetFileAttributesA(b) == INVALID_FILE_ATTRIBUTES || strcmp(v1, v2)) {
            if (copy_tree(ws, mine)) log_line("upscaling add-on: FrameBridge copied in from the Workshop item 3828791891 (%s)", v1[0] ? strtok(v1, "\r\n") : "no version");
            else log_line("upscaling add-on: copying FrameBridge from the Workshop item failed (error %lu)", GetLastError());
        }
    }
    snprintf(b, sizeof b, "%s\\helper64.exe", mine);
    if (GetFileAttributesA(b) == INVALID_FILE_ATTRIBUTES) return;   // not installed: nothing to switch
    // its settings.txt: every line kept, the addon= line set (added at the top when there is none)
    char path[MAX_PATH * 2 + 32], text[16384], out[16384 + 32];
    snprintf(path, sizeof path, "%s\\settings.txt", mine);
    read_small(path, text, sizeof text);
    const char *want = on ? "addon=on" : "addon=off";
    size_t n = 0;
    int found = 0, same = 0;
    for (char *l = text; *l;) {
        char *e = strchr(l, '\n');
        size_t len = e ? (size_t)(e - l + 1) : strlen(l);
        if (!_strnicmp(l, "addon=", 6)) {
            found = 1;
            same = !_strnicmp(l, want, strlen(want)) && (l[strlen(want)] == '\r' || l[strlen(want)] == '\n' || !l[strlen(want)]);
            n += (size_t)snprintf(out + n, sizeof out - n, "%s\r\n", want);
        } else if (n + len < sizeof out) { memcpy(out + n, l, len); n += len; }
        l += len;
    }
    if (found && same) return;
    if (!found) {   // at the top
        char tmp[16384 + 32];
        int k = snprintf(tmp, sizeof tmp, "%s\r\n", want);
        if (k > 0 && (size_t)k + n < sizeof tmp) { memcpy(tmp + k, out, n); n += (size_t)k; memcpy(out, tmp, n); }
    }
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(out, 1, n, f); fclose(f); log_line("upscaling add-on: %s (start window tick, FrameBridge\\settings.txt)", want); }
}

// The upscaling tick: ticked = the add-on in the game folder (back from attila64\unticked first), its switch set on;
// unticked = its switch set off, then the folder moved out to attila64\unticked. JGdaCaesar's 32-bit dinput8.dll can copy
// the add-on in again from the Workshop when normal Attila starts; the next 64-bit start then moves it out again.
static void framebridge_prepare(void) {
    int on = settings_upscaling();
    if (on) unticked_move("upscaling add-on (FrameBridge)", framebridge_path, "\\helper64.exe", 1, 1);
    framebridge_set();
    if (!on) unticked_move("upscaling add-on (FrameBridge)", framebridge_path, "\\helper64.exe", 0, 1);
}

// His DLL's version marker ("AttilaCPUPerfFix-version:<number>", the number only grows: 7.5 = 750), read from the first
// 2 MB of the file the way his heapfix/update.c does. 0 = no file, not a DLL, or no marker (such a file is never taken).
static int perffix_version(const char *path) {
    static const char mark[] = "AttilaCPUPerfFix-version:";
    enum { CAP = 2 << 20 };
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char *buf = (char *)malloc(CAP);
    size_t n = buf ? fread(buf, 1, CAP, f) : 0;
    fclose(f);
    int v = 0;
    if (n >= 0x400 && buf[0] == 'M' && buf[1] == 'Z')
        for (size_t i = 0; !v && i + sizeof mark <= n; i++)
            if (buf[i] == 'A' && !memcmp(buf + i, mark, sizeof mark - 1))
                for (size_t j = i + sizeof mark - 1; j < n && buf[j] >= '0' && buf[j] <= '9' && v < 100000; j++)
                    v = v * 10 + (buf[j] - '0');   // the bare prefix (his scanner's own constant) gives 0 and is skipped
    free(buf);
    return v;
}

// 1 = the file is at least as long as its own PE header says (end of its last section). Steam writes the version marker
// early, so a Workshop download still in progress can show the new number before the rest of the file is there.
static int pe_complete(const char *path) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    static unsigned char h[4096];
    DWORD n = 0, size = GetFileSize(f, NULL);
    BOOL ok = ReadFile(f, h, sizeof h, &n, NULL);
    CloseHandle(f);
    if (!ok || n < 0x40 || h[0] != 'M' || h[1] != 'Z') return 0;
    DWORD pe = *(DWORD *)(h + 0x3C);
    if (pe > n - sizeof(IMAGE_NT_HEADERS64)) return 0;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(h + pe);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    DWORD end = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if ((unsigned char *)(sec + i + 1) > h + n) return 0;
        if (sec[i].PointerToRawData + sec[i].SizeOfRawData > end) end = sec[i].PointerToRawData + sec[i].SizeOfRawData;
    }
    return end && size != INVALID_FILE_SIZE && size >= end;
}

// Takes his Workshop copy into plugins following his own update rules (heapfix/update.c, update64.c), so the two never
// fight: only a NEWER version, only a finished download, never with attila64\update_off.txt, and any failure keeps the
// DLL that is there. His DLL then keeps itself current from inside the game the same way.
static void perffix_take_workshop(const char *mine, const char *ws) {
    char off[MAX_PATH * 2], tmp[MAX_PATH * 2 + 8];
    snprintf(off, sizeof off, "%s\\..\\update_off.txt", g_plugin_dir);
    if (GetFileAttributesA(off) != INVALID_FILE_ATTRIBUTES) {
        log_line("engine fix: update_off.txt found, perffix64.dll stays as installed (version %d)", perffix_version(mine));
        return;
    }
    int have = perffix_version(mine), theirs = perffix_version(ws);
    if (!theirs) { log_line("engine fix: the Workshop copy has no version number, not taken"); return; }
    if (theirs <= have) {
        log_line(theirs == have ? "engine fix: perffix64.dll up to date with the Workshop item (version %d)"
                                : "engine fix: perffix64.dll in plugins is newer than the Workshop item's (%d > %d), kept", have, theirs);
        return;
    }
    if (!pe_complete(ws)) { log_line("engine fix: Steam is still downloading version %d, taken at a later start", theirs); return; }
    CreateDirectoryA(g_plugin_dir, NULL);
    snprintf(tmp, sizeof tmp, "%s.new", mine);
    if (CopyFileA(ws, tmp, FALSE) && perffix_version(tmp) == theirs && pe_complete(tmp) && MoveFileExA(tmp, mine, MOVEFILE_REPLACE_EXISTING))
        log_line("engine fix: perffix64.dll version %d copied into plugins from the Workshop item 3824249154 (was %d)", theirs, have);
    else {
        log_line("engine fix: could not take version %d from the Workshop item (error %lu), version %d kept", theirs, GetLastError(), have);
        DeleteFileA(tmp);
    }
}

static void perffix_prepare(void) {
    // unticked: plugins\perffix64.dll moved out to attila64\unticked; ticked again: moved back (then the Workshop check)
    unticked_move("engine fix (perffix64.dll)", perffix_path, "", settings_perf_fix(), 0);
    if (!settings_perf_fix()) return;
    char mine[MAX_PATH * 2], ws[MAX_PATH * 2], off[MAX_PATH * 2];
    perffix_path(0, mine, sizeof mine);
    perffix_path(1, ws, sizeof ws);
    // His Workshop item is where the DLL comes from (newer versions only, see above). Not subscribed: a copy already in
    // plugins is used as it is (the start window says to subscribe when there is none).
    if (GetFileAttributesA(ws) != INVALID_FILE_ATTRIBUTES) perffix_take_workshop(mine, ws);
    else if (GetFileAttributesA(mine) != INVALID_FILE_ATTRIBUTES)
        log_line("engine fix: perffix64.dll in plugins, not from the Workshop (subscribe to Attila Engine Performance Fix for updates)");
    else log_line("engine fix: not installed (no perffix64.dll; subscribe to Attila Engine Performance Fix on the Workshop)");
    // The DLL reads attila64\flagwave_off.txt at its start (Ctrl+Shift+F in game still switches the flags off / on).
    snprintf(off, sizeof off, "%s\\..\\flagwave_off.txt", g_plugin_dir);
    if (!settings_flag_wave()) {
        HANDLE f = CreateFileA(off, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            static const char note[] = "Made by 64-bit Attila: \"Waving flags\" is unticked in the start window. Tick it there to remove this file.\r\n";
            DWORD w;
            WriteFile(f, note, sizeof note - 1, &w, NULL);
            CloseHandle(f);
        }
        log_line("engine fix: waving flags unticked in the start window (flagwave_off.txt)");
    } else if (DeleteFileA(off)) log_line("engine fix: waving flags ticked again, flagwave_off.txt removed");
}

static void plugins_load(const Module *game) {
    if (getenv("ATTILA_NO_PLUGINS") || getenv("ATTILA_PLAIN")) return;
    GetModuleFileNameA(NULL, g_plugin_dir, sizeof g_plugin_dir - 16);
    char *s = strrchr(g_plugin_dir, '\\');
    strcpy(s ? s + 1 : g_plugin_dir, "plugins");
    perffix_prepare();
    framebridge_prepare();   // also with the engine fix unticked, so an unticked add-on leaves the game folder
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
        if (!_stricmp(names[i], "perffix64.dll") && !settings_perf_fix()) {
            log_line("plugin %s: engine performance fix unticked in the start window, left out", names[i]);
            continue;
        }
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
