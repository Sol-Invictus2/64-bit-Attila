// Measuring only (ATTILA_TASK_WAITS=1): which tasks the main thread waits for, from where in the game, and how long; and
// how long the draw-list preparation tasks run. Nothing here is hooked without the switch. Part of loader.c (included
// after sort_fast.inc).
//
// tbb::childthreadtask::wait_for_all (30620f0) is how the game waits for a task it started: it first tries to take the
// task back from the queue and run it itself (task_scheduler_init::remove_and_execute_task_during_wait 3061f30, which
// calls ThreadTaskManager::remove_task), and when a task thread already has it, sleeps on the task's semaphore (+0x30,
// CA::SemaphorePlatform::acquire_one 4e50b0) until the task is done. acquire_one is reached by a tail jump, so the
// stack then still holds remove_task's old return address: that is the profile's "asleep in remove_task".
// With the switch, wait_for_all is replaced by the same two steps with a clock around them (main thread only), and the
// two prepare tasks' bodies (PREPARE_TBB_INSTANCE_LIST_TASK<WEIGHTED_INSTANCE / RIGID_INSTANCE_V2>::PREPARE_TASK::execute
// 354fcb0 / 354fad0) are timed. One block of lines with every fps line.
typedef uint8_t (SYSV *TwRemoveExecFn)(void *task);
typedef void (SYSV *TwSemFn)(void *sem);
typedef uint64_t (SYSV *TwExecFn)(void *task);
typedef struct { const void *vt; uintptr_t from; uint64_t waits, wait_ticks, runs, run_ticks, longest; } TaskWaitStat;
// Where the main thread spins asking for tasks (task_wait.inc's get_task_peek tells us): the time between two empty asks
// from the same place is counted for that place.
typedef struct { uintptr_t from; uint64_t ticks, polls; } SpinStat;
static SpinStat g_tw_spin[24];
static uint64_t g_tw_spin_last_t;
static uintptr_t g_tw_spin_last_from;
static void tw_note_poll(uintptr_t from, int empty) {
    uint64_t now = __builtin_ia32_rdtsc();
    if (g_tw_spin_last_from == from && now - g_tw_spin_last_t < 50000) {
        for (int i = 0; i < 24; i++) {
            if (g_tw_spin[i].from == from || !g_tw_spin[i].from) { g_tw_spin[i].from = from; g_tw_spin[i].ticks += now - g_tw_spin_last_t; g_tw_spin[i].polls++; break; }
        }
    }
    g_tw_spin_last_from = empty ? from : 0;
    g_tw_spin_last_t = now;
}
// The per-soldier display work: EMPIREBATTLE::ENTITY_DISPLAY::draw's parallel_for (a5a580; range, body, x) and its body
// (TBB_DRAW_TRACKER::operator() a5a9b0; body, range), run for each piece on whichever thread takes it.
typedef unsigned (SYSV *PfDrawFn)(const uint32_t *range, const void *body, unsigned x);
typedef void (SYSV *PfBodyFn)(const void *body, uint32_t *range);
static PfDrawFn g_tw_pf_orig;
static PfBodyFn g_tw_body_orig;
static volatile LONG64 g_tw_pf_calls, g_tw_pf_items, g_tw_pf_wall, g_tw_pf_longest, g_tw_body_ticks, g_tw_body_main, g_tw_body_pieces, g_tw_body_longest;
static volatile LONG64 g_tw_pf_site_wall[2], g_tw_pf_site_items[2], g_tw_pf_site_calls[2];   // by caller: a5a297 (first list, +e0) / a5a3eb (second, +f0)
static volatile int g_tw_pf_site_now = -1;   // the list whose parallel_for the main thread is in (the two never overlap)
static volatile LONG64 g_tw_site_work[2], g_tw_site_pieces[2], g_tw_site_main_work[2], g_tw_site_main_pieces[2];
static volatile DWORD g_tw_body_tid[32];
static volatile LONG64 g_tw_body_tid_ticks[32];
static SYSV unsigned tw_pf_draw(const uint32_t *range, const void *body, unsigned x) {
    if (!g_tw_main || GetCurrentThreadId() != g_tw_main) return g_tw_pf_orig(range, body, x);
    uint64_t t0 = __builtin_ia32_rdtsc();
    unsigned items = range[1] - range[0];
    {
        uintptr_t from0 = (uintptr_t)__builtin_return_address(0);
        g_tw_pf_site_now = from0 == 0xa5a297 ? 0 : from0 == 0xa5a3eb ? 1 : -1;
    }
    unsigned r = g_tw_pf_orig(range, body, x);
    LONG64 dt = (LONG64)(__builtin_ia32_rdtsc() - t0);
    g_tw_pf_wall += dt; g_tw_pf_calls++; g_tw_pf_items += items;
    {
        uintptr_t from = (uintptr_t)__builtin_return_address(0);
        int k = from == 0xa5a297 ? 0 : from == 0xa5a3eb ? 1 : -1;
        if (k >= 0) { g_tw_pf_site_wall[k] += dt; g_tw_pf_site_items[k] += items; g_tw_pf_site_calls[k]++; }
    }
    g_tw_pf_site_now = -1;
    if (dt > g_tw_pf_longest) g_tw_pf_longest = dt;
    return r;
}
static SYSV void tw_body(const void *body, uint32_t *range) {
    uint64_t t0 = __builtin_ia32_rdtsc();
    g_tw_body_orig(body, range);
    LONG64 dt = (LONG64)(__builtin_ia32_rdtsc() - t0);
    DWORD me = GetCurrentThreadId();
    {
        int site = g_tw_pf_site_now;
        if (site >= 0) {
            InterlockedAdd64(&g_tw_site_work[site], dt); InterlockedIncrement64(&g_tw_site_pieces[site]);
            if (me == g_tw_main) { InterlockedAdd64(&g_tw_site_main_work[site], dt); InterlockedIncrement64(&g_tw_site_main_pieces[site]); }
        }
    }
    InterlockedAdd64(&g_tw_body_ticks, dt);
    InterlockedIncrement64(&g_tw_body_pieces);
    if (me == g_tw_main) InterlockedAdd64(&g_tw_body_main, dt);
    if (dt > g_tw_body_longest) g_tw_body_longest = dt;
    for (int i = 0; i < 32; i++) {
        DWORD v = g_tw_body_tid[i];
        if (v == me) { InterlockedAdd64(&g_tw_body_tid_ticks[i], dt); break; }
        if (!v && InterlockedCompareExchange((volatile LONG *)&g_tw_body_tid[i], (LONG)me, 0) == 0) { InterlockedAdd64(&g_tw_body_tid_ticks[i], dt); break; }
        if (g_tw_body_tid[i] == me) { InterlockedAdd64(&g_tw_body_tid_ticks[i], dt); break; }
    }
}
static TaskWaitStat g_tw[96];
static int g_tw_n;
static TwExecFn g_tw_prep_orig[2];
static volatile LONG64 g_tw_prep_ticks[2], g_tw_prep_runs[2], g_tw_prep_on_main[2], g_tw_prep_longest[2];

