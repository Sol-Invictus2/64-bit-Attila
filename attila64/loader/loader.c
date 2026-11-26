// Windows 64-bit loader for the Linux Attila binary.
// Maps the game file at its fixed address, loads libstdc++ and libgcc_s next to it, connects imports to the Windows
// shims, patches the stack-check register use, runs the initialisers and enters the game at its own _start.
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <dxgi.h>
#include <tlhelp32.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "loader.h"
#include "version.h"

typedef struct { uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff; uint32_t flags;
                 uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } Ehdr;
typedef struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } Phdr;
typedef struct { uint32_t name, type; uint64_t flags, addr, offset, size; uint32_t link, info; uint64_t addralign, entsize; } Shdr;
typedef struct { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; } Sym;
typedef struct { uint64_t offset, info; int64_t addend; } Rela;
typedef struct { int64_t tag; uint64_t val; } Dyn;

enum { PT_LOAD = 1, PT_DYNAMIC = 2, PT_TLS = 7, SHT_DYNSYM = 11, SHT_GNU_VERSYM = 0x6fffffff };
enum { DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8, DT_INIT = 12, DT_JMPREL = 23, DT_PLTRELSZ = 2,
       DT_INIT_ARRAY = 25, DT_INIT_ARRAYSZ = 27 };
enum { R_64 = 1, R_COPY = 5, R_GLOB_DAT = 6, R_JUMP_SLOT = 7, R_RELATIVE = 8, R_DTPMOD64 = 16, R_DTPOFF64 = 17, R_TPOFF64 = 18 };

typedef struct {
    LoadedModule pub;
    const Sym *dynsym;
    const char *dynstr;
    const uint16_t *versym;
    size_t nsyms;
    const Dyn *dynamic;
    uint64_t entry;
    uint8_t *lo, *hi;
} Module;

#define MAX_MODULES 8
static Module g_mods[MAX_MODULES];
static int g_nmods;
static FILE *g_log;

static unsigned g_dpi_percent; static int g_dpi_own;   // Windows' display scaling and whether the game's window handles it itself
// ---- The log. Every line goes to attila64_log.txt and to stderr, flushed, as always. Since 2026-11-13 a writer thread
// does the writing: in hidden test runs of a big fight the main thread sat in NtWriteFile (each line flushed to disk,
// twice) for about 4.5% of its time, and for up to 30 s at once when writes to the disk stalled (cause not known).
// A line is now only copied into a buffer under a lock, and the writer thread is woken to write it, in order.
// Before the program ends (crash, fatal error, exit, the test end) log_go_sync() writes what is waiting from the
// calling thread and switches back to writing at once, so the last lines are never lost.
// ATTILA_LOG_SYNC=1 = the old way (every thread writes its own lines at once); ATTILA_LOG_FLIP=1 = test: the two ways
// switched with every fps line (log_flip_report).
static SRWLOCK g_lb_lock = SRWLOCK_INIT;       // the waiting text
static SRWLOCK g_lb_out_lock = SRWLOCK_INIT;   // one writer at a time, so lines stay in order
static char *g_lb_buf, *g_lb_spare;
static size_t g_lb_len, g_lb_cap, g_lb_spare_cap;
static HANDLE g_lb_event;
static volatile LONG g_lb_async, g_lb_started;
static int g_lb_flip;

static void lb_out(const char *p, size_t n) {
    if (!n) return;
    fwrite(p, 1, n, stderr);
    fflush(stderr);
    if (g_log) { fwrite(p, 1, n, g_log); fflush(g_log); }
}
static int lb_try(SRWLOCK *l, DWORD wait_ms) {   // a crash may have stopped a thread that holds a lock: do not wait for ever
    for (DWORD t = 0;; t++) {
        if (TryAcquireSRWLockExclusive(l)) return 1;
        if (t >= wait_ms) return 0;
        Sleep(1);
    }
}
// Writes what is waiting, in order. wait_ms = INFINITE for the writer thread.
static void lb_drain(DWORD wait_ms) {
    if (wait_ms == INFINITE) AcquireSRWLockExclusive(&g_lb_out_lock);
    else if (!lb_try(&g_lb_out_lock, wait_ms)) return;
    for (;;) {
        if (wait_ms == INFINITE) AcquireSRWLockExclusive(&g_lb_lock);
        else if (!lb_try(&g_lb_lock, wait_ms)) break;
        char *p = g_lb_buf;
        size_t n = g_lb_len, cap = g_lb_cap;
        g_lb_buf = g_lb_spare; g_lb_cap = g_lb_spare_cap; g_lb_len = 0;
        ReleaseSRWLockExclusive(&g_lb_lock);
        g_lb_spare = p; g_lb_spare_cap = cap;
        if (!n) break;
        lb_out(p, n);
    }
    ReleaseSRWLockExclusive(&g_lb_out_lock);
}
static DWORD WINAPI lb_writer(void *arg) {
    (void)arg;
    for (;;) { WaitForSingleObject(g_lb_event, INFINITE); lb_drain(INFINITE); }
    return 0;
}
static int lb_add(const char *p, size_t n) {
    AcquireSRWLockExclusive(&g_lb_lock);
    if (g_lb_len + n > g_lb_cap) {
        size_t cap = g_lb_cap ? g_lb_cap : 65536;
        while (cap < g_lb_len + n) cap *= 2;
        char *nb = cap <= ((size_t)64 << 20) ? realloc(g_lb_buf, cap) : NULL;
        if (!nb) { ReleaseSRWLockExclusive(&g_lb_lock); return 0; }   // the writer is stuck or no memory: write at once
        g_lb_buf = nb; g_lb_cap = cap;
    }
    memcpy(g_lb_buf + g_lb_len, p, n);
    g_lb_len += n;
    ReleaseSRWLockExclusive(&g_lb_lock);
    SetEvent(g_lb_event);
    return 1;
}
// Before the program ends: what is waiting is written now, from this thread, and later lines are written at once.
void log_go_sync(void) {
    if (!InterlockedExchange(&g_lb_async, 0)) return;
    lb_drain(500);
    Sleep(0);
    lb_drain(100);   // a line another thread was adding just then
}
static void log_start_writer(void) {
    if (getenv("ATTILA_LOG_SYNC")) return;
    g_lb_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE h = g_lb_event ? CreateThread(NULL, 1 << 20, lb_writer, NULL, 0, NULL) : NULL;
    if (!h) return;
    CloseHandle(h);
    g_lb_started = 1;
    g_lb_flip = getenv("ATTILA_LOG_FLIP") != NULL;
    InterlockedExchange(&g_lb_async, 1);
    atexit(log_go_sync);
}
// test (ATTILA_LOG_FLIP=1), with every fps line: the log written by the writer thread and at once, in turns
void log_flip_report(void) {
    if (!g_lb_flip || !g_lb_started) return;
    int was = g_lb_async != 0;
    if (was) log_go_sync();
    log_line("log flip test: the window just logged had the log written %s", was ? "by the writer thread" : "at once by each thread");
    if (!was) InterlockedExchange(&g_lb_async, 1);
}

void log_line(const char *fmt, ...) {
    char small[1024], *line = small;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n + 2 > sizeof small) {   // a long line: formatted again into a block of its size
        char *big = malloc((size_t)n + 2);
        if (big) { va_start(ap, fmt); vsnprintf(big, (size_t)n + 1, fmt, ap); va_end(ap); line = big; }
        else n = (int)sizeof small - 2;
    }
    line[n++] = '\n';
    if (!g_lb_async || !lb_add(line, (size_t)n)) {
        int got = lb_try(&g_lb_out_lock, 500);
        lb_out(line, (size_t)n);
        if (got) ReleaseSRWLockExclusive(&g_lb_out_lock);
    }
    if (line != small) free(line);
}

