// One-file installer for players: everything except the Linux game file is inside this exe. The game file is the player's own Steam download.
// Finds Steam and Attila on any drive, copies the files into <game>\attila64, gets the game file, makes a desktop icon.
// Arguments: "uninstall" removes it, "find" only prints what it found. ATTILA_INSTALL_TEST=<folder> redirects everything into that folder
// (game = <folder>\game, download = <folder>\depot\Attila, icon = <folder>\icon.lnk) and skips Steam, the clipboard and the pauses.
// ATTILA_INSTALL_STEAM=<folder> is for tests of the search: that folder is used as Steam's folder instead of the one in the registry.
// Folder names: Steam keeps its library list in UTF-8, while this installer and the game itself use the PC's own code page (the Windows
// "A" functions), so library paths are converted; a name with letters outside that code page gets a clear message instead of a miss.
#define COBJMACROS
#include <windows.h>
#include <shlobj.h>
#include <objidl.h>
#include <shobjidl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define GAME_FILE_SIZE 758951919ull
#define DOWNLOAD_COMMAND "download_depot 325610 325624"

#define EMBED(sym, path) \
    __asm__(".section .rdata\n.global " #sym "_start\n" #sym "_start:\n.incbin \"" path "\"\n.global " #sym "_end\n" #sym "_end:\n.text\n"); \
    extern const uint8_t sym##_start[], sym##_end[];
EMBED(f_loader, "attila64.exe")
EMBED(f_sdl, "SDL2.dll")
EMBED(f_steam, "steam_api64.dll")
EMBED(f_cpp, "../lib/libstdc++.so.6")
EMBED(f_gcc, "../lib/libgcc_s.so.1")
EMBED(f_notices, "../friend2/THIRD_PARTY_NOTICES.txt")
EMBED(f_license, "../LICENSE.txt")   // MIT licence of the loader and installer, credit to Sol Invictus
EMBED(f_pack_lobby, "../../attila_8player/out8/attila_8player_lobby.pack")   // 8-player campaign lobby
EMBED(f_pack_battle, "../../attila_8player/out8/attila_8v8_battle.pack")   // 8v8 battle setup screen
// Up to 1.2 Test 5 the installer also put Mesa's OpenGL-on-DirectX-12 files in a dx12 folder (the start window's DirectX 12
// choice). Since 2026-11-13 DirectX 11 is the only route offered, so they are no longer shipped, and an update removes them.
static const char *const g_old_files[] = {"dx12\\opengl32.dll", "dx12\\libgallium_wgl.dll", "dx12\\dxil.dll"};
static const struct { const char *name; const uint8_t *start, *end; } g_files[] = {
    {"attila64.exe", f_loader_start, f_loader_end},
    {"SDL2.dll", f_sdl_start, f_sdl_end},
    {"steam_api64.dll", f_steam_start, f_steam_end},
    {"libstdc++.so.6", f_cpp_start, f_cpp_end},
    {"libgcc_s.so.1", f_gcc_start, f_gcc_end},
    {"THIRD_PARTY_NOTICES.txt", f_notices_start, f_notices_end},
    {"LICENSE.txt", f_license_start, f_license_end},
    {"packs\\attila_8player_lobby.pack", f_pack_lobby_start, f_pack_lobby_end},   // loaded only by the 64-bit engine
    {"packs\\attila_8v8_battle.pack", f_pack_battle_start, f_pack_battle_end},
};

// The files of older versions that this one no longer uses, and their folder. 1 = something could not be deleted.
static int remove_old_files(const char *folder) {
    char p[MAX_PATH * 2];
    int left = 0;
    for (size_t i = 0; i < sizeof g_old_files / sizeof g_old_files[0]; i++) {
        snprintf(p, sizeof p, "%s\\%s", folder, g_old_files[i]);
        if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES && !DeleteFileA(p)) left = 1;
    }
    snprintf(p, sizeof p, "%s\\dx12", folder);
    RemoveDirectoryA(p);
    return left;
}
#define NFILES (sizeof g_files / sizeof g_files[0])

