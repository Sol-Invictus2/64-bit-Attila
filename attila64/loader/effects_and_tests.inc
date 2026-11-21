// Battle effects (particle pool locks, group check, task split) and test switches that hook the game (movies, battle step).
// Part of loader.c (included there, not built by itself; the order of the includes matters).

// ---- Particle pool locks, for battle effects updated on several threads (VFX_PARALLEL_UPDATER, see loader_game_settings).
// WARSCAPE::VFX_EMITTER_INSTANCE_POOL keeps one free list of particle slots (vector<u16>) shared by every effect of a kind:
// get_free_instance pops from it (two versions) and add_to_instance_dead_list pushes back (push_back may grow it). Run from
// several threads at once that corrupted the heap (crash c0000374 in VECTOR<u16>::push_back under add_to_instance_dead_list,
// on a task thread). All three now run under one lock; they are a few instructions each.
int g_vfx_pool_locked;
static SRWLOCK g_vfx_pool_lock = SRWLOCK_INIT;
typedef int (SYSV *PoolGetFn)(void *pool, int *idx);
typedef int (SYSV *PoolGetListFn)(int *idx, void *list, unsigned n);
typedef void (SYSV *PoolDeadFn)(void *pool, int *idx);
static PoolGetFn g_pool_get_orig;
static PoolGetListFn g_pool_get_list_orig;
static PoolDeadFn g_pool_dead_orig;
static SYSV int pool_get_locked(void *pool, int *idx) {
    AcquireSRWLockExclusive(&g_vfx_pool_lock);
    int r = g_pool_get_orig(pool, idx);
    ReleaseSRWLockExclusive(&g_vfx_pool_lock);
    return r;
}
static SYSV int pool_get_list_locked(int *idx, void *list, unsigned n) {
    AcquireSRWLockExclusive(&g_vfx_pool_lock);
    int r = g_pool_get_list_orig(idx, list, n);
    ReleaseSRWLockExclusive(&g_vfx_pool_lock);
    return r;
}
static SYSV void pool_dead_locked(void *pool, int *idx) {
    AcquireSRWLockExclusive(&g_vfx_pool_lock);
    g_pool_dead_orig(pool, idx);
    ReleaseSRWLockExclusive(&g_vfx_pool_lock);
}
static void patch_vfx_pool_locks(void) {
    static const uint8_t get_old[5] = {0x8B, 0x47, 0x14, 0x85, 0xC0};        // mov eax,[rdi+14h]; test eax,eax
    static const uint8_t list_old[5] = {0x8B, 0x46, 0x04, 0x85, 0xC0};       // mov eax,[rsi+4]; test eax,eax
    static const uint8_t dead_old[5] = {0x53, 0x48, 0x83, 0xEC, 0x10};       // push rbx; sub rsp,10h
    if (memcmp((void *)0x34e0130, get_old, 5) || memcmp((void *)0x34e0170, list_old, 5) || memcmp((void *)0x34e01a0, dead_old, 5)) {
        log_line("patch skipped, game code differs: particle pool locks"); return;
    }
    g_pool_get_orig = (PoolGetFn)hook_entry(0x34e0130, get_old, 5, (void *)pool_get_locked);
    g_pool_get_list_orig = (PoolGetListFn)hook_entry(0x34e0170, list_old, 5, (void *)pool_get_list_locked);
    g_pool_dead_orig = (PoolDeadFn)hook_entry(0x34e01a0, dead_old, 5, (void *)pool_dead_locked);
    g_vfx_pool_locked = g_pool_get_orig && g_pool_get_list_orig && g_pool_dead_orig;
    log_line(g_vfx_pool_locked ? "patch applied: particle pool locks (effects on several threads)" : "patch skipped, no memory near the game: particle pool locks");
}

