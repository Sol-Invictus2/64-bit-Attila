// Worker threads stay awake a moment before sleeping (settings.txt task_waits=game turns this off). Part of shim_gfx.c.
//
// Measured 2026-11-12 in the user's own big clash (installed game, profiler): the main thread spent about 4% of its time
// waking sleeping worker threads, and about 10% on the lock of the task queue.
// How the game does it (read from the code): every task thread sleeps on an SDL semaphore (CA::SemaphorePlatform::acquire_one
// -> SDL_SemWait) as soon as it has nothing to do. When the main thread hands out work, tbb::ThreadTaskManager::wake_up_threads
// posts the semaphore of every sleeping thread (SDL_SemPost), and each post that finds a sleeper is a call into Windows'
// kernel to wake it (NtAlertThreadByThreadIdEx in the profile), made by the main thread, one thread after the other.
// The game hands out work many times per frame, so the threads fall asleep and are woken again many times per frame.
//
// Our change (idea from JGdaCaesar's 32-bit performance fix, which found that waking worker threads costs the main thread;
// none of his code is used): a thread that is about to sleep first keeps trying the semaphore for a short time (50 µs by
// default; ATTILA_TASK_SPIN_US=n). If work comes in meanwhile, it takes it without having slept, and the post on the main
// thread finds nobody to wake, which costs no kernel call. Only after the short time does it sleep as before.
// The semaphore itself (SDL's) is untouched: the waits use SDL_SemTryWait, then SDL_SemWait, so the count stays exact.
//
// The same setting also turns on the task queue peek (task_wait.inc): an empty queue is seen without taking its lock.
// Not measured yet: the gain in frames per second.
static int g_spin_on = -1;
static long long g_spin_ticks;
static int (__cdecl *g_sem_wait)(void *), (__cdecl *g_sem_trywait)(void *);
static volatile long long g_spin_caught, g_spin_slept;

int task_spin_enabled(void) {
    if (g_spin_on < 0) {
        extern int settings_fast_task_waits(void);
        g_spin_on = settings_fast_task_waits() && !getenv("ATTILA_TASK_SPIN_OFF");
    }
    return g_spin_on;
}

SHIM(int, SDL_SemWait, (void *sem)) {
    if (!g_sem_wait) {
        g_sem_trywait = (void *)GetProcAddress(sdl_dll(), "SDL_SemTryWait");
        g_sem_wait = (void *)GetProcAddress(sdl_dll(), "SDL_SemWait");
        const char *e = getenv("ATTILA_TASK_SPIN_US");
        long us = e ? atol(e) : 50;
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        g_spin_ticks = us > 0 ? f.QuadPart * us / 1000000 : 0;
    }
    if (task_spin_enabled() && g_spin_ticks && g_sem_trywait) {
        LARGE_INTEGER t0, t;
        QueryPerformanceCounter(&t0);
        for (;;) {
            if (g_sem_trywait(sem) == 0) { g_spin_caught++; return 0; }
            for (int i = 0; i < 16; i++) __builtin_ia32_pause();
            QueryPerformanceCounter(&t);
            if (t.QuadPart - t0.QuadPart > g_spin_ticks) break;
        }
        g_spin_slept++;
    }
    return g_sem_wait(sem);
}

void task_spin_report(long frames) {
    if (g_spin_on != 1 || frames <= 0 || !(g_spin_caught + g_spin_slept)) return;
    log_line("task threads per frame: %.0f found work while staying awake, %.0f went to sleep", (double)g_spin_caught / frames, (double)g_spin_slept / frames);
    g_spin_caught = g_spin_slept = 0;
}
