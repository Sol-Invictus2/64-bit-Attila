// Linux stdio for the game: printf family with Linux argument rules, files always binary, Linux data sizes.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <direct.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "loader.h"

// The System V argument list as the caller lays it out.
typedef struct { uint32_t gp_offset, fp_offset; void *overflow; void *regs; } SvaList;

static uint64_t sva_gp(SvaList *v) {
    if (v->gp_offset < 48) { uint64_t r = *(uint64_t *)((char *)v->regs + v->gp_offset); v->gp_offset += 8; return r; }
    uint64_t r = *(uint64_t *)v->overflow;
    v->overflow = (char *)v->overflow + 8;
    return r;
}
static double sva_fp(SvaList *v) {
    if (v->fp_offset < 176) { double r = *(double *)((char *)v->regs + v->fp_offset); v->fp_offset += 16; return r; }
    double r = *(double *)v->overflow;
    v->overflow = (char *)v->overflow + 8;
    return r;
}

typedef struct { char *p; size_t cap; size_t len; } Out;
static void out_bytes(Out *o, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (o->len + 1 < o->cap) o->p[o->len] = s[i];
        o->len++;
    }
}

// Formats with Linux meanings: long is 64-bit and wide characters are 32-bit.
static int sva_format(char *dst, size_t cap, const char *f, SvaList *v) {
    Out o = {dst, cap, 0};
    char tmp[1024], spec[64];
    if (!f) { log_line("printf-style call with no format text"); f = ""; }
    while (*f) {
        if (*f != '%') { out_bytes(&o, f++, 1); continue; }
        const char *st = f++;
        if (*f == '%') { out_bytes(&o, "%", 1); f++; continue; }
        int si = 0;
        spec[si++] = '%';
        while (*f && strchr("-+ #0'", *f)) { if (*f != '\'' && si < 40) spec[si++] = *f; f++; }
        if (*f == '*') { si += snprintf(spec + si, 16, "%d", (int)sva_gp(v)); f++; }
        else while (*f >= '0' && *f <= '9') { if (si < 40) spec[si++] = *f; f++; }
        if (*f == '.') {
            spec[si++] = *f++;
            if (*f == '*') { si += snprintf(spec + si, 16, "%d", (int)sva_gp(v)); f++; }
            else while (*f >= '0' && *f <= '9') { if (si < 50) spec[si++] = *f; f++; }
        }
        int lng = 0, half = 0, ld = 0;
        for (;; f++) {
            if (*f == 'l' || *f == 'z' || *f == 'j' || *f == 't' || *f == 'q') lng = 1;
            else if (*f == 'h') half++;
            else if (*f == 'L') ld = 1;
            else break;
        }
        char c = *f;
        if (c) f++;
        size_t n = 0;
        switch (c) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            uint64_t a = sva_gp(v);
            spec[si++] = 'l'; spec[si++] = 'l'; spec[si++] = c; spec[si] = 0;
            long long val;
            if (lng) val = (long long)a;
            else if (half == 1) val = (c == 'd' || c == 'i') ? (long long)(short)a : (long long)(unsigned short)a;
            else if (half >= 2) val = (c == 'd' || c == 'i') ? (long long)(signed char)a : (long long)(unsigned char)a;
            else val = (c == 'd' || c == 'i') ? (long long)(int)a : (long long)(unsigned int)a;
            n = (size_t)snprintf(tmp, sizeof tmp, spec, val);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double d;
            if (ld) {
                long double *pl = (long double *)(((uintptr_t)v->overflow + 15) & ~(uintptr_t)15);
                uint8_t raw[16];
                memcpy(raw, pl, 16);
                v->overflow = (char *)pl + 16;
                uint64_t mant;
                uint16_t se;
                memcpy(&mant, raw, 8);
                memcpy(&se, raw + 8, 2);
                d = ldexp((double)mant, (se & 0x7fff) - 16383 - 63);
                if (se & 0x8000) d = -d;
            } else d = sva_fp(v);
            spec[si++] = c; spec[si] = 0;
            n = (size_t)snprintf(tmp, sizeof tmp, spec, d);
            break;
        }
        case 'c': {
            uint64_t a = sva_gp(v);
            spec[si++] = 'c'; spec[si] = 0;
            n = (size_t)snprintf(tmp, sizeof tmp, spec, (int)(a & 0xff));
            break;
        }
        case 'p': {
            uint64_t a = sva_gp(v);
            n = (size_t)snprintf(tmp, sizeof tmp, "0x%llx", (unsigned long long)a);
            break;
        }
        case 's': {
            const void *a = (const void *)sva_gp(v);
            spec[si++] = 's'; spec[si] = 0;
            if (!a) a = lng ? (const void *)L"(null)" : (const void *)"(null)";
            if (lng) {
                // wide text from Linux code is 32 bits per character
                const uint32_t *w = a;
                static __thread char nar[2048];
                size_t k = 0;
                while (w[k] && k + 1 < sizeof nar) { nar[k] = w[k] < 256 ? (char)w[k] : '?'; k++; }
                nar[k] = 0;
                a = nar;
            }
            int need = snprintf(NULL, 0, spec, (const char *)a);
            if (need >= (int)sizeof tmp) {
                char *big = malloc((size_t)need + 1);
                snprintf(big, (size_t)need + 1, spec, (const char *)a);
                out_bytes(&o, big, (size_t)need);
                free(big);
                n = 0;
            } else n = (size_t)snprintf(tmp, sizeof tmp, spec, (const char *)a);
            break;
        }
        case 'n': sva_gp(v); break;
        default:
            out_bytes(&o, st, (size_t)(f - st));
            break;
        }
        if (n) out_bytes(&o, tmp, n < sizeof tmp ? n : sizeof tmp - 1);
    }
    if (cap) dst[o.len < cap ? o.len : cap - 1] = 0;
    return (int)o.len;
}