// ---- Effect gate: which effects may update at the same time (VFX_PARALLEL_UPDATER, see loader_game_settings).
// Mapped 2026-11-10 from the call graph under VFX_INSTANCE_IMPL::update (34b3070). An effect leaves its own data in these places:
//   - particle slot pools (locked above) and the shared spawn buffer (VFX_SCENE_NODE::spawn_particle, locked here);
//   - event links (spawn / death of a particle starts another effect): VFX_IMPL_BASE::notify_* -> spawn_vfx_instance_internal
//     registers effects, loads files and appends to an effect type's instance list;
//   - rigid emitters (debris): add_rigid_instances_internal -> RIGID_SCENE_NODE_V2::add_simple_instance (the Test 17 crash);
//   - ribbons, sound (update_sound), particles tracked by / tracking another effect.
// One rule instead of a lock per place: an effect made only of plain billboard emitters (CONTINUOUS_MESH_EMITTER_INSTANCE)
// with no event links, no sound and no tracking updates freely; every other effect updates alone, under g_vfx_gate held
// exclusively. Tasks read the instance list under the shared side, because an effect behind the gate may grow that list.
// The task body (ParallelForTask<ParallelVFXInstanceUpdater>::execute, 35314b0) is replaced by vfx_task_execute.
int g_vfx_gated;
static SRWLOCK g_vfx_gate = SRWLOCK_INIT;
static volatile long g_vfx_free_runs, g_vfx_gated_runs;
typedef void (SYSV *VfxUpdateFn)(void *inst, void *time, void *wind, void *cam);
typedef uint64_t (SYSV *VfxSpawnFn)(void *node, const void *particle);
static VfxSpawnFn g_vfx_spawn_orig;
#define VFX_MESH_EMITTER_VTABLE 0x3c5f750u      // vtable for CONTINUOUS_MESH_EMITTER_INSTANCE + 10h
static int vfx_keeps_to_itself(const uint8_t *inst) {
    if (*(void *const *)(inst + 0x10) || *(void *const *)(inst + 0x18)) return 0;        // TRACKING_INFO in use
    uint32_t n = *(const uint32_t *)(inst + 0x7c);
    uint8_t *const *em = *(uint8_t *const *const *)(inst + 0x80);
    if (n && !em) return 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = em[i];
        if (!e || *(const uint64_t *)e != VFX_MESH_EMITTER_VTABLE) return 0;
        const uint8_t *ctrl = *(uint8_t *const *)(e + 0x18);
        const uint8_t *desc = ctrl ? *(uint8_t *const *)(ctrl + 0xa8) : NULL;
        if (!desc) return 0;
        if (*(const uint32_t *)(desc + 0x40c)) return 0;                                   // event links
        if (*(const uint32_t *)(desc + 0x380) || *(const uint32_t *)(desc + 0x398)) return 0;   // sound names (string lengths)
    }
    return 1;
}
static SYSV uint64_t vfx_task_execute(uint8_t *task) {
    void **u = *(void ***)(task + 0x40);            // ParallelVFXInstanceUpdater: instance list, time, wind, camera
    VfxUpdateFn update = (VfxUpdateFn)0x34b3070;
    for (uint32_t i = *(uint32_t *)(task + 0x30); i != *(uint32_t *)(task + 0x34); i++) {
        AcquireSRWLockShared(&g_vfx_gate);
        uint8_t *inst = *(uint8_t **)(*(uint8_t **)((uint8_t *)u[0] + 8) + (size_t)i * 16);
        ReleaseSRWLockShared(&g_vfx_gate);
        if (vfx_keeps_to_itself(inst)) { update(inst, u[1], u[2], u[3]); g_vfx_free_runs++; }
        else {
            AcquireSRWLockExclusive(&g_vfx_gate);
            update(inst, u[1], u[2], u[3]);
            g_vfx_gated_runs++;
            ReleaseSRWLockExclusive(&g_vfx_gate);
        }
    }
    return 0;
}
static SYSV uint64_t vfx_spawn_locked(void *node, const void *particle) {
    AcquireSRWLockExclusive(&g_vfx_pool_lock);
    uint64_t r = g_vfx_spawn_orig(node, particle);
    ReleaseSRWLockExclusive(&g_vfx_pool_lock);
    return r;
}
// test (ATTILA_PARALLEL_VFX=flip): effects on all cores for one fps window, one by one for the next, and so on, so one run
// of a steady scene shows both. The game reads its setting (526f272) at every effects update, so it can change between frames.
int g_vfx_flip;
void vfx_flip(void) {
    if (!g_vfx_flip) return;
    uint8_t *on = (uint8_t *)0x526f272;
    log_line("effects flip test: the window just logged had effects %s", *on ? "on all cores" : "one by one");
    *on = !*on;
}
void vfx_gate_report(void) {    // one line per fps line while measuring
    long a = InterlockedExchange(&g_vfx_free_runs, 0), b = InterlockedExchange(&g_vfx_gated_runs, 0);
    if (a || b) log_line("effects: %ld updated freely, %ld one at a time", a, b);
}
static void patch_vfx_gate(void) {
    static const uint8_t task_old[18] = {0x41, 0x57, 0x41, 0x56, 0x53, 0x49, 0x89, 0xFE, 0x41, 0x8B, 0x5E, 0x30, 0x41, 0x3B, 0x5E, 0x34, 0x74, 0x3C};
    static const uint8_t spawn_old[16] = {0x48, 0x8B, 0xBF, 0x28, 0x02, 0x00, 0x00, 0x48, 0x8B, 0x07, 0x48, 0x8B, 0x40, 0x40, 0xFF, 0xE0};
    static const uint8_t list_read[13] = {0x41, 0x8B, 0x5E, 0x2C, 0xC7, 0x04, 0x24, 0x00, 0x00, 0x00, 0x00, 0x89, 0x5C};   // 34ad1c6: count at +2Ch of the list at +28h
    static const uint8_t loop_read[18] = {0x49, 0x8B, 0x8C, 0x24, 0x80, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x1C, 0xC1, 0xF3, 0x0F, 0x10, 0x44, 0x24, 0x18};   // 34b34e2: emitters at +80h
    if (!g_vfx_pool_locked) return;
    if (memcmp((void *)0x35314b0, task_old, sizeof task_old) || memcmp((void *)0x32ffa60, spawn_old, sizeof spawn_old) ||
        memcmp((void *)0x34ad1c6, list_read, sizeof list_read) || memcmp((void *)0x34b34e2, loop_read, sizeof loop_read) ||
        *(uint64_t *)(uintptr_t)(VFX_MESH_EMITTER_VTABLE + 0x20) != 0x349cd50) {
        log_line("patch skipped, game code differs: effect gate"); return;
    }
    g_vfx_spawn_orig = (VfxSpawnFn)hook_entry(0x32ffa60, spawn_old, 7, (void *)vfx_spawn_locked);
    g_vfx_gated = g_vfx_spawn_orig && hook_entry(0x35314b0, task_old, 5, (void *)vfx_task_execute);
    log_line(g_vfx_gated ? "patch applied: effect gate (plain effects on all cores, the rest one at a time)" : "patch skipped, no memory near the game: effect gate");
}

