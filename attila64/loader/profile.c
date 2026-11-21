// ATTILA_PROFILE=1: where the CPU time goes. Every 5 ms looks where each thread is, and every 30 seconds writes a report next to the exe
// (attila64_profile_<n>.txt) and starts counting again. Names are added afterwards by tools\prof_sum.sh. With ATTILA_PASS_STATS=1 as
// well, main thread lines are marked S while the shadow map view draws and B during the main picture (tools\viewsum.sh).
// While a thread is suspended nothing here may touch the heap or the loader lock, or it could wait on that thread for ever.
#include <windows.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const char *loader_where(void *addr);
extern const char *shim_nearest(void *addr);
extern void log_line(const char *fmt, ...);

#define GAME_LO 0x400000ull
#define GAME_HI 0x3700000ull
#define TABLE_BITS 20
#define MAX_THREADS 256

typedef struct { uint64_t rip, caller; uint32_t count; uint32_t is_main; } Hit;
typedef struct { DWORD id; HANDLE h; uint64_t cpu_start; uint32_t samples, in_windows; } Th;

static Hit *g_table;
static uint64_t g_used;
static Th g_th[MAX_THREADS];
static int g_nth;
static DWORD g_main_id;

static void count_hit(uint64_t rip, uint64_t caller, int is_main) {
    uint64_t mask = (1ull << TABLE_BITS) - 1, h = ((rip * 0x9E3779B97F4A7C15ull) ^ (caller * 0xC2B2AE3D27D4EB4Full) ^ (uint64_t)is_main) & mask;
    for (int probe = 0; probe < 64; probe++, h = (h + 1) & mask) {
        Hit *e = &g_table[h];
        if (!e->count) { e->rip = rip; e->caller = caller; e->is_main = (uint32_t)is_main; e->count = 1; g_used++; return; }
        if (e->rip == rip && e->caller == caller && e->is_main == (uint32_t)is_main) { e->count++; return; }
    }
}

// A value on the stack counts as a return address into the game only if a call instruction sits right before it.
static int looks_like_return(uint64_t v) {
    if (v < GAME_LO + 8 || v >= GAME_HI) return 0;
    const uint8_t *p = (const uint8_t *)v;
    if (p[-5] == 0xE8) return 1;
    if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return 1;
    if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) return 1;
    if (p[-4] == 0xFF && (p[-3] & 0x38) == 0x10) return 1;
    if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return 1;
    if (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10) return 1;
    return 0;
}

// Name of the exported function a code address falls in, for Windows' own DLLs.
static void export_name(uint64_t rip, char *out, size_t size) {
    MEMORY_BASIC_INFORMATION mi;
    out[0] = 0;
    if (!VirtualQuery((void *)rip, &mi, sizeof mi) || !mi.AllocationBase) return;
    uint8_t *base = mi.AllocationBase;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir->Size) return;
    IMAGE_EXPORT_DIRECTORY *ex = (IMAGE_EXPORT_DIRECTORY *)(base + dir->VirtualAddress);
    uint32_t *names = (uint32_t *)(base + ex->AddressOfNames), *funcs = (uint32_t *)(base + ex->AddressOfFunctions);
    uint16_t *ords = (uint16_t *)(base + ex->AddressOfNameOrdinals);
    uint32_t want = (uint32_t)(rip - (uint64_t)base), best = 0;
    const char *best_name = NULL;
    for (uint32_t i = 0; i < ex->NumberOfNames; i++) {
        uint32_t rva = funcs[ords[i]];
        if (rva <= want && rva > best) { best = rva; best_name = (const char *)(base + names[i]); }
    }
    if (best_name) snprintf(out, size, "%s", best_name);
}

