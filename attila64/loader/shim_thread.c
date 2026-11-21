// Linux threads for the game, on Windows primitives. The Linux objects are plain memory the game owns and all-zero means
// "ready to use", so SRW locks and condition variables (also all-zero when ready) fit directly inside them.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "loader.h"

enum { LX_ETIMEDOUT = 110, LX_EBUSY = 16, LX_EDEADLK = 35 };

// pthread_mutex_t is 40 bytes on Linux; glibc keeps its kind at offset 16, and 1 = recursive.
typedef struct { SRWLOCK lock; DWORD owner; int count; int kind; int pad[3]; } MutexL;

SHIM(int, pthread_mutexattr_init, (int *a)) { *a = 0; return 0; }
SHIM(int, pthread_mutexattr_destroy, (int *a)) { return 0; }
SHIM(int, pthread_mutexattr_settype, (int *a, int t)) { *a = t; return 0; }
SHIM(int, pthread_mutex_init, (MutexL *m, const int *a)) {
    memset(m, 0, sizeof *m);
    m->kind = a ? *a : 0;
    return 0;
}
SHIM(int, pthread_mutex_destroy, (MutexL *m)) { return 0; }
// A taken lock is tried again for a short while before the thread is put to sleep (ATTILA_MUTEX_SPIN=n tries, 0 = sleep
// at once as before 2026-11-13). Why: the game's task queue has ONE lock that eight threads take for a few instructions
// at a time, many thousand times a frame. Windows' lock puts a waiting thread to sleep almost at once, and whoever
// frees the lock must then wake it with a call into Windows. Profile 2026-11-13 (DirectX 11, benchmark): the main thread
// spent 10% asleep in tbb::ThreadTaskManager::remove_task waiting for that lock and about 5% waking other threads.
// Holding on for a moment is cheaper than both, because the lock is nearly always free again within that moment.
static int g_mutex_spin = -1;
static void mutex_spin_setup(void) {
    const char *e = getenv("ATTILA_MUTEX_SPIN");
    g_mutex_spin = e && *e ? atoi(e) : 0;
    if (g_mutex_spin < 0) g_mutex_spin = 0;
}
// ---- test: ATTILA_MUTEX_STATS=1: numbers about ONE lock, the game's task queue lock (tbb::ThreadTaskManager +0x48;
// task_wait.inc points g_stat_mutex at it): how often the main thread and the task threads take it, how often and how
// long they have to wait for it, how long it is held, and which places in the game hold it longest. Logged with the fps
// lines. Times come from the processor's cycle counter.
void *volatile g_stat_mutex;
static DWORD g_ms_main;
static volatile LONG64 g_ms[8];   // 0 main locks, 1 main waited, 2 main wait cycles, 3 other locks, 4 other waited, 5 other wait cycles, 6 held cycles, 7 longest hold
static uint64_t g_ms_at; static void *g_ms_site;
static struct { void *site; LONG64 n, held, longest; } g_ms_sites[24];
static int stat_lock(MutexL *m, DWORD me, void *site) {
    uint64_t t0 = __builtin_ia32_rdtsc();
    int waited = !TryAcquireSRWLockExclusive(&m->lock);
    if (waited) AcquireSRWLockExclusive(&m->lock);
    uint64_t t1 = __builtin_ia32_rdtsc();
    int k = me == g_ms_main ? 0 : 3;
    InterlockedIncrement64(&g_ms[k]);
    if (waited) { InterlockedIncrement64(&g_ms[k + 1]); InterlockedAdd64(&g_ms[k + 2], (LONG64)(t1 - t0)); }
    g_ms_at = t1; g_ms_site = site;
    m->owner = me; m->count = 1;
    return 0;
}
static void stat_unlock(void) {   // still holding the lock
    LONG64 held = (LONG64)(__builtin_ia32_rdtsc() - g_ms_at);
    g_ms[6] += held;
    if (held > g_ms[7]) g_ms[7] = held;
    for (int i = 0; i < 24; i++) {
        if (g_ms_sites[i].site == g_ms_site || !g_ms_sites[i].site) {
            g_ms_sites[i].site = g_ms_site; g_ms_sites[i].n++; g_ms_sites[i].held += held;
            if (held > g_ms_sites[i].longest) g_ms_sites[i].longest = held;
            break;
        }
    }
}
void mutex_stats_report(long frames) {   // from the main thread, with the fps lines
    static uint64_t last_tsc; static LARGE_INTEGER last_qpc;
    if (!g_stat_mutex) return;
    g_ms_main = GetCurrentThreadId();
    LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
    uint64_t tsc = __builtin_ia32_rdtsc();
    double us_per_cycle = last_tsc && tsc > last_tsc ? (double)(q.QuadPart - last_qpc.QuadPart) * 1e6 / (double)f.QuadPart / (double)(tsc - last_tsc) : 0;
    last_tsc = tsc; last_qpc = q;
    LONG64 v[8];
    for (int i = 0; i < 8; i++) v[i] = InterlockedExchange64(&g_ms[i], 0);
    if (frames <= 0 || !us_per_cycle || !(v[0] + v[3])) return;
    double fr = (double)frames;
    log_line("task queue lock per frame: main thread took it %.0f times (%.0f waited, %.0f us waiting), task threads %.0f times (%.0f waited, %.0f us waiting); held %.0f us in all, longest hold %.1f us",
             v[0] / fr, v[1] / fr, v[2] * us_per_cycle / fr, v[3] / fr, v[4] / fr, v[5] * us_per_cycle / fr, v[6] * us_per_cycle / fr, v[7] * us_per_cycle);
    for (int round = 0; round < 5; round++) {   // the five places that held it longest in all
        int best = -1;
        for (int i = 0; i < 24; i++) if (g_ms_sites[i].site && g_ms_sites[i].held > 0 && (best < 0 || g_ms_sites[i].held > g_ms_sites[best].held)) best = i;
        if (best < 0) break;
        log_line("   held from game code at %p: %.0f times a frame, %.2f us each on average, longest %.1f us", g_ms_sites[best].site, g_ms_sites[best].n / fr,
                 g_ms_sites[best].held * us_per_cycle / (double)(g_ms_sites[best].n ? g_ms_sites[best].n : 1), g_ms_sites[best].longest * us_per_cycle);
        g_ms_sites[best].held = 0;
    }
    memset(g_ms_sites, 0, sizeof g_ms_sites);
}
SHIM(int, pthread_mutex_lock, (MutexL *m)) {
    DWORD me = GetCurrentThreadId();
    if (m->kind == 1 && m->owner == me) { m->count++; return 0; }
    if ((void *)m == g_stat_mutex) return stat_lock(m, me, __builtin_return_address(0));
    if (!TryAcquireSRWLockExclusive(&m->lock)) {
        if (g_mutex_spin < 0) mutex_spin_setup();
        int got = 0;
        for (int i = 0; i < g_mutex_spin && !got; i++) {
            __builtin_ia32_pause();
            if (!((uintptr_t)*(void *volatile *)&m->lock & 1)) got = TryAcquireSRWLockExclusive(&m->lock);   // bit 0 = taken
        }
        if (!got) AcquireSRWLockExclusive(&m->lock);
    }
    m->owner = me;
    m->count = 1;
    return 0;
}
SHIM(int, pthread_mutex_trylock, (MutexL *m)) {
    DWORD me = GetCurrentThreadId();
    if (m->kind == 1 && m->owner == me) { m->count++; return 0; }
    if (!TryAcquireSRWLockExclusive(&m->lock)) return LX_EBUSY;
    m->owner = me;
    m->count = 1;
    return 0;
}
SHIM(int, pthread_mutex_unlock, (MutexL *m)) {
    if (--m->count > 0) return 0;
    if ((void *)m == g_stat_mutex) stat_unlock();
    m->owner = 0;
    ReleaseSRWLockExclusive(&m->lock);
    return 0;
}

