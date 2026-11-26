// Mod picker shown before the game starts. Steam's launch option and the desktop icon both skip the normal launcher, so this replaces its mod screen.
// Lists the mods in the game's data folder and Workshop folders. The list of ticked mods, in their order, is 64-bit Attila's
// own: mod_list.txt next to the exe (the launcher's format). Up to 1.2 Test 6 it was the game folder's used_mods.txt, which
// normal Attila's launcher rewrites from its own data whenever it starts the game, so the order kept going back to the
// launcher's (a Workshop report, 2026-11-14). The first start of a version with its own list copies used_mods.txt once.
// The working folder is the game's folder when this runs. ATTILA_NO_PICKER skips it. ATTILA_PICKER_AUTO=1 is for tests: untick the first, move the last to the top, Play.
#include <windows.h>
#include <commctrl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MODS 2000
typedef struct { char name[160]; char dir[MAX_PATH]; int on; } Mod;
static Mod *g_mods;
static int g_nmods;
static char (*g_used)[160];  // the list as it was read, to tell whether anything changed
static int g_nused;
static char *g_other;        // lines of used_mods.txt that are neither mods nor folders; kept as they are
static HWND g_list, g_size_box, g_sharp_box, g_shadow_box, g_fps_box, g_dx11_again, g_gfx_box, g_perf_box, g_flag_box, g_up_box;
static int g_result;

enum { ID_LIST = 100, ID_UP, ID_DOWN, ID_ALL, ID_NONE, ID_PLAY, ID_QUIT, ID_SIZE, ID_SHARP, ID_GFX, ID_SHADOW, ID_COPYLOG, ID_SHOWLOG, ID_FPS, ID_DX11_AGAIN, ID_PICTURE, ID_PERF, ID_FLAGS, ID_UPSCALE, ID_THEME, ID_TOP, ID_LAUNCHER, WM_AUTO = WM_APP + 1, WM_CRASH_OFFER = WM_APP + 7 };

// Interface size, kept in settings.txt next to the exe (a percentage, or "like1080p" for the automatic size). The engine reads it
// at start. Since Test 18 the first entry (the default, also for the old "interface_size=auto" that earlier versions saved
// without the player choosing) is 100%: in battle the zoomed interface put the drag-selection box in the wrong place.
static const struct { const char *text; int percent; } g_sizes[] = {
    {"100%", 100}, {"Auto (like 1080p)", 0}, {"125%", 125}, {"150%", 150}, {"175%", 175}, {"200%", 200}, {"250%", 250}};
enum { SIZE_AUTO = 1 };
enum { N_SIZES = sizeof g_sizes / sizeof g_sizes[0] };

static void settings_path(char *out, size_t size) {
    GetModuleFileNameA(NULL, out, (DWORD)size);
    char *s = strrchr(out, '\\');
    snprintf(s ? s + 1 : out, size - (size_t)(s ? s + 1 - out : 0), "settings.txt");
}

// 64-bit Attila's own mod list, next to the exe. Made once from the game folder's used_mods.txt (normal Attila's list) when it
// is not there yet, so a player's ticks and order carry over; after that normal Attila's launcher cannot change it.
const char *mod_list_path(void) {
    static char path[MAX_PATH];
    if (!path[0]) {
        GetModuleFileNameA(NULL, path, sizeof path);
        char *s = strrchr(path, '\\');
        snprintf(s ? s + 1 : path, sizeof path - (size_t)(s ? s + 1 - path : 0), "mod_list.txt");
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES && GetFileAttributesA("used_mods.txt") != INVALID_FILE_ATTRIBUTES)
            CopyFileA("used_mods.txt", path, TRUE);   // the working folder is the game's folder here
    }
    return path;
}

// settings.txt holds one "name=value" per line. The value of one name (the last such line counts); 0 when it is not there.
static int settings_get(const char *name, char *out, size_t size) {
    char path[MAX_PATH], line[128];
    settings_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    size_t nl = strlen(name);
    int found = 0;
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        if (_strnicmp(line, name, nl) || line[nl] != '=') continue;
        line[strcspn(line, "\r\n")] = 0;
        snprintf(out, size, "%s", line + nl + 1);
        found = 1;
    }
    fclose(f);
    return found;
}

// Sets one name and keeps every other line. The file is written only when the value really changes.
static void settings_put(const char *name, const char *value) {
    char path[MAX_PATH], old[128], lines[32][128];
    if (settings_get(name, old, sizeof old) && !strcmp(old, value)) return;
    settings_path(path, sizeof path);
    size_t nl = strlen(name);
    int n = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        while (n < 31 && fgets(lines[n], sizeof lines[n], f)) {
            lines[n][strcspn(lines[n], "\r\n")] = 0;
            if (lines[n][0] && (_strnicmp(lines[n], name, nl) || lines[n][nl] != '=')) n++;
        }
        fclose(f);
    }
    snprintf(lines[n++], sizeof lines[0], "%s=%s", name, value);
    f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < n; i++) fprintf(f, "%s\n", lines[i]);
    fclose(f);
}

static int settings_size_index(void) {
    char v[128];
    // default: relative to the screen (height / 1080), now that the battle selection box follows the zoom
    if (!settings_get("interface_size", v, sizeof v) || !_strnicmp(v, "like1080p", 9) || !_strnicmp(v, "auto", 4)) return SIZE_AUTO;
    int pc = atoi(v);
    for (int i = 0; i < N_SIZES; i++) if (pc > 0 && g_sizes[i].percent == pc) return i;
    return SIZE_AUTO;
}

static void settings_save_size(int index) {
    char v[16];
    snprintf(v, sizeof v, "%d", g_sizes[index < 0 || index >= N_SIZES ? 0 : index].percent);
    settings_put("interface_size", index == SIZE_AUTO ? "like1080p" : v);
}

// Graphics route, "graphics=opengl|dx11|dx12" in settings.txt. OpenGL is the card's own driver (the default). DirectX 12 loads the Mesa
// files in the dx12 folder next to the exe, which turn the game's OpenGL drawing into DirectX 12: the same picture, slower on
// cards with a good OpenGL driver, a second way for cards whose OpenGL driver has trouble. Offered only when the files are there.
int settings_dx12_available(void) {
    char path[MAX_PATH + 32];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char *s = strrchr(path, '\\');
    snprintf(s ? s + 1 : path, 32, "dx12\\opengl32.dll");
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

// Since 2026-11-13 (the user: "remove directx 12 and opengl make the directx 11 default") the start window offers no graphics
// choice: DirectX 11 always. DirectX 12 (Mesa) is no longer used from the settings (ATTILA_GRAPHICS=dx12 still works for tests).
// "cursor_lock=off" in settings.txt: the cursor is never kept on the game's screen in full screen (shim_gfx.c cursor_lock_tick).
int settings_cursor_lock_off(void) {
    char v[128];
    return settings_get("cursor_lock", v, sizeof v) && !_stricmp(v, "off");
}
// "discord_button=off" in settings.txt: no 64-bit Attila Discord button on the main menu (menu_button.c), as ATTILA_NO_DISCORD_BUTTON=1.
int settings_discord_button_off(void) {
    char v[128];
    return settings_get("discord_button", v, sizeof v) && !_stricmp(v, "off");
}
int settings_graphics_dx12(void) {
    return 0;
}

// DirectX 11: the game's own DirectX 11 shaders drawn by a real DirectX 11 device (loader: dx11_device.inc). The only route,
// unless a DirectX 11 start failed on this PC: a start that never reached a running game leaves dx11_starting.txt, and the
// start window then writes graphics=opengl_fallback (settings_dx11_failed_last_time) and says so; from then on this PC uses
// OpenGL, the card's own driver. Older settings files' graphics=opengl / dx12 / dx11 choices are not read any more.
int settings_graphics_dx11(void) {
    char v[128];
    return !(settings_get("graphics", v, sizeof v) && (!_stricmp(v, "opengl_fallback") || !_stricmp(v, "opengl_chosen")));   // opengl_chosen: the player's own choice in the start window (2026-11-26); plain "opengl" is an old value and is not read
}
// "dx11_mode=basic" in settings.txt (no box in the start window): DirectX 11 tells the game "shader model 4", as OpenGL
// does, instead of the game's full DirectX 11 path (shader model 5: the 2x / 4x / 8x anti-aliasing choices, particles
// sorted on the card). A way back if the full path misbehaves somewhere it was not tested.
int settings_dx11_basic(void) {
    char v[128];
    return settings_get("dx11_mode", v, sizeof v) && !_stricmp(v, "basic");
}
static int settings_dx11_failed_last_time(void) {
    char path[MAX_PATH + 32];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char *s = strrchr(path, '\\');
    snprintf(s ? s + 1 : path, 32, "dx11_starting.txt");
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return 0;
    DeleteFileA(path);
    settings_put("graphics", "opengl_fallback");
    return 1;
}


// Texture sharpness (anisotropic filtering), "texture_sharpness=game|0..4" in settings.txt. The game's options screen has no box
// for it with the OpenGL renderer, so the choice is made here. -1 = leave the game's own setting alone, 0 = off (trilinear),
// 1 to 4 = 2x, 4x, 8x, 16x: the game's own numbers for gfx_texture_filtering. Shown to reach the graphics card in battle
// (2026-11-11, log line "texture sharpness sent to the graphics card"); the menu does not use it.
static const char *const g_sharp_texts[] = {"Game setting", "Off", "2x", "4x", "8x", "16x"};
enum { N_SHARP = sizeof g_sharp_texts / sizeof g_sharp_texts[0] };

int settings_texture_filtering(void) {
    char v[128];
    if (!settings_get("texture_sharpness", v, sizeof v) || v[0] < '0' || v[0] > '4' || v[1]) return -1;
    return v[0] - '0';
}

// Shadow refresh, "shadow_refresh=1..4" in settings.txt: the shadow map is redrawn every nth frame while the camera stands still
// (shadow_rate.inc). 1 = every frame, the game as it was, and the default.
// Texture uploads, "texture_uploads=full" in settings.txt: every animation texture upload sends the whole texture, as up to
// 1.2 Test 3. Anything else (or no line) = only the changed rows (1.2 Test 4 on). Read by tex_trace.inc.
// Task waits, "task_waits=fast" in settings.txt: worker threads stay awake a moment before sleeping (task_spin.inc) and an
// empty task queue is peeked without its lock. Anything else (or no line) = the game's own way: threads sleep at once, the
// queue is always locked. OFF by default since 2026-11-26 (1.3.24): measured 2026-11-11, no fps gain (133.4 off,
// 132.8 / 133.5 on) but processor use 535% against 338%, which can slow PCs with few cores or laptops.
int settings_fast_task_waits(void) {
    char v[16];
    return settings_get("task_waits", v, sizeof v) && !strcmp(v, "fast");
}
int settings_full_uploads(void) {
    char v[16];
    return settings_get("texture_uploads", v, sizeof v) && !strcmp(v, "full");
}
// "Show frame rate" tick box, show_fps=1|0 in settings.txt: a small frame-rate number drawn by the loader itself on every
// graphics route (fps_overlay.c). Off unless ticked.
int settings_show_fps(void) {
    char v[128];
    return settings_get("show_fps", v, sizeof v) && v[0] == '1';
}

// JGdaCaesar's Attila Engine Performance Fix, 64-bit part: plugins\perffix64.dll (CPU fixes + Flag Wave cloth banners in one
// DLL). Not shipped with this mod (its licence does not allow it): players get it from his Workshop item 3824249154, whose
// install.bat and 32-bit dinput8.dll copy it into attila64\plugins. Two ticks in the start window:
//   perf_fix=1|0   load perffix64.dll or leave it out (on unless unticked)
//   flag_wave=1|0  waving banners; plugins.inc turns this into flagwave_off.txt, the switch the DLL reads. The DLL only
//                  waves flags when a loaded pack carries flagwave\flag_wave.txt, which the 1212AD performance pack does.
int settings_perf_fix(void) {
    char v[128];
    return !(settings_get("perf_fix", v, sizeof v) && v[0] == '0');
}
int settings_flag_wave(void) {
    char v[128];
    return !(settings_get("flag_wave", v, sizeof v) && v[0] == '0');
}
// out = <exe folder>\plugins\perffix64.dll (which = 0) or the Workshop copy (which = 1): <game>\attila64 -> steamapps\workshop\...
void perffix_path(int which, char *out, size_t size) {
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    char *s = strrchr(exe, '\\');
    if (s) *s = 0;
    snprintf(out, size, which ? "%s\\..\\..\\..\\workshop\\content\\325610\\3824249154\\perffix64.dll" : "%s\\plugins\\perffix64.dll", exe);
}
// 1 = plugins\perffix64.dll is there or the Workshop copy is (plugins.inc copies it in at the start of the game).
static int perffix_available(void) {
    char p[MAX_PATH * 2];
    for (int i = 0; i < 2; i++) {
        perffix_path(i, p, sizeof p);
        if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) return 1;
    }
    return 0;
}

