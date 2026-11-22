// Linux C library functions for the game, written on top of Windows. First batch: memory, strings, maths.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "loader.h"

// The table of everything offered to the Linux code; the $a and $c markers bound it.
__attribute__((section(".shimtab$a"), used)) static const ShimEntry shim_first = {0, 0};
__attribute__((section(".shimtab$c"), used)) static const ShimEntry shim_last = {0, 0};

const ShimEntry *shim_find(const char *name) {
    for (const ShimEntry *e = &shim_first + 1; e < &shim_last; e++)
        if (e->name && !strcmp(e->name, name)) return e;
    return NULL;
}

const char *shim_nearest(void *addr) {
    const ShimEntry *best = NULL;
    for (const ShimEntry *e = &shim_first + 1; e < &shim_last; e++)
        if (e->name && e->ptr <= addr && (!best || e->ptr > best->ptr)) best = e;
    return best ? best->name : "?";
}

// The game's home folder (what Linux calls HOME; settings, saves and replays live under it), Windows style, no slash at
// the end: %USERPROFILE%\attila64_home. ATTILA_TEST_HOME=<folder> puts it somewhere else: unattended tests work on a copy
// of the settings there (attila64\dx11\autobench.ps1), so the player's own settings and saves are never touched.
// Letters outside English (a Cyrillic or Polish user name, ...): the Linux game reads HOME as UTF-8, and its converter
// (CA::must_fix_string_widen, no checks) garbles any other byte above 127. Paths it builds as plain text from HOME still
// work (settings, writing a save), but the file lists widen and narrow the folder first (CA::Filesystem::enumerate), so the
// Load Campaign list, Continue Campaign and the multiplayer save list came up empty while the saves sat in the folder
// (player report 2026-11-15; reproduced with a test home named test_home_åäö). Such a folder is given by its Windows short
// name (8.3, English letters only): the same folder, so settings and saves stay where they are. The log never shows the
// folder (players post logs in public). ATTILA_HOME_LONG=1 keeps the long name (the old way, for tests).
static int plain_english(const wchar_t *s) { for (; *s; s++) if (*s < 32 || *s > 126) return 0; return 1; }
const char *attila_home_dir(void) {
    static char dir[MAX_PATH];
    if (!dir[0]) {
        wchar_t w[MAX_PATH], s[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"ATTILA_TEST_HOME", w, MAX_PATH);
        int test = n > 0 && n < MAX_PATH;
        if (!test) {
            n = GetEnvironmentVariableW(L"USERPROFILE", w, MAX_PATH - 20);
            if (!n || n >= MAX_PATH - 20) wcscpy(w, L".");
            wcscat(w, L"\\attila64_home");
        }
        int how = 0;   // 0 English letters only, 1 short name given, 2 no short name, 3 long name kept on purpose
        if (!plain_english(w)) {
            how = getenv("ATTILA_HOME_LONG") ? 3 : 2;
            if (how == 2) {
                CreateDirectoryW(w, NULL);   // a short name needs the folder to exist; the game makes the folders below it
                DWORD k = GetShortPathNameW(w, s, MAX_PATH);
                if (k && k < MAX_PATH && plain_english(s)) { wcscpy(w, s); how = 1; }
            }
        }
        if (!WideCharToMultiByte(CP_ACP, 0, w, -1, dir, sizeof dir, NULL, NULL)) snprintf(dir, sizeof dir, ".\\attila64_home");
        if (test) log_line("test run: the game's home folder is %s", dir);
        if (how == 1) log_line("home folder: its name has letters outside English, so the game is given its short name (the same folder)");
        else if (how == 2) log_line("home folder: its name has letters outside English and Windows keeps no short name for it: saved games may not show in the load lists");
        else if (how == 3) log_line("home folder: letters outside English, long name kept (ATTILA_HOME_LONG)");
    }
    return dir;
}