static const char *g_test;

static void finish(int code) {
    if (!g_test) {
        printf("\nPress Enter to close this window.\n");
        getchar();
    }
    exit(code);
}

static int is_folder(const char *p) {
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static uint64_t file_size(const char *p) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(p, GetFileExInfoStandard, &d) || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    return ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

// A folder Attila is in whose name the game can't use on this PC (UTF-8), for the message when nothing usable is found.
static char g_unusable[MAX_PATH * 3];

// Wide text to the PC's code page. 0 when a letter has no place in it: the game could not use such a folder name.
static int wide_to_acp(const WCHAR *w, char *out, size_t size) {
    BOOL lost = FALSE;
    if (!WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w, -1, out, (int)size, NULL, &lost) || lost) return 0;
    return 1;
}

// Steam's text files are UTF-8: converts in place to the PC's code page (plain English text stays the same).
static int utf8_to_acp(char *s, size_t size) {
    WCHAR w[MAX_PATH * 3];
    char tmp[MAX_PATH * 3];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, MAX_PATH * 3)) return 1;   // not UTF-8 at all: leave it
    if (!wide_to_acp(w, tmp, sizeof tmp)) return 0;
    snprintf(s, size, "%s", tmp);
    return 1;
}

// Prints UTF-8 text so any letters show right in the window.
static void print_utf8(const char *s) {
    WCHAR w[MAX_PATH * 3];
    DWORD mode, done;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, w, MAX_PATH * 3);
    fflush(stdout);
    if (n > 1 && GetConsoleMode(out, &mode)) WriteConsoleW(out, w, (DWORD)(n - 1), &done, NULL);
    else fputs(s, stdout);
}

static int find_steam(char *out, DWORD size) {
    const char *test = getenv("ATTILA_INSTALL_STEAM");
    if (test) { snprintf(out, size, "%s", test); return is_folder(out); }
    WCHAR w[MAX_PATH];
    DWORD n = sizeof w;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, NULL, w, &n) != ERROR_SUCCESS) {
        n = sizeof w;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath", RRF_RT_REG_SZ, NULL, w, &n) != ERROR_SUCCESS) return 0;
    }
    if (!wide_to_acp(w, out, size)) {   // Steam itself sits in a folder the game can't use on this PC
        char u[MAX_PATH * 3];
        if (WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof u, NULL, NULL)) snprintf(g_unusable, sizeof g_unusable, "%s", u);
        return 0;
    }
    for (char *c = out; *c; c++) if (*c == '/') *c = '\\';
    return is_folder(out);
}

// Reads one small text file into a fresh buffer.
static char *read_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (32 << 20)) { fclose(f); return NULL; }
    char *b = malloc((size_t)n + 1);
    size_t got = b ? fread(b, 1, (size_t)n, f) : 0;
    if (b) b[got] = 0;
    fclose(f);
    return b;
}

// Value after "key" in a Valve text file, starting at *from. Doubled backslashes become single ones.
static int vdf_value(const char **from, const char *key, char *out, size_t size) {
    char want[64];
    snprintf(want, sizeof want, "\"%s\"", key);
    const char *p = strstr(*from, want);
    if (!p) return 0;
    p = strchr(p + strlen(want), '"');
    if (!p) return 0;
    p++;
    size_t w = 0;
    while (*p && *p != '"' && w + 1 < size) {
        if (*p == '\\' && p[1]) p++;
        out[w++] = *p++;
    }
    out[w] = 0;
    *from = p;
    return 1;
}

