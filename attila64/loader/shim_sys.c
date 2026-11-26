// Linux system services for the game: memory mapping, time, environment, sorting, text tables, wide text (32-bit on Linux).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ctype.h>
#include <errno.h>
#include <io.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "loader.h"

// errno
SHIM(int *, __errno_location, (void)) { return _errno(); }

// Exit-time jobs: the game saves its settings in one of these, so they must run when it ends, last registered first.
typedef SYSV void (*exit_job_fn)(void *);
typedef struct { exit_job_fn fn; void *arg; } ExitJob;
static ExitJob *g_exit_jobs;
static size_t g_exit_count, g_exit_cap;
static SRWLOCK g_exit_lock = SRWLOCK_INIT;
SHIM(int, __cxa_atexit, (exit_job_fn f, void *a, void *d)) {
    AcquireSRWLockExclusive(&g_exit_lock);
    if (g_exit_count == g_exit_cap) {
        g_exit_cap = g_exit_cap ? g_exit_cap * 2 : 1024;
        g_exit_jobs = realloc(g_exit_jobs, g_exit_cap * sizeof(ExitJob));
    }
    g_exit_jobs[g_exit_count].fn = f;
    g_exit_jobs[g_exit_count].arg = a;
    g_exit_count++;
    ReleaseSRWLockExclusive(&g_exit_lock);
    return 0;
}
void *shim_settings_object(int refresh);
void shim_run_exit_jobs(void) {
    static volatile LONG once;
    if (InterlockedExchange(&once, 1)) return;
    // The registered jobs just tear down memory that is about to go anyway, and running them while the game's threads are still
    // alive corrupted the heap, so they are skipped. Only the settings need saving, the same way the game's save command does it.
    typedef SYSV void (*save_fn)(void *);
    save_fn save = (save_fn)0x573320;   // EMPIRE::ENVIRONMENT::save_preferences
    static const uint8_t save_start[5] = {0x41, 0x57, 0x41, 0x56, 0x53};
    if (memcmp((void *)save, save_start, sizeof save_start)) { log_line("settings not saved: game code differs"); return; }
    void *env = shim_settings_object(0);
    if (!env) { log_line("settings not saved: settings object never seen"); return; }
    log_line("saving settings");
    save(env);
    log_line("settings saved");
}

// EMPIRE_APP_MODULE::s_instance -> variable_manager_get() is the settings object. Noted while the game runs, because the module
// may be gone by the time main has returned.
void *shim_settings_object(int refresh) {
    static void *volatile env;
    if (refresh && !env) {
        void *module = *(void **)0x4c19150;
        typedef SYSV void *(*get_fn)(void *);
        if (module) env = ((get_fn)0x57d090)(module);
        if (env) log_line("settings object found at %p", env);
    }
    return env;
}
SHIM(int, __cxa_thread_atexit_impl, (void *f, void *a, void *d)) { return 0; }
SHIM(void, __assert_fail, (const char *e, const char *file, unsigned line, const char *fn)) { fatal("assert %s at %s:%u in %s", e, file, line, fn); }

// memory mapping
enum { LX_MAP_ANON = 0x20, LX_PROT_WRITE = 2 };
SHIM(void *, mmap, (void *addr, size_t len, int prot, int flags, int fd, long long off)) {
    if (flags & LX_MAP_ANON) {
        void *p = VirtualAlloc(NULL, len, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (!p) { errno = ENOMEM; return (void *)-1; }
        return p;
    }
    HANDLE f = (HANDLE)_get_osfhandle(fd);
    HANDLE map = CreateFileMappingA(f, NULL, (prot & LX_PROT_WRITE) ? PAGE_WRITECOPY : PAGE_READONLY, 0, 0, NULL);
    if (!map) { errno = EACCES; return (void *)-1; }
    long long aligned = off & ~0xFFFFLL;
    size_t delta = (size_t)(off - aligned);
    char *v = MapViewOfFile(map, (prot & LX_PROT_WRITE) ? FILE_MAP_COPY : FILE_MAP_READ, (DWORD)(aligned >> 32), (DWORD)aligned, len + delta);
    CloseHandle(map);
    if (!v) { errno = EACCES; return (void *)-1; }
    return v + delta;
}
SHIM(int, munmap, (void *addr, size_t len)) {
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery(addr, &m, sizeof m)) return -1;
    if (m.Type == MEM_MAPPED) UnmapViewOfFile(m.AllocationBase);
    else if (m.AllocationBase == addr && m.RegionSize >= len) {
        MEMORY_BASIC_INFORMATION rest;
        char *after = (char *)addr + m.RegionSize;
        if (VirtualQuery(after, &rest, sizeof rest) && rest.AllocationBase == addr) VirtualFree(addr, len, MEM_DECOMMIT);
        else VirtualFree(addr, 0, MEM_RELEASE);
    } else VirtualFree(addr, len, MEM_DECOMMIT);
    return 0;
}