// JGdaCaesar's Upscaling and Framegen add-on (NVIDIA DLSS / AMD FSR upscaling and frame generation): <game>\FrameBridge,
// run by perffix64.dll (its bridge), so only with the engine fix ticked. Players get it from his Workshop item 3828791891;
// plugins.inc copies the folder in from there at the start of the game. The add-on's own switch is addon=on|off in
// FrameBridge\settings.txt (its F8 panel writes it too); the tick writes upscaling=1|0 here and plugins.inc puts it there.
// out = <game>\FrameBridge (which = 0) or the Workshop copy's FrameBridge folder (which = 1)
void framebridge_path(int which, char *out, size_t size) {
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    char *s = strrchr(exe, '\\');
    if (s) *s = 0;
    snprintf(out, size, which ? "%s\\..\\..\\..\\workshop\\content\\325610\\3828791891\\FrameBridge" : "%s\\..\\FrameBridge", exe);
}
static int framebridge_available(void) {
    char p[MAX_PATH * 2], h[MAX_PATH * 2 + 16];
    for (int i = 0; i < 2; i++) {
        framebridge_path(i, p, sizeof p);
        snprintf(h, sizeof h, "%s\\helper64.exe", p);
        if (GetFileAttributesA(h) != INVALID_FILE_ATTRIBUTES) return 1;
    }
    return 0;
}
// 1 = "addon=off" is in <game>\FrameBridge\settings.txt
static int framebridge_says_off(void) {
    char p[MAX_PATH * 2], line[256];
    framebridge_path(0, p, sizeof p);
    strncat(p, "\\settings.txt", sizeof p - strlen(p) - 1);
    FILE *f = fopen(p, "r");
    int off = 0;
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) if (!_strnicmp(line, "addon=off", 9)) off = 1;
    fclose(f);
    return off;
}
// upscaling=1|0; not set yet: what the add-on's own settings say
int settings_upscaling(void) {
    char v[128];
    if (settings_get("upscaling", v, sizeof v)) return v[0] != '0';
    return !framebridge_says_off();
}

int settings_shadow_every(void) {
    char v[128];
    if (!settings_get("shadow_refresh", v, sizeof v) || v[0] < '1' || v[0] > '4' || v[1]) return 1;
    return v[0] - '0';
}

// 0 = auto (looks like 1080p), otherwise the zoom factor (1 = the game's own size, the default).
float settings_ui_size(void) {
    int i = settings_size_index();
    return i == SIZE_AUTO ? 0.0f : g_sizes[i].percent / 100.0f;
}

// The log of the last game. This window opens before the engine starts a new log, so attila64_log.txt is still the one
// from the game that just ended: the right moment to send it after a crash.
static void last_log_path(char *out, size_t size) {
    GetModuleFileNameA(NULL, out, (DWORD)size);
    char *s = strrchr(out, '\\');
    snprintf(s ? s + 1 : out, size - (size_t)(s ? s + 1 - out : 0), "attila64_log.txt");
}

// The whole log as text (the caller frees it), NULL when there is none. Logs are small; anything past 8 MB is left out.
static char *last_log_text(void) {
    char path[MAX_PATH + 32];
    last_log_path(path, MAX_PATH);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 8u << 20, n = 0;
    char *text = malloc(cap + 1);
    if (text) { n = fread(text, 1, cap, f); text[n] = 0; }
    fclose(f);
    if (text && !n) { free(text); text = NULL; }
    return text;
}

static int last_game_crashed(void) {
    char *text = last_log_text();
    int crashed = text && (!strncmp(text, "CRASH ", 6) || strstr(text, "\nCRASH ") != NULL);   // the crash handler's own lines only
    free(text);
    return crashed;
}

static void copy_last_log(HWND w) {
    char *text = last_log_text();
    if (!text) {
        MessageBoxA(w, "There is no log yet. One is written each time the game runs.", "64-bit Attila", MB_ICONINFORMATION);
        return;
    }
    size_t n = strlen(text) + 1;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, n);
    int ok = 0;
    if (mem) {
        char *p = GlobalLock(mem);
        if (p) { memcpy(p, text, n); GlobalUnlock(mem); }
        if (p && OpenClipboard(w)) {
            EmptyClipboard();
            ok = SetClipboardData(CF_TEXT, mem) != NULL;  // the clipboard owns the memory once this succeeds
            CloseClipboard();
        }
        if (!ok) GlobalFree(mem);
    }
    free(text);
    if (ok) MessageBoxA(w, "The log of the last game is copied.\nPaste it with Ctrl+V where you report the problem.\n\n"
                           "If it is too long to paste there, press \"Show log file\" and send the file.", "64-bit Attila", MB_ICONINFORMATION);
    else MessageBoxA(w, "Could not copy the log. Press \"Show log file\" and send the file instead.", "64-bit Attila", MB_ICONWARNING);
}

#include "version.h"
#include "crash_send.inc"   // the "send the crash report?" question

// Opens the folder with the log selected, so it can be attached or uploaded.
static void show_log_file(HWND w) {
    char path[MAX_PATH + 32], cmd[MAX_PATH + 96];
    last_log_path(path, MAX_PATH);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxA(w, "There is no log yet. One is written each time the game runs.", "64-bit Attila", MB_ICONINFORMATION);
        return;
    }
    snprintf(cmd, sizeof cmd, "explorer.exe /select,\"%s\"", path);
    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof si;
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
}

static Mod *find_mod(const char *name) {
    for (int i = 0; i < g_nmods; i++) if (!_stricmp(g_mods[i].name, name)) return &g_mods[i];
    return NULL;
}

static void add_mod(const char *name, const char *dir) {
    if (g_nmods >= MAX_MODS || find_mod(name)) return;
    Mod *m = &g_mods[g_nmods++];
    snprintf(m->name, sizeof m->name, "%s", name);
    snprintf(m->dir, sizeof m->dir, "%s", dir);
}

static void scan_folder(const char *dir) {
    char pat[MAX_PATH * 2];
    snprintf(pat, sizeof pat, "%s/*.pack", dir);
    WIN32_FIND_DATAA f;
    HANDLE h = FindFirstFileA(pat, &f);
    if (h == INVALID_HANDLE_VALUE) return;
    do add_mod(f.cFileName, dir); while (FindNextFileA(h, &f));
    FindClose(h);
}

// A pack in data counts as a mod when the game's manifest does not list it and its header says mod (3) or movie (4); DLC packs say otherwise.
static int data_pack_is_mod(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t h[8] = {0};
    size_t n = fread(h, 1, 8, f);
    fclose(f);
    if (n != 8 || memcmp(h, "PFH", 3)) return 0;
    int type = h[4] & 0xF;
    return type == 3 || type == 4;
}

static void scan_data(void) {
    char *manifest = NULL;
    FILE *mf = fopen("data/manifest.txt", "rb");
    if (mf) {
        fseek(mf, 0, SEEK_END);
        long n = ftell(mf);
        fseek(mf, 0, SEEK_SET);
        manifest = n > 0 ? malloc((size_t)n + 1) : NULL;
        if (manifest) manifest[fread(manifest, 1, (size_t)n, mf)] = 0;
        fclose(mf);
    }
    WIN32_FIND_DATAA f;
    HANDLE h = FindFirstFileA("data/*.pack", &f);
    if (h == INVALID_HANDLE_VALUE) { free(manifest); return; }
    do {
        char path[MAX_PATH + 8], line[MAX_PATH + 4];
        snprintf(path, sizeof path, "data/%s", f.cFileName);
        snprintf(line, sizeof line, "%s\t", f.cFileName);
        if (manifest && strstr(manifest, line)) continue;
        if (data_pack_is_mod(path)) add_mod(f.cFileName, "");
    } while (FindNextFileA(h, &f));
    FindClose(h);
    free(manifest);
}