// lib is in the PC's code page. Attila's folder name comes from Steam's record of it (UTF-8).
static int game_in_library(const char *lib, char *game, size_t size) {
    char path[MAX_PATH * 2], dir[MAX_PATH] = "Total War Attila";
    snprintf(path, sizeof path, "%s\\steamapps\\appmanifest_325610.acf", lib);
    char *acf = read_text(path);
    if (!acf) return 0;
    const char *from = acf;
    vdf_value(&from, "installdir", dir, sizeof dir);
    free(acf);
    if (!utf8_to_acp(dir, sizeof dir)) {
        WCHAR w[MAX_PATH * 3];
        char u[MAX_PATH * 3], l[MAX_PATH * 3] = "";
        if (MultiByteToWideChar(CP_ACP, 0, lib, -1, w, MAX_PATH * 3)) WideCharToMultiByte(CP_UTF8, 0, w, -1, l, sizeof l, NULL, NULL);
        snprintf(u, sizeof u, "%s\\steamapps\\common\\", l);
        snprintf(g_unusable, sizeof g_unusable, "%.*s", (int)(sizeof g_unusable - 1), u);
        strncat(g_unusable, dir, sizeof g_unusable - strlen(g_unusable) - 1);   // dir is still UTF-8 here
        return 0;
    }
    snprintf(game, size, "%s\\steamapps\\common\\%s", lib, dir);
    return is_folder(game);
}

// True when Attila's Steam record is in this library (UTF-8 path), whatever letters the path has.
static int library_has_attila_utf8(const char *lib) {
    char u[MAX_PATH * 3];
    WCHAR w[MAX_PATH * 3];
    snprintf(u, sizeof u, "%s\\steamapps\\appmanifest_325610.acf", lib);
    return MultiByteToWideChar(CP_UTF8, 0, u, -1, w, MAX_PATH * 3) && GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
}

static int find_game(const char *steam, char *game, size_t size) {
    if (game_in_library(steam, game, size)) return 1;
    char path[MAX_PATH * 2], lib[MAX_PATH * 3], acp[MAX_PATH * 3];
    snprintf(path, sizeof path, "%s\\steamapps\\libraryfolders.vdf", steam);
    char *vdf = read_text(path);
    if (!vdf) return 0;
    const char *from = vdf;
    int found = 0;
    while (!found && vdf_value(&from, "path", lib, sizeof lib)) {   // Steam writes these in UTF-8
        snprintf(acp, sizeof acp, "%s", lib);
        if (utf8_to_acp(acp, sizeof acp)) found = game_in_library(acp, game, size);
        else if (library_has_attila_utf8(lib)) snprintf(g_unusable, sizeof g_unusable, "%s", lib);
    }
    free(vdf);
    return found;
}

static int write_file(const char *path, const uint8_t *data, size_t n) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD done = 0;
    BOOL ok = WriteFile(h, data, (DWORD)n, &done, NULL) && done == n;
    CloseHandle(h);
    return ok;
}

static void set_clipboard(const char *text) {
    if (!OpenClipboard(NULL)) return;
    EmptyClipboard();
    size_t n = strlen(text) + 1;
    HGLOBAL m = GlobalAlloc(GMEM_MOVEABLE, n);
    if (m) {
        memcpy(GlobalLock(m), text, n);
        GlobalUnlock(m);
        if (!SetClipboardData(CF_TEXT, m)) GlobalFree(m);
    }
    CloseClipboard();
}

// True when the file has the right start, and the right size or has stopped growing for a minute.
static int game_file_ready(const char *path, uint64_t *last, DWORD *since) {
    uint64_t n = file_size(path);
    if (n != *last) { *last = n; *since = GetTickCount(); }
    if (n < (100u << 20)) return 0;
    if (n != GAME_FILE_SIZE && GetTickCount() - *since < 60000) return 0;
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    char magic[4] = {0};
    DWORD got = 0;
    ReadFile(h, magic, 4, &got, NULL);
    CloseHandle(h);
    return got == 4 && !memcmp(magic, "\x7f" "ELF", 4);
}

static void icon_path(char *out, size_t size) {
    if (g_test) { snprintf(out, size, "%s\\icon.lnk", g_test); return; }
    char desk[MAX_PATH];
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, desk))) desk[0] = 0;
    snprintf(out, size, "%s\\64-bit Attila.lnk", desk);
}

