// Waiting for worker tasks without hammering the task queue's lock. Part of loader.c.
//
// When the game splits work over its task threads (parallel_for, start_task), the thread that handed the work out waits
// like this:  while (tasks of my group are unfinished) { t = get_task(); if (t) run it; }
// tbb::ThreadTaskManager::get_task (6e88f0) locks the queue (mutex at +0x48), looks at the first entry (+0x18), unlocks.
// Once the queue is empty and the workers are busy with the last tasks, that loop only locks and unlocks, as fast as it
// can: about 11% of the main thread's samples in a battle, on the very lock the workers need to finish and to fetch.
//
// Here get_task first looks at the first entry without the lock. Empty: one short processor pause, answer "nothing", as
// the game's own code would have after locking. Not empty: the game's function as it was. A task added in the same
// instant is simply found on the caller's next round (every caller loops or sleeps on a counted semaphore).
// OFF unless ATTILA_TASK_PEEK=1: measured 2026-11-11 in the game's benchmark, two runs each: 131.6 / 129.0 fps without,
// 132.0 / 135.5 with. That is inside the run-to-run spread, so it is not given to players.
typedef void *(SYSV *GetTaskFn)(uint8_t *manager, int only_marked);
static GetTaskFn g_get_task_orig;
static int g_tw_on;                    // task_waits.inc (measuring): where the main thread spins for tasks
static DWORD g_tw_main;
static void tw_note_poll(uintptr_t from, int empty);

static int g_mutex_stats_on = -1;
static SYSV void *get_task_peek(uint8_t *manager, int only_marked) {
    if (g_mutex_stats_on) {   // test (ATTILA_MUTEX_STATS=1): tell shim_thread.c which lock is the task queue's
        extern void *volatile g_stat_mutex;
        if (g_mutex_stats_on < 0) g_mutex_stats_on = getenv("ATTILA_MUTEX_STATS") != NULL;
        if (g_mutex_stats_on && !g_stat_mutex) g_stat_mutex = manager + 0x48;
    }
    if (g_tw_on && GetCurrentThreadId() == g_tw_main) tw_note_poll((uintptr_t)__builtin_return_address(0), !*(void *volatile *)(manager + 0x18));
    if (!*(void *volatile *)(manager + 0x18)) { __builtin_ia32_pause(); return NULL; }
    return g_get_task_orig(manager, only_marked);
}

static void patch_task_wait(void) {
    extern int task_spin_enabled(void);
    if (!getenv("ATTILA_TASK_PEEK") && !task_spin_enabled()) return;
    static const uint8_t old[5] = {0x55, 0x41, 0x57, 0x41, 0x56};   // push rbp; push r15; push r14
    g_get_task_orig = (GetTaskFn)hook_entry(0x6e88f0, old, 5, (void *)get_task_peek);
    log_line(g_get_task_orig ? "patch applied: task queue looked at before locking" : "patch skipped, game code differs: task queue peek");
}

// ---- How many task threads the game starts. tbb::ThreadTaskManager::init_cpu_config (3062320) works that out from the
// processor's cores (CA::get_current_cpu_config) and the constructor starts that many NativeTaskThreads. The number is
// logged; ATTILA_TASK_THREADS=n (test) makes it smaller, never larger than what the game chose (its per-core tables are
// that long).
typedef uint32_t (SYSV *CpuConfigFn)(uint8_t *manager, uint32_t asked);
static CpuConfigFn g_cpu_config_orig;
static SYSV uint32_t task_thread_count(uint8_t *manager, uint32_t asked) {
    uint32_t chosen = g_cpu_config_orig(manager, asked), use = chosen;
    const char *e = getenv("ATTILA_TASK_THREADS");
    if (e && atoi(e) >= 1 && (uint32_t)atoi(e) < chosen) use = (uint32_t)atoi(e);
    log_line("task threads: asked %u, the game chose %u for this processor (cores it counted: %u), using %u", asked, chosen, *(uint32_t *)(manager + 0x74), use);
    return use;
}
static void patch_task_threads(void) {
    static const uint8_t old[5] = {0x55, 0x41, 0x57, 0x41, 0x56};   // push rbp; push r15; push r14
    g_cpu_config_orig = (CpuConfigFn)hook_entry(0x3062320, old, 5, (void *)task_thread_count);
    if (!g_cpu_config_orig) log_line("patch skipped, game code differs: task thread count");
}