static void scan_workshop(void) {
    char root[MAX_PATH];
    if (!GetFullPathNameA("..\\..\\workshop\\content\\325610", sizeof root, root, NULL)) return;
    for (char *c = root; *c; c++) if (*c == '\\') *c = '/';
    char pat[MAX_PATH + 4];
    snprintf(pat, sizeof pat, "%s/*", root);
    WIN32_FIND_DATAA f;
    HANDLE h = FindFirstFileA(pat, &f);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || f.cFileName[0] == '.') continue;
        char dir[MAX_PATH * 2];
        snprintf(dir, sizeof dir, "%s/%s", root, f.cFileName);
        scan_folder(dir);
    } while (FindNextFileA(h, &f));
    FindClose(h);
}

// Text between the first pair of double quotes in a line.
static int quoted(const char *line, char *out, size_t size) {
    const char *a = strchr(line, '"'), *b = a ? strchr(a + 1, '"') : NULL;
    if (!b || (size_t)(b - a - 1) >= size) return 0;
    memcpy(out, a + 1, (size_t)(b - a - 1));
    out[b - a - 1] = 0;
    return 1;
}

static int by_name(const void *a, const void *b) { return _stricmp(((const Mod *)a)->name, ((const Mod *)b)->name); }

static void load_all(void) {
    g_mods = calloc(MAX_MODS, sizeof(Mod));
    g_used = calloc(MAX_MODS, sizeof *g_used);
    g_other = calloc(1, 1);
    static char used_names[MAX_MODS][160];
    int nnames = 0;
    FILE *f = fopen(mod_list_path(), "r");
    if (f) {
        char line[2048], text[1024];
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "add_working_directory", 21)) { if (quoted(line, text, sizeof text)) scan_folder(text); }
            else if (!strncmp(line, "mod ", 4)) {
                if (quoted(line, text, sizeof text) && nnames < MAX_MODS) snprintf(used_names[nnames++], 160, "%s", text);
            } else if (line[0] && line[0] != '\n' && line[0] != '\r') {
                size_t have = strlen(g_other), add = strlen(line);
                g_other = realloc(g_other, have + add + 1);
                memcpy(g_other + have, line, add + 1);
            }
        }
        fclose(f);
    }
    scan_data();
    scan_workshop();
    // ticked mods first in their saved order, the rest by name
    Mod *sorted = calloc(MAX_MODS, sizeof(Mod));
    int n = 0;
    for (int i = 0; i < nnames; i++) {
        snprintf(g_used[g_nused++], 160, "%s", used_names[i]);
        Mod *m = find_mod(used_names[i]);
        if (m && !m->on) { m->on = 1; sorted[n++] = *m; }
    }
    int first_rest = n;
    for (int i = 0; i < g_nmods; i++) if (!g_mods[i].on) sorted[n++] = g_mods[i];
    qsort(sorted + first_rest, (size_t)(n - first_rest), sizeof(Mod), by_name);
    free(g_mods);
    g_mods = sorted;
    g_nmods = n;
}

// ---- Mod picture (2026-11-22, the user: "show picture of the mod"): the picture next to the pack with the pack's name
// (<name>.png / .jpg), as normal Attila's launcher shows it: in the Workshop item's folder or in data. A Workshop folder
// without one shows its first .png / .jpg. Drawn with GDI+ (gdiplus.dll, loaded when the window opens; no link change).
typedef struct { UINT32 version; void *debug_cb; BOOL no_thread; BOOL no_codecs; } GpStartIn;
static struct {
    HMODULE dll;
    ULONG_PTR token;
    int (WINAPI *startup)(ULONG_PTR *, const GpStartIn *, void *);
    void (WINAPI *shutdown)(ULONG_PTR);
    int (WINAPI *load)(const WCHAR *, void **);
    int (WINAPI *width)(void *, UINT *);
    int (WINAPI *height)(void *, UINT *);
    int (WINAPI *from_hdc)(HDC, void **);
    int (WINAPI *interp)(void *, int);
    int (WINAPI *draw)(void *, void *, INT, INT, INT, INT);
    int (WINAPI *del_graphics)(void *);
    int (WINAPI *dispose)(void *);
} gp;
static void *g_pic;                 // the shown mod's picture (GDI+ image), NULL = none
static char g_pic_name[160];        // whose picture is shown
static HWND g_pic_box, g_pic_label;
static int g_moving;                // rows are being rewritten: selection changes are not picture changes

static void gdiplus_start(void) {
    GpStartIn in = {1, NULL, FALSE, FALSE};
    if (!(gp.dll = LoadLibraryA("gdiplus.dll"))) return;
#define GP(f, n) *(FARPROC *)&gp.f = GetProcAddress(gp.dll, n)
    GP(startup, "GdiplusStartup"); GP(shutdown, "GdiplusShutdown"); GP(load, "GdipLoadImageFromFile");
    GP(width, "GdipGetImageWidth"); GP(height, "GdipGetImageHeight"); GP(from_hdc, "GdipCreateFromHDC");
    GP(interp, "GdipSetInterpolationMode"); GP(draw, "GdipDrawImageRectI"); GP(del_graphics, "GdipDeleteGraphics");
    GP(dispose, "GdipDisposeImage");
#undef GP
    if (!gp.startup || !gp.shutdown || !gp.load || !gp.width || !gp.height || !gp.from_hdc || !gp.interp || !gp.draw ||
        !gp.del_graphics || !gp.dispose || gp.startup(&gp.token, &in, NULL) != 0) {
        FreeLibrary(gp.dll);
        memset(&gp, 0, sizeof gp);
    }
}

static void *g_theme_img[8];   // the themes' pictures (GDI+ images), each loaded once (themes, further down)
static void gdiplus_stop(void) {
    if (g_pic) { gp.dispose(g_pic); g_pic = NULL; }
    for (int i = 0; i < 8; i++) if (g_theme_img[i]) { gp.dispose(g_theme_img[i]); g_theme_img[i] = NULL; }
    if (gp.dll) { gp.shutdown(gp.token); FreeLibrary(gp.dll); memset(&gp, 0, sizeof gp); }
}

static int file_exists(const char *p) {
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// The picture file of a mod; 0 when it has none.
static int picture_path(const Mod *m, char *out, size_t size) {
    static const char *const exts[] = {".png", ".jpg", ".jpeg"};
    const char *dir = m->dir[0] ? m->dir : "data";
    char base[160];
    snprintf(base, sizeof base, "%s", m->name);
    char *dot = strrchr(base, '.');
    if (dot && !_stricmp(dot, ".pack")) *dot = 0;
    for (int i = 0; i < 3; i++) {
        snprintf(out, size, "%s/%s%s", dir, base, exts[i]);
        if (file_exists(out)) return 1;
    }
    if (!strstr(m->dir, "/325610/")) return 0;   // only a Workshop item's own folder holds just that mod's files
    for (int i = 0; i < 3; i++) {
        char pat[MAX_PATH * 2];
        snprintf(pat, sizeof pat, "%s/*%s", dir, exts[i]);
        WIN32_FIND_DATAA f;
        HANDLE h = FindFirstFileA(pat, &f);
        if (h == INVALID_HANDLE_VALUE) continue;
        FindClose(h);
        snprintf(out, size, "%s/%s", dir, f.cFileName);
        return 1;
    }
    return 0;
}

static void picture_show(int i) {
    if (i < 0 || i >= g_nmods || !g_pic_box) return;
    if (!_stricmp(g_pic_name, g_mods[i].name)) return;
    snprintf(g_pic_name, sizeof g_pic_name, "%s", g_mods[i].name);
    if (g_pic) { gp.dispose(g_pic); g_pic = NULL; }
    char path[MAX_PATH * 2];
    WCHAR wpath[MAX_PATH * 2], full[MAX_PATH * 2];
    if (gp.dll && picture_path(&g_mods[i], path, sizeof path) &&
        MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH * 2) &&
        GetFullPathNameW(wpath, MAX_PATH * 2, full, NULL)) {
        if (gp.load(full, &g_pic) != 0) g_pic = NULL;
    }
    SetWindowTextA(g_pic_label, g_mods[i].name);
    InvalidateRect(g_pic_box, NULL, TRUE);
}