// time
typedef struct { int64_t sec, nsec; } TimespecL;
typedef struct { int64_t sec, usec; } TimevalL;
// struct tm on Linux has two more fields after the shared nine.
typedef struct { struct tm t; int pad; long long gmtoff; const char *zone; } TmL;

SHIM(long long, time, (long long *o)) {
    long long t = (long long)_time64(NULL);
    if (o) *o = t;
    return t;
}
extern volatile long g_tt_on, g_tt_gtod, g_tt_clock, g_tt_sleep[4];
void time_trace_tick(void);
SHIM(int, gettimeofday, (TimevalL *o, void *tz)) {
    if (g_tt_on) { InterlockedIncrement(&g_tt_gtod); time_trace_tick(); }
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    int64_t t = ((int64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000LL;
    o->sec = t / 10000000;
    o->usec = (t % 10000000) / 10;
    return 0;
}
// Test switch ATTILA_TIME_TRACE=1: every 5 s, how often each clock and wait was used (to find what paces the battle).
volatile long g_tt_on, g_tt_cg[8], g_tt_gtod, g_tt_clock, g_tt_sleep[4], g_tt_cond[5], g_tt_condinf, g_tt_sem[5];
void time_trace_tick(void) {
    static volatile LONG busy; static ULONGLONG last;
    ULONGLONG now = GetTickCount64();
    if (now - last < 5000 || InterlockedExchange(&busy, 1)) return;
    if (last) log_line("time trace /5s: clock_gettime id0..7 %ld %ld %ld %ld %ld %ld %ld %ld, gettimeofday %ld, clock() %ld, sleeps <0.1ms %ld <2ms %ld <20ms %ld more %ld, cond waits 0ms %ld <2 %ld <20 %ld <200 %ld more %ld forever %ld, sem waits 0ms %ld <2 %ld <20 %ld <200 %ld more %ld",
        g_tt_cg[0], g_tt_cg[1], g_tt_cg[2], g_tt_cg[3], g_tt_cg[4], g_tt_cg[5], g_tt_cg[6], g_tt_cg[7], g_tt_gtod, g_tt_clock, g_tt_sleep[0], g_tt_sleep[1], g_tt_sleep[2], g_tt_sleep[3],
        g_tt_cond[0], g_tt_cond[1], g_tt_cond[2], g_tt_cond[3], g_tt_cond[4], g_tt_condinf, g_tt_sem[0], g_tt_sem[1], g_tt_sem[2], g_tt_sem[3], g_tt_sem[4]);
    memset((void *)g_tt_cg, 0, sizeof g_tt_cg); g_tt_gtod = g_tt_clock = g_tt_condinf = 0;
    memset((void *)g_tt_sleep, 0, sizeof g_tt_sleep); memset((void *)g_tt_cond, 0, sizeof g_tt_cond); memset((void *)g_tt_sem, 0, sizeof g_tt_sem);
    last = now; busy = 0;
}
SHIM(int, clock_gettime, (int id, TimespecL *o)) {
    if (g_tt_on) { InterlockedIncrement(&g_tt_cg[id & 7]); time_trace_tick(); }
    if (id == 0) {
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        int64_t t = ((int64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000LL;
        o->sec = t / 10000000;
        o->nsec = (t % 10000000) * 100;
    } else {
        static LARGE_INTEGER fq;
        LARGE_INTEGER c;
        if (!fq.QuadPart) QueryPerformanceFrequency(&fq);
        QueryPerformanceCounter(&c);
        o->sec = c.QuadPart / fq.QuadPart;
        o->nsec = (c.QuadPart % fq.QuadPart) * 1000000000LL / fq.QuadPart;
    }
    return 0;
}
SHIM(long long, clock, (void)) {
    if (g_tt_on) InterlockedIncrement(&g_tt_clock);
    static int mode = -1;
    if (mode < 0) mode = getenv("ATTILA_CLOCK_REAL") ? 1 : 0;
    // Film stutter (2026-11-26, Workshop reports of stuttering intro film and event clips): a film's frame comes from the sound
    // engine's play position (VP8::MOVIE::calculate_target_frame 342c4a0 -> get_movie_time -> Wwise CAkPositionRepository::
    // GetCurrPosition 368d760), which adds "clock() since the last sound buffer" to the buffer's position. clock() is processor
    // time: smooth on Linux, but Windows counts it in 15.6 ms steps, so the film position jumped 15-20 ms at a time and fell
    // back 10 ms at each new buffer (film frames shown for 10-50 ms instead of 33, some skipped). Wwise uses clock() as its
    // one stopwatch (all 17 of its calls: position + segment repositories, CAkAudioMgr::Perform, CAkLEngine::GetBuffer, the
    // stream manager), and the time stamps are passed between those parts: so for every call from the Wwise library
    // (363a000..3740000) clock() is real time. Only the position repository's calls (tried first) gave a half-speed film.
    // The game's other clock() calls (Lua os.clock, debug menu) keep processor time. ATTILA_NO_FILM_CLOCK_FIX=1 = off.
    static int film_fix = -1;
    if (film_fix < 0) { film_fix = getenv("ATTILA_NO_FILM_CLOCK_FIX") == NULL; if (film_fix) log_line("film clock: the sound engine counts real time"); }
    uintptr_t from = (uintptr_t)__builtin_return_address(0);
    if (mode || (film_fix && from >= 0x363a000 && from < 0x3740000)) {   // real time since the first call, in millionths of a second
        static LARGE_INTEGER fq, c0; LARGE_INTEGER c;
        if (!fq.QuadPart) { QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&c0); }
        QueryPerformanceCounter(&c);
        return (long long)((c.QuadPart - c0.QuadPart) * 1000000 / fq.QuadPart);
    }
    FILETIME a, b, k, u;
    GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u);
    int64_t t = ((int64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((int64_t)u.dwHighDateTime << 32 | u.dwLowDateTime);
    return t / 10;      // Linux counts millionths of a second
}
SHIM(double, difftime, (long long a, long long b)) { return (double)(a - b); }
static __thread TmL g_tm;
static TmL *keep_tm(struct tm *s) {
    if (!s) return NULL;
    memset(&g_tm, 0, sizeof g_tm);
    g_tm.t = *s;
    g_tm.zone = "UTC";
    return &g_tm;
}
SHIM(TmL *, gmtime, (const long long *t)) { __time64_t v = *t; return keep_tm(_gmtime64(&v)); }
SHIM(TmL *, localtime, (const long long *t)) { __time64_t v = *t; return keep_tm(_localtime64(&v)); }
SHIM(long long, mktime, (struct tm *t)) { return (long long)_mktime64(t); }
SHIM(size_t, strftime, (char *d, size_t n, const char *f, const struct tm *t)) { return strftime(d, n, f, t); }
// Sleep() only counts whole milliseconds at about 15 ms steps: a 500 microsecond wait became no wait at all, a 1 ms wait became 15 ms.
// A high resolution timer per thread sleeps for what was asked, about half a millisecond at best; shorter waits just give up the CPU.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#endif
static void sleep_ns(uint64_t ns) {
    static __thread HANDLE timer;
    static __thread int no_timer;
    if (g_tt_on) InterlockedIncrement(&g_tt_sleep[ns < 100000 ? 0 : ns < 2000000 ? 1 : ns < 20000000 ? 2 : 3]);
    if (ns < 100000) { SwitchToThread(); return; }
    if (!timer && !no_timer) {
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) no_timer = 1;  // older Windows
    }
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(ns / 100);
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) { WaitForSingleObject(timer, INFINITE); return; }
    }
    Sleep((DWORD)((ns + 999999) / 1000000));
}
SHIM(int, usleep, (unsigned us)) { sleep_ns((uint64_t)us * 1000); return 0; }
SHIM(int, nanosleep, (const TimespecL *t, TimespecL *rem)) {
    sleep_ns((uint64_t)t->sec * 1000000000ull + (uint64_t)t->nsec);
    if (rem) { rem->sec = 0; rem->nsec = 0; }
    return 0;
}

// environment and identity
SHIM(char *, getenv, (const char *n)) {
    extern char **shim_environ(void);
    size_t k = strlen(n);
    for (char **e = shim_environ(); *e; e++)
        if (!strncmp(*e, n, k) && (*e)[k] == '=') return *e + k + 1;
    return NULL;
}
SHIM(int, getpid, (void)) { return (int)GetCurrentProcessId(); }
SHIM(unsigned, getuid, (void)) { return 1000; }
SHIM(int, kill, (int p, int s)) { return 0; }
SHIM(int, raise, (int s)) { log_line("raise(%d)", s); return 0; }
SHIM(int, getpagesize, (void)) { return 4096; }
SHIM(int, setrlimit, (int r, const void *l)) { return 0; }
SHIM(int, get_nprocs, (void)) { SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }
SHIM(long long, sysconf, (int name)) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    switch (name) {
    case 30: return 4096;                          // _SC_PAGESIZE
    case 83: case 84: {                            // _SC_NPROCESSORS_CONF / ONLN
        // The game starts one task thread per processor it is told of, less one for the main thread (8 -> 7 here).
        // ATTILA_CORES=n (test) tells it another number: 9 gives 8 task threads on an 8-thread processor.
        const char *e = getenv("ATTILA_CORES");
        if (e && atoi(e) >= 2 && atoi(e) <= 64) return atoi(e);
        return si.dwNumberOfProcessors;
    }
    case 85: case 86: {                            // _SC_PHYS_PAGES / AVPHYS_PAGES
        MEMORYSTATUSEX ms;
        ms.dwLength = sizeof ms;
        GlobalMemoryStatusEx(&ms);
        return (long long)((name == 85 ? ms.ullTotalPhys : ms.ullAvailPhys) / 4096);
    }
    }
    log_line("sysconf(%d) not handled", name);
    return -1;
}
typedef struct { char *name, *passwd; unsigned uid, gid; char *gecos, *dir, *shell; } PasswdL;
SHIM(PasswdL *, getpwuid, (unsigned u)) {
    static PasswdL p;
    p.name = "player";
    p.passwd = "x";
    p.uid = p.gid = 1000;
    p.gecos = "";
    p.dir = sh_getenv("HOME");
    p.shell = "";
    return &p;
}
SHIM(int, dladdr, (const void *a, void *info)) { return 0; }