// ---- Is the battle simulation running beside the drawing? After each battle step
// EMPIREBATTLE::BATTLE_ENV::post_model_update_synchronise (b9c700) decides whether the next step may run as a task on a worker
// thread while the main thread draws (setting number 0x5f on, at least two threads, allow_multi_thread_model) and keeps the
// answer at +0x645e8. Measuring only: the answers are counted and logged with the fps lines during a battle.
static void (SYSV *g_model_sync_orig)(uint8_t *battle);
static volatile long g_model_beside, g_model_alone;
volatile long g_battle_steps_total;   // battle steps since the program started: the clock of a replay test (test_auto.c)
volatile long g_battle_steps_base = -1;   // the count before the first step that ran beside the drawing: a replay runs its opening steps on their own first (157 in the kept clash replay)
static SYSV void model_sync_counted(uint8_t *battle) {
    g_model_sync_orig(battle);
    if (battle[0x645e8]) { g_model_beside++; if (g_battle_steps_base < 0) g_battle_steps_base = g_battle_steps_total; } else g_model_alone++;
    g_battle_steps_total++;
}
void task_report(void) {
    if (!(g_model_beside + g_model_alone)) return;
    log_line("battle steps: %ld ran beside the drawing (on a worker thread), %ld on their own", g_model_beside, g_model_alone);
    g_model_beside = g_model_alone = 0;
}
static void patch_model_sync_count(void) {
    if (!getenv("ATTILA_TASK_TRACE") && !getenv("ATTILA_AUTO_REPLAY")) return;   // measuring / replay tests only: the battle code stays untouched for players
    static const uint8_t old[8] = {0x53, 0x48, 0x89, 0xFB, 0x48, 0x8B, 0x43, 0x58};   // push rbx; mov rbx,rdi; mov rax,[rbx+58h]
    g_model_sync_orig = (void (SYSV *)(uint8_t *))hook_entry(0xb9c700, old, 8, (void *)model_sync_counted);
    if (!g_model_sync_orig) log_line("patch skipped, game code differs: battle step count");
}

// ---- Uploads of only the changed rows (tex_trace.inc has the story). OPENGL_TEXTURE_2D::update_subresource (33db8b0) ignores
// the box it is given; this hook hands the box to the OpenGL layer for the length of the game's own call.
// With ATTILA_TEX_TRACE=1 the first 8 different callers are logged too.
typedef void (SYSV *UpdateSubFn)(uint8_t *tex, void *ctx, uint32_t sub, const uint32_t *box, const void *data, uint32_t pitch, uint32_t depth_pitch);
static UpdateSubFn g_update_sub_orig;
static int g_update_sub_trace;
extern void upload_rows_begin(const uint32_t *box);
extern void upload_rows_end(void);
static SYSV void update_sub_boxed(uint8_t *tex, void *ctx, uint32_t sub, const uint32_t *box, const void *data, uint32_t pitch, uint32_t depth_pitch) {
    if (g_update_sub_trace) {
        static void *seen[8];
        static int n;
        void *from = __builtin_return_address(0);
        int known = 0;
        for (int i = 0; i < n; i++) if (seen[i] == from) known = 1;
        if (!known && n < 8) {
            seen[n++] = from;
            log_line("texture upload from memory asked at %p: subresource %u, pitch %u, box %u,%u,%u - %u,%u,%u", from, sub, pitch,
                     box ? box[0] : 0, box ? box[1] : 0, box ? box[2] : 0, box ? box[3] : 0, box ? box[4] : 0, box ? box[5] : 0);
        }
    }
    upload_rows_begin(box);
    g_update_sub_orig(tex, ctx, sub, box, data, pitch, depth_pitch);
    upload_rows_end();
}
static void patch_upload_rows(void) {
    static const uint8_t old[5] = {0x55, 0x41, 0x57, 0x41, 0x56};   // push rbp; push r15; push r14
    g_update_sub_trace = getenv("ATTILA_TEX_TRACE") != NULL;
    g_update_sub_orig = (UpdateSubFn)hook_entry(0x33db8b0, old, 5, (void *)update_sub_boxed);
    log_line(g_update_sub_orig ? "patch applied: texture uploads send only their changed rows" : "patch skipped, game code differs: changed-row uploads");
}