static COLORREF theme_field(void), theme_edge(void), theme_dim(void);   // the theme's colours, 0xFFFFFFFF = plain
static void picture_draw(const DRAWITEMSTRUCT *d) {
    RECT r = d->rcItem;
    if (theme_field() != 0xFFFFFFFF) {
        HBRUSH f = CreateSolidBrush(theme_field()), e = CreateSolidBrush(theme_edge());
        FillRect(d->hDC, &r, f);
        FrameRect(d->hDC, &r, e);
        DeleteObject(f); DeleteObject(e);
    } else {
        FillRect(d->hDC, &r, GetSysColorBrush(COLOR_WINDOW));
        FrameRect(d->hDC, &r, GetSysColorBrush(COLOR_BTNSHADOW));
    }
    UINT iw = 0, ih = 0;
    if (g_pic && !gp.width(g_pic, &iw) && !gp.height(g_pic, &ih) && iw && ih) {
        int bw = r.right - r.left - 4, bh = r.bottom - r.top - 4;
        int w = bw, h = (int)((double)ih * bw / iw);
        if (h > bh) { h = bh; w = (int)((double)iw * bh / ih); }
        void *g = NULL;
        if (!gp.from_hdc(d->hDC, &g)) {
            gp.interp(g, 7);   // high quality bicubic
            gp.draw(g, g_pic, r.left + 2 + (bw - w) / 2, r.top + 2 + (bh - h) / 2, w, h);
            gp.del_graphics(g);
        }
        return;
    }
    SetBkMode(d->hDC, TRANSPARENT);
    SetTextColor(d->hDC, theme_dim() != 0xFFFFFFFF ? theme_dim() : GetSysColor(COLOR_GRAYTEXT));
    SelectObject(d->hDC, GetStockObject(DEFAULT_GUI_FONT));
    DrawTextA(d->hDC, g_pic_name[0] ? "This mod has no picture" : "Select a mod to see its picture", -1, &r,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// One row of the list from g_mods[i] (the row must exist).
static void list_set_row(int i) {
    LVITEMA it = {0};
    it.mask = LVIF_TEXT;
    it.iItem = i;
    it.pszText = g_mods[i].name;
    SendMessageA(g_list, LVM_SETITEMA, 0, (LPARAM)&it);
    ListView_SetCheckState(g_list, i, g_mods[i].on);
    const char *slash = strstr(g_mods[i].dir, "/325610/");
    char where[64];
    if (slash) snprintf(where, sizeof where, "Workshop %s", slash + 8);
    else snprintf(where, sizeof where, "%s", g_mods[i].dir[0] ? "Workshop" : "Game data folder");
    LVITEMA sub = {0};
    sub.iSubItem = 1;
    sub.pszText = where;
    SendMessageA(g_list, LVM_SETITEMTEXTA, (WPARAM)i, (LPARAM)&sub);
}

static void list_fill(void) {
    ListView_DeleteAllItems(g_list);
    for (int i = 0; i < g_nmods; i++) {
        LVITEMA it = {0};
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = g_mods[i].name;
        SendMessageA(g_list, LVM_INSERTITEMA, 0, (LPARAM)&it);
        list_set_row(i);
    }
}

static void read_checks(void) {
    for (int i = 0; i < g_nmods; i++) g_mods[i].on = ListView_GetCheckState(g_list, i) != 0;
}

// Moves the mod in row `from` to row `to`; the rows between shift by one. Only the changed rows are rewritten, so the
// list keeps its scroll position (dragging, and Move up / down).
static void move_mod(int from, int to) {
    if (from < 0 || to < 0 || from >= g_nmods || to >= g_nmods || from == to) return;
    read_checks();
    Mod t = g_mods[from];
    if (from < to) memmove(&g_mods[from], &g_mods[from + 1], (size_t)(to - from) * sizeof(Mod));
    else memmove(&g_mods[to + 1], &g_mods[to], (size_t)(from - to) * sizeof(Mod));
    g_mods[to] = t;
    g_moving = 1;
    int lo = from < to ? from : to, hi = from < to ? to : from;
    for (int i = lo; i <= hi; i++) list_set_row(i);
    ListView_SetItemState(g_list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(g_list, to, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    g_moving = 0;
    ListView_EnsureVisible(g_list, to, FALSE);
}

static void move_selected(int by) {
    int i = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (i < 0) return;
    move_mod(i, i + by);
    SetFocus(g_list);
}

static void move_selected_to_top(void) {   // 1.3.22, a player's request: "move to top"
    int i = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (i <= 0) return;
    move_mod(i, 0);
    SetFocus(g_list);
}

// ---- Normal Attila's own launcher (1.3.22). With our Launch Options, Steam's Play starts us and hands us the path of CA's
// launcher, so its Mod Manager (the only way to upload Attila mods to the Workshop) could not be reached any more (a modder's
// report, 2026-11-25). This button starts that launcher as Attila (SteamAppId 325610), and this exe waits for it to close so
// Steam keeps showing Attila as running. loader.c fills g_ca_launcher from Steam's arguments; otherwise launcher\launcher.exe
// in the game folder (the working folder here) is used.
char g_ca_launcher[MAX_PATH];
static HANDLE g_launcher_proc;

static int start_ca_launcher(HWND w) {
    char exe[MAX_PATH], dir[MAX_PATH];
    if (!g_ca_launcher[0] || GetFileAttributesA(g_ca_launcher) == INVALID_FILE_ATTRIBUTES)
        GetFullPathNameA("launcher\\launcher.exe", sizeof g_ca_launcher, g_ca_launcher, NULL);
    snprintf(exe, sizeof exe, "%s", g_ca_launcher);
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxA(w, "Could not find normal Attila's launcher (launcher\\launcher.exe in the game folder).\n\n"
                       "To use it from Steam's Play button instead: Steam > right-click Attila > Properties > General, clear Launch Options.",
                    "64-bit Attila", MB_ICONWARNING);
        return 0;
    }
    snprintf(dir, sizeof dir, "%s", exe);
    char *s = strrchr(dir, '\\');
    if (s) *s = 0;
    SetEnvironmentVariableA("SteamAppId", "325610");
    SetEnvironmentVariableA("SteamGameId", "325610");
    char cmd[MAX_PATH + 4];
    snprintf(cmd, sizeof cmd, "\"%s\"", exe);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        char m[MAX_PATH + 160];
        snprintf(m, sizeof m, "Could not start normal Attila's launcher (error %lu):\n%s", GetLastError(), exe);
        MessageBoxA(w, m, "64-bit Attila", MB_ICONWARNING);
        return 0;
    }
    CloseHandle(pi.hThread);
    g_launcher_proc = pi.hProcess;
    return 1;
}

// ---- Dragging a mod up or down the list (2026-11-22, the user: "make it that you can drag the mods"). The row follows the
// mouse while the button is held; above or below the list it scrolls.
static int g_drag = -1;             // the dragged mod's row, -1 = no drag
enum { DRAG_TIMER = 1 };

static void drag_to(POINT pt) {    // pt in the list's client coordinates
    RECT rc, hr;
    GetClientRect(g_list, &rc);
    int top = 0;
    HWND header = ListView_GetHeader(g_list);
    if (header && GetWindowRect(header, &hr)) top = hr.bottom - hr.top;
    LVHITTESTINFO ht = {0};
    ht.pt.x = 4;
    ht.pt.y = pt.y < top ? top : pt.y >= rc.bottom ? rc.bottom - 1 : pt.y;
    int t = ListView_HitTest(g_list, &ht);
    if (t < 0) t = pt.y < top ? ListView_GetTopIndex(g_list) : g_nmods - 1;   // under the last row
    if (t != g_drag) { move_mod(g_drag, t); g_drag = t; }
}

static void drag_scroll_tick(void) {
    POINT pt;
    RECT rc, ir, hr;
    GetCursorPos(&pt);
    ScreenToClient(g_list, &pt);
    GetClientRect(g_list, &rc);
    int top = 0, row = 18;
    HWND header = ListView_GetHeader(g_list);
    if (header && GetWindowRect(header, &hr)) top = hr.bottom - hr.top;
    if (g_nmods && ListView_GetItemRect(g_list, 0, &ir, LVIR_BOUNDS)) row = ir.bottom - ir.top;
    if (pt.y < top) ListView_Scroll(g_list, 0, -row);
    else if (pt.y >= rc.bottom) ListView_Scroll(g_list, 0, row);
    else return;
    drag_to(pt);
}

static void drag_end(HWND w) {
    if (g_drag < 0) return;
    g_drag = -1;
    KillTimer(w, DRAG_TIMER);
    SetCursor(LoadCursor(NULL, IDC_ARROW));
}

static void set_all(int on) {
    for (int i = 0; i < g_nmods; i++) ListView_SetCheckState(g_list, i, on);
}

// Writes the own mod list (mod_list.txt) only when it really changed. Normal Attila's used_mods.txt is not touched.
static void save_list(void) {
    int same = 1, k = 0;
    for (int i = 0; i < g_nmods; i++) {
        if (!g_mods[i].on) continue;
        if (k >= g_nused || _stricmp(g_used[k], g_mods[i].name)) same = 0;
        k++;
    }
    if (k != g_nused) same = 0;
    if (same && GetFileAttributesA(mod_list_path()) != INVALID_FILE_ATTRIBUTES) return;
    char tmp[MAX_PATH + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", mod_list_path());
    FILE *f = fopen(tmp, "w");
    if (f) {
        for (int i = 0; i < g_nmods; i++) {
            if (!g_mods[i].on || !g_mods[i].dir[0]) continue;
            int seen = 0;
            for (int j = 0; j < i; j++) if (g_mods[j].on && !_stricmp(g_mods[j].dir, g_mods[i].dir)) seen = 1;
            if (!seen) fprintf(f, "add_working_directory \"%s\";\n", g_mods[i].dir);
        }
        for (int i = 0; i < g_nmods; i++) if (g_mods[i].on) fprintf(f, "mod \"%s\";\n", g_mods[i].name);
        fputs(g_other, f);
        int ok = !ferror(f);
        fclose(f);
        if (ok && MoveFileExA(tmp, mod_list_path(), MOVEFILE_REPLACE_EXISTING)) return;
    }
    DeleteFileA(tmp);
    MessageBoxA(NULL, "Could not save the mod list. The game will start with the previous list.", "64-bit Attila", MB_ICONWARNING);
}

// ---- Full screen (2026-11-24, the user: "give the option for the mod launcher to go full screen"): the window can be
// maximized (title-bar button, double-click on the title, F11) or dragged bigger; picker_maximized=1 in settings.txt
// opens it maximized next time. Every control keeps the place it was made at for the normal 912 x 638 size; the extra
// room goes to the mod list (wider and taller) and the mod picture (bigger square), the columns right of the list move
// along, and the log / Play / Quit buttons stay at the bottom.
enum { BASE_W = 912, BASE_H = 638, COL_X = 524, PIC_X = 644, PIC_SIZE = 256, BOTTOM_Y = 479 };
static struct { HWND h; int x, y, w, h_; DWORD style; int push; } g_lay[64];   // push = a push button (owner-drawn in a theme)
static int g_nlay;
static void theme_rebuild(HWND w);

static HWND add_control(HWND parent, const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent, (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    if (c && g_nlay < (int)(sizeof g_lay / sizeof g_lay[0])) {
        g_lay[g_nlay].h = c; g_lay[g_nlay].x = x; g_lay[g_nlay].y = y; g_lay[g_nlay].w = w; g_lay[g_nlay].h_ = h;
        g_lay[g_nlay].style = style;
        g_lay[g_nlay].push = !_stricmp(cls, "BUTTON") && ((style & BS_TYPEMASK) == BS_PUSHBUTTON || (style & BS_TYPEMASK) == BS_DEFPUSHBUTTON);
        g_nlay++;
    }
    return c;
}

static void layout(HWND w) {
    RECT rc;
    GetClientRect(w, &rc);
    int dx = rc.right - BASE_W, dy = rc.bottom - BASE_H;
    if (dx < 0) dx = 0;
    if (dy < 0) dy = 0;
    int pic = dx / 2 < dy ? dx / 2 : dy;   // the picture grows as a square, by up to half the extra width
    int shift = dx - pic;                   // the rest widens the list; everything right of it moves by that much
    HDWP d = BeginDeferWindowPos(g_nlay);
    for (int i = 0; i < g_nlay; i++) {
        if (!IsWindow(g_lay[i].h)) continue;   // e.g. "Use DirectX 11 again" after it was pressed
        int x = g_lay[i].x, y = g_lay[i].y, cw = g_lay[i].w, ch = g_lay[i].h_;
        if (g_lay[i].h == g_list) { cw += shift; ch += dy; }
        else if (x < COL_X) cw += dx;                                  // the text across the top
        else if (x < PIC_X) { x += shift; if (y >= BOTTOM_Y) y += dy; } // the button column
        else {                                                         // the picture column
            x += shift;
            if (g_lay[i].w >= 200) cw += pic;   // the picture-wide labels and ticks; the Theme box keeps its size
            if (g_lay[i].h == g_pic_box) ch += pic;
            else if (y > 66) y += pic;   // under the picture
        }
        if (d) d = DeferWindowPos(d, g_lay[i].h, NULL, x, y, cw, ch, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (d) EndDeferWindowPos(d);
    ListView_SetColumnWidth(g_list, 0, 320 + shift);
    theme_rebuild(w);   // the background picture is made for the window's size
    InvalidateRect(w, NULL, TRUE);
    InvalidateRect(g_pic_box, NULL, TRUE);   // the picture is drawn to the box's size
}

static void toggle_full_screen(HWND w) { ShowWindow(w, IsZoomed(w) ? SW_RESTORE : SW_MAXIMIZE); }

// ---- Themes (2026-11-24, the user: "Medieval stained glass for background islamic art and then mongol painting"): the
// Theme box under the engine-fix ticks, theme= in settings.txt. Plain grey = the window as before (nothing below runs).
// The others: a background picture the size of the window (art, then dark see-through panels behind the button column,
// the picture column and the top text, with edges), shown by WM_ERASEBKGND; labels and tick boxes draw their text over
// the matching piece of it (pattern brush); the list gets the theme's colours and a coloured selected row (custom draw);
// push buttons are owner-drawn. The art is a real picture in <exe folder>\themes (art_photo, GDI+ like the mod
// pictures; sources in themes\CREDITS.txt): glass.jpg Reims Cathedral's 13th-century north rose window, islamic.jpg an
// Ottoman Iznik tile panel (Met Museum, CC0), mongol.jpg "Mongol military manoeuvres" (Diez Albums, early 14th c.).
// A missing file: stained glass and tiles are drawn instead (art_glass, art_islamic), the Mongol theme looks like Dark.
enum { TH_PLAIN, TH_DARK, TH_GLASS, TH_ISLAMIC, TH_MONGOL, N_THEMES };
static const char *const g_theme_names[N_THEMES] = {"Plain grey", "Dark", "Stained glass", "Islamic tiles", "Mongol painting"};
static const char *const g_theme_keys[N_THEMES] = {"plain", "dark", "glass", "islamic", "mongol"};
typedef struct { COLORREF panel; int alpha; COLORREF field, text, dim, sel, btn, edge, play; } Theme;
static const Theme g_themes[N_THEMES] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0},
    {RGB(38, 39, 42), 0, RGB(31, 32, 35), RGB(214, 214, 214), RGB(140, 140, 140), RGB(58, 74, 94), RGB(54, 55, 59), RGB(84, 85, 90), RGB(74, 109, 148)},
    {RGB(20, 16, 24), 212, RGB(24, 20, 28), RGB(230, 226, 216), RGB(169, 163, 184), RGB(90, 63, 115), RGB(42, 36, 48), RGB(201, 161, 74), RGB(122, 42, 42)},
    {RGB(10, 24, 38), 214, RGB(14, 28, 42), RGB(230, 226, 216), RGB(159, 179, 196), RGB(31, 95, 107), RGB(22, 50, 74), RGB(216, 179, 90), RGB(138, 106, 36)},
    {RGB(36, 22, 12), 214, RGB(36, 22, 12), RGB(232, 224, 210), RGB(196, 173, 136), RGB(122, 31, 31), RGB(60, 40, 24), RGB(201, 161, 74), RGB(122, 31, 31)},
};
static int g_theme;
static HWND g_theme_box;
static HDC g_bg_dc;
static HBITMAP g_bg_bmp, g_bg_old;
static HBRUSH g_bg_brush, g_field_brush;
static int g_photo_tried;   // bit per theme: its picture file was looked for
static int g_list_image;      // the list shows the art behind its rows
static HDC g_list_dc;
static HBITMAP g_list_bmp, g_list_old_bmp;   // that art, the list's size
static WNDPROC g_list_old;     // the list's own window procedure (its header is drawn in the theme)

static void theme_free(void) {
    if (g_list_dc) { SelectObject(g_list_dc, g_list_old_bmp); DeleteDC(g_list_dc); g_list_dc = NULL; }
    if (g_list_bmp) { DeleteObject(g_list_bmp); g_list_bmp = NULL; }
    if (g_bg_brush) { DeleteObject(g_bg_brush); g_bg_brush = NULL; }
    if (g_field_brush) { DeleteObject(g_field_brush); g_field_brush = NULL; }
    if (g_bg_dc) { SelectObject(g_bg_dc, g_bg_old); DeleteDC(g_bg_dc); g_bg_dc = NULL; }
    if (g_bg_bmp) { DeleteObject(g_bg_bmp); g_bg_bmp = NULL; }
}

static unsigned g_seed;
static int rnd(int n) { g_seed = g_seed * 1103515245u + 12345u; return (int)((g_seed >> 16) % (unsigned)n); }

static void art_glass(HDC dc, int W, int H) {   // leaded panes: a jittered grid cut into triangles
    static const COLORREF cols[] = {RGB(122, 20, 36), RGB(18, 63, 122), RGB(31, 106, 58), RGB(201, 150, 42), RGB(90, 42, 115), RGB(29, 94, 122), RGB(154, 58, 20), RGB(150, 24, 40)};
    enum { CELL = 84 };
    int nx = W / CELL + 2, ny = H / CELL + 2;
    POINT *p = (POINT *)malloc(sizeof(POINT) * (size_t)(nx + 1) * (size_t)(ny + 1));
    if (!p) return;
    g_seed = 1212;
    for (int j = 0; j <= ny; j++)
        for (int i = 0; i <= nx; i++) {
            POINT *q = &p[j * (nx + 1) + i];
            q->x = i * CELL + (i && i < nx ? rnd(CELL / 2) - CELL / 4 : 0);
            q->y = j * CELL + (j && j < ny ? rnd(CELL / 2) - CELL / 4 : 0);
        }
    HPEN lead = CreatePen(PS_SOLID, 5, RGB(18, 18, 18)), op = (HPEN)SelectObject(dc, lead);
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++) {
            POINT a = p[j * (nx + 1) + i], b = p[j * (nx + 1) + i + 1], c = p[(j + 1) * (nx + 1) + i + 1], d = p[(j + 1) * (nx + 1) + i];
            POINT t1[3] = {a, b, c}, t2[3] = {a, c, d};
            for (int k = 0; k < 2; k++) {
                HBRUSH br = CreateSolidBrush(cols[rnd(8)]), ob = (HBRUSH)SelectObject(dc, br);
                Polygon(dc, k ? t2 : t1, 3);
                SelectObject(dc, ob);
                DeleteObject(br);
            }
        }
    SelectObject(dc, op);
    DeleteObject(lead);
    free(p);
}

static void art_islamic(HDC dc, int W, int H) {   // eight-pointed stars (two squares) on lapis, gold lines
    enum { T = 64, R = 20 };
    RECT all = {0, 0, W, H};
    HBRUSH bg = CreateSolidBrush(RGB(18, 58, 92)), star = CreateSolidBrush(RGB(29, 107, 116)), gold = CreateSolidBrush(RGB(216, 179, 90));
    FillRect(dc, &all, bg);
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(216, 179, 90)), op = (HPEN)SelectObject(dc, pen);
    HBRUSH ob = (HBRUSH)SelectObject(dc, star);
    for (int y = 0; y < H + T; y += T)
        for (int x = 0; x < W + T; x += T) {
            int cx = x + T / 2, cy = y + T / 2, d = (R * 141) / 100;   // the turned square's half-diagonal
            POINT sq[4] = {{cx - R, cy - R}, {cx + R, cy - R}, {cx + R, cy + R}, {cx - R, cy + R}};
            POINT dm[4] = {{cx, cy - d}, {cx + d, cy}, {cx, cy + d}, {cx - d, cy}};
            SelectObject(dc, star);
            Polygon(dc, sq, 4);
            Polygon(dc, dm, 4);
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            Polygon(dc, sq, 4);   // outline again so both squares' lines show inside the star
            SelectObject(dc, gold);
            Ellipse(dc, cx - 5, cy - 5, cx + 6, cy + 6);
            POINT k[4] = {{x, y - 7}, {x + 7, y}, {x, y + 7}, {x - 7, y}};   // small gold diamond where four tiles meet
            Polygon(dc, k, 4);
        }
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen); DeleteObject(bg); DeleteObject(star); DeleteObject(gold);
}

// The theme's picture <exe folder>\themes\<file> (see themes\CREDITS.txt), filling the window (the rest cropped).
// 0 = no picture (file missing or GDI+ not there).
static int art_photo(HDC dc, int W, int H, int theme) {
    static const char *const files[N_THEMES] = {NULL, NULL, "glass.jpg", "islamic.jpg", "mongol.jpg"};
    if (!files[theme]) return 0;
    if (!(g_photo_tried & (1 << theme)) && gp.dll) {
        g_photo_tried |= 1 << theme;
        char path[MAX_PATH * 2];
        WCHAR wp[MAX_PATH * 2];
        GetModuleFileNameA(NULL, path, MAX_PATH);
        char *s = strrchr(path, '\\');
        snprintf(s ? s + 1 : path, MAX_PATH, "themes\\%s", files[theme]);
        if (MultiByteToWideChar(CP_ACP, 0, path, -1, wp, MAX_PATH * 2) && gp.load(wp, &g_theme_img[theme]) != 0) g_theme_img[theme] = NULL;
    }
    void *img = g_theme_img[theme];
    UINT iw = 0, ih = 0;
    if (!img || gp.width(img, &iw) || gp.height(img, &ih) || !iw || !ih) return 0;
    double sc = (double)W / iw > (double)H / ih ? (double)W / iw : (double)H / ih;   // fill the window, crop the rest
    int dw = (int)(iw * sc + 1), dh = (int)(ih * sc + 1);
    void *g = NULL;
    if (gp.from_hdc(dc, &g)) return 0;
    gp.interp(g, 7);
    gp.draw(g, img, (W - dw) / 2, (H - dh) / 2, dw, dh);
    gp.del_graphics(g);
    return 1;
}

// The rectangle (in w's client coordinates) around every control first placed in [x0, x1), grown by `pad`; 0 = none.
static int group_rect(HWND w, int x0, int x1, int pad, RECT *out) {
    int any = 0;
    for (int i = 0; i < g_nlay; i++) {
        if (g_lay[i].x < x0 || g_lay[i].x >= x1 || !IsWindow(g_lay[i].h) || !IsWindowVisible(g_lay[i].h)) continue;
        RECT r;
        GetWindowRect(g_lay[i].h, &r);
        MapWindowPoints(NULL, w, (POINT *)&r, 2);
        if (!any) *out = r; else UnionRect(out, out, &r);
        any = 1;
    }
    if (any) InflateRect(out, pad, pad);
    return any;
}

static void blend(unsigned char *bits, int W, int H, RECT r, COLORREF c, int a) {
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > W) r.right = W;
    if (r.bottom > H) r.bottom = H;
    for (int y = r.top; y < r.bottom; y++) {
        unsigned char *p = bits + ((size_t)y * (size_t)W + (size_t)r.left) * 4;
        for (int x = r.left; x < r.right; x++, p += 4) {
            p[0] = (unsigned char)((p[0] * (255 - a) + GetBValue(c) * a) / 255);
            p[1] = (unsigned char)((p[1] * (255 - a) + GetGValue(c) * a) / 255);
            p[2] = (unsigned char)((p[2] * (255 - a) + GetRValue(c) * a) / 255);
        }
    }
}