// random numbers: Linux range, not the much smaller Windows one
static unsigned long long g_rand = 1;
SHIM(void, srand, (unsigned s)) { g_rand = s; }
SHIM(int, rand, (void)) {
    g_rand = g_rand * 6364136223846793005ULL + 1442695040888963407ULL;
    return (int)((g_rand >> 33) & 0x7fffffff);
}

// sorting: the compare function is Linux code. Stable merge sort, like glibc's usual path.
typedef SYSV int (*cmp_fn)(const void *, const void *);
static void merge_sort(char *a, char *tmp, size_t n, size_t w, cmp_fn c) {
    if (n < 2) return;
    size_t h = n / 2;
    merge_sort(a, tmp, h, w, c);
    merge_sort(a + h * w, tmp, n - h, w, c);
    char *l = a, *r = a + h * w, *le = r, *re = a + n * w, *o = tmp;
    while (l < le && r < re) {
        if (c(l, r) <= 0) { memcpy(o, l, w); l += w; } else { memcpy(o, r, w); r += w; }
        o += w;
    }
    if (l < le) { memcpy(o, l, (size_t)(le - l)); o += le - l; }
    if (r < re) { memcpy(o, r, (size_t)(re - r)); o += re - r; }
    memcpy(a, tmp, n * w);
}
SHIM(void, qsort, (void *base, size_t n, size_t w, cmp_fn c)) {
    if (n < 2 || !w) return;
    char *tmp = malloc(n * w);
    merge_sort(base, tmp, n, w, c);
    free(tmp);
}