SHIM(int, pthread_condattr_init, (int *a)) { *a = 0; return 0; }
SHIM(int, pthread_condattr_destroy, (int *a)) { return 0; }
SHIM(int, pthread_cond_init, (CONDITION_VARIABLE *c, const int *a)) { InitializeConditionVariable(c); return 0; }
SHIM(int, pthread_cond_destroy, (CONDITION_VARIABLE *c)) { return 0; }
SHIM(int, pthread_cond_signal, (CONDITION_VARIABLE *c)) { WakeConditionVariable(c); return 0; }
SHIM(int, pthread_cond_broadcast, (CONDITION_VARIABLE *c)) { WakeAllConditionVariable(c); return 0; }

static int cond_sleep(CONDITION_VARIABLE *c, MutexL *m, DWORD ms) {
    int cnt = m->count;
    m->owner = 0;
    BOOL ok = SleepConditionVariableSRW(c, &m->lock, ms, 0);
    m->owner = GetCurrentThreadId();
    m->count = cnt;
    return ok ? 0 : LX_ETIMEDOUT;
}
extern volatile long g_tt_on, g_tt_cond[5], g_tt_condinf, g_tt_sem[5];
static int tt_bucket(DWORD ms) { return ms == 0 ? 0 : ms < 2 ? 1 : ms < 20 ? 2 : ms < 200 ? 3 : 4; }
SHIM(int, pthread_cond_wait, (CONDITION_VARIABLE *c, MutexL *m)) { if (g_tt_on) InterlockedIncrement(&g_tt_condinf); return cond_sleep(c, m, INFINITE); }

typedef struct { int64_t sec, nsec; } TimespecL;