// ---- How soldiers are grouped into draw calls, and why a melee is not. WARSCAPE::MESH_INSTANCE_BATCHER::next_batch (310ea10)
// cuts a mesh's list of copies into batches, one draw call each. Batcher: +0x08 "changes in use", +0x10 / +0x18 start and end
// of an array with 8 bytes a copy (the second 4 bytes are that copy's "change handle"), +0x20 first copy of the batch, +0x24
// copies in it, +0x28 most copies allowed, +0x2c the batch's handle. The game's rule: a batch takes its first copy's handle and
// goes on only while the following copies have handle 0. Every copy with a handle other than 0 starts a new batch, also when
// its handle is the same as the batch's. Measured on the user's game in a big melee (2026-11-12): about 57,000 one-copy draw
// calls a frame, 19-23 fps. (JGdaCaesar found this fault in the 32-bit game; this is our own reading of the 64-bit code.)
//   ATTILA_BATCH_STATS=1: only counts, logged with the fps lines: batches, one-copy batches, and how many batches end at a copy
//     whose handle equals the batch's own (those would join under the changed rule).
//   ATTILA_BATCH_EQUAL=1 (test): the changed rule: a batch also goes on over copies whose handle equals the batch's.
typedef uint8_t (SYSV *NextBatchFn)(uint8_t *batcher);
static NextBatchFn g_next_batch_orig;
static int g_batch_equal;
static volatile unsigned long long g_batch_n, g_batch_one, g_batch_could_join, g_batch_copies, g_batch_joined;
static SYSV uint8_t next_batch_counted(uint8_t *b) {
    uint8_t more = g_next_batch_orig(b);
    if (!more) return more;
    uint32_t start = *(uint32_t *)(b + 0x20), count = *(uint32_t *)(b + 0x24);
    if (b[8]) {
        const uint32_t *changes = *(const uint32_t *const *)(b + 0x10);
        uint32_t all = (uint32_t)((*(const uint8_t *const *)(b + 0x18) - (const uint8_t *)changes) >> 3);
        uint32_t most = *(uint32_t *)(b + 0x28), handle = *(uint32_t *)(b + 0x2c);
        if (g_batch_equal && handle) {
            uint32_t was = count;
            while (count < most && start + count < all && (changes[(start + count) * 2 + 1] == handle || changes[(start + count) * 2 + 1] == 0)) count++;
            if (count != was) { *(uint32_t *)(b + 0x24) = count; g_batch_joined += count - was; }
        } else if (handle && count < most && start + count < all && changes[(start + count) * 2 + 1] == handle) g_batch_could_join++;
    }
    g_batch_n++; g_batch_copies += count;
    if (count == 1) g_batch_one++;
    return more;
}
void batch_report(long frames) {
    soldier_order_report(frames);
    if (!g_next_batch_orig || frames <= 0 || !g_batch_n) return;
    log_line("batches per frame: %.0f for %.0f copies, %.0f of one copy; %s %.0f", (double)g_batch_n / frames, (double)g_batch_copies / frames, (double)g_batch_one / frames,
             g_batch_equal ? "copies joined to a batch by the changed rule:" : "batches that end at a copy with the batch's own handle:", (double)(g_batch_equal ? g_batch_joined : g_batch_could_join) / frames);
    g_batch_n = g_batch_one = g_batch_could_join = g_batch_copies = g_batch_joined = 0;
}
static void patch_batch_rule(void) {
    g_batch_equal = getenv("ATTILA_BATCH_EQUAL") != NULL;
    if (!g_batch_equal && !getenv("ATTILA_BATCH_STATS")) return;
    static const uint8_t old[7] = {0x55, 0x41, 0x56, 0x53, 0x48, 0x89, 0xFB};   // push rbp; push r14; push rbx; mov rbx,rdi
    g_next_batch_orig = (NextBatchFn)hook_entry(0x310ea10, old, 7, (void *)next_batch_counted);
    log_line(!g_next_batch_orig ? "patch skipped, game code differs: batch rule" : g_batch_equal ? "patch applied: copies with the batch's own handle stay in the batch (test, ATTILA_BATCH_EQUAL)" : "batch counting: on");
}