static SYSV void tw_wait_for_all(uint8_t *task) {
    TwRemoveExecFn remove_exec = (TwRemoveExecFn)(uintptr_t)0x3061f30;
    TwSemFn acquire = (TwSemFn)(uintptr_t)0x4e50b0;
    if (!g_tw_main || GetCurrentThreadId() != g_tw_main) {
        if (!remove_exec(task)) acquire(task + 0x30);
        return;
    }
    const void *vt = *(const void *const *)task;    // read first: a task run here may be gone afterwards
    uintptr_t from = (uintptr_t)__builtin_return_address(0);
    uint64_t t0 = __builtin_ia32_rdtsc();
    int ran = remove_exec(task);
    if (!ran) acquire(task + 0x30);
    uint64_t dt = __builtin_ia32_rdtsc() - t0;
    TaskWaitStat *s = NULL;
    for (int i = 0; i < g_tw_n; i++) if (g_tw[i].vt == vt && g_tw[i].from == from) { s = &g_tw[i]; break; }
    if (!s && g_tw_n < 96) { s = &g_tw[g_tw_n++]; memset(s, 0, sizeof *s); s->vt = vt; s->from = from; }
    if (!s) return;
    if (ran) { s->runs++; s->run_ticks += dt; } else { s->waits++; s->wait_ticks += dt; }
    if (dt > s->longest) s->longest = dt;
}
static void tw_prep_note(int k, uint64_t dt) {
    InterlockedAdd64(&g_tw_prep_ticks[k], (LONG64)dt);
    InterlockedIncrement64(&g_tw_prep_runs[k]);
    if (GetCurrentThreadId() == g_tw_main) InterlockedIncrement64(&g_tw_prep_on_main[k]);
    if ((LONG64)dt > g_tw_prep_longest[k]) g_tw_prep_longest[k] = (LONG64)dt;
}
static SYSV uint64_t tw_prep_w(void *task) { uint64_t t0 = __builtin_ia32_rdtsc(), r = g_tw_prep_orig[0](task); tw_prep_note(0, __builtin_ia32_rdtsc() - t0); return r; }
static SYSV uint64_t tw_prep_r(void *task) { uint64_t t0 = __builtin_ia32_rdtsc(), r = g_tw_prep_orig[1](task); tw_prep_note(1, __builtin_ia32_rdtsc() - t0); return r; }