// Name of one of OUR OWN functions for a code address inside attila64.exe, through Windows' dbghelp.dll. Works only with
// a build that has names beside it (build.ps1 -Sym makes attila64_sym.exe + .pdb with the same options plus -g; run that
// exe for the profile). The normal build has no names (and no folder paths inside it): then the answer is empty.
static int own_function_name(uint64_t where, char *out, size_t size) {
    typedef struct { ULONG SizeOfStruct, TypeIndex; ULONG64 Reserved[2]; ULONG Index, Size; ULONG64 ModBase; ULONG Flags; ULONG64 Value, Address;
                     ULONG Register, Scope, Tag, NameLen, MaxNameLen; CHAR Name[1]; } SymInfo;
    static int state;   // 0 not tried, 1 ready, 2 not there
    static BOOL (WINAPI *from_addr)(HANDLE, DWORD64, DWORD64 *, SymInfo *);
    out[0] = 0;
    if (!state) {
        state = 2;
        HMODULE dh = LoadLibraryA("dbghelp.dll");
        BOOL (WINAPI *init)(HANDLE, const char *, BOOL) = dh ? (void *)GetProcAddress(dh, "SymInitialize") : NULL;
        DWORD64 (WINAPI *load)(HANDLE, HANDLE, const char *, const char *, DWORD64, DWORD, void *, DWORD) = dh ? (void *)GetProcAddress(dh, "SymLoadModuleEx") : NULL;
        from_addr = dh ? (void *)GetProcAddress(dh, "SymFromAddr") : NULL;
        char exe[MAX_PATH];
        if (init && load && from_addr && GetModuleFileNameA(NULL, exe, sizeof exe)) {
            char dir[MAX_PATH];   // the names file (attila64.pdb) is looked for next to the exe, not in the working folder
            snprintf(dir, sizeof dir, "%s", exe);
            { char *slash = strrchr(dir, '\\'); if (slash) *slash = 0; }
            BOOL ok = init(GetCurrentProcess(), dir, FALSE);
            DWORD e1 = GetLastError();
            DWORD64 base = ok ? load(GetCurrentProcess(), NULL, exe, NULL, (DWORD64)(uintptr_t)GetModuleHandleA(NULL), 0, NULL, 0) : 0;
            DWORD e2 = GetLastError();
            if (base) state = 1;
            log_line("profiler: names of our own functions: %s (dbghelp start %d/%lu, module %llx/%lu)", base ? "on" : "not available", (int)ok, (unsigned long)e1, (unsigned long long)base, (unsigned long)e2);
        } else log_line("profiler: names of our own functions: dbghelp.dll not usable");
    }
    if (state != 1) return 0;
    union { SymInfo s; char room[sizeof(SymInfo) + 200]; } u;
    memset(&u, 0, sizeof u);
    u.s.SizeOfStruct = sizeof(SymInfo); u.s.MaxNameLen = 199;
    DWORD64 disp = 0;
    if (!from_addr(GetCurrentProcess(), where, &disp, &u.s) || !u.s.Name[0]) return 0;
    snprintf(out, size, "%s", u.s.Name);
    return 1;
}

static uint64_t thread_cpu(HANDLE t) {
    FILETIME a, b, k, u;
    if (!GetThreadTimes(t, &a, &b, &k, &u)) return 0;
    return ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime) + ((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime);
}

static void refresh_threads(DWORD me) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        int known = 0;
        for (int i = 0; i < g_nth; i++) if (g_th[i].id == te.th32ThreadID) known = 1;
        if (known || g_nth >= MAX_THREADS) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        g_th[g_nth].id = te.th32ThreadID;
        g_th[g_nth].h = h;
        g_th[g_nth].cpu_start = thread_cpu(h);
        g_nth++;
    }
    CloseHandle(snap);
}