// glibc's character tables: index -128..255, with its own bit values
enum { F_UP = 0x100, F_LO = 0x200, F_AL = 0x400, F_DI = 0x800, F_XD = 0x1000, F_SP = 0x2000, F_PR = 0x4000, F_GR = 0x8000,
       F_BL = 0x1, F_CN = 0x2, F_PU = 0x4, F_AN = 0x8 };
static unsigned short g_ctype[384];
static int g_lower[384], g_upper[384];
static const unsigned short *g_ctype_p;
static const int *g_lower_p, *g_upper_p;
__attribute__((constructor)) static void ctype_setup(void) {
    for (int i = -128; i < 256; i++) {
        unsigned short f = 0;
        int c = i & 0xff;
        if (i >= 0 && i < 128) {
            if (c >= 'A' && c <= 'Z') f |= F_UP | F_AL | F_AN;
            if (c >= 'a' && c <= 'z') f |= F_LO | F_AL | F_AN;
            if (c >= '0' && c <= '9') f |= F_DI | F_XD | F_AN;
            if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) f |= F_XD;
            if (c == ' ' || (c >= 9 && c <= 13)) f |= F_SP;
            if (c == ' ' || c == 9) f |= F_BL;
            if (c < 32 || c == 127) f |= F_CN;
            if (c >= 32 && c < 127) f |= F_PR;
            if (c > 32 && c < 127) f |= F_GR;
            if (c > 32 && c < 127 && !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) f |= F_PU;
        }
        g_ctype[i + 128] = f;
        g_lower[i + 128] = (i >= 'A' && i <= 'Z') ? i + 32 : i;
        g_upper[i + 128] = (i >= 'a' && i <= 'z') ? i - 32 : i;
    }
    g_ctype_p = g_ctype + 128;
    g_lower_p = g_lower + 128;
    g_upper_p = g_upper + 128;
}
SHIM(const unsigned short **, __ctype_b_loc, (void)) { return &g_ctype_p; }
SHIM(const int **, __ctype_tolower_loc, (void)) { return &g_lower_p; }
SHIM(const int **, __ctype_toupper_loc, (void)) { return &g_upper_p; }
SHIM(size_t, __ctype_get_mb_cur_max, (void)) { return 1; }
SHIM(int, isalnum, (int c)) { return c >= 0 && c < 128 && isalnum(c); }