static const char *tw_type_name(const void *vt) {
    const uint8_t *ti = vt ? ((const uint8_t *const *)vt)[-1] : NULL;
    if ((uintptr_t)ti < 0x400000 || (uintptr_t)ti >= 0x5280000) return "?";
    const char *n = *(const char *const *)(ti + 8);
    return ((uintptr_t)n >= 0x400000 && (uintptr_t)n < 0x5280000) ? n : "?";
}

// With each fps line (shim_gfx.c, the main thread).
void task_waits_report(long frames) {
    if (!g_tw_on) return;
    static LARGE_INTEGER f, last_q;
    static uint64_t last_t;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    uint64_t t = __builtin_ia32_rdtsc();
    if (!g_tw_main) {   // the first report only learns the main thread and starts the clock
        g_tw_main = GetCurrentThreadId();
        QueryPerformanceFrequency(&f); last_q = q; last_t = t;
        return;
    }
    double mpt = t > last_t ? (double)(q.QuadPart - last_q.QuadPart) * 1000.0 / (double)f.QuadPart / (double)(t - last_t) : 0;
    last_q = q; last_t = t;
    if (frames < 1) frames = 1;
    double all = 0;
    for (int i = 0; i < g_tw_n; i++) all += (double)(g_tw[i].wait_ticks + g_tw[i].run_ticks);
    log_line("task waits on the main thread: %.3f ms a frame in all (waiting for a task thread, or running the task itself)", all * mpt / (double)frames);
    for (int shown = 0; shown < 10; shown++) {
        int best = -1;
        for (int i = 0; i < g_tw_n; i++)
            if (g_tw[i].waits + g_tw[i].runs && (best < 0 || g_tw[i].wait_ticks + g_tw[i].run_ticks > g_tw[best].wait_ticks + g_tw[best].run_ticks)) best = i;
        if (best < 0) break;
        TaskWaitStat *s = &g_tw[best];
        log_line("   %6.3f ms waited (%4.1f a frame) + %6.3f ms run itself (%4.1f a frame), longest %.2f ms, from %llx: %.110s",
                 (double)s->wait_ticks * mpt / (double)frames, (double)s->waits / (double)frames, (double)s->run_ticks * mpt / (double)frames,
                 (double)s->runs / (double)frames, (double)s->longest * mpt, (unsigned long long)s->from, tw_type_name(s->vt));
        s->waits = s->runs = 0;   // shown: out of the next search
    }
    for (int i = 0; i < g_tw_n; i++) { g_tw[i].waits = g_tw[i].runs = g_tw[i].wait_ticks = g_tw[i].run_ticks = g_tw[i].longest = 0; }
    {   // the main thread spinning for tasks, by place
        int shown = 0;
        for (int round = 0; round < 5; round++) {
            int best = -1;
            for (int i = 0; i < 24; i++) if (g_tw_spin[i].from && g_tw_spin[i].polls && (best < 0 || g_tw_spin[i].ticks > g_tw_spin[best].ticks)) best = i;
            if (best < 0) break;
            if (!shown++) log_line("   main thread spinning for tasks (asking an empty queue), by place:");
            log_line("   %6.3f ms a frame spinning, from %llx (%.0f asks a frame)", (double)g_tw_spin[best].ticks * mpt / (double)frames, (unsigned long long)g_tw_spin[best].from, (double)g_tw_spin[best].polls / (double)frames);
            g_tw_spin[best].polls = 0;
        }
        for (int i = 0; i < 24; i++) g_tw_spin[i].ticks = g_tw_spin[i].polls = 0;
    }
    if (g_tw_pf_orig) {   // the per-soldier display work
        LONG64 calls = g_tw_pf_calls, items = g_tw_pf_items, wall = g_tw_pf_wall, pl = g_tw_pf_longest;
        g_tw_pf_calls = g_tw_pf_items = g_tw_pf_wall = g_tw_pf_longest = 0;
        LONG64 work = InterlockedExchange64(&g_tw_body_ticks, 0), main_work = InterlockedExchange64(&g_tw_body_main, 0);
        LONG64 pieces = InterlockedExchange64(&g_tw_body_pieces, 0), bl = InterlockedExchange64(&g_tw_body_longest, 0);
        int threads = 0;
        char per[400]; int at = 0;
        per[0] = 0;
        for (int i = 0; i < 32; i++) {
            LONG64 tk = InterlockedExchange64(&g_tw_body_tid_ticks[i], 0);
            if (!tk) continue;
            threads++;
            if (at < 380) at += snprintf(per + at, sizeof per - (size_t)at, "%s%.2f", at ? " " : "", (double)tk * mpt / (double)frames);
        }
        for (int k = 0; k < 2; k++) {
            LONG64 c = g_tw_pf_site_calls[k], it = g_tw_pf_site_items[k], w = g_tw_pf_site_wall[k];
            g_tw_pf_site_calls[k] = g_tw_pf_site_items[k] = g_tw_pf_site_wall[k] = 0;
            LONG64 sw = InterlockedExchange64(&g_tw_site_work[k], 0), sp = InterlockedExchange64(&g_tw_site_pieces[k], 0);
            LONG64 smw = InterlockedExchange64(&g_tw_site_main_work[k], 0), smp = InterlockedExchange64(&g_tw_site_main_pieces[k], 0);
            if (c) log_line("   per-soldier display, %s list: %.1f calls a frame, %.0f soldiers each, main thread inside it %.3f ms a frame; pieces %.0f a frame (%.0f by the main thread), their work %.3f ms a frame (%.3f by the main thread)",
                            k ? "second (+f0)" : "first (+e0)", (double)c / (double)frames, (double)it / (double)c, (double)w * mpt / (double)frames,
                            (double)sp / (double)frames, (double)smp / (double)frames, (double)sw * mpt / (double)frames, (double)smw * mpt / (double)frames);
        }
        if (calls)
            log_line("   per-soldier display (ENTITY_DISPLAY::draw parallel_for): %.1f calls a frame, %.0f soldiers each; main thread inside it %.3f ms a frame (longest call %.3f ms); the pieces' work %.3f ms a frame on all threads, %.3f of it on the main thread; %.0f pieces a frame, longest %.3f ms; %d threads took part (ms a frame each: %s)",
                     (double)calls / (double)frames, (double)items / (double)calls, (double)wall * mpt / (double)frames, (double)pl * mpt,
                     (double)work * mpt / (double)frames, (double)main_work * mpt / (double)frames, (double)pieces / (double)frames, (double)bl * mpt, threads, per);
    }
    static const char *const kind[2] = {"soldiers (WEIGHTED_INSTANCE)", "rigid parts (RIGID_INSTANCE_V2)"};
    for (int k = 0; k < 2; k++) {
        LONG64 ticks = InterlockedExchange64(&g_tw_prep_ticks[k], 0), runs = InterlockedExchange64(&g_tw_prep_runs[k], 0);
        LONG64 on_main = InterlockedExchange64(&g_tw_prep_on_main[k], 0), longest = InterlockedExchange64(&g_tw_prep_longest[k], 0);
        if (runs) log_line("   prepare task, %s: %.1f runs a frame (%lld on the main thread), %.3f ms each on average, longest %.3f ms",
                           kind[k], (double)runs / (double)frames, (long long)on_main, (double)ticks * mpt / (double)runs, (double)longest * mpt);
    }
}