// ---- WARSCAPE::VFX_GROUP_INSTANCE_IMPL::needs_destroy (34b20e0) answers "may this effect group go": its own flag (+8) and
// every member effect's "finished" flag (+8a), members in a shared_ptr array (count +4c, data +50, 16 bytes each). It took
// about 9% of the main thread in a big clash: for every member it copied and dropped the shared_ptr (two locked memory
// operations) and went on to the end even after the answer was known. Same answer, read only, stops at the first "no".
// Its only caller, VFX_GROUP_IMPL::update, removes the groups that may go, every frame; each group is now looked at every 4th
// frame (spread by its address), so a finished, invisible effect may stay up to 3 frames longer. ATTILA_SLOW_VFX_CHECK=1 keeps
// the game's own.
extern volatile unsigned g_frame_no;
static SYSV int vfx_group_needs_destroy(const uint8_t *group) {
    if ((((uintptr_t)group >> 7) + g_frame_no) & 3) return 0;
    int r = group[0x8] != 0;
    uint32_t n = *(const uint32_t *)(group + 0x4c);
    const uint8_t *const *members = *(const uint8_t *const *const *)(group + 0x50);
    for (uint32_t i = 0; r && i < n; i++) r = members[2 * i][0x8a] != 0;
    return r;
}
static void patch_vfx_group_check(void) {
    if (getenv("ATTILA_SLOW_VFX_CHECK")) return;
    static const uint8_t old[16] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x10, 0x44, 0x8A, 0x77, 0x08};
    if (memcmp((void *)0x34b20e0, old, sizeof old)) { log_line("patch skipped, game code differs: effect group check"); return; }
    if (!hook_entry(0x34b20e0, old, 5, (void *)vfx_group_needs_destroy)) { log_line("patch skipped, no memory near the game: effect group check"); return; }
    log_line("patch applied: effect group check without reference counting");
}