// the C locale, in the layout libstdc++ reads directly
typedef struct {
    void *locales[13];
    const unsigned short *ctype_b;
    const int *ctype_tolower, *ctype_toupper;
    const char *names[13];
} LocaleL;
static LocaleL g_c_locale;
static LocaleL *c_locale(void) {
    g_c_locale.ctype_b = g_ctype_p;
    g_c_locale.ctype_tolower = g_lower_p;
    g_c_locale.ctype_toupper = g_upper_p;
    for (int i = 0; i < 13; i++) g_c_locale.names[i] = "C";
    return &g_c_locale;
}
SHIM(LocaleL *, __newlocale, (int mask, const char *n, LocaleL *base)) { return c_locale(); }
SHIM(LocaleL *, __duplocale, (LocaleL *l)) { return c_locale(); }
SHIM(void, __freelocale, (LocaleL *l)) {}
SHIM(LocaleL *, __uselocale, (LocaleL *l)) { return c_locale(); }
SHIM(char *, setlocale, (int cat, const char *n)) { return "C"; }
SHIM(const char *, __nl_langinfo_l, (int item, LocaleL *l)) {
    switch (item) {
    case 0x10000: return ".";    // RADIXCHAR
    case 0x10001: return "";     // THOUSEP
    case 14: return "ANSI_X3.4-1968"; // CODESET
    }
    return "";
}
SHIM(int, __strcoll_l, (const char *a, const char *b, LocaleL *l)) { return strcmp(a, b); }
SHIM(size_t, __strxfrm_l, (char *d, const char *s, size_t n, LocaleL *l)) {
    size_t k = strlen(s);
    if (n) { strncpy(d, s, n); if (k < n) d[k] = 0; }
    return k;
}
SHIM(double, __strtod_l, (const char *s, char **e, LocaleL *l)) { return strtod(s, e); }
SHIM(float, __strtof_l, (const char *s, char **e, LocaleL *l)) { return strtof(s, e); }
SHIM(size_t, __strftime_l, (char *d, size_t n, const char *f, const struct tm *t, LocaleL *l)) { return strftime(d, n, f, t); }
SHIM(char *, gettext, (const char *s)) { return (char *)s; }
SHIM(char *, textdomain, (const char *s)) { return (char *)s; }
SHIM(char *, bindtextdomain, (const char *a, const char *b)) { return (char *)b; }
typedef struct { char *decimal_point; char *rest[32]; } LconvL;
SHIM(LconvL *, localeconv, (void)) {
    static LconvL lc;
    static char dot[] = ".", none[] = "";
    lc.decimal_point = dot;
    for (int i = 0; i < 9; i++) lc.rest[i] = none;
    return &lc;
}
SHIM(double, ceil, (double x)) { return ceil(x); }