static void theme_rebuild(HWND w) {
    theme_free();
    g_list_image = 0;
    if (g_theme == TH_PLAIN || !g_list) return;
    const Theme *t = &g_themes[g_theme];
    g_field_brush = CreateSolidBrush(t->field);
    RECT rc;
    GetClientRect(w, &rc);
    int W = rc.right > 0 ? rc.right : 1, H = rc.bottom > 0 ? rc.bottom : 1;
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    g_bg_bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!g_bg_bmp || !bits) { theme_free(); return; }
    g_bg_dc = CreateCompatibleDC(NULL);
    g_bg_old = (HBITMAP)SelectObject(g_bg_dc, g_bg_bmp);
    RECT all = {0, 0, W, H};
    int art = 1;
    if (art_photo(g_bg_dc, W, H, g_theme)) {}
    else if (g_theme == TH_GLASS) art_glass(g_bg_dc, W, H);       // drawn instead when glass.jpg is missing
    else if (g_theme == TH_ISLAMIC) art_islamic(g_bg_dc, W, H);   // the same for islamic.jpg
    else art = 0;
    if (!art) { HBRUSH b = CreateSolidBrush(g_themes[TH_DARK].panel); FillRect(g_bg_dc, &all, b); DeleteObject(b); }
    // panels: top text, the button column, the picture column; a frame round the list
    RECT lr, panels[3];
    int np = 0;
    GetWindowRect(g_list, &lr);
    MapWindowPoints(NULL, w, (POINT *)&lr, 2);
    RECT top = {0, 0, W, lr.top - 6};
    panels[np++] = top;
    if (group_rect(w, COL_X, PIC_X, 6, &panels[np])) np++;
    if (group_rect(w, PIC_X, 100000, 8, &panels[np])) np++;
    GdiFlush();
    if (art) for (int i = 0; i < np; i++) blend((unsigned char *)bits, W, H, panels[i], t->panel, t->alpha);
    HPEN pen = CreatePen(PS_SOLID, 2, t->edge), op = (HPEN)SelectObject(g_bg_dc, pen);
    HBRUSH ob = (HBRUSH)SelectObject(g_bg_dc, GetStockObject(NULL_BRUSH));
    for (int i = 1; i < np; i++) Rectangle(g_bg_dc, panels[i].left, panels[i].top, panels[i].right, panels[i].bottom);
    Rectangle(g_bg_dc, lr.left - 3, lr.top - 3, lr.right + 3, lr.bottom + 3);
    SelectObject(g_bg_dc, ob);
    SelectObject(g_bg_dc, op);
    DeleteObject(pen);
    GdiFlush();
    g_bg_brush = CreatePatternBrush(g_bg_bmp);
    // The list shows the art too: its piece of the background, darkened so the names stay easy to read. The list gets no
    // background colour of its own and paints this picture when it erases (list_proc).
    int lw = lr.right - lr.left, lh = lr.bottom - lr.top;
    if (art && lw > 0 && lh > 0) {
        BITMAPINFO b2 = bi;
        b2.bmiHeader.biWidth = lw;
        b2.bmiHeader.biHeight = -lh;
        void *lb = NULL;
        g_list_bmp = CreateDIBSection(NULL, &b2, DIB_RGB_COLORS, &lb, NULL, 0);
        if (g_list_bmp && lb) {
            g_list_dc = CreateCompatibleDC(NULL);
            g_list_old_bmp = (HBITMAP)SelectObject(g_list_dc, g_list_bmp);
            BitBlt(g_list_dc, 0, 0, lw, lh, g_bg_dc, lr.left, lr.top, SRCCOPY);
            GdiFlush();
            RECT whole = {0, 0, lw, lh};
            blend((unsigned char *)lb, lw, lh, whole, t->field, 196);
            g_list_image = 1;
        } else if (g_list_bmp) { DeleteObject(g_list_bmp); g_list_bmp = NULL; }
    }
    ListView_SetBkColor(g_list, g_list_image ? CLR_NONE : t->field);
    ListView_SetTextBkColor(g_list, g_list_image ? CLR_NONE : t->field);
}