static int by_count(const void *a, const void *b) {
    uint32_t x = (*(Hit *const *)a)->count, y = (*(Hit *const *)b)->count;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void write_report(int number, double seconds) {
    char path[MAX_PATH + 40];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    while (n && path[n - 1] != '\\') n--;
    snprintf(path + n, sizeof path - n, "attila64_profile_%d.txt", number);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# window %.1f s\n", seconds);
    for (int i = 0; i < g_nth; i++) {
        uint64_t cpu = thread_cpu(g_th[i].h);
        if (g_th[i].samples)
            fprintf(f, "T %lu %s cpu %.2f s samples %u blocked %u\n", g_th[i].id, g_th[i].id == g_main_id ? "MAIN" : "other",
                    (double)(cpu - g_th[i].cpu_start) / 1e7, g_th[i].samples, g_th[i].in_windows);
        g_th[i].cpu_start = cpu;
        g_th[i].samples = g_th[i].in_windows = 0;
    }
    uint64_t size = 1ull << TABLE_BITS, used = 0;
    Hit **list = malloc(sizeof(Hit *) * (g_used + 1));
    for (uint64_t i = 0; i < size && used < g_used; i++) if (g_table[i].count) list[used++] = &g_table[i];
    qsort(list, used, sizeof *list, by_count);
    for (uint64_t i = 0; i < used; i++) {
        Hit *e = list[i];
        if (e->rip >= GAME_LO && e->rip < GAME_HI) fprintf(f, "H %s G %llx %llx %u\n", e->is_main == 2 ? "S" : e->is_main == 3 ? "B" : e->is_main == 4 ? "s" : e->is_main == 5 ? "b" : e->is_main ? "M" : "O", (unsigned long long)e->rip, (unsigned long long)e->caller, e->count);
        else {
            if (i >= 3000) break;
            char label[200], fn[120];
            MEMORY_BASIC_INFORMATION mi;
            uint8_t *self = (uint8_t *)GetModuleHandleA(NULL);
            uint64_t where = e->rip & ~(1ull << 63);
            int blocked = (e->rip >> 63) != 0;
            if (VirtualQuery((void *)where, &mi, sizeof mi) && mi.AllocationBase == self) {
                if (own_function_name(where, fn, sizeof fn)) snprintf(label, sizeof label, "shim:%s", fn);   // our own function, by name (a build with names)
                else snprintf(label, sizeof label, "shim:%s", shim_nearest((void *)where));
            }
            else if (VirtualQuery((void *)where, &mi, sizeof mi) && mi.AllocationBase && GetModuleFileNameA((HMODULE)mi.AllocationBase, label, sizeof label)) {
                char *slash = strrchr(label, '\\');
                if (slash) memmove(label, slash + 1, strlen(slash));
                export_name(where, fn, sizeof fn);
                if (fn[0]) { strncat(label, "!", sizeof label - strlen(label) - 1); strncat(label, fn, sizeof label - strlen(label) - 1); }
            } else snprintf(label, sizeof label, "%s", loader_where((void *)where));
            if (blocked) { char tmp[200]; snprintf(tmp, sizeof tmp, "BLOCKED:%s", label); snprintf(label, sizeof label, "%s", tmp); }
            else if (strstr(label, "atio6axx") && mi.AllocationBase) {
                // driver code has few names: keep the exact spot, to look at later
                char *bang = strchr(label, '!');
                if (bang) *bang = 0;
                size_t have = strlen(label);
                snprintf(label + have, sizeof label - have, "@%llx", (unsigned long long)(where - (uint64_t)mi.AllocationBase));
            }
            fprintf(f, "H %s X %s %llx %u\n", e->is_main == 2 ? "S" : e->is_main == 3 ? "B" : e->is_main == 4 ? "s" : e->is_main == 5 ? "b" : e->is_main ? "M" : "O", label, (unsigned long long)e->caller, e->count);
        }
    }
    free(list);
    fclose(f);
    memset(g_table, 0, sizeof(Hit) << TABLE_BITS);
    g_used = 0;
}

static DWORD WINAPI profile_thread(void *unused) {
    DWORD me = GetCurrentThreadId();
    g_table = VirtualAlloc(NULL, sizeof(Hit) << TABLE_BITS, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_table) return 0;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 2 /* high resolution */, TIMER_ALL_ACCESS);
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    int number = 0;
    unsigned round = 0;
    for (;;) {
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -50000;
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(timer, INFINITE);
        } else Sleep(5);
        if (round++ % 200 == 0) refresh_threads(me);
        for (int i = 0; i < g_nth; i++) {
            Th *t = &g_th[i];
            if (SuspendThread(t->h) == (DWORD)-1) continue;
            CONTEXT c;
            c.ContextFlags = CONTEXT_CONTROL;
            uint64_t rip = 0, caller = 0;
            if (GetThreadContext(t->h, &c)) {
                rip = c.Rip;
                if (rip < GAME_LO || rip >= GAME_HI) {
                    MEMORY_BASIC_INFORMATION mi;
                    if (VirtualQuery((void *)c.Rsp, &mi, sizeof mi) && mi.State == MEM_COMMIT) {
                        uint64_t *sp = (uint64_t *)c.Rsp, *end = (uint64_t *)((uint8_t *)mi.BaseAddress + mi.RegionSize);
                        int depth = t->id == g_main_id ? 8192 : 1024;
                        for (int k = 0; k < depth && sp + k + 1 <= end; k++)   // + 1: a stack pointer that is not a multiple of 8 must not read across the end
                            if (looks_like_return(sp[k])) { caller = sp[k]; break; }
                    }
                }
            }
            ResumeThread(t->h);
            if (!rip) continue;
            t->samples++;
            // A thread sitting right after a syscall instruction is blocked in Windows, not using the CPU.
            MEMORY_BASIC_INFORMATION code;
            int blocked = 0;
            if (rip > 2 && (rip < GAME_LO || rip >= GAME_HI) && VirtualQuery((void *)(rip - 2), &code, sizeof code) && code.State == MEM_COMMIT && (code.Protect & 0xF0))
                blocked = ((uint8_t *)rip)[-2] == 0x0F && ((uint8_t *)rip)[-1] == 0x05;
            if (blocked) t->in_windows++;
            { extern volatile int g_view_kind;   // pass_stats.inc, with ATTILA_PASS_STATS=1: 1 = the shadow map view is drawing, 2 = the main picture
              count_hit(blocked ? (rip | (1ull << 63)) : (rip >= GAME_LO && rip < GAME_HI ? rip & ~0xFull : rip & ~0xFull), caller, t->id == g_main_id ? 1 + g_view_kind : g_view_kind ? 3 + g_view_kind : 0); }   // other threads: 4 / 5 while the main thread is in those views
        }
        QueryPerformanceCounter(&now);
        double secs = (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
        if (secs >= 30) {
            write_report(number++, secs);
            QueryPerformanceCounter(&start);
        }
    }
    return 0;
}

void profile_start(DWORD main_thread_id) {
    g_main_id = main_thread_id;
    CreateThread(NULL, 0, profile_thread, NULL, 0, NULL);
    log_line("profiler on");
}