// Environment handed to the game: a Linux-style view of what it asks for.
static char *g_env[8];
char **shim_environ(void) {
    static int done;
    if (!done) {
        static char home[MAX_PATH + 8];
        snprintf(home, sizeof home, "HOME=%s", attila_home_dir());
        for (char *p = home; *p; p++) if (*p == '\\') *p = '/';
        g_env[0] = home;
        g_env[1] = "LANG=C";
        // Mod scripts written for Windows Attila ask for these (The Dawnless Days: os.getenv("APPDATA").."\The Creative
        // Assembly\Attila\army_setups\"; without it the script stops with "attempt to concatenate a nil value"). The Windows
        // values; shim_stdio.c sends paths in normal Attila's settings folder to this engine's own (attila_home_dir).
        static char ad[MAX_PATH + 16], lad[MAX_PATH + 16], up[MAX_PATH + 16];
        int k = 2;
        const char *v;
        if ((v = getenv("APPDATA"))) { snprintf(ad, sizeof ad, "APPDATA=%s", v); g_env[k++] = ad; }
        if ((v = getenv("LOCALAPPDATA"))) { snprintf(lad, sizeof lad, "LOCALAPPDATA=%s", v); g_env[k++] = lad; }
        if ((v = getenv("USERPROFILE"))) { snprintf(up, sizeof up, "USERPROFILE=%s", v); g_env[k++] = up; }
        // The sign for mod scripts that this is 64-bit Attila: os.getenv("ATTILA64") == "1" here, nil in normal Attila
        // (1212AD's script offers the switch to 64-bit only when it is nil).
        g_env[k++] = "ATTILA64=1";
        done = 1;
    }
    return g_env;
}

// memory
static uint64_t g_big_calls, g_big_ticks, g_big_bytes, g_bigfree_calls, g_bigfree_ticks;
int g_measure_memory;
#define BIG_BLOCK (1u << 20)
SHIM(void *, malloc, (size_t n)) {
    if (n < BIG_BLOCK) return malloc(n);
    uint64_t t0 = __builtin_ia32_rdtsc();
    void *p = malloc(n);
    g_big_ticks += __builtin_ia32_rdtsc() - t0;
    g_big_calls++;
    g_big_bytes += n;
    return p;
}
void shim_mem_report(double per_ms) {
    log_line("big memory blocks: %llu allocations %.0f MB %.1f ms, %llu frees %.1f ms", (unsigned long long)g_big_calls, (double)g_big_bytes / 1048576.0, (double)g_big_ticks / per_ms,
             (unsigned long long)g_bigfree_calls, (double)g_bigfree_ticks / per_ms);
    g_big_calls = g_big_ticks = g_big_bytes = g_bigfree_calls = g_bigfree_ticks = 0;
}
// ATTILA_DEBUG=1 turns on the slow checks and detailed logs.
int shim_debug(void) {
    static int v = -1;
    if (v < 0) v = getenv("ATTILA_DEBUG") != NULL;
    return v;
}

// Debug only: a block that did not come from this heap is named and skipped, instead of ending the process with no clue who did it.
static int heap_block_ok(void *p, const char *who, void *caller) {
    if (!shim_debug() || HeapValidate(GetProcessHeap(), 0, p)) return 1;
    static int shown;
    if (shown++ < 20) {
        extern const char *loader_where(void *addr);
        log_line("%s of a bad block %p from %s", who, p, loader_where(caller));
    }
    return 0;
}
SHIM(void, free, (void *p)) {
    if (p && !heap_block_ok(p, "free", __builtin_return_address(0))) return;
    if (g_measure_memory && p && _msize(p) >= BIG_BLOCK) {
        uint64_t t0 = __builtin_ia32_rdtsc();
        free(p);
        g_bigfree_ticks += __builtin_ia32_rdtsc() - t0;
        g_bigfree_calls++;
        return;
    }
    free(p);
}
SHIM(void *, calloc, (size_t a, size_t b)) { return calloc(a, b); }
SHIM(void *, realloc, (void *p, size_t n)) {
    if (p && !heap_block_ok(p, "realloc", __builtin_return_address(0))) return malloc(n);
    return realloc(p, n);
}
// The compiler's built-in copies; timed against ucrtbase.dll's on 2026-11-07 (16 B to 6 MB): no clear winner, so they stay.
SHIM(void *, memcpy, (void *d, const void *s, size_t n)) { return memcpy(d, s, n); }
SHIM(void *, memmove, (void *d, const void *s, size_t n)) { return memmove(d, s, n); }
SHIM(void *, memset, (void *d, int c, size_t n)) { return memset(d, c, n); }
SHIM(void *, __memset_chk, (void *d, int c, size_t n, size_t dl)) { return memset(d, c, n); }
SHIM(int, memcmp, (const void *a, const void *b, size_t n)) { return memcmp(a, b, n); }
SHIM(void *, memchr, (const void *a, int c, size_t n)) { return (void *)memchr(a, c, n); }