// A variadic entry saves the register arguments, then hands them to a normal C handler.
#define VARIADIC(sym) \
    SYSV __attribute__((used)) int vh_##sym(uint64_t *regs, void *overflow); \
    __attribute__((naked, sysv_abi)) int sh_##sym(void) { \
        __asm__("sub $0xD8, %rsp\n" \
                "mov %rdi, 0(%rsp)\n mov %rsi, 8(%rsp)\n mov %rdx, 16(%rsp)\n" \
                "mov %rcx, 24(%rsp)\n mov %r8, 32(%rsp)\n mov %r9, 40(%rsp)\n" \
                "movups %xmm0, 48(%rsp)\n movups %xmm1, 64(%rsp)\n movups %xmm2, 80(%rsp)\n movups %xmm3, 96(%rsp)\n" \
                "movups %xmm4, 112(%rsp)\n movups %xmm5, 128(%rsp)\n movups %xmm6, 144(%rsp)\n movups %xmm7, 160(%rsp)\n" \
                "mov %rsp, %rdi\n lea 0xE0(%rsp), %rsi\n" \
                "call vh_" #sym "\n" \
                "add $0xD8, %rsp\n ret\n"); \
    } \
    SHIM_REG(sym, sh_##sym); \
    SYSV __attribute__((used)) int vh_##sym(uint64_t *regs, void *overflow)

static SvaList sva_make(uint64_t *regs, void *overflow, int fixed) {
    SvaList v = {(uint32_t)(fixed * 8), 48, overflow, regs};
    return v;
}

static int file_vprint(FILE *f, const char *fmt, SvaList *v) {
    SvaList copy = *v;
    char small[1024];
    int n = sva_format(small, sizeof small, fmt, v);
    if (n < (int)sizeof small) return (int)fwrite(small, 1, (size_t)n, f);
    char *big = malloc((size_t)n + 1);
    sva_format(big, (size_t)n + 1, fmt, &copy);
    int r = (int)fwrite(big, 1, (size_t)n, f);
    free(big);
    return r;
}

VARIADIC(printf) { SvaList v = sva_make(regs, overflow, 1); return file_vprint(stdout, (const char *)regs[0], &v); }
VARIADIC(__printf_chk) { SvaList v = sva_make(regs, overflow, 2); return file_vprint(stdout, (const char *)regs[1], &v); }
VARIADIC(fprintf) { SvaList v = sva_make(regs, overflow, 2); return file_vprint((FILE *)regs[0], (const char *)regs[1], &v); }
VARIADIC(sprintf) { SvaList v = sva_make(regs, overflow, 2); return sva_format((char *)regs[0], (size_t)1 << 30, (const char *)regs[1], &v); }
VARIADIC(snprintf) { SvaList v = sva_make(regs, overflow, 3); return sva_format((char *)regs[0], (size_t)regs[1], (const char *)regs[2], &v); }
VARIADIC(__snprintf_chk) { SvaList v = sva_make(regs, overflow, 5); return sva_format((char *)regs[0], (size_t)regs[1], (const char *)regs[4], &v); }
SHIM(int, vsnprintf, (char *d, size_t n, const char *f, SvaList *v)) { SvaList c = *v; return sva_format(d, n, f, &c); }
SHIM(int, vprintf, (const char *f, SvaList *v)) { SvaList c = *v; return file_vprint(stdout, f, &c); }
SHIM(int, __vfprintf_chk, (FILE *s, int flag, const char *f, SvaList *v)) { SvaList c = *v; return file_vprint(s, f, &c); }

// scanf: pointers only. Rewrite Linux long to 64-bit and hand over up to 16 targets.
static void scan_fix(char *d, const char *s, size_t cap) {
    size_t o = 0;
    int in = 0;
    for (; *s && o + 2 < cap; s++) {
        if (!in) { d[o++] = *s; if (*s == '%') in = 1; continue; }
        if (*s == '%') { d[o++] = *s; in = 0; continue; }
        if (*s == 'l' && strchr("diuxXo", s[1])) { d[o++] = 'l'; d[o++] = 'l'; continue; }
        if (*s == 'z' || *s == 'j' || *s == 't') { d[o++] = 'l'; d[o++] = 'l'; continue; }
        d[o++] = *s;
        if (strchr("diuoxXeEfgGaAscpn[", *s)) in = 0;
    }
    d[o] = 0;
}
static int scan_common(int isfile, void *src, const char *fmt, uint64_t *regs, void *overflow, int fixed) {
    char f2[512];
    scan_fix(f2, fmt, sizeof f2);
    void *a[16];
    SvaList v = sva_make(regs, overflow, fixed);
    for (int i = 0; i < 16; i++) a[i] = (void *)sva_gp(&v);
#define ARGS a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8],a[9],a[10],a[11],a[12],a[13],a[14],a[15]
    return isfile ? fscanf((FILE *)src, f2, ARGS) : sscanf((const char *)src, f2, ARGS);
}
VARIADIC(sscanf) { return scan_common(0, (void *)regs[0], (const char *)regs[1], regs, overflow, 2); }
VARIADIC(__isoc99_sscanf) { return scan_common(0, (void *)regs[0], (const char *)regs[1], regs, overflow, 2); }
VARIADIC(__isoc99_fscanf) { return scan_common(1, (void *)regs[0], (const char *)regs[1], regs, overflow, 2); }

// Files the game could not open, for finding out what is missing or mis-spelled.
// ATTILA_FILE_TRACE=1 (test only, 2026-11-25 lobby freeze hunt): every stat / open / fopen / glob / scandir with its time.
static int ftrace_on(void) { static int on = -1; if (on < 0) on = getenv("ATTILA_FILE_TRACE") != NULL; return on; }
static double ftrace_us(LARGE_INTEGER t0) {
    static LARGE_INTEGER f; LARGE_INTEGER t; if (!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - t0.QuadPart) * 1e6 / (double)f.QuadPart;
}
static void ftrace(const char *what, const char *p, LARGE_INTEGER t0, int ok) {
    static volatile LONG n;
    double us = ftrace_us(t0);
    if (InterlockedIncrement(&n) <= 3000000) log_line("file %s %s %.0f us: %.200s", what, ok ? "ok" : "MISS", us, p ? p : "?");
}
#define FT_START LARGE_INTEGER ft0 = {0}; if (ftrace_on()) QueryPerformanceCounter(&ft0)
// Workshop folder check (2026-11-25, lobby / custom battle freeze): the game looks for every interface picture as a loose
// file in EVERY subscribed Workshop item's folder (steamapps/workshop/content/325610/<item>/ui/...), often several times:
// measured 41,600 stat/open/glob calls for one faction change in the custom battle screen (208 items), 0.5-1 s frozen on
// Windows (cheap on Linux). Almost no item has such a subfolder. So the first folder level under an item is checked once
// and remembered; a file under a folder that is not there is answered "not there" at once. Only reads, only Workshop
// item folders (the game never writes there). A "not there" answer is believed for 5 seconds, then looked up again, so a
// folder Steam downloads while the game runs is still found. ATTILA_NO_WS_DIR_CACHE=1 = the old way (every check on disk).
static SRWLOCK g_wsd_lock = SRWLOCK_INIT;
static struct { char *key; int exists; ULONGLONG when; } g_wsd[8192];
static int ws_dir_missing(const char *p) {
    static int off = -1;
    if (off < 0) off = getenv("ATTILA_NO_WS_DIR_CACHE") != NULL;
    if (off || !p) return 0;
    char b[MAX_PATH * 2];
    size_t n = 0;
    for (; p[n] && n < sizeof b - 1; n++) { char c = p[n]; if (c == '\\') c = '/'; if (c >= 'A' && c <= 'Z') c = (char)(c + 32); b[n] = c; }
    if (p[n]) return 0;
    b[n] = 0;
    char *m = strstr(b, "steamapps/workshop/content/");
    if (!m) return 0;
    char *q = m + 27;                             // <app>/<item>/<first>/...
    for (int seg = 0; seg < 2; seg++) { q = strchr(q, '/'); if (!q) return 0; q++; }
    char *end = strchr(q, '/');                   // a file directly in the item folder: not handled here
    if (!end || end == q) return 0;
    *end = 0;                                     // b = the item's first-level folder
    uint32_t h = 2166136261u;
    for (const char *c = b; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
    uint32_t i = h & 8191, k;
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockShared(&g_wsd_lock);
    for (k = 0; k < 8192 && g_wsd[i].key; k++, i = (i + 1) & 8191)
        if (!strcmp(g_wsd[i].key, b)) {
            if (!g_wsd[i].exists && now - g_wsd[i].when > 5000) break;   // an old "not there": look again
            int e = g_wsd[i].exists; ReleaseSRWLockShared(&g_wsd_lock); return !e;
        }
    ReleaseSRWLockShared(&g_wsd_lock);
    DWORD a = GetFileAttributesA(b);
    int exists = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    AcquireSRWLockExclusive(&g_wsd_lock);
    static int used;
    for (i = h & 8191; g_wsd[i].key && strcmp(g_wsd[i].key, b); i = (i + 1) & 8191) {}
    if (g_wsd[i].key) { g_wsd[i].exists = exists; g_wsd[i].when = now; }
    else if (used < 7000) {                       // table full: just no remembering any more
        char *key = _strdup(b);
        if (key) { g_wsd[i].key = key; g_wsd[i].exists = exists; g_wsd[i].when = now; used++; }
    }
    ReleaseSRWLockExclusive(&g_wsd_lock);
    static volatile LONG told;
    if (InterlockedIncrement(&told) <= 3) log_line("Workshop folder check: %s %s (remembered; a missing one is looked up again after 5 s)", m, exists ? "there" : "not there");
    return !exists;
}

static void note_open_failure(const char *how, const char *path, const char *mode) {
    static volatile LONG shown;
    if (shim_debug() && InterlockedIncrement(&shown) <= 150) log_line("%s failed: '%s' mode %s, errno %d", how, path, mode, errno);
}

// Paths a mod script builds for normal Windows Attila: %APPDATA%\The Creative Assembly\Attila\... (saved battle set-ups, battle
// preferences; The Dawnless Days' custom battle crash fix reads and deletes files there). This engine keeps those files under
// its own home (<HOME>\.Creative Assembly\Attila\): such paths go there, so a mod works on the 64-bit game's own files and
// never changes normal Attila's. Logged without the user's folder.
static const char *game_path(const char *p, char *buf, size_t n) {
    static char from[MAX_PATH];
    static size_t fl;
    static volatile LONG init;
    if (!init) {
        char ad[MAX_PATH];
        DWORD k = GetEnvironmentVariableA("APPDATA", ad, sizeof ad);
        if (k && k < sizeof ad) { snprintf(from, sizeof from, "%s\\The Creative Assembly\\Attila", ad); fl = strlen(from); }
        init = 1;
    }
    if (!p || !fl) return p;
    for (size_t i = 0; i < fl; i++) {
        char a = p[i], b = from[i];
        if (!a) return p;
        if (a == '/') a = '\\';
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return p;
    }
    if (p[fl] && p[fl] != '\\' && p[fl] != '/') return p;
    extern const char *attila_home_dir(void);
    snprintf(buf, n, "%s\\.Creative Assembly\\Attila%s", attila_home_dir(), p + fl);
    static volatile LONG told;
    if (InterlockedIncrement(&told) <= 6) log_line("normal Attila's settings folder asked for (%.80s): the 64-bit game's own used", p + fl);
    return buf;
}
// A mod script that checks its packs reads normal Attila's mod list, used_mods.txt in the game folder (The Dawnless Days'
// frontend_pack_check.lua). This engine's list is mod_list.txt next to its exe (picker.c): reads of used_mods.txt go there,
// in text mode, so a Lua line reader sees no carriage returns (mod_list.txt is written with CR LF; Windows Attila's Lua reads
// text files in text mode).
static const char *mod_list_read(const char *p, const char *mode, char *buf, size_t n, int *text) {
    *text = 0;
    if (!p || mode[0] != 'r' || strchr(mode, '+')) return p;
    const char *b = p;
    if (b[0] == '.' && (b[1] == '/' || b[1] == '\\')) b += 2;
    if (_stricmp(b, "used_mods.txt")) return p;
    extern const char *mod_list_path(void);
    const char *ml = mod_list_path();
    if (!ml || GetFileAttributesA(ml) == INVALID_FILE_ATTRIBUTES) return p;
    snprintf(buf, n, "%s", ml);
    *text = 1;
    static volatile LONG told;
    if (InterlockedIncrement(&told) <= 2) log_line("used_mods.txt read by a script: the 64-bit mod list (mod_list.txt) given");
    return buf;
}

// Linux has no text mode; every file must stay byte-exact.
static FILE *open_binary(const char *path, const char *mode) {
    char m[16], pb[MAX_PATH * 2], lb[MAX_PATH * 2];
    int text;
    path = mod_list_read(game_path(path, pb, sizeof pb), mode, lb, sizeof lb, &text);
    size_t k = 0;
    for (const char *p = mode; *p && k < 12; p++) if (*p != 'b' && *p != 'e' && *p != 'c' && *p != 'm' && *p != 't') m[k++] = *p;
    m[k++] = text ? 't' : 'b';
    m[k] = 0;
    if (mode[0] == 'r' && !strchr(mode, '+') && ws_dir_missing(path)) { errno = ENOENT; return NULL; }
    FILE *f = fopen(path, m);
    if (!f) note_open_failure("fopen", path, mode);
    if (mode[0] != 'r' || strchr(mode, '+')) {
        static volatile LONG shown;
        if (shim_debug() && InterlockedIncrement(&shown) <= 30) log_line("opened for writing: '%s' mode %s -> %s", path, mode, f ? "ok" : "FAILED");
    }
    return f;
}
SHIM(FILE *, fopen, (const char *p, const char *m)) { FT_START; FILE *r = open_binary(p, m); if (ftrace_on()) ftrace("fopen", p, ft0, r != NULL); return r; }
SHIM(FILE *, fopen64, (const char *p, const char *m)) { FT_START; FILE *r = open_binary(p, m); if (ftrace_on()) ftrace("fopen", p, ft0, r != NULL); return r; }
SHIM(FILE *, fdopen, (int fd, const char *m)) { return _fdopen(fd, m); }
SHIM(int, fclose, (FILE *f)) { return fclose(f); }
SHIM(size_t, fread, (void *d, size_t a, size_t b, FILE *f)) { return fread(d, a, b, f); }
SHIM(size_t, fwrite, (const void *d, size_t a, size_t b, FILE *f)) { return fwrite(d, a, b, f); }
SHIM(int, fflush, (FILE *f)) { return fflush(f); }
SHIM(int, fseek, (FILE *f, long long o, int w)) { return _fseeki64(f, o, w); }
SHIM(long long, ftell, (FILE *f)) { return _ftelli64(f); }
SHIM(int, fseeko64, (FILE *f, long long o, int w)) { return _fseeki64(f, o, w); }
SHIM(long long, ftello64, (FILE *f)) { return _ftelli64(f); }
SHIM(int, feof, (FILE *f)) { return feof(f); }
SHIM(int, ferror, (FILE *f)) { return ferror(f); }
SHIM(void, clearerr, (FILE *f)) { clearerr(f); }
SHIM(int, fileno, (FILE *f)) { return _fileno(f); }
SHIM(char *, fgets, (char *d, int n, FILE *f)) { return fgets(d, n, f); }
SHIM(int, fputs, (const char *s, FILE *f)) { return fputs(s, f); }
SHIM(int, fputc, (int c, FILE *f)) { return fputc(c, f); }
SHIM(int, putc, (int c, FILE *f)) { return putc(c, f); }
SHIM(int, getc, (FILE *f)) { return getc(f); }
SHIM(int, _IO_getc, (FILE *f)) { return getc(f); }
SHIM(int, ungetc, (int c, FILE *f)) { return ungetc(c, f); }
SHIM(int, setvbuf, (FILE *f, char *b, int mode, size_t n)) { return 0; }
SHIM(int, fgetpos, (FILE *f, uint64_t *p)) { p[0] = (uint64_t)_ftelli64(f); return 0; }
SHIM(int, fsetpos, (FILE *f, const uint64_t *p)) { return _fseeki64(f, (long long)p[0], SEEK_SET); }
SHIM(FILE *, tmpfile, (void)) { return tmpfile(); }
SHIM(char *, tmpnam, (char *s)) { return tmpnam(s); }
SHIM(int, remove, (const char *p)) { char pb[MAX_PATH * 2]; return remove(game_path(p, pb, sizeof pb)); }
// Linux rename replaces an existing target; Windows rename refuses.
SHIM(int, rename, (const char *a, const char *b)) {
    char ab[MAX_PATH * 2], bb[MAX_PATH * 2];
    a = game_path(a, ab, sizeof ab);
    b = game_path(b, bb, sizeof bb);
    if (MoveFileExA(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) return 0;
    log_line("rename failed: '%s' -> '%s' (error %lu)", a, b, GetLastError());
    errno = EACCES;
    return -1;
}
#undef popen
#undef pclose
SHIM(FILE *, popen, (const char *c, const char *m)) { log_line("popen refused: %s", c); return NULL; }
SHIM(int, pclose, (FILE *f)) { return -1; }
// os.execute from mod scripts: this engine runs no programs; mod_support.inc does what mods use it for (building-slot tools,
// mkdir, opening a web page) and refuses the rest.
SHIM(int, system, (const char *c)) { extern int mod_command(const char *); return c ? mod_command(c) : 0; }

// 80-bit long double helpers libstdc++ refers to; not reached by the game's own formatting so far.
SHIM(void, frexpl, (void)) { fatal("frexpl called"); }
SHIM(void, strtold_l, (void)) { fatal("strtold_l called"); }

static FILE *g_in, *g_out, *g_err;
__attribute__((constructor)) static void std_files(void) { g_in = stdin; g_out = stdout; g_err = stderr; }
SHIM_REG(stdin, &g_in);
SHIM_REG(stdout, &g_out);
SHIM_REG(stderr, &g_err);

// low-level files with Linux flag values
// Saved key sets (2026-11-26, Workshop reports "keybinds don't save"): the game keeps the chosen key set's name in its
// registry file and at the next start loads <home>/custom_keys/<name>.keys.xml through its pack file system (SHORTCUT_HANDLER::
// load_keys -> CA::FileIn::open). That code takes a path for a full one only when it starts with '/', as on Linux; a Windows
// path "c:/users/..." is taken for a file inside the game's folders, so it asks for "<data folder>/c:/users/.../x.keys.xml":
// never there, and every start went back to the default keys. A ':' can only be the second letter of a real Windows path,
// so "<anything>/<letter>:/<rest>" is answered with "<letter>:/<rest>". ATTILA_NO_INNER_PATH_FIX=1 = off.
static const char *inner_drive_path(const char *p) {
    static int off = -1;
    if (off < 0) off = getenv("ATTILA_NO_INNER_PATH_FIX") != NULL;
    if (off || !p) return p;
    const char *last = NULL;
    for (const char *c = p + 1; c[0] && c[1] && c[2] && c[3]; c++)
        if ((c[0] == '/' || c[0] == 0x5c) && ((c[1] | 32) >= 'a' && (c[1] | 32) <= 'z') && c[2] == ':' && (c[3] == '/' || c[3] == 0x5c)) last = c + 1;
    if (!last) return p;
    static volatile LONG told;
    if (InterlockedIncrement(&told) <= 4) log_line("full path inside a game folder path: the full path used (...%.60s)", strlen(last) > 60 ? last + strlen(last) - 60 : last);
    return last;
}
SHIM(int, open, (const char *p, int fl, int mode)) {
    char pb[MAX_PATH * 2];
    p = game_path(inner_drive_path(p), pb, sizeof pb);
    int w = _O_BINARY;
    int acc = fl & 3;
    w |= acc == 0 ? _O_RDONLY : acc == 1 ? _O_WRONLY : _O_RDWR;
    if (fl & 0x40) w |= _O_CREAT;
    if (fl & 0x80) w |= _O_EXCL;
    if (fl & 0x200) w |= _O_TRUNC;
    if (fl & 0x400) w |= _O_APPEND;
    FT_START;
    if (acc == 0 && !(fl & 0x40) && ws_dir_missing(p)) { errno = ENOENT; if (ftrace_on()) ftrace("open", p, ft0, 0); return -1; }
    int fd = _open(p, w, _S_IREAD | _S_IWRITE);
    if (ftrace_on()) ftrace("open", p, ft0, fd >= 0);
    if (fd < 0) note_open_failure("open", p, (fl & 3) ? "write" : "read");
    return fd;
}
SHIM(int, close, (int fd)) {
    extern int shim_is_socket(int), shim_close_socket(int);
    if (shim_is_socket(fd)) return shim_close_socket(fd);
    return _close(fd);
}
SHIM(long long, read, (int fd, void *d, size_t n)) { return _read(fd, d, n > 0x7fffffff ? 0x7fffffff : (unsigned)n); }
SHIM(long long, write, (int fd, const void *d, size_t n)) { return _write(fd, d, n > 0x7fffffff ? 0x7fffffff : (unsigned)n); }
SHIM(long long, lseek, (int fd, long long o, int w)) { return _lseeki64(fd, o, w); }
SHIM(long long, lseek64, (int fd, long long o, int w)) { return _lseeki64(fd, o, w); }
struct iovec_l { void *base; size_t len; };
SHIM(long long, writev, (int fd, const struct iovec_l *v, int n)) {
    long long t = 0;
    for (int i = 0; i < n; i++) t += _write(fd, v[i].base, (unsigned)v[i].len);
    return t;
}
SHIM(int, ioctl, (int fd, unsigned long long req, void *arg)) { errno = EINVAL; return -1; }

// Linux struct stat layout on x86-64
typedef struct {
    uint64_t dev, ino, nlink;
    uint32_t mode, uid, gid, pad0;
    uint64_t rdev;
    int64_t size, blksize, blocks;
    int64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
    int64_t unused[3];
} StatL;

static void fill_stat(StatL *o, const struct _stat64 *s) {
    memset(o, 0, sizeof *o);
    o->mode = (s->st_mode & _S_IFDIR) ? (0x4000 | 0755) : (0x8000 | 0644);
    o->nlink = 1;
    o->uid = o->gid = 1000;
    o->size = s->st_size;
    o->blksize = 4096;
    o->blocks = (s->st_size + 511) / 512;
    o->atime = s->st_atime;
    o->mtime = s->st_mtime;
    o->ctime = s->st_ctime;
}
SHIM(int, __xstat, (int ver, const char *p, StatL *o)) {
    struct _stat64 s;
    char pb[MAX_PATH * 2];
    FT_START;
    const char *gp = game_path(inner_drive_path(p), pb, sizeof pb);
    int bad = ws_dir_missing(gp) ? (errno = ENOENT, -1) : _stat64(gp, &s);
    if (ftrace_on()) ftrace("stat", p, ft0, !bad);
    if (bad) return -1;
    fill_stat(o, &s);
    return 0;
}
SHIM(int, __fxstat, (int ver, int fd, StatL *o)) {
    struct _stat64 s;
    if (_fstat64(fd, &s)) return -1;
    fill_stat(o, &s);
    return 0;
}
SHIM(int, __fxstat64, (int ver, int fd, StatL *o)) {
    struct _stat64 s;
    if (_fstat64(fd, &s)) return -1;
    fill_stat(o, &s);
    return 0;
}

// folders
SHIM(char *, getcwd, (char *d, size_t n)) {
    char *r = _getcwd(d, (int)n);
    if (r) for (char *p = r; *p; p++) if (*p == '\\') *p = '/';
    return r;
}
SHIM(int, chdir, (const char *p)) { return _chdir(p); }
SHIM(int, mkdir, (const char *p, int mode)) { char pb[MAX_PATH * 2]; return _mkdir(game_path(p, pb, sizeof pb)); }
SHIM(int, rmdir, (const char *p)) { return _rmdir(p); }
SHIM(int, chmod, (const char *p, int mode)) { return 0; }
SHIM(int, utimes, (const char *p, const void *t)) { return 0; }

// struct dirent on x86-64: ino, off, reclen, type, name
typedef struct { uint64_t ino, off; uint16_t reclen; uint8_t type; char name[256]; } DirentL;
typedef SYSV int (*dir_filter)(const DirentL *);
typedef SYSV int (*dir_compare)(const DirentL **, const DirentL **);
SHIM(int, scandir, (const char *dir, DirentL ***list, dir_filter filter, dir_compare cmp)) {
    char pat[MAX_PATH + 4];
    snprintf(pat, sizeof pat, "%s/*", dir);
    WIN32_FIND_DATAA fd;
    FT_START;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (ftrace_on()) ftrace("scandir", pat, ft0, h != INVALID_HANDLE_VALUE);
    if (h == INVALID_HANDLE_VALUE) { errno = ENOENT; return -1; }
    size_t n = 0, cap = 64;
    DirentL **arr = malloc(cap * sizeof *arr);
    do {
        DirentL *e = calloc(1, sizeof *e);
        e->reclen = sizeof *e;
        e->type = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 4 : 8;
        strncpy(e->name, fd.cFileName, 255);
        if (filter && !filter(e)) { free(e); continue; }
        if (n == cap) arr = realloc(arr, (cap *= 2) * sizeof *arr);
        arr[n++] = e;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (cmp)
        for (size_t i = 1; i < n; i++) {
            DirentL *x = arr[i];
            size_t j = i;
            while (j && cmp((const DirentL **)&arr[j - 1], (const DirentL **)&x) > 0) { arr[j] = arr[j - 1]; j--; }
            arr[j] = x;
        }
    *list = arr;
    return (int)n;
}

typedef struct { size_t pathc; char **pathv; size_t offs; int flags; void *pad[5]; } GlobL;
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
SHIM(int, glob, (const char *pat, int flags, void *err, GlobL *g)) {
    WIN32_FIND_DATAA fd;
    FT_START;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (ftrace_on()) ftrace("glob", pat, ft0, h != INVALID_HANDLE_VALUE);
    memset(g, 0, sizeof *g);
    if (h == INVALID_HANDLE_VALUE) return 3;    // GLOB_NOMATCH
    const char *slash = strrchr(pat, '/');
    size_t pre = slash ? (size_t)(slash - pat) + 1 : 0, n = 0, cap = 32;
    char **v = malloc((cap + 1) * sizeof *v);
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        size_t len = pre + strlen(fd.cFileName) + 1;
        char *s = malloc(len);
        memcpy(s, pat, pre);
        strcpy(s + pre, fd.cFileName);
        if (n == cap) v = realloc(v, ((cap *= 2) + 1) * sizeof *v);
        v[n++] = s;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(v, n, sizeof *v, cmp_str);
    v[n] = NULL;
    g->pathc = n;
    g->pathv = v;
    return n ? 0 : 3;
}
SHIM(void, globfree, (GlobL *g)) {
    for (size_t i = 0; i < g->pathc; i++) free(g->pathv[i]);
    free(g->pathv);
}