static int64_t now_ms(void) {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    int64_t t = ((int64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000LL;
    return t / 10000;
}
static DWORD ms_until(const TimespecL *t) {
    int64_t d = t->sec * 1000 + t->nsec / 1000000 - now_ms();
    return d <= 0 ? 0 : (d > 0x7fffffff ? 0x7fffffff : (DWORD)d);
}
SHIM(int, pthread_cond_timedwait, (CONDITION_VARIABLE *c, MutexL *m, const TimespecL *t)) { DWORD ms = ms_until(t); if (g_tt_on) InterlockedIncrement(&g_tt_cond[tt_bucket(ms)]); return cond_sleep(c, m, ms); }

// threads
typedef SYSV void *(*thread_start)(void *);
typedef struct Thread { HANDLE handle; thread_start fn; void *arg; void *result; DWORD id; int detached; } Thread;

#define MAX_KEYS 256
typedef SYSV void (*key_dtor)(void *);
static key_dtor g_key_dtors[MAX_KEYS];
static DWORD g_key_slots[MAX_KEYS];
static volatile LONG g_key_next;

static DWORD g_self_slot = TLS_OUT_OF_INDEXES;
__attribute__((constructor)) static void thread_setup(void) { g_self_slot = TlsAlloc(); }

static void run_key_dtors(void) {
    for (int round = 0; round < 4; round++)
        for (LONG i = 0; i < g_key_next; i++) {
            void *v = TlsGetValue(g_key_slots[i]);
            if (v && g_key_dtors[i]) {
                TlsSetValue(g_key_slots[i], NULL);
                g_key_dtors[i](v);
            }
        }
}

static DWORD WINAPI thread_entry(void *p) {
    Thread *t = p;
    TlsSetValue(g_self_slot, t);
    t->result = t->fn(t->arg);
    run_key_dtors();
    return 0;
}

static Thread *current_thread(void) {
    Thread *t = TlsGetValue(g_self_slot);
    if (!t) {
        t = calloc(1, sizeof *t);
        t->id = GetCurrentThreadId();
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t->handle, 0, FALSE, DUPLICATE_SAME_ACCESS);
        TlsSetValue(g_self_slot, t);
    }
    return t;
}

// pthread_attr_t is 56 bytes; only the stack size and detach state matter here.
typedef struct { size_t stack; int detached; int pad[9]; } AttrL;
SHIM(int, pthread_attr_init, (AttrL *a)) { memset(a, 0, sizeof *a); return 0; }
SHIM(int, pthread_attr_destroy, (AttrL *a)) { return 0; }
SHIM(int, pthread_attr_setstacksize, (AttrL *a, size_t n)) { a->stack = n; return 0; }
SHIM(int, pthread_attr_setdetachstate, (AttrL *a, int d)) { a->detached = d; return 0; }
SHIM(int, pthread_attr_setschedpolicy, (AttrL *a, int p)) { return 0; }
SHIM(int, pthread_attr_setschedparam, (AttrL *a, const int *p)) { return 0; }
SHIM(int, pthread_attr_getschedparam, (const AttrL *a, int *p)) { *p = 0; return 0; }
SHIM(int, sched_get_priority_max, (int p)) { return 0; }
SHIM(int, sched_get_priority_min, (int p)) { return 0; }
SHIM(int, sched_yield, (void)) { SwitchToThread(); return 0; }

SHIM(int, pthread_create, (Thread **out, const AttrL *a, thread_start fn, void *arg)) {
    Thread *t = calloc(1, sizeof *t);
    t->fn = fn;
    t->arg = arg;
    t->detached = a ? a->detached : 0;
    // stacks are fully committed: Linux code does not touch guard pages in order
    size_t size = a && a->stack ? a->stack : (size_t)8 << 20;
    if (size < ((size_t)1 << 20)) size = (size_t)1 << 20;
    t->handle = CreateThread(NULL, size, thread_entry, t, 0, &t->id);
    if (!t->handle) { free(t); return 11; }
    *out = t;
    return 0;
}
SHIM(int, pthread_join, (Thread *t, void **ret)) {
    WaitForSingleObject(t->handle, INFINITE);
    if (ret) *ret = t->result;
    CloseHandle(t->handle);
    free(t);
    return 0;
}
SHIM(void, pthread_exit, (void *v)) {
    Thread *t = current_thread();
    t->result = v;
    run_key_dtors();
    ExitThread(0);
}
SHIM(int, pthread_cancel, (Thread *t)) {
    log_line("pthread_cancel asked for thread %lu; ignored", t ? t->id : 0);
    return 0;
}
SHIM(Thread *, pthread_self, (void)) { return current_thread(); }
SHIM(int, pthread_detach, (Thread *t)) { return 0; }

static int key_create(unsigned *k, key_dtor d) {
    LONG i = InterlockedIncrement(&g_key_next) - 1;
    if (i >= MAX_KEYS) fatal("out of thread keys");
    g_key_slots[i] = TlsAlloc();
    g_key_dtors[i] = d;
    *k = (unsigned)i;
    return 0;
}
SHIM(int, pthread_key_create, (unsigned *k, key_dtor d)) { return key_create(k, d); }
SHIM(int, __pthread_key_create, (unsigned *k, key_dtor d)) { return key_create(k, d); }
SHIM(int, pthread_key_delete, (unsigned k)) { g_key_dtors[k] = NULL; return 0; }
SHIM(void *, pthread_getspecific, (unsigned k)) { return TlsGetValue(g_key_slots[k]); }
SHIM(int, pthread_setspecific, (unsigned k, const void *v)) { TlsSetValue(g_key_slots[k], (void *)v); return 0; }

typedef SYSV void (*once_fn)(void);
SHIM(int, pthread_once, (volatile LONG *o, once_fn f)) {
    for (;;) {
        LONG s = InterlockedCompareExchange(o, 1, 0);
        if (s == 0) { f(); InterlockedExchange(o, 2); return 0; }
        if (s == 2) return 0;
        SwitchToThread();
    }
}

// sem_t is 32 bytes on Linux; a Windows semaphore handle lives in the first 8.
SHIM(int, sem_init, (HANDLE *s, int shared, unsigned v)) {
    *s = CreateSemaphoreA(NULL, (LONG)v, 0x7fffffff, NULL);
    return *s ? 0 : -1;
}
SHIM(int, sem_destroy, (HANDLE *s)) { CloseHandle(*s); return 0; }
SHIM(int, sem_post, (HANDLE *s)) { ReleaseSemaphore(*s, 1, NULL); return 0; }
SHIM(int, sem_wait, (HANDLE *s)) { WaitForSingleObject(*s, INFINITE); return 0; }
SHIM(int, sem_timedwait, (HANDLE *s, const TimespecL *t)) {
    DWORD ms = ms_until(t);
    if (g_tt_on) InterlockedIncrement(&g_tt_sem[tt_bucket(ms)]);
    if (WaitForSingleObject(*s, ms) == WAIT_OBJECT_0) return 0;
    errno = 110;
    return -1;
}

// libstdc++ waits on one-time initialisation guards through the Linux futex system call.
enum { SYS_futex = 202, FUTEX_WAIT = 0, FUTEX_WAKE = 1, FUTEX_MASK = 127 };
SHIM(long long, syscall, (long long nr, volatile int *addr, long long op, long long val)) {
    if (nr == SYS_futex) {
        static BOOL (WINAPI *wait_addr)(volatile void *, void *, SIZE_T, DWORD);
        static void (WINAPI *wake_all)(void *);
        if (!wait_addr) {
            HMODULE k = LoadLibraryA("kernelbase.dll");
            wake_all = (void *)GetProcAddress(k, "WakeByAddressAll");
            wait_addr = (void *)GetProcAddress(k, "WaitOnAddress");
        }
        if ((op & FUTEX_MASK) == FUTEX_WAIT) {
            int expect = (int)val;
            wait_addr(addr, &expect, sizeof expect, INFINITE);
            return 0;
        }
        if ((op & FUTEX_MASK) == FUTEX_WAKE) { wake_all((void *)addr); return 0; }
    }
    log_line("system call %lld not handled", nr);
    errno = ENOSYS;
    return -1;
}

// _setjmp and longjmp keep the System V saved registers.
__attribute__((naked, sysv_abi)) int sh__setjmp(void *b) {
    __asm__("mov %rbx, 0(%rdi)\n mov %rbp, 8(%rdi)\n mov %r12, 16(%rdi)\n mov %r13, 24(%rdi)\n"
            "mov %r14, 32(%rdi)\n mov %r15, 40(%rdi)\n"
            "lea 8(%rsp), %rax\n mov %rax, 48(%rdi)\n"
            "mov (%rsp), %rax\n mov %rax, 56(%rdi)\n"
            "xor %eax, %eax\n ret\n");
}
SHIM_REG(_setjmp, sh__setjmp);
__attribute__((naked, sysv_abi)) void sh_longjmp(void *b, int v) {
    __asm__("mov %esi, %eax\n test %eax, %eax\n jnz 1f\n mov $1, %eax\n1:\n"
            "mov 0(%rdi), %rbx\n mov 8(%rdi), %rbp\n mov 16(%rdi), %r12\n mov 24(%rdi), %r13\n"
            "mov 32(%rdi), %r14\n mov 40(%rdi), %r15\n mov 48(%rdi), %rsp\n"
            "jmp *56(%rdi)\n");
}
SHIM_REG(longjmp, sh_longjmp);