static LRESULT CALLBACK list_proc(HWND, UINT, WPARAM, LPARAM);
static void theme_apply(HWND w) {
    const Theme *t = &g_themes[g_theme];
    if (!g_list_old) g_list_old = (WNDPROC)SetWindowLongPtrA(g_list, GWLP_WNDPROC, (LONG_PTR)list_proc);
    int on = g_theme != TH_PLAIN;
    ListView_SetBkColor(g_list, on ? t->field : GetSysColor(COLOR_WINDOW));
    ListView_SetTextBkColor(g_list, on ? t->field : GetSysColor(COLOR_WINDOW));
    ListView_SetTextColor(g_list, on ? t->text : GetSysColor(COLOR_WINDOWTEXT));
    for (int i = 0; i < g_nlay; i++) {
        if (!g_lay[i].push || !IsWindow(g_lay[i].h)) continue;
        DWORD st = (DWORD)GetWindowLongA(g_lay[i].h, GWL_STYLE) & ~(DWORD)BS_TYPEMASK;
        SetWindowLongA(g_lay[i].h, GWL_STYLE, (LONG)(st | (on ? (DWORD)BS_OWNERDRAW : (g_lay[i].style & BS_TYPEMASK))));
    }
    theme_rebuild(w);
    InvalidateRect(ListView_GetHeader(g_list), NULL, TRUE);
    RedrawWindow(w, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

static COLORREF theme_field(void) { return g_theme == TH_PLAIN ? 0xFFFFFFFF : g_themes[g_theme].field; }
static COLORREF theme_edge(void) { return g_theme == TH_PLAIN ? 0xFFFFFFFF : g_themes[g_theme].edge; }
static COLORREF theme_dim(void) { return g_theme == TH_PLAIN ? 0xFFFFFFFF : g_themes[g_theme].dim; }

// Labels, tick boxes and the combo boxes in a theme (WM_CTLCOLOR*); NULL = plain, let Windows do it.
static HBRUSH theme_ctlcolor(HWND w, UINT msg, HDC dc, HWND c) {
    if (g_theme == TH_PLAIN || !g_field_brush) return NULL;
    const Theme *t = &g_themes[g_theme];
    char cls[32] = "";
    GetClassNameA(c, cls, sizeof cls);
    if (msg == WM_CTLCOLORLISTBOX || msg == WM_CTLCOLOREDIT || !_stricmp(cls, "ComboBox") || !_stricmp(cls, "ComboLBox")) {
        SetTextColor(dc, t->text);
        SetBkColor(dc, t->field);
        return g_field_brush;
    }
    SetTextColor(dc, IsWindowEnabled(c) ? t->text : t->dim);
    SetBkMode(dc, TRANSPARENT);
    if (!g_bg_brush) { SetBkColor(dc, t->field); return g_field_brush; }
    POINT p = {0, 0};
    MapWindowPoints(c, w, &p, 1);
    SetBrushOrgEx(dc, -p.x, -p.y, NULL);   // the control's piece of the background picture
    return g_bg_brush;
}

// The list's rows in a theme: the selected one in the theme's colour instead of Windows' blue.
static LRESULT theme_list_draw(NMLVCUSTOMDRAW *cd) {
    if (g_theme == TH_PLAIN) return CDRF_DODEFAULT;
    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
        const Theme *t = &g_themes[g_theme];
        int sel = ListView_GetItemState(g_list, (int)cd->nmcd.dwItemSpec, LVIS_SELECTED) != 0;
        cd->clrText = t->text;
        cd->clrTextBk = sel ? t->sel : g_list_image ? CLR_NONE : t->field;
        cd->nmcd.uItemState &= ~(UINT)(CDIS_SELECTED | CDIS_FOCUS);
        return CDRF_NEWFONT;
    }
    return CDRF_DODEFAULT;
}

// The list's column header in a theme (its NM_CUSTOMDRAW goes to the list, so the list is subclassed for it).
static LRESULT CALLBACK list_proc(HWND l, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND && g_theme != TH_PLAIN && g_list_image && g_list_dc) {
        RECT r;
        GetClientRect(l, &r);
        POINT o = {0, 0};   // the picture was cut at the list's outer edge; the client area starts inside its border
        MapWindowPoints(l, NULL, &o, 1);
        RECT wr;
        GetWindowRect(l, &wr);
        BitBlt((HDC)wp, 0, 0, r.right, r.bottom, g_list_dc, o.x - wr.left, o.y - wr.top, SRCCOPY);
        return 1;
    }
    if (msg == WM_NOTIFY && g_theme != TH_PLAIN && ((NMHDR *)lp)->code == NM_CUSTOMDRAW &&
        ((NMHDR *)lp)->hwndFrom == ListView_GetHeader(l)) {
        NMCUSTOMDRAW *cd = (NMCUSTOMDRAW *)lp;
        const Theme *t = &g_themes[g_theme];
        if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
        if (cd->dwDrawStage == CDDS_ITEMPREPAINT || cd->dwDrawStage == CDDS_POSTPAINT) {
            HWND hd = cd->hdr.hwndFrom;
            RECT r = cd->rc;
            if (cd->dwDrawStage == CDDS_POSTPAINT) {   // the part right of the last column
                RECT all, last;
                GetClientRect(hd, &all);
                int n = Header_GetItemCount(hd);
                r = all;
                if (n > 0 && Header_GetItemRect(hd, n - 1, &last)) r.left = last.right;
            }
            HBRUSH b = CreateSolidBrush(t->btn), e = CreateSolidBrush(t->edge);
            FillRect(cd->hdc, &r, b);
            RECT line = {r.left, r.bottom - 1, r.right, r.bottom};
            FillRect(cd->hdc, &line, e);
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                RECT sep = {r.right - 1, r.top + 3, r.right, r.bottom - 3};
                FillRect(cd->hdc, &sep, e);
                char text[64] = "";
                HDITEMA it = {0};
                it.mask = HDI_TEXT;
                it.pszText = text;
                it.cchTextMax = sizeof text;
                SendMessageA(hd, HDM_GETITEMA, cd->dwItemSpec, (LPARAM)&it);
                SetBkMode(cd->hdc, TRANSPARENT);
                SetTextColor(cd->hdc, t->dim);
                HFONT of = (HFONT)SelectObject(cd->hdc, (HFONT)SendMessageA(hd, WM_GETFONT, 0, 0));
                RECT tr = r;
                tr.left += 6;
                DrawTextA(cd->hdc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                SelectObject(cd->hdc, of);
            }
            DeleteObject(b); DeleteObject(e);
            return CDRF_SKIPDEFAULT;
        }
    }
    return CallWindowProcA(g_list_old, l, msg, wp, lp);
}