static void patch_task_waits(void) {
    if (!getenv("ATTILA_TASK_WAITS")) return;
    static const uint8_t wait_old[16] = {0x53, 0x48, 0x89, 0xFB, 0xE8, 0x37, 0xFE, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x02, 0x5B, 0xC3, 0x48};
    static const uint8_t wait_tail[5] = {0xE9, 0xA4, 0x2F, 0x48, 0xFD};      // 3062107: jmp 4e50b0 (acquire_one)
    static const uint8_t remove_exec_old[7] = {0x41, 0x56, 0x53, 0x50, 0x49, 0x89, 0xFE};   // 3061f30: push r14; push rbx; push rax; mov r14,rdi
    static const uint8_t prep_old[5] = {0x50, 0x48, 0x8B, 0x4F, 0x38};        // push rax; mov rcx,[rdi+38h]
    if (memcmp((void *)0x30620f0, wait_old, 16) || memcmp((void *)0x3062107, wait_tail, 5) || memcmp((void *)0x3061f30, remove_exec_old, 7)) {
        log_line("patch skipped, game code differs: task wait measuring"); return;
    }
    uint8_t *code = near_game_code(16), *f = (uint8_t *)0x30620f0;
    if (!code) { log_line("patch skipped, no memory near the game: task wait measuring"); return; }
    uint64_t target = (uint64_t)(void *)tw_wait_for_all;
    code[0] = 0x48; code[1] = 0xB8; memcpy(code + 2, &target, 8); code[10] = 0xFF; code[11] = 0xE0;   // mov rax, fn; jmp rax
    uint8_t jump[5] = {0xE9};
    int32_t rel = (int32_t)(code - (f + 5));
    memcpy(jump + 1, &rel, 4);
    memcpy(f, jump, 5);
    g_tw_prep_orig[0] = (TwExecFn)hook_entry(0x354fcb0, prep_old, 5, (void *)tw_prep_w);
    g_tw_prep_orig[1] = (TwExecFn)hook_entry(0x354fad0, prep_old, 5, (void *)tw_prep_r);
    static const uint8_t pf_old[10] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53};   // a5a580 and a5a9b0 begin alike
    if (!memcmp((void *)0xa5a580, pf_old, 10) && !memcmp((void *)0xa5a9b0, pf_old, 10)) {
        g_tw_pf_orig = (PfDrawFn)hook_entry(0xa5a580, pf_old, 5, (void *)tw_pf_draw);
        g_tw_body_orig = (PfBodyFn)hook_entry(0xa5a9b0, pf_old, 5, (void *)tw_body);
        if (!g_tw_body_orig) g_tw_pf_orig = NULL;
    }
    g_tw_on = 1;
    log_line("measuring: the game's settings enable_tbb_draw_tracker %u, tbb_draw_tracker_grain %u, tracker_balancing_sort_block_size %u",
             (unsigned)*(const uint8_t *)(uintptr_t)0x4e8854a, *(const uint32_t *)(uintptr_t)0x4e884e4, *(const uint32_t *)(uintptr_t)0x4e886ec);
    log_line("measuring: task waits of the main thread%s%s", g_tw_prep_orig[0] && g_tw_prep_orig[1] ? ", prepare tasks timed" : " (prepare tasks NOT timed)",
             g_tw_pf_orig ? ", per-soldier display timed" : " (per-soldier display NOT timed)");
}
