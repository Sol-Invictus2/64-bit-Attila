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
static HWND g_list, g_size_box, g_sharp_box, g_shadow_box, g_fps_box;
static int g_result;

enum { ID_LIST = 100, ID_UP, ID_DOWN, ID_ALL, ID_NONE, ID_PLAY, ID_QUIT, ID_SIZE, ID_SHARP, ID_GFX, ID_SHADOW, ID_COPYLOG, ID_SHOWLOG, ID_FPS, WM_AUTO = WM_APP + 1 };

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
int settings_graphics_dx12(void) {
    return 0;
}

// DirectX 11: the game's own DirectX 11 shaders drawn by a real DirectX 11 device (loader: dx11_device.inc). The only route,
// unless a DirectX 11 start failed on this PC: a start that never reached a running game leaves dx11_starting.txt, and the
// start window then writes graphics=opengl_fallback (settings_dx11_failed_last_time) and says so; from then on this PC uses
// OpenGL, the card's own driver. Older settings files' graphics=opengl / dx12 / dx11 choices are not read any more.
int settings_graphics_dx11(void) {
    char v[128];
    return !(settings_get("graphics", v, sizeof v) && !_stricmp(v, "opengl_fallback"));
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
// Task waits, "task_waits=game" in settings.txt: worker threads sleep at once and the task queue is always locked, as the
// game does. Anything else (or no line) = they stay awake a moment first (task_spin.inc) and an empty queue is peeked.
int settings_fast_task_waits(void) {
    char v[16];
    return !(settings_get("task_waits", v, sizeof v) && !strcmp(v, "game"));
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

static void list_fill(void) {
    ListView_DeleteAllItems(g_list);
    for (int i = 0; i < g_nmods; i++) {
        LVITEMA it = {0};
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = g_mods[i].name;
        SendMessageA(g_list, LVM_INSERTITEMA, 0, (LPARAM)&it);
        ListView_SetCheckState(g_list, i, g_mods[i].on);
        const char *from = g_mods[i].dir[0] ? "Workshop" : "Game data folder";
        const char *slash = strstr(g_mods[i].dir, "/325610/");
        char where[64];
        if (slash) snprintf(where, sizeof where, "Workshop %s", slash + 8);
        else if (g_mods[i].dir[0]) snprintf(where, sizeof where, "%s", from);
        else snprintf(where, sizeof where, "%s", from);
        LVITEMA sub = {0};
        sub.iSubItem = 1;
        sub.pszText = where;
        SendMessageA(g_list, LVM_SETITEMTEXTA, (WPARAM)i, (LPARAM)&sub);
    }
}

static void read_checks(void) {
    for (int i = 0; i < g_nmods; i++) g_mods[i].on = ListView_GetCheckState(g_list, i) != 0;
}

static void move_selected(int by) {
    int i = ListView_GetNextItem(g_list, -1, LVNI_SELECTED), j = i + by;
    if (i < 0 || j < 0 || j >= g_nmods) return;
    read_checks();
    Mod t = g_mods[i];
    g_mods[i] = g_mods[j];
    g_mods[j] = t;
    list_fill();
    ListView_SetItemState(g_list, j, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(g_list, j, FALSE);
    SetFocus(g_list);
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

static HWND add_control(HWND parent, const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent, (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return c;
}

static LRESULT CALLBACK picker_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        add_control(w, "STATIC", "Tick the mods to use. Upper mods win when two mods change the same thing.\n"
                                 "For multiplayer, every player needs the same mods in the same order.",
                    0, 12, 10, 620, 34, 0);
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
        add_control(w, "BUTTON", "Move up", WS_TABSTOP, 524, 48, 108, 28, ID_UP);
        add_control(w, "BUTTON", "Move down", WS_TABSTOP, 524, 80, 108, 28, ID_DOWN);
        add_control(w, "BUTTON", "Tick all", WS_TABSTOP, 524, 124, 108, 28, ID_ALL);
        add_control(w, "BUTTON", "Tick none", WS_TABSTOP, 524, 156, 108, 28, ID_NONE);
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
                           "If you report this, press \"Copy last log\" and say which graphics card you have.", "64-bit Attila", MB_ICONINFORMATION);
        add_control(w, "STATIC", "Shadows redrawn\n(less often = faster)", 0, 524, 328, 108, 30, 0);
        g_shadow_box = add_control(w, "COMBOBOX", "", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 524, 360, 108, 200, ID_SHADOW);
        { static const char *const texts[4] = {"Every frame", "Every 2nd frame", "Every 3rd frame", "Every 4th frame"}; for (int i = 0; i < 4; i++) SendMessageA(g_shadow_box, CB_ADDSTRING, 0, (LPARAM)texts[i]); }
        SendMessageA(g_shadow_box, CB_SETCURSEL, (WPARAM)(settings_shadow_every() - 1), 0);
        g_fps_box = add_control(w, "BUTTON", "Show frame rate", WS_TABSTOP | BS_AUTOCHECKBOX, 524, 390, 108, 18, ID_FPS);
        SendMessageA(g_fps_box, BM_SETCHECK, settings_show_fps() ? BST_CHECKED : BST_UNCHECKED, 0);
        if (last_game_crashed()) add_control(w, "STATIC", "Last game crashed:\nplease send its log.", 0, 524, 479, 108, 30, 0);
        add_control(w, "BUTTON", "Copy last log", WS_TABSTOP, 524, 511, 108, 24, ID_COPYLOG);
        add_control(w, "BUTTON", "Show log file", WS_TABSTOP, 524, 537, 108, 24, ID_SHOWLOG);
        add_control(w, "BUTTON", "Play", WS_TABSTOP | BS_DEFPUSHBUTTON, 524, 566, 108, 34, ID_PLAY);
        add_control(w, "BUTTON", "Quit", WS_TABSTOP, 524, 604, 108, 26, ID_QUIT);
        list_fill();
        if (getenv("ATTILA_PICKER_AUTO")) PostMessageA(w, WM_AUTO, 0, 0);
        return 0;
    }
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
            g_result = 1;
            DestroyWindow(w);
            break;
        case ID_COPYLOG: copy_last_log(w); break;
        case ID_SHOWLOG: show_log_file(w); break;
        case ID_QUIT: DestroyWindow(w); break;
        }
        return 0;
    case WM_DESTROY:
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
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = picker_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "Attila64Mods";
    RegisterClassA(&wc);
    RECT r = {0, 0, 644, 638};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    HWND w = CreateWindowA("Attila64Mods", "64-bit Attila - Mods", style, (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww, wh, NULL, NULL, wc.hInstance, NULL);
    if (!w) return 1;  // no window: start with the saved list
    if (getenv("ATTILA_PICKER_AUTO") && getenv("ATTILA_TEST_HIDDEN")) ShowWindow(w, SW_HIDE);   // a test that clicks by itself: not on the screen
    else { ShowWindow(w, SW_SHOW); SetForegroundWindow(w); }
    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        if (IsDialogMessageA(w, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return g_result;
}