void fatal(const char *fmt, ...) {
    log_go_sync();
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log_line("FATAL: %s", buf);
    MessageBoxA(NULL, buf, "64-bit Attila could not continue", MB_ICONERROR);
    ExitProcess(3);
}

// ---- Keeping the game's fixed address range free. The Linux game file must sit at 0x400000 and up (78 MB). On a PC where
// Windows' address randomization for memory (bottom-up ASLR) is off - system-wide, by a "gaming tweak" or Exploit Protection -
// the main thread's stack and early heaps start at the bottom of memory and can land in that range; load_elf then failed with
// "cannot reserve 78 MB at 0x400000 (error 487)" (Workshop tester, 2026-11-09). Now the range is reserved first thing in main;
// if something already sits in it, the loader starts itself once more with bottom-up + high-entropy randomization forced on
// for its own process only (CreateProcess mitigation policy), which moves the stack and heaps away; if it is still taken, the
// error names what is there. ATTILA_TEST_BLOCK_RANGE=1 puts something in the way (first start only; 2 = both starts).
static uint8_t *g_elf_res;             // the range kept since the start of main; load_elf commits inside it
static uint64_t g_elf_res_size;
static void *g_main_stack_alloc;       // allocation base of the start-up thread's stack, to name it in the error

static int elf_load_range(const char *path, uint64_t *lo, uint64_t *hi) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    Ehdr eh;
    Phdr ph[64];
    DWORD got = 0;
    int ok = 0;
    if (ReadFile(f, &eh, sizeof eh, &got, NULL) && got == sizeof eh && !memcmp(eh.ident, "\177ELF\2\1", 6) && eh.phnum &&
        eh.phnum <= 64) {
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)eh.phoff;
        DWORD want = eh.phnum * (DWORD)sizeof(Phdr);
        if (SetFilePointerEx(f, li, NULL, FILE_BEGIN) && ReadFile(f, ph, want, &got, NULL) && got == want) {
            uint64_t l = ~0ull, h = 0;
            for (int i = 0; i < eh.phnum; i++)
                if (ph[i].type == PT_LOAD) {
                    if (ph[i].vaddr < l) l = ph[i].vaddr;
                    if (ph[i].vaddr + ph[i].memsz > h) h = ph[i].vaddr + ph[i].memsz;
                }
            if (h > l) { *lo = l & ~0xfffull; *hi = (h + 0xfff) & ~0xfffull; ok = 1; }
        }
    }
    CloseHandle(f);
    return ok;
}

static void describe_range(uint64_t lo, uint64_t hi, char *out, size_t n) {   // what sits in [lo, hi), one line per block
    size_t k = 0;
    out[0] = 0;
    typedef DWORD (WINAPI *MappedNameFn)(HANDLE, LPVOID, LPSTR, DWORD);
    MappedNameFn mapped_name = (MappedNameFn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32GetMappedFileNameA");
    MEMORY_BASIC_INFORMATION here;
    void *this_stack = VirtualQuery(&here, &here, sizeof here) ? here.AllocationBase : NULL;
    for (uint64_t a = lo; a < hi && k + 140 < n;) {
        MEMORY_BASIC_INFORMATION mb;
        if (!VirtualQuery((void *)(uintptr_t)a, &mb, sizeof mb)) break;
        uint64_t b = (uint64_t)(uintptr_t)mb.BaseAddress, e = b + mb.RegionSize;
        if (mb.State != MEM_FREE) {
            char what[MAX_PATH + 40] = "memory of this program (heap or stack)";
            if (mb.Type == MEM_IMAGE || mb.Type == MEM_MAPPED) {
                char full[MAX_PATH];
                if (mapped_name && mapped_name(GetCurrentProcess(), mb.BaseAddress, full, sizeof full)) {
                    const char *s = strrchr(full, '\\');
                    snprintf(what, sizeof what, "%s %s", mb.Type == MEM_IMAGE ? "program file" : "file", s ? s + 1 : full);
                } else snprintf(what, sizeof what, "%s", mb.Type == MEM_IMAGE ? "a program file" : "a mapped file");
            } else if (mb.AllocationBase && (mb.AllocationBase == g_main_stack_alloc || mb.AllocationBase == this_stack))
                snprintf(what, sizeof what, "the stack of a thread of this program");
            k += (size_t)snprintf(out + k, n - k, "\n  %llx-%llx: %s", (unsigned long long)b, (unsigned long long)e, what);
        }
        if (e <= a) break;
        a = e;
    }
}

#ifndef PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY
#define PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY 0x00020007
#endif
static int relaunch_randomized(const char *why, DWORD *code) {   // 1 = the second start ran (and ended with *code)
    if (GetEnvironmentVariableA("ATTILA64_RELAUNCHED", NULL, 0)) return 0;   // once only
    SIZE_T sz = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &sz);
    LPPROC_THREAD_ATTRIBUTE_LIST al = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, sz);
    if (!al) return 0;
    if (!InitializeProcThreadAttributeList(al, 1, 0, &sz)) { HeapFree(GetProcessHeap(), 0, al); return 0; }
    DWORD64 policy = (1ull << 16) | (1ull << 20);   // BOTTOM_UP_ASLR_ALWAYS_ON | HIGH_ENTROPY_ASLR_ALWAYS_ON
    int ok = 0;
    if (UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, &policy, sizeof policy, NULL, NULL)) {
        STARTUPINFOEXW si;
        memset(&si, 0, sizeof si);
        si.StartupInfo.cb = sizeof si;
        si.lpAttributeList = al;
        PROCESS_INFORMATION pi;
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        wchar_t *cmd = _wcsdup(GetCommandLineW());
        char note[600];
        snprintf(note, sizeof note, "%s", why && *why ? why : " (not known)");
        SetEnvironmentVariableA("ATTILA64_RELAUNCHED", note);
        if (cmd && CreateProcessW(exe, cmd, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &si.StartupInfo, &pi)) {
            CloseHandle(pi.hThread);
            WaitForSingleObject(pi.hProcess, INFINITE);
            if (!GetExitCodeProcess(pi.hProcess, code)) *code = 0;
            CloseHandle(pi.hProcess);
            ok = 1;
        } else SetEnvironmentVariableA("ATTILA64_RELAUNCHED", NULL);
        free(cmd);
    }
    DeleteProcThreadAttributeList(al);
    HeapFree(GetProcessHeap(), 0, al);
    return ok;
}

static int keep_game_range(const char *elf_path, DWORD *code) {   // 1 = a second start ran instead; leave with *code
    MEMORY_BASIC_INFORMATION mb;
    if (VirtualQuery(&mb, &mb, sizeof mb)) g_main_stack_alloc = mb.AllocationBase;
    uint64_t lo, hi;
    if (!elf_load_range(elf_path, &lo, &hi)) return 0;   // load_elf reports a missing or broken file
    const char *test = getenv("ATTILA_TEST_BLOCK_RANGE");
    if (test && (!GetEnvironmentVariableA("ATTILA64_RELAUNCHED", NULL, 0) || atoi(test) == 2))
        VirtualAlloc((void *)(uintptr_t)(lo + 0x800000), 0x100000, MEM_RESERVE, PAGE_READWRITE);   // test: something in the way
    g_elf_res = VirtualAlloc((void *)(uintptr_t)lo, hi - lo, MEM_RESERVE, PAGE_NOACCESS);
    if (g_elf_res) { g_elf_res_size = hi - lo; return 0; }
    char why[500];
    describe_range(lo, hi, why, sizeof why);
    return relaunch_randomized(why, code);
}

int loader_module_count(void) { return g_nmods; }
const LoadedModule *loader_module(int i) { return &g_mods[i].pub; }