// strings
SHIM(size_t, strlen, (const char *s)) { return strlen(s); }
SHIM(char *, strcpy, (char *d, const char *s)) { return strcpy(d, s); }
SHIM(char *, strncpy, (char *d, const char *s, size_t n)) { return strncpy(d, s, n); }
SHIM(char *, __strncpy_chk, (char *d, const char *s, size_t n, size_t dl)) { return strncpy(d, s, n); }
SHIM(char *, strcat, (char *d, const char *s)) { return strcat(d, s); }
SHIM(char *, strncat, (char *d, const char *s, size_t n)) { return strncat(d, s, n); }
SHIM(int, strcmp, (const char *a, const char *b)) { return strcmp(a, b); }
SHIM(int, strncmp, (const char *a, const char *b, size_t n)) { return strncmp(a, b, n); }
SHIM(int, strcoll, (const char *a, const char *b)) { return strcoll(a, b); }
SHIM(char *, strchr, (const char *s, int c)) { return (char *)strchr(s, c); }
SHIM(char *, strrchr, (const char *s, int c)) { return (char *)strrchr(s, c); }
SHIM(char *, strstr, (const char *a, const char *b)) { return (char *)strstr(a, b); }
SHIM(char *, strpbrk, (const char *a, const char *b)) { return (char *)strpbrk(a, b); }
SHIM(char *, strtok, (char *a, const char *b)) { return strtok(a, b); }
SHIM(char *, strerror, (int e)) { return strerror(e); }
SHIM(long long, strtol, (const char *s, char **e, int b)) { return strtoll(s, e, b); }
SHIM(unsigned long long, strtoul, (const char *s, char **e, int b)) { return strtoull(s, e, b); }
SHIM(double, strtod, (const char *s, char **e)) { return strtod(s, e); }
SHIM(double, atof, (const char *s)) { return atof(s); }
SHIM(int, atoi, (const char *s)) { return atoi(s); }
SHIM(int, isspace, (int c)) { return isspace(c); }
SHIM(int, toupper, (int c)) { return toupper(c); }