// ---- How finely the game's task system cuts parallel work: tbb::ThreadTaskManager::start_task<ParallelForTask<...>> (39
// copies, one per kind of work) makes (threads + 1) x 20 pieces of every parallel loop ("imul reg,reg,14h; add reg,14h"), each
// piece taken from one shared list under one lock. The factor is set here (ATTILA_TASK_SPLIT, default the game's 20). Only how
// the same loop is shared between threads changes, not what each element computes.
static const struct { uint32_t at; uint8_t modrm_mul, modrm_add; } g_split_sites[] = {
    {0x6e8a4a, 0xC8, 0xC1}, {0xdfb175, 0xC8, 0xC1}, {0xdfb721, 0xC8, 0xC1}, {0x1cacc34, 0xC8, 0xC1}, {0x1cad455, 0xF8, 0xC7},
    {0x1cadec5, 0xF8, 0xC7}, {0x1d75f01, 0xE8, 0xC5}, {0x1d765f5, 0xF8, 0xC7}, {0x1d76d05, 0xF8, 0xC7}, {0x1d77705, 0xF8, 0xC7},
    {0x1d78d05, 0xF8, 0xC7}, {0x1d85385, 0xF8, 0xC7}, {0x3072b23, 0xF8, 0xC7}, {0x3073153, 0xF8, 0xC7}, {0x3074895, 0xC8, 0xC1},
    {0x30750bf, 0xF8, 0xC7}, {0x30757af, 0xC8, 0xC1}, {0x3075e6f, 0xC8, 0xC1}, {0x30764ff, 0xC8, 0xC1}, {0x3076b8f, 0xC8, 0xC1},
    {0x3077227, 0xC8, 0xC1}, {0x30777f7, 0xC8, 0xC1}, {0x3077d77, 0xC8, 0xC1}, {0x318a468, 0xF8, 0xC7}, {0x318acc6, 0xF8, 0xC7},
    {0x318d005, 0xF8, 0xC7}, {0x3373865, 0xF8, 0xC7}, {0x3388ef5, 0xF8, 0xC7}, {0x338a065, 0xF8, 0xC7}, {0x3390c98, 0xF0, 0xC6},
    {0x33965f8, 0xF8, 0xC7}, {0x3397938, 0xF8, 0xC7}, {0x3398d58, 0xF8, 0xC7}, {0x339a218, 0xF8, 0xC7}, {0x3529d46, 0xF8, 0xC7},
    {0x3530ff3, 0xF8, 0xC7}, {0x3533ba5, 0xF8, 0xC7}, {0x354c508, 0xF0, 0xC6}, {0x354cab3, 0xF0, 0xC6},
};
static void patch_task_split(int factor) {
    if (factor < 1 || factor > 100 || factor == 20) return;
    int done = 0;
    for (size_t i = 0; i < sizeof g_split_sites / sizeof g_split_sites[0]; i++) {
        uint8_t *p = (uint8_t *)(uintptr_t)g_split_sites[i].at;
        if (p[0] != 0x6B || p[1] != g_split_sites[i].modrm_mul || p[2] != 0x14 || p[3] != 0x83 || p[4] != g_split_sites[i].modrm_add ||
            p[5] != 0x14) continue;
        p[2] = (uint8_t)factor; p[5] = (uint8_t)factor; done++;
    }
    log_line("patch applied: parallel work cut into (threads + 1) x %d pieces (game: 20), %d of %d places", factor, done,
             (int)(sizeof g_split_sites / sizeof g_split_sites[0]));
}