static void read_at(HANDLE f, uint64_t off, void *dst, size_t n) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(f, li, NULL, FILE_BEGIN)) fatal("seek failed");
    uint8_t *p = dst;
    while (n) {
        DWORD chunk = n > (1u << 30) ? (1u << 30) : (DWORD)n, got = 0;
        if (!ReadFile(f, p, chunk, &got, NULL) || !got) fatal("read failed at %llu", (unsigned long long)off);
        p += got;
        n -= got;
    }
}

static int g_next_tls;

static Module *load_elf(const char *path, const char *name, int fixed) {
    if (g_nmods == MAX_MODULES) fatal("too many modules");
    Module *m = &g_mods[g_nmods++];
    m->pub.name = name;
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) fatal("cannot open %s", path);
    Ehdr eh;
    read_at(f, 0, &eh, sizeof eh);
    if (memcmp(eh.ident, "\177ELF\2\1", 6) || eh.machine != 62) fatal("%s is not a 64-bit x86 ELF file", path);
    Phdr *ph = malloc(eh.phnum * sizeof(Phdr));
    read_at(f, eh.phoff, ph, eh.phnum * sizeof(Phdr));

    uint64_t lo = ~0ull, hi = 0;
    for (int i = 0; i < eh.phnum; i++)
        if (ph[i].type == PT_LOAD) {
            if (ph[i].vaddr < lo) lo = ph[i].vaddr;
            if (ph[i].vaddr + ph[i].memsz > hi) hi = ph[i].vaddr + ph[i].memsz;
        }
    lo &= ~0xfffull;
    hi = (hi + 0xfff) & ~0xfffull;

    uint8_t *want = fixed ? (uint8_t *)lo : NULL;
    uint8_t *mem = NULL;
    if (fixed && g_elf_res && (uint64_t)(uintptr_t)g_elf_res <= lo && hi <= (uint64_t)(uintptr_t)g_elf_res + g_elf_res_size)
        mem = VirtualAlloc(want, hi - lo, MEM_COMMIT, PAGE_EXECUTE_READWRITE);   // inside the range kept since the start
    if (!mem) mem = VirtualAlloc(want, hi - lo, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!mem) {
        DWORD err = GetLastError();
        char who[700] = "";
        if (fixed) describe_range(lo, hi, who, sizeof who);
        fatal("cannot reserve %llu MB at %p for %s (error %lu).%s%s\n\nPlease send attila64_log.txt (next to attila64.exe).",
              (unsigned long long)((hi - lo) >> 20), want, name, err, who[0] ? "\n\nIn use there:" : "", who);
    }
    uint8_t *bias = mem - lo;
    m->pub.base = bias;
    m->lo = mem;
    m->hi = mem + (hi - lo);
    m->entry = eh.entry;

    for (int i = 0; i < eh.phnum; i++) {
        if (ph[i].type == PT_LOAD) read_at(f, ph[i].offset, bias + ph[i].vaddr, ph[i].filesz);
        else if (ph[i].type == PT_DYNAMIC) m->dynamic = (const Dyn *)(bias + ph[i].vaddr);
    }
    for (int i = 0; i < eh.phnum; i++)
        if (ph[i].type == PT_TLS) {
            m->pub.tls_image = bias + ph[i].vaddr;
            m->pub.tls_filesz = ph[i].filesz;
            m->pub.tls_memsz = ph[i].memsz;
            m->pub.tls_align = ph[i].align;
            m->pub.tls_module = ++g_next_tls;
        }
    m->pub.phnum = eh.phnum;
    Phdr *ph_copy = malloc(eh.phnum * sizeof(Phdr));
    memcpy(ph_copy, ph, eh.phnum * sizeof(Phdr));
    m->pub.phdr = ph_copy;

    Shdr *sh = malloc(eh.shnum * sizeof(Shdr));
    read_at(f, eh.shoff, sh, eh.shnum * sizeof(Shdr));
    for (int i = 0; i < eh.shnum; i++) {
        if (sh[i].type == SHT_DYNSYM) m->nsyms = sh[i].size / sizeof(Sym);
        else if (sh[i].type == SHT_GNU_VERSYM) m->versym = (const uint16_t *)(bias + sh[i].addr);
    }
    free(sh);
    free(ph);
    CloseHandle(f);

    for (const Dyn *d = m->dynamic; d && d->tag; d++) {
        if (d->tag == DT_SYMTAB) m->dynsym = (const Sym *)(bias + d->val);
        else if (d->tag == DT_STRTAB) m->dynstr = (const char *)(bias + d->val);
    }
    log_line("loaded %-14s at %p, %llu MB, %llu symbols, thread-local block %d", name, m->lo,
             (unsigned long long)((hi - lo) >> 20), (unsigned long long)m->nsyms, m->pub.tls_module);
    return m;
}

// Global symbol table: first definition wins, in load order; older hidden versions are skipped.
typedef struct { const char *name; void *addr; int module; const Sym *sym; } GlobalSym;
#define GSIZE (1u << 18)
static GlobalSym *g_syms;