// maths: floating point arguments arrive in the same registers on both systems for these signatures
SHIM(double, sin, (double x)) { return sin(x); }
SHIM(double, cos, (double x)) { return cos(x); }
SHIM(double, tan, (double x)) { return tan(x); }
SHIM(double, asin, (double x)) { return asin(x); }
SHIM(double, acos, (double x)) { return acos(x); }
SHIM(double, atan, (double x)) { return atan(x); }
SHIM(double, atan2, (double y, double x)) { return atan2(y, x); }
SHIM(double, sinh, (double x)) { return sinh(x); }
SHIM(double, cosh, (double x)) { return cosh(x); }
SHIM(double, tanh, (double x)) { return tanh(x); }
SHIM(double, exp, (double x)) { return exp(x); }
SHIM(double, log, (double x)) { return log(x); }
SHIM(double, log10, (double x)) { return log10(x); }
SHIM(double, pow, (double x, double y)) { return pow(x, y); }
SHIM(double, sqrt, (double x)) { return sqrt(x); }
SHIM(double, floor, (double x)) { return floor(x); }
SHIM(double, fmod, (double x, double y)) { return fmod(x, y); }
SHIM(double, ldexp, (double x, int e)) { return ldexp(x, e); }
SHIM(double, frexp, (double x, int *e)) { return frexp(x, e); }
SHIM(double, modf, (double x, double *i)) { return modf(x, i); }
SHIM(float, sinf, (float x)) { return sinf(x); }
SHIM(float, cosf, (float x)) { return cosf(x); }
SHIM(float, tanf, (float x)) { return tanf(x); }
SHIM(float, asinf, (float x)) { return asinf(x); }
SHIM(float, acosf, (float x)) { return acosf(x); }
SHIM(float, atan2f, (float y, float x)) { return atan2f(y, x); }
SHIM(float, expf, (float x)) { return expf(x); }
SHIM(float, logf, (float x)) { return logf(x); }
SHIM(float, log10f, (float x)) { return log10f(x); }
SHIM(float, powf, (float x, float y)) { return powf(x, y); }
SHIM(float, sqrtf, (float x)) { return sqrtf(x); }
SHIM(float, floorf, (float x)) { return floorf(x); }
SHIM(float, ceilf, (float x)) { return ceilf(x); }
SHIM(float, fmodf, (float x, float y)) { return fmodf(x, y); }
SHIM(float, modff, (float x, float *i)) { return modff(x, i); }
SHIM(void, sincos, (double x, double *s, double *c)) { *s = sin(x); *c = cos(x); }
SHIM(void, sincosf, (float x, float *s, float *c)) { *s = sinf(x); *c = cosf(x); }

// process start: the game's _start ends up here
typedef SYSV int (*main_fn)(int, char **, char **);
typedef SYSV void (*start_init_fn)(int, char **, char **);
SHIM(int, __libc_start_main, (main_fn m, int argc, char **argv, start_init_fn init, void (*fini)(void), void (*rtld)(void), void *stack)) {
    char **envp = shim_environ();
    log_line("__libc_start_main: running the game's own initialisers");
    if (init) init(argc, argv, envp);
    log_line("reached the game's main()");
    extern void loader_game_settings(void);
    loader_game_settings();
    int r = m(argc, argv, envp);
    log_go_sync();
    log_line("main returned %d", r);
    { extern void dx12_start_worked(void); dx12_start_worked(); }   // the game ended by itself: not a failed DirectX 12 start
    extern void shim_run_exit_jobs(void);
    shim_run_exit_jobs();
    ExitProcess((UINT)r);
    return r;
}

SHIM(void, abort, (void)) { fatal("the game called abort()"); }
SHIM(void, __stack_chk_fail, (void)) { fatal("stack check failed"); }
SHIM(void, _exit, (int c)) { log_go_sync(); log_line("_exit(%d)", c); ExitProcess((UINT)c); }
SHIM(void, exit, (int c)) {
    extern void shim_run_exit_jobs(void);
    log_go_sync();
    log_line("exit(%d)", c);
    shim_run_exit_jobs();
    ExitProcess((UINT)c);
}

// thread-local storage used by libstdc++
typedef struct { uint64_t module, offset; } TlsIndex;
SHIM(void *, __tls_get_addr, (TlsIndex *ti)) { return (char *)tls_block_for((int)ti->module) + ti->offset; }

// lets the C++ exception unwinder find code ranges
struct dl_phdr_info_l { uint64_t addr; const char *name; const void *phdr; uint16_t phnum; uint64_t adds, subs; size_t tls_modid; void *tls_data; };
typedef SYSV int (*phdr_cb)(struct dl_phdr_info_l *, size_t, void *);
SHIM(int, dl_iterate_phdr, (phdr_cb cb, void *data)) {
    int r = 0;
    for (int i = 0; i < loader_module_count() && !r; i++) {
        const LoadedModule *m = loader_module(i);
        struct dl_phdr_info_l info;
        memset(&info, 0, sizeof info);
        info.addr = (uint64_t)m->base;
        info.name = m->name;
        info.phdr = m->phdr;
        info.phnum = (uint16_t)m->phnum;
        info.adds = (uint64_t)loader_module_count();
        r = cb(&info, sizeof info, data);
    }
    return r;
}