// wide text: 32 bits per character on Linux, so nothing can go to the Windows wide functions
typedef uint32_t wl;
SHIM(size_t, wcslen, (const wl *s)) { size_t n = 0; while (s[n]) n++; return n; }
SHIM(int, wcscmp, (const wl *a, const wl *b)) {
    while (*a && *a == *b) { a++; b++; }
    return *a < *b ? -1 : *a > *b;
}
SHIM(int, wcsncmp, (const wl *a, const wl *b, size_t n)) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return *a < *b ? -1 : 1;
        if (!*a) return 0;
    }
    return 0;
}
SHIM(wl *, wcscpy, (wl *d, const wl *s)) { wl *r = d; while ((*d++ = *s++)) {} return r; }
SHIM(wl *, wmemcpy, (wl *d, const wl *s, size_t n)) { return memcpy(d, s, n * 4); }
SHIM(wl *, wmemmove, (wl *d, const wl *s, size_t n)) { return memmove(d, s, n * 4); }
SHIM(wl *, wmemset, (wl *d, wl c, size_t n)) { for (size_t i = 0; i < n; i++) d[i] = c; return d; }
SHIM(int, wmemcmp, (const wl *a, const wl *b, size_t n)) {
    for (; n; n--, a++, b++) if (*a != *b) return *a < *b ? -1 : 1;
    return 0;
}
SHIM(wl *, wmemchr, (const wl *a, wl c, size_t n)) {
    for (; n; n--, a++) if (*a == c) return (wl *)a;
    return NULL;
}
SHIM(size_t, wcstombs, (char *d, const wl *s, size_t n)) {
    size_t i = 0;
    for (; s[i] && (!d || i < n); i++) if (d) d[i] = s[i] < 256 ? (char)s[i] : '?';
    if (d && i < n) d[i] = 0;
    return i;
}
SHIM(wl, btowc, (int c)) { return c >= 0 && c < 256 ? (wl)c : (wl)-1; }
SHIM(int, wctob, (wl c)) { return c < 256 ? (int)c : -1; }
SHIM(size_t, mbrtowc, (wl *o, const char *s, size_t n, void *st)) {
    if (!s) return 0;
    if (!n) return (size_t)-2;
    if (o) *o = (uint8_t)*s;
    return *s ? 1 : 0;
}
SHIM(size_t, wcrtomb, (char *o, wl c, void *st)) {
    if (!o) return 1;
    *o = c < 256 ? (char)c : '?';
    return 1;
}
SHIM(size_t, mbsrtowcs, (wl *d, const char **s, size_t n, void *st)) {
    const char *p = *s;
    size_t i = 0;
    for (; (!d || i < n); i++, p++) {
        if (d) d[i] = (uint8_t)*p;
        if (!*p) { if (d) *s = NULL; return i; }
    }
    *s = p;
    return i;
}
SHIM(size_t, mbsnrtowcs, (wl *d, const char **s, size_t nms, size_t n, void *st)) {
    const char *p = *s;
    size_t i = 0;
    for (; i < nms && (!d || i < n); i++, p++) {
        if (d) d[i] = (uint8_t)*p;
        if (!*p) { if (d) *s = NULL; return i; }
    }
    if (d) *s = p;
    return i;
}
SHIM(size_t, wcsnrtombs, (char *d, const wl **s, size_t nwc, size_t n, void *st)) {
    const wl *p = *s;
    size_t i = 0;
    for (; i < nwc && (!d || i < n); i++, p++) {
        if (d) d[i] = *p < 256 ? (char)*p : '?';
        if (!*p) { if (d) *s = NULL; return i; }
    }
    if (d) *s = p;
    return i;
}
SHIM(int, __wcscoll_l, (const wl *a, const wl *b, void *l)) { return sh_wcscmp(a, b); }
SHIM(size_t, __wcsxfrm_l, (wl *d, const wl *s, size_t n, void *l)) {
    size_t k = sh_wcslen(s);
    for (size_t i = 0; i < n && i <= k; i++) d[i] = s[i];
    return k;
}
SHIM(size_t, __wcsftime_l, (wl *d, size_t n, const wl *f, const void *t, void *l)) { if (n) d[0] = 0; return 0; }
SHIM(wl, __towlower_l, (wl c, void *l)) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
SHIM(wl, __towupper_l, (wl c, void *l)) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
SHIM(unsigned long long, __wctype_l, (const char *n, void *l)) {
    static const struct { const char *n; unsigned short f; } m[] = {{"upper", F_UP}, {"lower", F_LO}, {"alpha", F_AL}, {"digit", F_DI},
        {"xdigit", F_XD}, {"space", F_SP}, {"print", F_PR}, {"graph", F_GR}, {"blank", F_BL}, {"cntrl", F_CN}, {"punct", F_PU}, {"alnum", F_AN}};
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) if (!strcmp(n, m[i].n)) return m[i].f;
    return 0;
}
SHIM(int, __iswctype_l, (wl c, unsigned long long t, void *l)) { return c < 128 ? (g_ctype[c + 128] & t) != 0 : 0; }
SHIM(int, getwc, (FILE *f)) { return getc(f); }
SHIM(int, putwc, (wl c, FILE *f)) { return putc(c < 256 ? (int)c : '?', f); }
SHIM(int, ungetwc, (wl c, FILE *f)) { return ungetc((int)c, f); }

// uuid text, as libuuid writes it
SHIM(void, uuid_unparse, (const uint8_t *u, char *o)) {
    sprintf(o, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}
SHIM(int, uuid_parse, (const char *s, uint8_t *u)) {
    int v[16];
    if (sscanf(s, "%2x%2x%2x%2x-%2x%2x-%2x%2x-%2x%2x-%2x%2x%2x%2x%2x%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
               &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &v[14], &v[15]) != 16) return -1;
    for (int i = 0; i < 16; i++) u[i] = (uint8_t)v[i];
    return 0;
}