static uint32_t hash_name(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static GlobalSym *slot_for(const char *name) {
    uint32_t i = hash_name(name) & (GSIZE - 1);
    while (g_syms[i].name && strcmp(g_syms[i].name, name)) i = (i + 1) & (GSIZE - 1);
    return &g_syms[i];
}

static void add_module_symbols(int mi) {
    Module *m = &g_mods[mi];
    for (size_t i = 1; i < m->nsyms; i++) {
        const Sym *s = &m->dynsym[i];
        int type = s->info & 15, bind = s->info >> 4;
        // bind 10 is GNU_UNIQUE: one shared copy across all modules, such as the empty string std::string compares against
        if (!s->shndx || (bind != 1 && bind != 2 && bind != 10) || (type == 6)) continue;
        if (m->versym && (m->versym[i] & 0x8000)) continue;
        const char *n = m->dynstr + s->name;
        GlobalSym *g = slot_for(n);
        if (g->name) continue;
        g->name = n;
        g->addr = m->pub.base + s->value;
        g->module = mi;
        g->sym = s;
    }
}

void *loader_lookup(const char *name) {
    GlobalSym *g = slot_for(name);
    if (g->name) return g->addr;
    const ShimEntry *e = shim_find(name);
    return e ? e->ptr : NULL;
}

// A function nobody supplies yet: stop and name it the first time the game calls it.
SYSV static void missing_called(const char *name) { fatal("the game called a function not supplied yet: %s", name); }

static uint8_t *g_stub_page;
static size_t g_stub_used;
static int g_missing;

static void *make_missing_stub(const char *name) {
    if (!g_stub_page) g_stub_page = VirtualAlloc(NULL, 1 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    uint8_t *p = g_stub_page + g_stub_used;
    g_stub_used += 32;
    if (g_stub_used > (1 << 20)) fatal("too many missing functions");
    p[0] = 0x48; p[1] = 0xBF;                       // mov rdi, name
    memcpy(p + 2, &name, 8);
    p[10] = 0x48; p[11] = 0xB8;                     // mov rax, handler
    void *h = (void *)missing_called;
    memcpy(p + 12, &h, 8);
    p[20] = 0xFF; p[21] = 0xE0;                     // jmp rax
    g_missing++;
    log_line("not supplied yet: %s", name);
    return p;
}

static void *resolve_for(Module *m, uint32_t symi, int excl_module) {
    const Sym *s = &m->dynsym[symi];
    const char *n = m->dynstr + s->name;
    GlobalSym *g = slot_for(n);
    if (g->name && g->module != excl_module) return g->addr;
    const ShimEntry *e = shim_find(n);
    if (e) return e->ptr;
    if (s->shndx) return m->pub.base + s->value;
    extern void *shim_dynamic(const char *);
    void *dyn = shim_dynamic(n);
    if (dyn) return dyn;
    if ((s->info >> 4) == 2) return NULL;           // weak and absent is allowed
    return make_missing_stub(n);
}

// Copy relocations fill the game's own storage, so the source must be the definition in another module.
static void *find_definition_elsewhere(Module *self, const char *name) {
    for (int k = 0; k < g_nmods; k++) {
        Module *o = &g_mods[k];
        if (o == self) continue;
        for (size_t i = 1; i < o->nsyms; i++) {
            const Sym *s = &o->dynsym[i];
            if (!s->shndx || (o->versym && (o->versym[i] & 0x8000))) continue;
            if (!strcmp(o->dynstr + s->name, name)) return o->pub.base + s->value;
        }
    }
    const ShimEntry *e = shim_find(name);
    return e ? e->ptr : NULL;
}

static void apply_table(Module *m, const Rela *r, size_t count, int do_copy) {
    for (size_t i = 0; i < count; i++, r++) {
        uint32_t type = (uint32_t)r->info, symi = (uint32_t)(r->info >> 32);
        uint64_t *where = (uint64_t *)(m->pub.base + r->offset);
        if (type == R_COPY) {
            if (do_copy) {
                const Sym *s = &m->dynsym[symi];
                void *src = find_definition_elsewhere(m, m->dynstr + s->name);
                if (src) memcpy(where, src, s->size);
                else log_line("nothing to copy for %s", m->dynstr + s->name);
            }
            continue;
        }
        if (do_copy) continue;
        switch (type) {
        case R_RELATIVE: *where = (uint64_t)(m->pub.base + r->addend); break;
        case R_64: *where = (uint64_t)resolve_for(m, symi, -1) + r->addend; break;
        case R_GLOB_DAT:
        case R_JUMP_SLOT: *where = (uint64_t)resolve_for(m, symi, -1); break;
        case R_DTPMOD64: *where = (uint64_t)m->pub.tls_module; break;
        case R_DTPOFF64: *where = (symi ? m->dynsym[symi].value : 0) + r->addend; break;
        default: fatal("%s: relocation type %u not handled", m->pub.name, type);
        }
    }
}

static void relocate(Module *m, int do_copy) {
    const Rela *rela = NULL, *jmp = NULL;
    size_t relasz = 0, jmpsz = 0;
    for (const Dyn *d = m->dynamic; d && d->tag; d++) {
        if (d->tag == DT_RELA) rela = (const Rela *)(m->pub.base + d->val);
        else if (d->tag == DT_RELASZ) relasz = d->val;
        else if (d->tag == DT_JMPREL) jmp = (const Rela *)(m->pub.base + d->val);
        else if (d->tag == DT_PLTRELSZ) jmpsz = d->val;
    }
    if (rela) apply_table(m, rela, relasz / sizeof(Rela), do_copy);
    if (jmp) apply_table(m, jmp, jmpsz / sizeof(Rela), do_copy);
}

// ---- The game changes, by topic. One translation unit: these files see everything above and each other, in this order.
#include "game_fixes.inc"
#include "interface.inc"
#include "effects_and_tests.inc"
#include "multiplayer.inc"
#include "battle_choice.inc"
#include "frontend_extras.inc"
#include "script_trace.inc"
#include "key_trace.inc"
#include "film_trace.inc"
#include "mouse_cursor.inc"
#include "mod_support.inc"
#include "pass_stats.inc"
#include "shadow_rate.inc"
#include "soldier_order.inc"
#include "task_wait.inc"
#include "sort_fast.inc"
#include "task_waits.inc"
#include "prep_parallel.inc"
#include "tracker_grain.inc"
#include "plugins.inc"
#include "dx11_device.inc"
#include "ui_x2.inc"

static void apply_game_patches(void) {
    { extern void upscale_start(void); upscale_start(); }
    // Test switch ATTILA_PLAIN=1: the game exactly as shipped, none of our changes or settings; only the measuring hooks.
    if (getenv("ATTILA_PLAIN")) { log_line("PLAIN: no game patches applied"); patch_skip_movies(); patch_tick_trace(); return; }
    patch_battle_8v8();
    patch_loading_screen_icons();
    patch_lobby_join_guard();
    patch_mp_text_decode();
    patch_event_feed_humans();
    patch_script_direct_mode();
    patch_battle_choice();
    patch_lobby_map_picture();
    patch_menu_screen_watch();
    patch_script_trace();
    patch_key_trace();
    patch_film_trace();
    patch_mouse_cursor();
    patch_animation_missing_tracks();
    test_mod_command();
    patch_eight_player_campaign();
    patch_player_drop();
    patch_turn_times();
    patch_ai_move_speed();
    patch_music_track_bounds();
    patch_sprite_step();
    patch_vfx_register();
    patch_curl_monitor();
    patch_text_trace();
    patch_dx11();
    patch_lobby_trace();
    patch_ui_size();
    patch_sharp_text();
    patch_gfx_trace();
    patch_tick_trace();
    patch_skymap_trace();
    patch_version_text();
    patch_skip_movies();
    patch_vfx_pool_locks();
    patch_vfx_gate();
    patch_shadow_rate();
    patch_task_wait();
    patch_task_threads();
    patch_model_sync_count();
    patch_upload_rows();
    patch_batch_rule(); patch_soldier_order(); patch_batch_sets();
    patch_fast_sort();
    patch_task_waits();
    patch_prep_parallel();
    patch_tracker_grain();
    patch_ui_x2();
    patch_vfx_group_check();
    { const char *e = getenv("ATTILA_TASK_SPLIT"); patch_task_split(e ? atoi(e) : 20); }
    // Graphics options screen: show the real card memory instead of the 32-bit budget, which stops at 4095 MB. Display only.
    // Replaces "call rax; shr eax,20" in UIDLL::OptionsGraphics::InitializeCardDropdown with "mov eax,<MB>".
    uint8_t *show = (uint8_t *)0x2c87e9e;
    static const uint8_t show_old[5] = {0xFF, 0xD0, 0xC1, 0xE8, 0x14};
    unsigned mb = card_memory_mb();
    if (!mb) log_line("card memory not available from Windows; the options screen keeps its own number");
    else if (memcmp(show, show_old, 5)) log_line("patch skipped, game code differs: card memory display");
    else {
        show[0] = 0xB8;
        memcpy(show + 1, &mb, 4);
        log_line("patch applied: options screen shows %u MB of card memory", mb);
    }
    for (size_t i = 0; i < sizeof g_game_patches / sizeof g_game_patches[0]; i++) {
        const GamePatch *p = &g_game_patches[i];
        { const char *skip = getenv("ATTILA_SKIP_PATCH"); if (skip && skip[0] && strstr(p->name, skip)) { log_line("patch left out for a test: %s", p->name); continue; } }
        if (memcmp((void *)p->addr, p->old_bytes, (size_t)p->len)) { log_line("patch skipped, game code differs: %s", p->name); continue; }
        memcpy((void *)p->addr, p->new_bytes, (size_t)p->len);
        log_line("patch applied: %s", p->name);
    }
}

typedef SYSV void (*init_fn)(int, char **, char **);

static void run_inits(Module *m, int argc, char **argv, char **envp) {
    init_fn init = NULL;
    init_fn *arr = NULL;
    size_t n = 0;
    for (const Dyn *d = m->dynamic; d && d->tag; d++) {
        if (d->tag == DT_INIT) init = (init_fn)(m->pub.base + d->val);
        else if (d->tag == DT_INIT_ARRAY) arr = (init_fn *)(m->pub.base + d->val);
        else if (d->tag == DT_INIT_ARRAYSZ) n = d->val / 8;
    }
    if (init) init(argc, argv, envp);
    for (size_t i = 0; i < n; i++)
        if (arr[i] && arr[i] != (init_fn)-1) arr[i](argc, argv, envp);
}

// Thread-local blocks for libstdc++ and friends, one set per thread.
static DWORD g_tls_index;

void *tls_block_for(int module_id) {
    void **set = TlsGetValue(g_tls_index);
    if (!set) {
        set = calloc(MAX_MODULES + 1, sizeof(void *));
        TlsSetValue(g_tls_index, set);
    }
    if (!set[module_id]) {
        for (int i = 0; i < g_nmods; i++)
            if (g_mods[i].pub.tls_module == module_id) {
                uint8_t *b = calloc(1, g_mods[i].pub.tls_memsz + 64);
                memcpy(b, g_mods[i].pub.tls_image, g_mods[i].pub.tls_filesz);
                set[module_id] = b;
            }
    }
    return set[module_id];
}

// Enter the game the way the Linux kernel would: argc and argv on the stack at its _start.
__attribute__((naked)) static void enter_at(void *entry, void *sp) {
    __asm__("mov %rdx, %rsp\n"
            "xor %edx, %edx\n"
            "xor %ebp, %ebp\n"
            "jmp *%rcx\n");
}

static void trace_setup(void);
static void cover_frames(Module *m);
typedef struct { const char *exe, *libdir; int argc; char **argv; } StartArgs;

static DWORD WINAPI game_thread(void *param) {
    StartArgs *a = param;
    char path[MAX_PATH];
    extern char **shim_environ(void);
    char **envp = shim_environ();

    g_syms = calloc(GSIZE, sizeof(GlobalSym));
    Module *game = load_elf(a->exe, "Attila", 1);
    snprintf(path, sizeof path, "%s\\libgcc_s.so.1", a->libdir);
    Module *gcc = load_elf(path, "libgcc_s", 0);
    snprintf(path, sizeof path, "%s\\libstdc++.so.6", a->libdir);
    Module *cxx = load_elf(path, "libstdc++", 0);

    for (int i = 0; i < g_nmods; i++) add_module_symbols(i);
    relocate(gcc, 0);
    relocate(cxx, 0);
    relocate(game, 0);
    relocate(game, 1);
    log_line("functions not supplied yet: %d", g_missing);

    for (int i = 0; i < g_nmods; i++) patch_stack_checks(&g_mods[i]);
    for (int i = 0; i < g_nmods; i++) cover_frames(&g_mods[i]);   // see crash_filter
    apply_game_patches();
    plugins_load(game);   // after our own fixes, so a plugin sees the game as it will run
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);

    run_inits(gcc, a->argc, a->argv, envp);
    run_inits(cxx, a->argc, a->argv, envp);
    trace_setup();
    log_line("libraries started; entering the game at %p", (void *)game->entry);

    // kernel-style start stack: argc, argv..., NULL, envp..., NULL, then an empty aux vector
    size_t envc = 0;
    while (envp[envc]) envc++;
    size_t words = 1 + (size_t)a->argc + 1 + envc + 1 + 2;
    uint64_t *top = alloca((words + 2) * 8 + 16);
    top = (uint64_t *)(((uintptr_t)top + 15) & ~(uintptr_t)15);
    size_t w = 0;
    top[w++] = (uint64_t)a->argc;
    for (int i = 0; i < a->argc; i++) top[w++] = (uint64_t)a->argv[i];
    top[w++] = 0;
    for (size_t i = 0; i < envc; i++) top[w++] = (uint64_t)envp[i];
    top[w++] = 0;
    top[w++] = 0;
    top[w++] = 0;
    enter_at((void *)game->entry, top);
    return 0;
}

const char *loader_where(void *addr) {
    static __thread char buf[96];
    for (int k = 0; k < g_nmods; k++)
        if ((uint8_t *)addr >= g_mods[k].lo && (uint8_t *)addr < g_mods[k].hi) {
            snprintf(buf, sizeof buf, "%s+%llx", g_mods[k].pub.name, (unsigned long long)((uint8_t *)addr - g_mods[k].pub.base));
            return buf;
        }
    snprintf(buf, sizeof buf, "%p", addr);
    return buf;
}

// Every few seconds, note where each thread is running: shows what a stuck game is waiting for.
static DWORD WINAPI sampler_thread(void *unused) {
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    uint8_t *self = (uint8_t *)GetModuleHandleA(NULL);
    extern const char *shim_nearest(void *addr);
    for (int round = 0; round < 40; round++) {
        Sleep(15000);
        log_line("=== sample %d", round);
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te;
        te.dwSize = sizeof te;
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
            HANDLE t = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
            if (!t) continue;
            SuspendThread(t);
            CONTEXT c;
            c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(t, &c)) {
                FILETIME a, b, k, u;
                GetThreadTimes(t, &a, &b, &k, &u);
                double busy = ((double)u.dwLowDateTime + 4294967296.0 * u.dwHighDateTime) / 1e7;
                char line[600];
                int n = snprintf(line, sizeof line, "t%lu user %.0fs rip %s", te.th32ThreadID, busy, loader_where((void *)c.Rip));
                uint64_t *sp = (uint64_t *)c.Rsp;
                int shown = 0;
                for (int i = 0; i < 400 && shown < 7; i++) {
                    MEMORY_BASIC_INFORMATION mbi;
                    if (!VirtualQuery(sp + i, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
                    uint64_t v = sp[i];
                    if (v >= 0x400000 && v < 0x3d00000) { n += snprintf(line + n, sizeof line - (size_t)n, " %llx", v); shown++; }
                    if (n > 540) break;
                }
                if (busy > 1 || shown) log_line("%s", line);
            }
            ResumeThread(t);
            CloseHandle(t);
        }
        CloseHandle(snap);
    }
    return 0;
}

// Tracer: ATTILA_TRACE="hexaddr=name,hexaddr=name:s" logs calls to game functions and what they return.
// ":s" means the first argument is text. Uses one-byte breakpoints, put back after each hit.
typedef struct { uint8_t *addr; char name[60]; int text; uint8_t saved; int ret_of; long hits; } TracePoint;
#define MAX_TRACE 64
static TracePoint g_trace[MAX_TRACE];
static int g_ntrace;
static CRITICAL_SECTION g_trace_lock;
static __thread TracePoint *t_rearm;

static void trace_setup(void) {
    const char *spec = getenv("ATTILA_TRACE");
    if (!spec) return;
    InitializeCriticalSection(&g_trace_lock);
    char buf[2048];
    strncpy(buf, spec, sizeof buf - 1);
    for (char *item = strtok(buf, ","); item && g_ntrace < MAX_TRACE / 2; item = strtok(NULL, ",")) {
        TracePoint *t = &g_trace[g_ntrace++];
        char *eq = strchr(item, '=');
        if (!eq) continue;
        *eq = 0;
        t->addr = (uint8_t *)strtoull(item, NULL, 16);
        char *colon = strchr(eq + 1, ':');
        if (colon) { *colon = 0; t->text = 1; }
        strncpy(t->name, eq + 1, sizeof t->name - 1);
        t->ret_of = -1;
        t->saved = *t->addr;
        *t->addr = 0xCC;
    }
    log_line("tracer: %d points", g_ntrace);
}

static LONG trace_exception(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    if (r->ExceptionCode == EXCEPTION_SINGLE_STEP && t_rearm) {
        t_rearm->addr[0] = 0xCC;
        t_rearm = NULL;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (r->ExceptionCode != EXCEPTION_BREAKPOINT || !g_ntrace) return EXCEPTION_CONTINUE_SEARCH;
    EnterCriticalSection(&g_trace_lock);
    for (int i = 0; i < MAX_TRACE; i++) {
        TracePoint *t = &g_trace[i];
        if (!t->addr || t->addr != (uint8_t *)c->Rip) continue;
        if (t->ret_of >= 0) {
            // one-shot return point
            log_line("  %s returned %lld (0x%llx)", g_trace[t->ret_of].name, (long long)c->Rax, c->Rax);
            *t->addr = t->saved;
            t->addr = NULL;
            LeaveCriticalSection(&g_trace_lock);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        long n = ++t->hits;
        if (!strcmp(t->name, "MUSICITEM")) {
            // fingerprint of the data handed to CAkMusicTrack::SetInitialValues(this, data, size, ...), to compare between runs
            const uint8_t *d = (const uint8_t *)c->Rsi;
            unsigned size = (unsigned)c->Rdx;
            uint32_t h = 2166136261u;
            for (unsigned k = 0; k < size; k++) h = (h ^ d[k]) * 16777619u;
            log_line("MUSICITEM %ld size %u hash %08x at %p", n, size, h, d);
        } else if (n <= 25 || n % 500 == 0) {
            if (t->text && c->Rdi) log_line("[%ld] %s('%s', %llx, %llx)", n, t->name, (const char *)c->Rdi, c->Rsi, c->Rdx);
            else log_line("[%ld] %s(%llx, %llx, %llx)", n, t->name, c->Rdi, c->Rsi, c->Rdx);
            uint8_t *ret = *(uint8_t **)c->Rsp;
            for (int k = MAX_TRACE / 2; k < MAX_TRACE; k++)
                if (!g_trace[k].addr) {
                    g_trace[k].addr = ret;
                    g_trace[k].ret_of = i;
                    g_trace[k].saved = *ret;
                    *ret = 0xCC;
                    break;
                }
        }
        // run the real first instruction once, then put the breakpoint back
        *t->addr = t->saved;
        t_rearm = t;
        c->EFlags |= 0x100;
        LeaveCriticalSection(&g_trace_lock);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    LeaveCriticalSection(&g_trace_lock);
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG crash_report(EXCEPTION_RECORD *r, CONTEXT *c);

// 1 = the address is in the game or a library loaded next to it (Linux code), in attila64.exe, or in the page near the game
// (hook trampolines, patches, plugin code).
static int in_own_code(DWORD64 a) {
    uint8_t *self = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(self + ((IMAGE_DOS_HEADER *)self)->e_lfanew);
    if ((uint8_t *)a >= self && (uint8_t *)a < self + nt->OptionalHeader.SizeOfImage) return 1;
    if (near_page && (uint8_t *)a >= near_page && (uint8_t *)a < near_page + 0x2000) return 1;
    for (int k = 0; k < g_nmods; k++)
        if ((uint8_t *)a >= g_mods[k].lo && (uint8_t *)a < g_mods[k].hi) return 1;
    return 0;
}

// Windows calls this when its search for a handler of an exception reaches a frame of the Linux code: nothing between the
// fault and the game caught it. c is the context of the fault.
static EXCEPTION_DISPOSITION game_frame_handler(EXCEPTION_RECORD *r, void *frame, CONTEXT *c, void *dc) {
    (void)frame; (void)dc;
    if (r->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) return ExceptionContinueSearch;
    log_line("nothing caught exception %08lx before the game's code", r->ExceptionCode);
    crash_report(r, c);
    return ExceptionContinueSearch;
}

// The Linux code has no Windows unwind data, so Windows' search for a handler gets lost in its frames and an exception that
// a Windows DLL raised but nobody caught ended the game with no report at all. Each module's range gets one function table
// entry (no unwind codes) with game_frame_handler as its handler: the search stops at the first frame there. The unwind
// info and a jump to the handler go in a page above the module, within 4 GB (the table uses 32-bit offsets).
static void cover_frames(Module *m) {
    static RUNTIME_FUNCTION rf[MAX_MODULES];
    static int n;
    uint8_t *at = NULL;
    for (uint64_t p = ((uint64_t)(uintptr_t)m->hi + 0xffff) & ~0xffffull; !at && n < MAX_MODULES && p < (uint64_t)(uintptr_t)m->lo + 0xF0000000ull; p += 0x10000)
        at = VirtualAlloc((void *)(uintptr_t)p, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!at) { log_line("crash reports: no memory near %s, its frames are not covered", m->pub.name); return; }
    at[0] = 1 | (UNW_FLAG_EHANDLER << 3); at[1] = 0; at[2] = 0; at[3] = 0;   // version 1, a handler, no prolog, no codes
    uint8_t *jump = at + 8;   // mov rax,game_frame_handler; jmp rax
    uint32_t h = (uint32_t)(jump - m->lo);
    memcpy(at + 4, &h, 4);
    uint64_t target = (uint64_t)game_frame_handler;
    jump[0] = 0x48; jump[1] = 0xB8;
    memcpy(jump + 2, &target, 8);
    jump[10] = 0xFF; jump[11] = 0xE0;
    rf[n].BeginAddress = 0;
    rf[n].EndAddress = (DWORD)(m->hi - m->lo);
    rf[n].UnwindData = (DWORD)(at - m->lo);
    if (!RtlAddFunctionTable(&rf[n], 1, (DWORD64)(uintptr_t)m->lo)) { log_line("crash reports: function table for %s refused", m->pub.name); return; }
    n++;
}

__attribute__((noinline)) static void log_left_alone(EXCEPTION_RECORD *r, CONTEXT *c) {
    HMODULE m = NULL;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)c->Rip, &m))
        GetModuleFileNameA(m, name, sizeof name);
    const char *s = strrchr(name, '\\');
    log_line("a Windows DLL raised exception %08lx, left to its own handler (at %p, %s)", r->ExceptionCode, (void *)c->Rip, s ? s + 1 : name);
}

// Kept small (the report is in crash_report): after a stack overflow this runs on what is left of the stack.
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    LONG tr = trace_exception(ep);
    if (tr != EXCEPTION_CONTINUE_SEARCH) return tr;
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    if (r->ExceptionCode == 0x406D1388 || r->ExceptionCode == 0x40010006 || r->ExceptionCode == 0x4001000A)
        return EXCEPTION_CONTINUE_SEARCH;
    // A C++ exception thrown and caught inside a Windows DLL (graphics driver, overlay, ...) is normal; this filter sees it before the DLL's own catch.
    // If nobody catches it, game_frame_handler or unhandled_note below reports it.
    if (r->ExceptionCode == 0xE06D7363) {
        static volatile LONG seen;
        if (InterlockedIncrement(&seen) <= 5) log_line("a Windows DLL raised a C++ exception, left to its own handler (from %p)", (void *)c->Rip);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // The same for any other exception in a Windows DLL: drivers, overlays such as ReShade and system DLLs catch some of their
    // own a moment later (an access violation inside a guarded memory check, RPC errors like 6ba). One nobody catches reaches a
    // game frame (game_frame_handler) or, on a thread of its own, unhandled_note: both report it. Stack overflow and heap damage
    // are reported at once.
    if (r->ExceptionCode != 0xC00000FD && r->ExceptionCode != 0xC0000374 && !in_own_code(c->Rip)) {
        static volatile LONG seen_dll;
        if (InterlockedIncrement(&seen_dll) <= 5) log_left_alone(r, c);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    return crash_report(r, c);
}

// The report: what happened, the registers and the game's and our code addresses on the stack, then the game ends.
__attribute__((noinline)) static LONG crash_report(EXCEPTION_RECORD *r, CONTEXT *c) {
    log_go_sync();
    log_line("CRASH code %08lx at %p (read/write %llu, address %p)", r->ExceptionCode, (void *)c->Rip,
             r->NumberParameters ? (unsigned long long)r->ExceptionInformation[0] : 0,
             r->NumberParameters > 1 ? (void *)r->ExceptionInformation[1] : NULL);
    log_line("rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx rsp=%llx rbp=%llx", c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rsp, c->Rbp);
    extern const char *shim_nearest(void *addr);
    uint8_t *self = (uint8_t *)GetModuleHandleA(NULL);
    log_line("in attila64.exe at offset %llx, nearest shim before it: %s", (unsigned long long)((uint8_t *)c->Rip - self), shim_nearest((void *)c->Rip));
    uint64_t *sp = (uint64_t *)c->Rsp;
    int shown = 0;
    for (int i = 0; i < 1200 && shown < 24; i++) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(sp + i, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
        uint64_t v = sp[i];
        if ((uint8_t *)v >= self && (uint8_t *)v < self + (4u << 20)) log_line("  stack-exe %03x: %s", i * 8, shim_nearest((void *)v));
        for (int k = 0; k < g_nmods; k++)
            if ((uint8_t *)v >= g_mods[k].lo && (uint8_t *)v < g_mods[k].hi && (k || v < 0x3d00000)) {
                log_line("  stack+%03x: %s+%llx", i * 8, g_mods[k].pub.name, v - (uint64_t)g_mods[k].pub.base);
                shown++;
            }
    }
    ExitProcess(4);
    return EXCEPTION_EXECUTE_HANDLER;
}

// Laptops with two graphics chips: these two markers ask the NVIDIA and AMD drivers to use the strong one.
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

static LONG WINAPI unhandled_note(EXCEPTION_POINTERS *ep) {
    log_go_sync();
    log_line("CRASH: nobody handled exception %08lx at %p", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    return crash_report(ep->ExceptionRecord, ep->ContextRecord);
}

#include "settings_copy.inc"
#include "graphics_choice.inc"
#include "self_update.inc"

int main(int argc, char **argv) {
    // Two ways to start:
    //   attila64 <linux game file> <folder with libstdc++ and libgcc_s> [game arguments]   (development)
    //   attila64 [anything Steam adds]    everything sits next to the exe, in a folder inside the game's folder
    int dev = argc >= 3 && GetFileAttributesA(argv[1]) != INVALID_FILE_ATTRIBUTES && !(GetFileAttributesA(argv[1]) & FILE_ATTRIBUTE_DIRECTORY) &&
              !strstr(argv[1], ".exe") && (GetFileAttributesA(argv[2]) & FILE_ATTRIBUTE_DIRECTORY) && GetFileAttributesA(argv[2]) != INVALID_FILE_ATTRIBUTES;
    {   // the game's fixed address range first, before anything else takes memory (see keep_game_range)
        char elf[MAX_PATH];
        if (dev) snprintf(elf, sizeof elf, "%s", argv[1]);
        else {
            GetModuleFileNameA(NULL, elf, sizeof elf);
            char *s = strrchr(elf, '\\');
            if (s) strcpy(s + 1, "Attila");
        }
        DWORD code = 0;
        if (keep_game_range(elf, &code)) return (int)code;
    }
    static char own_dir[MAX_PATH], own_game[MAX_PATH];
    static char *own_argv[64];
    if (!dev) {
        GetModuleFileNameA(NULL, own_dir, sizeof own_dir);
        char *s = strrchr(own_dir, '\\');
        if (s) *s = 0;
        snprintf(own_game, sizeof own_game, "%s\\Attila", own_dir);
        {   // a newer version from our Workshop item (self_update.inc): before the start window, so it is the new one
            int code = self_update(own_dir);
            if (code >= 0) return code;
            if (getenv("ATTILA_TEST_SELF_UPDATE_ONLY")) {   // test: write the outcome next to the exe and stop
                char p[MAX_PATH + 32];
                snprintf(p, sizeof p, "%s\\self_update_test.txt", own_dir);
                FILE *f = fopen(p, "w");
                if (f) { fprintf(f, "%s\n%s\n", APP_VERSION_TEXT, g_update_note); fclose(f); }
                return 0;
            }
        }
        // the game folder is the one above ours, unless a test says otherwise
        char work[MAX_PATH];
        const char *over = getenv("ATTILA_GAME_DIR");
        snprintf(work, sizeof work, "%s\\..", own_dir);
        if (!SetCurrentDirectoryA(over ? over : work)) {
            MessageBoxA(NULL, "Could not open the game folder.", "64-bit Attila", MB_ICONERROR);
            return 2;
        }
        {   // Steam's Play with our Launch Options hands us normal Attila's launcher (%command%): kept for the picker's
            // "Normal Attila launcher" button (picker.c start_ca_launcher)
            extern char g_ca_launcher[MAX_PATH];
            for (int i = 1; i < argc; i++) {
                size_t n = strlen(argv[i]);
                if (n >= 12 && !_stricmp(argv[i] + n - 12, "launcher.exe")) { snprintf(g_ca_launcher, MAX_PATH, "%s", argv[i]); break; }
            }
        }
        own_argv[0] = argv[0];
        own_argv[1] = own_game;
        own_argv[2] = own_dir;
        argc = 3;
        if (!getenv("ATTILA_NO_MODS") && !getenv("ATTILA_NO_PICKER")) {
            extern int mod_picker_run(void);
            if (!mod_picker_run()) return 0;
            if (getenv("ATTILA_PICKER_ONLY")) return 0;
        }
        // High-resolution screens. Windows scales the windows of a program that does not say it handles screen scaling itself:
        // at 200% (the usual setting on a 4K screen) the game got a 1920 x 1080 window that Windows blew up to 3840 x 2160, while
        // the game still drew its full-size frame and shrank it into that window: a soft picture and the cost of 4K (reports
        // 2026-11-11/12: "looks like 25% resolution", low fps on an RTX 4080). From here on this thread, which makes the game's
        // window, handles scaling itself (per monitor, version 2; Windows 10 1703 and later), so the window is in real pixels.
        // The start window above was made before this and stays scaled by Windows. ATTILA_NO_DPI_AWARE=1 turns it off.
        {
            HMODULE u32 = GetModuleHandleA("user32.dll");
            void *(WINAPI *set_ctx)(void *) = u32 ? (void *(WINAPI *)(void *))GetProcAddress(u32, "SetThreadDpiAwarenessContext") : NULL;
            UINT (WINAPI *sys_dpi)(void) = u32 ? (UINT (WINAPI *)(void))GetProcAddress(u32, "GetDpiForSystem") : NULL;
            UINT dpi = sys_dpi ? sys_dpi() : 96;
            int done = !getenv("ATTILA_NO_DPI_AWARE") && set_ctx && set_ctx((void *)(intptr_t)-4) != NULL;   // PER_MONITOR_AWARE_V2
            g_dpi_percent = dpi * 100 / 96; g_dpi_own = done;   // logged once the log file is open
        }
        {
            extern float settings_ui_size(void);   // picker.c: the "Interface size" choice
            g_ui_size = settings_ui_size();
        }
        // The player's mod list is 64-bit Attila's own, mod_list.txt next to the exe (picker.c mod_list_path: made once from the
        // game folder's used_mods.txt, which normal Attila's launcher rewrites; until 1.2 Test 6 that shared file was used).
        // The 64-bit engine's own packs (8-player campaign lobby, 8v8 battle screen) sit in <our folder>\packs and are loaded only by
        // this engine, never by the normal 32-bit game. The game reads its mod list as a script file: used_mods_64.txt = our folder
        // and packs first (they win over other mods' copies of the same screens), then the player's own list.
        {
            extern const char *mod_list_path(void);
            const char *list = getenv("ATTILA_NO_MODS") ? NULL : mod_list_path();
            int have_list = list && GetFileAttributesA(list) != INVALID_FILE_ATTRIBUTES;
            static char packs[MAX_PATH], list_arg[] = "used_mods_64.txt;";
            snprintf(packs, sizeof packs, "%s\\packs", own_dir);
            WIN32_FIND_DATAA fd;
            char pattern[MAX_PATH];
            snprintf(pattern, sizeof pattern, "%s\\*.pack", packs);
            HANDLE h = FindFirstFileA(pattern, &fd);
            // The waving-flags pack for 1212 AD (packs\1212\!!1212ad_flag_wave.pack: 1212's banner and pennant models with the
            // cloth data of JGdaCaesar's Flag Wave, made by mk1212_flags\build_flags.ps1): first of all, so it wins over
            // 1212's own copies, and only when the player's list has a 1212 pack and the engine fix and the flags are ticked.
            static char flagdir[MAX_PATH];
            int flags = 0;
            {
                extern int settings_perf_fix(void), settings_flag_wave(void);
                char fp[MAX_PATH + 40];
                snprintf(flagdir, sizeof flagdir, "%s\\1212", packs);
                snprintf(fp, sizeof fp, "%s\\!!1212ad_flag_wave.pack", flagdir);
                if (have_list && settings_perf_fix() && settings_flag_wave() && GetFileAttributesA(fp) != INVALID_FILE_ATTRIBUTES) {
                    FILE *u = fopen(list, "r");
                    char line[1024];
                    if (u) {
                        while (!flags && fgets(line, sizeof line, u))
                            if (!strncmp(line, "mod ", 4) && strstr(line, "1212") && !strstr(line, "!!1212ad_flag_wave")) flags = 1;
                        fclose(u);
                    }
                }
                log_line(flags ? "1212 AD in the mod list: its waving-flags pack goes in first (packs\\1212)" : "waving-flags pack for 1212 AD: not loaded (%s)",
                         !have_list ? "no mod list" : GetFileAttributesA(fp) == INVALID_FILE_ATTRIBUTES ? "not installed" :
                         !settings_perf_fix() || !settings_flag_wave() ? "unticked in the start window" : "no 1212 pack in the mod list");
            }
            if (h != INVALID_HANDLE_VALUE || have_list) {
                FILE *f = fopen("used_mods_64.tmp", "w");
                if (f) {
                    if (flags) fprintf(f, "add_working_directory \"%s\";\nmod \"!!1212ad_flag_wave.pack\";\n", flagdir);
                    if (h != INVALID_HANDLE_VALUE) {
                        fprintf(f, "add_working_directory \"%s\";\n", packs);
                        do fprintf(f, "mod \"%s\";\n", fd.cFileName); while (FindNextFileA(h, &fd));
                    }
                    if (have_list) {   // then the player's own list, unchanged
                        FILE *u = fopen(list, "r");
                        char line[1024];
                        if (u) { while (fgets(line, sizeof line, u)) if (!flags || !strstr(line, "!!1212ad_flag_wave")) fputs(line, f); fclose(u); }
                    }
                    int ok = fclose(f) == 0;
                    if (ok && MoveFileExA("used_mods_64.tmp", "used_mods_64.txt", MOVEFILE_REPLACE_EXISTING)) { own_argv[3] = list_arg; argc = 4; }
                }
            }
            if (h != INVALID_HANDLE_VALUE) FindClose(h);
        }
        // Settings for the game, written the way the game's own command line takes them. ATTILA_ARGS is for tests.
        {
            static char extra[1024];
            const char *e = getenv("ATTILA_ARGS");
            if (e) {
                snprintf(extra, sizeof extra, "%s", e);
                for (char *tok = strtok(extra, " "); tok && argc < 63; tok = strtok(NULL, " ")) own_argv[argc++] = tok;
            }
        }
        argv = own_argv;
    }
    // the log sits next to the exe: the working folder is the game's own folder
    // A restart must not erase the log of what just went wrong: the logs of the last four games are kept
    // (previous = the game before this one, previous4 = the oldest). The mod list window's log buttons and the installer use the same names.
    static const char *const log_names[] = {"attila64_log.txt", "attila64_log_previous.txt", "attila64_log_previous2.txt",
                                            "attila64_log_previous3.txt", "attila64_log_previous4.txt"};
    char logpath[MAX_PATH + 32], prevpath[MAX_PATH + 32];
    GetModuleFileNameA(NULL, logpath, MAX_PATH);
    char *slash = strrchr(logpath, '\\');
    int dir_len = slash ? (int)(slash + 1 - logpath) : 0;
    for (int i = 4; i > 0; i--) {
        snprintf(prevpath, sizeof prevpath, "%.*s%s", dir_len, logpath, log_names[i]);
        snprintf(logpath + dir_len, sizeof logpath - (size_t)dir_len, "%s", log_names[i - 1]);
        MoveFileExA(logpath, prevpath, MOVEFILE_REPLACE_EXISTING);
    }
    g_log = fopen(logpath, "w");  // logpath now ends in attila64_log.txt
    log_start_writer();
    log_line("64-bit Attila " APP_VERSION_TEXT);   // every log says which version wrote it
    if (g_update_note[0]) log_line("%s", g_update_note);
    if (g_dpi_percent) log_line("Windows display scaling %u%%; the game's window is %s", g_dpi_percent, g_dpi_own ? "in real pixels" : "left to Windows' scaling");
    {
        char why[600];
        if (GetEnvironmentVariableA("ATTILA64_RELAUNCHED", why, sizeof why))
            log_line("second start with Windows' address randomization on for this program: the game's memory range was in use:%s", why);
        if (!g_elf_res) log_line("game memory range not kept at the start (it was in use); loading it now");
    }
    copy_normal_graphics_settings();   // before the game reads its settings
    apply_texture_sharpness();         // the start window's choice, also before the game reads its settings
    apply_test_aa();
    { extern void steam_connect_early(void); steam_connect_early(); }   // Steam overlay: must be loaded before the swap chain is made
    setup_graphics_route();            // before anything loads OpenGL
    // Windows wakes timed waits (SleepConditionVariableSRW behind pthread_cond_timedwait, sem waits, Sleep) only on its timer
    // tick, 15.6 ms unless a program asks for a finer one; Linux wakes them when asked. Test: ask for 0.5 ms, as games do.
    if (getenv("ATTILA_FINE_TIMER")) {   // test only: measured 2026-11-10, no effect on battle steps, so not on by default
        typedef LONG (WINAPI *SetResFn)(ULONG want, BOOLEAN set, ULONG *now);
        SetResFn set_res = (SetResFn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtSetTimerResolution");
        ULONG now = 0;
        LONG st = set_res ? set_res(5000, TRUE, &now) : -1;
        log_line("timer: asked Windows for 0.5 ms waits -> %s, now %.2f ms", st == 0 ? "ok" : "refused", now / 10000.0);
    }
    { extern volatile long g_tt_on; if (getenv("ATTILA_TIME_TRACE")) g_tt_on = 1; }
    g_tls_index = TlsAlloc();
    AddVectoredExceptionHandler(1, crash_filter);
    SetUnhandledExceptionFilter(unhandled_note);

    static StartArgs a;
    a.exe = argv[1];
    a.libdir = argv[2];
    a.argc = argc - 2;
    a.argv = calloc((size_t)argc, sizeof(char *));
    a.argv[0] = argv[1];
    for (int i = 3; i < argc; i++) a.argv[i - 2] = argv[i];

    // The game expects to grow its stack freely; a fully committed one avoids Windows' guard-page probing rules.
    if (getenv("ATTILA_SAMPLE")) CreateThread(NULL, 0, sampler_thread, NULL, 0, NULL);
    DWORD game_thread_id = 0;
    HANDLE t = CreateThread(NULL, 256u << 20, game_thread, &a, 0, &game_thread_id);
    if (!t) fatal("cannot start the game thread");
    char profile_switch[MAX_PATH + 20];
    snprintf(profile_switch, sizeof profile_switch, "%s\\measure_on.txt", own_dir);
    if (getenv("ATTILA_PROFILE") || (own_dir[0] && GetFileAttributesA(profile_switch) != INVALID_FILE_ATTRIBUTES)) {
        extern void profile_start(DWORD main_thread_id);
        extern int g_measure_memory;
        g_measure_memory = 1;
        profile_start(game_thread_id);
    }
    WaitForSingleObject(t, INFINITE);
    DWORD code = 0;
    GetExitCodeThread(t, &code);
    return (int)code;
}