static void theme_draw_button(const DRAWITEMSTRUCT *d) {
    const Theme *t = &g_themes[g_theme];
    RECT r = d->rcItem;
    int play = d->CtlID == ID_PLAY, down = (d->itemState & ODS_SELECTED) != 0, off = (d->itemState & ODS_DISABLED) != 0;
    HBRUSH b = CreateSolidBrush(down ? t->sel : play ? t->play : t->btn);
    FillRect(d->hDC, &r, b);
    DeleteObject(b);
    HPEN pen = CreatePen(PS_INSIDEFRAME, play ? 2 : 1, t->edge), op = (HPEN)SelectObject(d->hDC, pen);
    HBRUSH ob = (HBRUSH)SelectObject(d->hDC, GetStockObject(NULL_BRUSH));
    Rectangle(d->hDC, r.left, r.top, r.right, r.bottom);
    SelectObject(d->hDC, ob);
    SelectObject(d->hDC, op);
    DeleteObject(pen);
    char text[128];
    GetWindowTextA(d->hwndItem, text, sizeof text);
    SetBkMode(d->hDC, TRANSPARENT);
    SetTextColor(d->hDC, off ? t->dim : t->text);
    HFONT of = (HFONT)SelectObject(d->hDC, (HFONT)SendMessageA(d->hwndItem, WM_GETFONT, 0, 0));
    RECT m = r, c = r;
    InflateRect(&m, -4, -2);
    c = m;
    DrawTextA(d->hDC, text, -1, &c, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
    OffsetRect(&m, 0, ((m.bottom - m.top) - (c.bottom - c.top)) / 2 + (down ? 1 : 0));
    DrawTextA(d->hDC, text, -1, &m, DT_CENTER | DT_WORDBREAK);
    SelectObject(d->hDC, of);
    if (d->itemState & ODS_FOCUS) { RECT f = r; InflateRect(&f, -3, -3); DrawFocusRect(d->hDC, &f); }
}

static LRESULT CALLBACK picker_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        add_control(w, "STATIC", "Tick the mods to use. Drag a mod up or down to change the order: upper mods win when two mods change the same thing.\n"
                                 "For multiplayer, every player needs the same mods in the same order.",
                    0, 12, 10, 890, 34, 0);
        add_control(w, "STATIC", "Mod picture", 0, 644, 48, 256, 16, 0);
        g_pic_box = add_control(w, "STATIC", "", SS_OWNERDRAW, 644, 66, 256, 256, ID_PICTURE);
        g_pic_label = add_control(w, "STATIC", "", SS_NOPREFIX | SS_ENDELLIPSIS, 644, 326, 256, 16, 0);
        g_list = add_control(w, WC_LISTVIEWA, "", WS_BORDER | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER, 12, 48, 500, 578, ID_LIST);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
        LVCOLUMNA col = {0};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = (char *)"Mod";
        col.cx = 320;
        SendMessageA(g_list, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);
        col.pszText = (char *)"From";
        col.cx = 150;
        SendMessageA(g_list, LVM_INSERTCOLUMNA, 1, (LPARAM)&col);
        add_control(w, "BUTTON", "Move to top", WS_TABSTOP, 524, 46, 108, 24, ID_TOP);
        add_control(w, "BUTTON", "Move up", WS_TABSTOP, 524, 72, 108, 24, ID_UP);
        add_control(w, "BUTTON", "Move down", WS_TABSTOP, 524, 98, 108, 24, ID_DOWN);
        add_control(w, "BUTTON", "Tick all", WS_TABSTOP, 524, 130, 108, 24, ID_ALL);
        add_control(w, "BUTTON", "Tick none", WS_TABSTOP, 524, 158, 108, 24, ID_NONE);
        add_control(w, "STATIC", "Interface size\n(Auto: like 1080p)", 0, 524, 206, 108, 30, 0);
        g_size_box = add_control(w, "COMBOBOX", "", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 524, 238, 108, 200, ID_SIZE);
        for (int i = 0; i < N_SIZES; i++) SendMessageA(g_size_box, CB_ADDSTRING, 0, (LPARAM)g_sizes[i].text);
        SendMessageA(g_size_box, CB_SETCURSEL, (WPARAM)settings_size_index(), 0);
        add_control(w, "STATIC", "Texture sharpness\n(seen in battle)", 0, 524, 268, 108, 30, 0);
        g_sharp_box = add_control(w, "COMBOBOX", "", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 524, 298, 108, 200, ID_SHARP);
        for (int i = 0; i < N_SHARP; i++) SendMessageA(g_sharp_box, CB_ADDSTRING, 0, (LPARAM)g_sharp_texts[i]);
        SendMessageA(g_sharp_box, CB_SETCURSEL, (WPARAM)(settings_texture_filtering() + 1), 0);
        if (settings_dx11_failed_last_time())
            MessageBoxA(w, "DirectX 11 did not start on this PC last time, so the game now uses OpenGL (the graphics card's own driver).\n\n"
                           "If the game was only stopped for another reason (for example Steam was not running), set \"Graphics\" back to DirectX 11.\n\n"
                           "If you report this, press \"Copy last log\" and say which graphics card you have.", "64-bit Attila", MB_ICONINFORMATION);
        // A PC on the OpenGL safety net gets a way back without editing settings.txt: a start without Steam used to leave
        // dx11_starting.txt behind (1.3.7 and older wrote it before connecting to Steam), which put working PCs on OpenGL.
        // 2026-11-26: the choice is always shown (players asked why they were on OpenGL and how to get back; it replaces the
        // "Use DirectX 11 again" button). DirectX 11 writes graphics=dx11, OpenGL writes graphics=opengl_chosen.
        add_control(w, "STATIC", "Graphics", 0, 524, 414, 108, 16, 0);
        g_gfx_box = add_control(w, "COMBOBOX", "", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 524, 432, 108, 200, ID_GFX);
        SendMessageA(g_gfx_box, CB_ADDSTRING, 0, (LPARAM)"DirectX 11");
        SendMessageA(g_gfx_box, CB_ADDSTRING, 0, (LPARAM)"OpenGL");
        SendMessageA(g_gfx_box, CB_SETCURSEL, settings_graphics_dx11() ? 0 : 1, 0);
        add_control(w, "STATIC", "Shadows redrawn\n(less often = faster)", 0, 524, 328, 108, 30, 0);
        g_shadow_box = add_control(w, "COMBOBOX", "", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 524, 360, 108, 200, ID_SHADOW);
        { static const char *const texts[4] = {"Every frame", "Every 2nd frame", "Every 3rd frame", "Every 4th frame"}; for (int i = 0; i < 4; i++) SendMessageA(g_shadow_box, CB_ADDSTRING, 0, (LPARAM)texts[i]); }
        SendMessageA(g_shadow_box, CB_SETCURSEL, (WPARAM)(settings_shadow_every() - 1), 0);
        g_fps_box = add_control(w, "BUTTON", "Show frame rate", WS_TABSTOP | BS_AUTOCHECKBOX, 524, 390, 108, 18, ID_FPS);
        SendMessageA(g_fps_box, BM_SETCHECK, settings_show_fps() ? BST_CHECKED : BST_UNCHECKED, 0);
        // JGdaCaesar's engine fix (plugins\perffix64.dll), under the mod picture
        g_perf_box = add_control(w, "BUTTON", "Engine performance fix (by JGdaCaesar)", WS_TABSTOP | BS_AUTOCHECKBOX, 644, 356, 256, 18, ID_PERF);
        g_flag_box = add_control(w, "BUTTON", "Waving flags (only for 1212AD)", WS_TABSTOP | BS_AUTOCHECKBOX, 664, 378, 236, 18, ID_FLAGS);
        SendMessageA(g_flag_box, BM_SETCHECK, settings_flag_wave() ? BST_CHECKED : BST_UNCHECKED, 0);
        g_up_box = add_control(w, "BUTTON", "Upscaling and frame generation", WS_TABSTOP | BS_AUTOCHECKBOX, 664, 400, 236, 18, ID_UPSCALE);
        SendMessageA(g_up_box, BM_SETCHECK, settings_upscaling() ? BST_CHECKED : BST_UNCHECKED, 0);
        if (perffix_available()) {
            SendMessageA(g_perf_box, BM_SETCHECK, settings_perf_fix() ? BST_CHECKED : BST_UNCHECKED, 0);
            EnableWindow(g_flag_box, settings_perf_fix());
            if (framebridge_available()) EnableWindow(g_up_box, settings_perf_fix());
            else {   // the engine fix is there, the add-on is not
                EnableWindow(g_up_box, FALSE);
                SendMessageA(g_up_box, BM_SETCHECK, BST_UNCHECKED, 0);
                add_control(w, "STATIC", "Upscaling not installed: subscribe to \"Upscaling\nand Framegen for Attila\" on the Steam Workshop.", 0, 644, 424, 256, 30, 0);
                g_up_box = NULL;
            }
        } else {   // not subscribed: shown, greyed, saved settings left as they are
            EnableWindow(g_perf_box, FALSE);
            EnableWindow(g_flag_box, FALSE);
            EnableWindow(g_up_box, FALSE);
            add_control(w, "STATIC", "Not installed: subscribe to \"Attila Engine\nPerformance Fix\" on the Steam Workshop.", 0, 644, 424, 256, 30, 0);
            g_perf_box = g_flag_box = g_up_box = NULL;
        }
        add_control(w, "STATIC", "Theme of this window", 0, 644, 470, 256, 16, 0);
        g_theme_box = add_control(w, "COMBOBOX", "", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 644, 488, 160, 200, ID_THEME);
        {
            char v[32];
            g_theme = TH_PLAIN;
            if (settings_get("theme", v, sizeof v))
                for (int i = 0; i < N_THEMES; i++) if (!_stricmp(v, g_theme_keys[i])) g_theme = i;
            for (int i = 0; i < N_THEMES; i++) SendMessageA(g_theme_box, CB_ADDSTRING, 0, (LPARAM)g_theme_names[i]);
            SendMessageA(g_theme_box, CB_SETCURSEL, (WPARAM)g_theme, 0);
        }
        if (last_game_crashed()) add_control(w, "STATIC", "Last game crashed:\nplease send its log.", 0, 524, 479, 108, 30, 0);
        PostMessageA(w, WM_CRASH_OFFER, 0, 0);   // asked once the window shows (crash_send.inc)
        add_control(w, "BUTTON", "Copy last log", WS_TABSTOP, 524, 511, 108, 24, ID_COPYLOG);
        add_control(w, "BUTTON", "Show log file", WS_TABSTOP, 524, 537, 108, 24, ID_SHOWLOG);
        add_control(w, "BUTTON", "Play", WS_TABSTOP | BS_DEFPUSHBUTTON, 524, 566, 108, 34, ID_PLAY);
        add_control(w, "BUTTON", "Normal Attila launcher\n(Mod Manager, Workshop upload)", WS_TABSTOP | BS_MULTILINE, 644, 566, 256, 34, ID_LAUNCHER);
        add_control(w, "BUTTON", "Quit", WS_TABSTOP, 524, 604, 108, 26, ID_QUIT);
        list_fill();
        if (g_nmods) {
            ListView_SetItemState(g_list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            picture_show(0);
        }
        if (getenv("ATTILA_PICKER_AUTO")) PostMessageA(w, WM_AUTO, 0, 0);
        theme_apply(w);
        return 0;
    }
    case WM_ERASEBKGND:
        if (g_theme != TH_PLAIN && g_bg_dc) {
            RECT r;
            GetClientRect(w, &r);
            BitBlt((HDC)wp, 0, 0, r.right, r.bottom, g_bg_dc, 0, 0, SRCCOPY);
            return 1;
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT: {
        HBRUSH b = theme_ctlcolor(w, msg, (HDC)wp, (HWND)lp);
        if (b) return (LRESULT)b;
        break;
    }
    case WM_NOTIFY: {
        const NMHDR *h = (const NMHDR *)lp;
        if (h->hwndFrom != g_list) break;
        if (h->code == NM_CUSTOMDRAW) return theme_list_draw((NMLVCUSTOMDRAW *)lp);
        const NMLISTVIEW *nm = (const NMLISTVIEW *)lp;
        if (h->code == LVN_BEGINDRAG && nm->iItem >= 0) {
            g_drag = nm->iItem;
            ListView_SetItemState(g_list, g_drag, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            SetCapture(w);
            SetTimer(w, DRAG_TIMER, 50, NULL);
            SetCursor(LoadCursor(NULL, IDC_SIZENS));
        } else if (h->code == LVN_ITEMCHANGED && !g_moving && (nm->uChanged & LVIF_STATE) &&
                   (nm->uNewState & LVIS_SELECTED) && !(nm->uOldState & LVIS_SELECTED))
            picture_show(nm->iItem);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_drag >= 0) {
            POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
            MapWindowPoints(w, g_list, &pt, 1);
            SetCursor(LoadCursor(NULL, IDC_SIZENS));
            drag_to(pt);
        }
        return 0;
    case WM_TIMER:
        if (wp == DRAG_TIMER && g_drag >= 0) drag_scroll_tick();
        return 0;
    case WM_LBUTTONUP:
        if (g_drag >= 0) ReleaseCapture();   // WM_CAPTURECHANGED ends the drag
        return 0;
    case WM_CAPTURECHANGED:
        drag_end(w);
        return 0;
    case WM_DRAWITEM:
        if (wp == ID_PICTURE) { picture_draw((const DRAWITEMSTRUCT *)lp); return TRUE; }
        if (((const DRAWITEMSTRUCT *)lp)->CtlType == ODT_BUTTON && g_theme != TH_PLAIN) { theme_draw_button((const DRAWITEMSTRUCT *)lp); return TRUE; }
        break;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED && g_list) layout(w);
        return 0;
    case WM_GETMINMAXINFO: {   // never smaller than the normal layout
        RECT r = {0, 0, BASE_W, BASE_H};
        AdjustWindowRect(&r, (DWORD)GetWindowLongA(w, GWL_STYLE), FALSE);
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = r.right - r.left;
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_CRASH_OFFER: crash_send_offer(w); return 0;
    case WM_AUTO:   // ATTILA_PICKER_AUTO=1: untick the first, move the last to the top, Play; =2: Play with the list as it is
        if (g_nmods > 1 && strcmp(getenv("ATTILA_PICKER_AUTO") ? getenv("ATTILA_PICKER_AUTO") : "", "2")) {
            ListView_SetCheckState(g_list, 0, FALSE);
            ListView_SetItemState(g_list, g_nmods - 1, LVIS_SELECTED, LVIS_SELECTED);
            ListView_SetCheckState(g_list, g_nmods - 1, TRUE);
            for (int i = g_nmods - 1; i > 0; i--) move_selected(-1);
        }
        PostMessageA(w, WM_COMMAND, ID_PLAY, 0);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_TOP: move_selected_to_top(); break;
        case ID_UP: move_selected(-1); break;
        case ID_DOWN: move_selected(1); break;
        case ID_ALL: set_all(1); break;
        case ID_NONE: set_all(0); break;
        case ID_PLAY:
            read_checks();
            save_list();
            settings_save_size((int)SendMessageA(g_size_box, CB_GETCURSEL, 0, 0));
            {
                int sharp = (int)SendMessageA(g_sharp_box, CB_GETCURSEL, 0, 0) - 1;
                char v[8];
                snprintf(v, sizeof v, "%d", sharp);
                settings_put("texture_sharpness", sharp < 0 || sharp > 4 ? "game" : v);
            }
            { char v[8]; snprintf(v, sizeof v, "%d", (int)SendMessageA(g_shadow_box, CB_GETCURSEL, 0, 0) + 1); settings_put("shadow_refresh", v); }

            if (g_fps_box) settings_put("show_fps", SendMessageA(g_fps_box, BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0");
            if (g_perf_box) settings_put("perf_fix", SendMessageA(g_perf_box, BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0");
            if (g_flag_box) settings_put("flag_wave", SendMessageA(g_flag_box, BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0");
            if (g_up_box) settings_put("upscaling", SendMessageA(g_up_box, BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0");
            g_result = 1;
            DestroyWindow(w);
            break;
        case ID_PERF:   // the flags are part of the same DLL: only choosable while it is ticked
            if (g_flag_box) EnableWindow(g_flag_box, SendMessageA(g_perf_box, BM_GETCHECK, 0, 0) == BST_CHECKED);
            if (g_up_box) EnableWindow(g_up_box, SendMessageA(g_perf_box, BM_GETCHECK, 0, 0) == BST_CHECKED);
            break;
        case ID_THEME:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int i = (int)SendMessageA(g_theme_box, CB_GETCURSEL, 0, 0);
                if (i >= 0 && i < N_THEMES && i != g_theme) { g_theme = i; theme_apply(w); }
            }
            break;
        case ID_COPYLOG: copy_last_log(w); break;
        case ID_SHOWLOG: show_log_file(w); break;
        case ID_GFX:
            if (HIWORD(wp) == CBN_SELCHANGE) settings_put("graphics", SendMessageA(g_gfx_box, CB_GETCURSEL, 0, 0) == 1 ? "opengl_chosen" : "dx11");
            break;
        case ID_DX11_AGAIN:
            settings_put("graphics", "dx11");
            if (g_dx11_again) { DestroyWindow(g_dx11_again); g_dx11_again = NULL; }
            MessageBoxA(w, "The game will use DirectX 11 again from the next Play.", "64-bit Attila", MB_ICONINFORMATION);
            break;
        case ID_LAUNCHER:   // the mod list is saved as it is, then the 64-bit game is not started
            read_checks();
            save_list();
            if (start_ca_launcher(w)) DestroyWindow(w);
            break;
        case ID_QUIT: DestroyWindow(w); break;
        }
        return 0;
    case WM_DESTROY:
        settings_put("theme", g_theme_keys[g_theme]);
        theme_free();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(w, msg, wp, lp);
}

// 1 = start the game, 0 = the player chose Quit.
int mod_picker_run(void) {
    INITCOMMONCONTROLSEX ic = {sizeof ic, ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&ic);
    load_all();
    gdiplus_start();
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = picker_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "Attila64Mods";
    RegisterClassA(&wc);
    RECT r = {0, 0, BASE_W, BASE_H};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;   // full screen taken out again (buggy on the user's PC)
    AdjustWindowRect(&r, style, FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    HWND w = CreateWindowA("Attila64Mods", "64-bit Attila - Mods", style, (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww, wh, NULL, NULL, wc.hInstance, NULL);
    if (!w) { gdiplus_stop(); return 1; }  // no window: start with the saved list
    if (getenv("ATTILA_PICKER_AUTO") && getenv("ATTILA_TEST_HIDDEN")) ShowWindow(w, SW_HIDE);   // a test that clicks by itself: not on the screen
    else {
        char v[16];
        ShowWindow(w, SW_SHOW);   // picker_maximized= in settings.txt is ignored since full screen was taken out
        SetForegroundWindow(w);
    }
    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        if (IsDialogMessageA(w, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    gdiplus_stop();   // the game must not keep the picture file open
    if (g_launcher_proc) {   // normal Attila's launcher was chosen: stay until it closes, then end without starting the 64-bit game
        WaitForSingleObject(g_launcher_proc, INFINITE);
        CloseHandle(g_launcher_proc);
        return 0;
    }
    return g_result;
}