// The icon's name until Test 14 ("Attila 64-bit"): removed when the new one is made, and on uninstall.
static void remove_old_icon(void) {
    if (g_test) return;
    char desk[MAX_PATH], old[MAX_PATH * 2];
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, desk))) return;
    snprintf(old, sizeof old, "%s\\Attila 64-bit.lnk", desk);
    DeleteFileA(old);
}

static int make_icon(const char *exe, const char *folder, const char *game) {
    char lnk[MAX_PATH * 2];
    WCHAR wide[MAX_PATH * 2];
    icon_path(lnk, sizeof lnk);
    (void)game;
    MultiByteToWideChar(CP_ACP, 0, lnk, -1, wide, MAX_PATH * 2);
    CoInitialize(NULL);
    IShellLinkA *sl;
    int ok = 0;
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkA, (void **)&sl))) {
        IShellLinkA_SetPath(sl, exe);
        IShellLinkA_SetWorkingDirectory(sl, folder);
        IShellLinkA_SetDescription(sl, "64-bit Attila");
        IShellLinkA_SetIconLocation(sl, exe, 0);
        IPersistFile *pf;
        if (SUCCEEDED(IShellLinkA_QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
            ok = SUCCEEDED(IPersistFile_Save(pf, wide, TRUE));
            IPersistFile_Release(pf);
        }
        if (ok) remove_old_icon();
        IShellLinkA_Release(sl);
    }
    CoUninitialize();
    return ok;
}

static void uninstall(const char *folder) {
    char p[MAX_PATH * 2];
    int left = 0;
    for (size_t i = 0; i < NFILES; i++) {
        snprintf(p, sizeof p, "%s\\%s", folder, g_files[i].name);
        if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES && !DeleteFileA(p)) left++;
    }
    static const char *const extra[] = {"Attila", "attila64_log.txt", "attila64_log_previous.txt", "attila64_log_previous2.txt", "attila64_log_previous3.txt",
                                          "attila64_log_previous4.txt", "settings.txt", "mod_list.txt", "mod_list.txt.tmp"};
    for (size_t i = 0; i < sizeof extra / sizeof extra[0]; i++) {
        snprintf(p, sizeof p, "%s\\%s", folder, extra[i]);
        if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES && !DeleteFileA(p)) left++;
    }
    left += remove_old_files(folder);
    icon_path(p, sizeof p);
    DeleteFileA(p);
    remove_old_icon();
    snprintf(p, sizeof p, "%s\\packs", folder);
    RemoveDirectoryA(p);
    RemoveDirectoryA(folder);
    if (left) {
        printf("Some files could not be removed. Close the game and run this again.\n");
        finish(1);
    }
    printf("64-bit Attila is removed. Your normal Attila and your saves are not touched.\n");
    printf("If you added a Launch Option in Steam for it, clear that line too.\n");
    finish(0);
}

static void unusable_message(const char *what) {
    printf("Found %s in this folder:\n  ", what);
    print_utf8(g_unusable);
    printf("\nIts name has letters that this PC's language setting can't hand to the 64-bit game, so the game can't run from there.\n");
    if (!strcmp(what, "Attila"))
        printf("In Steam you can move Attila to a library folder with plain letters: right-click Attila > Properties >\n"
               "Installed Files > Move install folder. Then run this again.\n");
    finish(1);
}

static void relaunch_as_admin(int argc, char **argv) {
    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, sizeof self);
    printf("Windows needs permission to write to the Attila folder. Please press Yes on the next window.\n");
    if ((INT_PTR)ShellExecuteA(NULL, "runas", self, argc > 1 ? argv[1] : NULL, NULL, SW_SHOWNORMAL) > 32) exit(0);
    printf("Could not get permission. Right-click this program and choose Run as administrator.\n");
    finish(1);
}