// ---- Test switch ATTILA_TEST_SKIP_MOVIES=1: every movie is skipped as soon as it starts (EMPIRE::BINK_VIDEO_UI_CONTROL::play
// 1c26b30, then its own skip() 1c273d0), so unattended tests reach the main menu without the 2.5-minute intro. Tests only.
typedef uint64_t (SYSV *MoviePlayFn)(void *ctl, const void *name, const void *params);
static MoviePlayFn g_movie_play_orig;
static SYSV uint64_t movie_play_skip(void *ctl, const void *name, const void *params) {
    uint64_t r = g_movie_play_orig(ctl, name, params);
    if (ctl) ((SYSV void (*)(void *))(uintptr_t)0x1c273d0)(ctl);
    return r;
}
static void patch_skip_movies(void) {
    if (!getenv("ATTILA_TEST_SKIP_MOVIES")) return;
    static const uint8_t old[5] = {0x55, 0x41, 0x57, 0x41, 0x56};
    g_movie_play_orig = (MoviePlayFn)hook_entry(0x1c26b30, old, 5, (void *)movie_play_skip);
    log_line("test switch: movies %s", g_movie_play_orig ? "skipped at once" : "NOT skipped (hook failed)");
}
// ---- Test switch ATTILA_TICK_TRACE=1: times EMPIREBATTLE::BATTLE_MODEL::tick (5cafa0, one step of the battle simulation):
// every 5 s the number of steps, their average and longest time, and the thread. Measuring only.
typedef uint64_t (SYSV *ModelTickFn)(void *a, void *b, void *c, void *d, void *e, void *f);   // result and spare arguments passed through untouched
static ModelTickFn g_model_tick_orig;
static SYSV uint64_t model_tick_trace(void *a0, void *a1, void *a2, void *a3, void *a4, void *a5) {
    static LARGE_INTEGER fq; static volatile LONG n; static double sum, worst; static ULONGLONG last;
    LARGE_INTEGER a, b;
    if (!fq.QuadPart) QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&a);
    uint64_t ret = g_model_tick_orig(a0, a1, a2, a3, a4, a5);
    QueryPerformanceCounter(&b);
    double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)fq.QuadPart;
    sum += ms; if (ms > worst) worst = ms; n++;
    ULONGLONG now = GetTickCount64();
    if (!last) last = now;
    if (now - last >= 5000) {
        log_line("battle steps: %ld in %.1f s, average %.1f ms, longest %.1f ms, thread %lu", (long)n, (double)(now - last) / 1000.0, n ? sum / n : 0.0, worst, (unsigned long)GetCurrentThreadId());
        n = 0; sum = 0; worst = 0; last = now;
    }
    return ret;
}
// ---- Test switch ATTILA_SKYMAP_TRACE=1: when the game bakes its sky reflection map (WARSCAPE::SKY_DOME::bake_env_map
// 32c9d80) and starts drawing each of its cube faces (RENDER_TARGET::begin_render_to_cube_face 30ce5b0: the face's view
// is texture +0x18 (manager entry) +0xb0 [face]). Measuring only; both pass every argument through (8 declared, the 7th
// and 8th come from the caller's stack and go back on it).
typedef uint64_t (SYSV *EightArgFn)(void *, void *, void *, void *, uint64_t, void *, void *, void *);
static EightArgFn g_bake_orig, g_cube_face_orig;
static SYSV uint64_t bake_trace(void *a0, void *a1, void *a2, void *a3, uint64_t a4, void *a5, void *a6, void *a7) {
    uint8_t *dome = a0;
    uint8_t *env = *(uint8_t **)(dome + 0x8f0);
    log_line("sky map trace: bake starts (flag %u, dome +0x69 = %u, env texture %p)", (unsigned)(uintptr_t)a1 & 0xff, dome[0x69], (void *)env);
    uint64_t r = g_bake_orig(a0, a1, a2, a3, a4, a5, a6, a7);
    log_line("sky map trace: bake ends");
    return r;
}
static SYSV uint64_t cube_face_trace(void *a0, void *a1, void *a2, void *a3, uint64_t face, void *a5, void *a6, void *a7) {
    static int told;
    if (told++ < 24) {
        uint8_t *tex = a2, *entry = tex ? *(uint8_t **)(tex + 0x18) : NULL;
        uint8_t **views = entry ? *(uint8_t ***)(entry + 0xb0) : NULL;
        uint8_t *rtv = views ? views[face] : NULL;
        const uint32_t *d = rtv ? (const uint32_t *)(rtv + 8) : NULL;
        log_line("sky map trace: cube face %u: texture %p, entry %p, face views %p, view %p -> device texture %p, desc {0x%x, %u, %u, %u, %u}, depth texture %p",
                 (unsigned)face, (void *)tex, (void *)entry, (void *)views, (void *)rtv, rtv ? *(void **)(rtv + 0x20) : NULL,
                 d ? d[0] : 0, d ? d[1] : 0, d ? d[2] : 0, d ? d[3] : 0, d ? d[4] : 0, a3);
    }
    return g_cube_face_orig(a0, a1, a2, a3, face, a5, a6, a7);
}
static void patch_skymap_trace(void) {
    if (!getenv("ATTILA_SKYMAP_TRACE")) return;
    static const uint8_t bake_old[5] = {0x55, 0x41, 0x57, 0x41, 0x56};                 // push rbp; push r15; push r14
    static const uint8_t face_old[7] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53};     // push r15; push r14; push r12; push rbx
    g_bake_orig = (EightArgFn)hook_entry(0x32c9d80, bake_old, 5, (void *)bake_trace);
    g_cube_face_orig = (EightArgFn)hook_entry(0x30ce5b0, face_old, 7, (void *)cube_face_trace);
    log_line("sky map trace: bake %s, cube faces %s", g_bake_orig ? "hooked" : "NOT hooked", g_cube_face_orig ? "hooked" : "NOT hooked");
}
static void patch_tick_trace(void) {
    if (!getenv("ATTILA_TICK_TRACE")) return;
    static const uint8_t old[8] = {0x55, 0x41, 0x56, 0x53, 0x48, 0x83, 0xEC, 0x50};   // push rbp; push r14; push rbx; sub rsp,50h
    g_model_tick_orig = (ModelTickFn)hook_entry(0x5cafa0, old, 8, (void *)model_tick_trace);
    log_line("tick trace: %s", g_model_tick_orig ? "battle step hooked" : "battle step NOT hooked");
}