int main(int argc, char **argv) {
    char steam[MAX_PATH] = "", game[MAX_PATH * 2] = "", folder[MAX_PATH * 2], target[MAX_PATH * 2], depot[MAX_PATH * 2], p[MAX_PATH * 3];
    const char *arg = argc > 1 ? argv[1] : "";
    g_test = getenv("ATTILA_INSTALL_TEST");
    // A click inside the window would otherwise pause the installer until a key is pressed.
    {
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode;
        if (GetConsoleMode(in, &mode)) SetConsoleMode(in, (mode | 0x80 /* extended flags */) & ~0x40u /* quick edit */);
    }
    // Paths below are in the PC's code page; the window shows them right only in that code page.
    {
        DWORD mode;
        if (GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode)) SetConsoleOutputCP(GetACP());
    }
    printf("=== 64-bit Attila installer ===\n\n");

    if (g_test) {
        snprintf(game, sizeof game, "%s\\game", g_test);
        snprintf(depot, sizeof depot, "%s\\depot\\Attila", g_test);
        CreateDirectoryA(game, NULL);
    } else {
        if (!find_steam(steam, sizeof steam)) {
            if (g_unusable[0]) unusable_message("Steam");
            printf("Could not find Steam on this PC. Please install Steam first.\n"); finish(1);
        }
        if (!find_game(steam, game, sizeof game)) {
            if (g_unusable[0]) unusable_message("Attila");
            printf("Could not find Total War: ATTILA in your Steam folders. Please install it in Steam first.\n"); finish(1);
        }
        // The game gets UTF-8 file names only on Windows 10 version 1903 (May 2019) and later; before that, letters such as é
        // in its folder name break its mod loading.
        if (GetACP() != CP_UTF8)
            for (const char *c = game; *c; c++)
                if ((unsigned char)*c >= 0x80) {
                    printf("Found Attila in: %s\nThis Windows is older than Windows 10 version 1903, and with a folder name like this one\n"
                           "the 64-bit game can't load mods. Update Windows, or move Attila in Steam to a folder with plain letters:\n"
                           "right-click Attila > Properties > Installed Files > Move install folder.\n", game);
                    finish(1);
                }
        snprintf(depot, sizeof depot, "%s\\steamapps\\content\\app_325610\\depot_325624\\Attila", steam);
    }
    snprintf(folder, sizeof folder, "%s\\attila64", game);
    snprintf(target, sizeof target, "%s\\Attila", folder);
    printf("Found Attila in: %s\n", game);
    if (!strcmp(arg, "find")) {
        printf("Steam: %s\nDownload place: %s (%llu bytes)\nInstalled already: %s\n", steam, depot, (unsigned long long)file_size(depot), is_folder(folder) ? "yes" : "no");
        return 0;
    }

    if (!strcmp(arg, "uninstall")) uninstall(folder);
    if (is_folder(folder) && !g_test) {
        printf("\n64-bit Attila is already here. Type 1 to update it, 2 to remove it, then press Enter: ");
        char line[16] = "";
        fgets(line, sizeof line, stdin);
        if (line[0] == '2') uninstall(folder);
        if (line[0] != '1') finish(0);
    }

    // our files
    if (!CreateDirectoryA(folder, NULL) && GetLastError() == ERROR_ACCESS_DENIED) relaunch_as_admin(argc, argv);
    printf("\nCopying files...\n");
    snprintf(p, sizeof p, "%s\\packs", folder);
    CreateDirectoryA(p, NULL);
    remove_old_files(folder);   // an update: the DirectX 12 files of older versions (not needed any more)
    for (size_t i = 0; i < NFILES; i++) {
        snprintf(p, sizeof p, "%s\\%s", folder, g_files[i].name);
        if (!write_file(p, g_files[i].start, (size_t)(g_files[i].end - g_files[i].start))) {
            DWORD e = GetLastError();
            if (e == ERROR_ACCESS_DENIED) relaunch_as_admin(argc, argv);
            if (e == ERROR_SHARING_VIOLATION) printf("The game is still open. Close it and run this again.\n");
            else printf("Could not write %s (error %lu).\n", p, e);
            finish(1);
        }
    }

    // the game file
    uint64_t last = 0;
    DWORD since = GetTickCount();
    if (file_size(target) == GAME_FILE_SIZE) printf("The 64-bit game file is already in place.\n");
    else {
        if (!game_file_ready(depot, &last, &since)) {
            printf("\nOne step for you: download the 64-bit game file from Steam (about 750 MB once unpacked).\n");
            if (!g_test) {
                set_clipboard(DOWNLOAD_COMMAND);
                ShellExecuteA(NULL, "open", "steam://open/console", NULL, NULL, SW_SHOWNORMAL);
            }
            printf("  1. Steam's console is opening now. It can take a few seconds.\n");
            printf("  2. Click the box at the bottom of the console, press Ctrl+V, then Enter.\n");
            printf("     (If nothing pastes, type this:  %s )\n", DOWNLOAD_COMMAND);
            DWORD console_mode;
            int live = GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &console_mode) != 0;   // a real window: show progress on one line
            // a window gets the live line below; output to a file (the tests) keeps the old "Waiting" and dots
            printf("  3. Leave this window open. It carries on by itself when the download ends.\n\n%s", live ? "" : "Waiting");
            fflush(stdout);
            unsigned ticks = 0;
            int hinted = 0;
            while (!game_file_ready(depot, &last, &since)) {
                Sleep(2000);
                ticks++;
                if (!live) {   // output goes to a file (tests): the old dots
                    if (ticks % 15 == 0) { printf("."); fflush(stdout); }
                    continue;
                }
                uint64_t have = file_size(depot);
                if (have >= GAME_FILE_SIZE) {   // full size but not ready yet: Steam may set the size first and fill it in
                    printf("\r  Steam is finishing the download...                              ");
                } else if (have) {
                    printf("\r  Steam is downloading: %llu of %llu MB (%d%%)      ", (unsigned long long)(have >> 20),
                           (unsigned long long)(GAME_FILE_SIZE >> 20), (int)(have * 100 / GAME_FILE_SIZE > 99 ? 99 : have * 100 / GAME_FILE_SIZE));
                } else {
                    printf("\r  Waiting for Steam to start the download: %u:%02u      ", ticks * 2 / 60, ticks * 2 % 60);
                    // Three minutes with nothing: most likely the command was not entered, or Steam has not shown the file yet.
                    if (!hinted && ticks >= 90) {
                        hinted = 1;
                        printf("\n\n  Nothing has arrived yet. If Steam's CONSOLE page says \"Downloading depot 325624\", just wait:\n"
                               "  the file can show up only near the end. If it does not say that, click the box at the bottom of\n"
                               "  that page, type this and press Enter:  %s\n"
                               "  If the CONSOLE page is not open: press the Windows key + R, type  steam://open/console  and press Enter.\n\n",
                               DOWNLOAD_COMMAND);
                    }
                }
                fflush(stdout);
            }
            printf("\nDownload found.\n");
        }
        printf("Copying the game file...\n");
        if (!CopyFileA(depot, target, FALSE)) {
            printf("Could not copy the game file (error %lu). Close the game and run this again.\n", GetLastError());
            finish(1);
        }
    }

    // desktop icon
    snprintf(p, sizeof p, "%s\\attila64.exe", folder);
    if (!make_icon(p, folder, game)) printf("Could not make the desktop icon. You can start this instead:\n  %s\n", p);
    else printf("Desktop icon made: 64-bit Attila\n");

    if (file_size(target) != GAME_FILE_SIZE) printf("\nNote: Steam's game file is a different size than the one this was tested with. It may not work.\n");
    printf("\nDone! Keep Steam running and start the game from the \"64-bit Attila\" icon on your desktop.\n");
    printf("A mod list opens first: tick your mods and press Play. The first start can take a minute or more.\n");
    printf("\nIMPORTANT: Steam's Play button still starts NORMAL 32-bit Attila. Use the desktop icon for the 64-bit game.\n");
    printf("If something goes wrong, send this file:\n  %s\\attila64_log.txt\n", folder);
    finish(0);
    return 0;
}
