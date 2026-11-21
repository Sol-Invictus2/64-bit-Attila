// Soldier draw lists prepared on several cores. Display only (the graphics engine's lists, nothing shared).
// Part of loader.c (included after sort_fast.inc and task_waits.inc).
// ATTILA_PARALLEL_PREPARE=0 = the game's way, =flip = switched off and on with every fps line (test);
// ATTILA_PREPARE_HELPERS=n = helper threads (default 3, 0-7).
//
// Found 2026-11-13 with task_waits.inc in the kept clash: every frame the main thread waits about 0.84 ms for one task,
// PREPARE_TBB_INSTANCE_LIST_TASK<WEIGHTED_INSTANCE>::PREPARE_TASK (execute 354fcb0, waited for at 33f552f), which runs
// about 1.3 ms on ONE task thread while the other task threads sit idle. Its body is
// RIGID_SCENE_NODE_V2::prepare_instances_from_mesh_render_list<WEIGHTED_INSTANCE> (354fd00; this, the frame's mesh list,
// a name):
//   1. the mesh list's per-thread parts are moved together (TBB_INSTANCE_LIST_THREAD_SAFE<RIGID_MESH*>::
//      transfer_thread_lists 354aca0) and the meshes sorted (MESH_LIST_SORTER, get_mesh_sort = vtable +1b8);
//   2. for each of 23 view list types t, for each mesh: WEIGHTED_MESH_V5::prepare_instances(t) (vtable +278, 3418420):
//      the mesh's list t (mesh + 3a8h + t*30h) gets its per-thread parts moved together (353df40), its soldiers sorted
//      by the key at soldier + 78h (std::sort), and every soldier pushed onto one shared render list (the VECTOR at
//      [mesh + 358h] + 30h); the running count before / after each type is kept in the mesh list (+40h / +9ch + t*4).
// The moving together and the sorting of step 2 touch only that mesh's own lists. Here, before the game's function runs,
// the mesh list is moved together and its meshes are shared out over this thread and helper threads; each does for its
// meshes what prepare_instances does before the push (move together, then the same fast sort as sort_fast.inc). Then
// the game's function runs as always on this thread: its moves find nothing left to move (both move functions clear
// their flag at +28h, so a second call does nothing) and its soldier sorts are skipped (t_sort_skip_w), so it only sorts
// the meshes and pushes, in its own order. The render list comes out exactly as before.
// Used only when every mesh in the list is a WEIGHTED_MESH_V5 (vtable +278 = 3418420) and the fast sort is in place;
// otherwise, or while another scene's soldier task is using the helpers, the game's function runs alone.
//
// The pushing in one go (pp_push_all; ATTILA_PREPARE_BULK=0 = the game's pushing): when in addition every mesh has the
// plain sort value function (vtable +1b8 = WEIGHTED_MESH_V5::get_mesh_sort 3417b00, which only reads fields) and all
// push onto the same render list, the game's function is not called at all. Its two remaining jobs are done here:
//   - the meshes are put in the order of MESH_LIST_SORTER: by sort value, and for equal values by address (read in its
//     inlined insertion step at 354fd80); that is a total order, so any sort gives exactly std::sort's result;
//   - for each type, for each mesh in that order, the mesh's list is appended to the render list, and the running
//     counts are written to the mesh list. The render list is a CA_STD::VECTOR<WEIGHTED_INSTANCE*, ES_DOUBLE>: capacity
//     (u32 +0), count (u32 +4), items (+8); VECTOR::push_back (353dc30) grows it to 1 when empty, else doubles it, with
//     CA::custom_malloc(bytes, heap 0) (525f10) and frees the old block with CA::custom_free (525fd0). Here it is grown
//     the same way once, to the size the pushes would have reached, and filled with memcpy.
//
// Rigid parts (buildings, walls, props, gear; ATTILA_PARALLEL_PREPARE_RIGID=0 = the game's way, =flip = test), added the
// same day: PREPARE_..._TASK<RIGID_INSTANCE_V2> (execute 354fad0, two a frame, about 0.12 ms each, waited for at
// 34e984f) runs prepare_instances_from_mesh_render_list<RIGID_INSTANCE_V2> (354fb20), the same steps, but its meshes'
// prepare_instances (RIGID_MESH_V5<RIGID_INSTANCE_V2> 35122f0: list at mesh + 358h + t*30h, moved together by 338f690,
// sorted by the key at part + 50h) does not push: the parts are drawn from the mesh's own lists. The list may hold other
// mesh kinds too (rivers, debug shapes, ...): only meshes with that prepare function are shared out, the rest are left
// to the game's function. Its sorts then find the shared-out lists already in order (the fast sort's first look at the
// keys), so no list is skipped by guesswork, and the game's function runs as always.
typedef void (SYSV *PrepNodeFn)(void *node, uint8_t *meshlist, const char *name);
typedef void (SYSV *ListMoveFn)(void *list);
static PrepNodeFn g_pp_orig, g_pp_rigid_orig;
static volatile int g_pp_on, g_pp_rigid_on;
static int g_pp_flip, g_pp_rigid_flip, g_pp_helpers = 3, g_pp_started;
static HANDLE g_pp_sem;
static struct { uint8_t **mesh; long n; volatile long next, busy; int kind; } g_pp_job;
// kind 0 = soldiers (WEIGHTED_MESH_V5), 1 = rigid parts (RIGID_MESH_V5<RIGID_INSTANCE_V2>)
static const struct { uintptr_t move, prepare; uint32_t list_at, key_at; } g_pp_kind[2] = {
    {0x353df40, 0x3418420, 0x3a8, 0x78},
    {0x338f690, 0x35122f0, 0x358, 0x50},
};
static volatile LONG64 g_ppr_runs, g_ppr_meshes, g_ppr_others, g_ppr_busy, g_ppr_by_helpers;
static uint8_t **g_pp_meshes;
static long g_pp_cap;
static volatile long g_pp_lock;
static volatile LONG64 g_pp_runs, g_pp_meshes_done, g_pp_by_helpers, g_pp_alone, g_pp_bulk_runs, g_pp_ticks_a, g_pp_ticks_b;
static int g_pp_bulk = 1;
typedef struct { uint32_t key; uint8_t *mesh; } PpMeshKey;
static PpMeshKey *g_pp_keys;
static long g_pp_keys_cap;

static void pp_prepare_mesh(uint8_t *mesh) {
    int kind = g_pp_job.kind;
    ListMoveFn move = (ListMoveFn)g_pp_kind[kind].move;
    for (int t = 0; t < 23; t++) {
        uint8_t *list = mesh + g_pp_kind[kind].list_at + t * 0x30;
        move(list);
        uint32_t n = *(uint32_t *)(list + 0xc);
        void **items = *(void ***)(list + 0x10);
        if (n < 2 || !items || fast_sort_range(items, n, g_pp_kind[kind].key_at)) continue;
        long depth = 2 * (63 - __builtin_clzll((unsigned long long)n));   // no memory for the fast sort: the game's std::sort
        t_sort_inside = 1;
        (kind ? g_sortr_loop_orig : g_sortw_loop_orig)(items, items + n, depth);
        t_sort_inside = 0;
        (kind ? g_sortr_final_orig : g_sortw_final_orig)(items, items + n);
    }
}
static long pp_take_work(void) {
    long done = 0;
    for (;;) {
        long i = InterlockedIncrement(&g_pp_job.next) - 1;
        if (i >= g_pp_job.n) return done;
        pp_prepare_mesh(g_pp_job.mesh[i]);
        done++;
    }
}
static DWORD WINAPI pp_helper(void *arg) {
    (void)arg;
    for (;;) {
        WaitForSingleObject(g_pp_sem, INFINITE);
        long done = pp_take_work();
        if (done) InterlockedAdd64(g_pp_job.kind ? &g_ppr_by_helpers : &g_pp_by_helpers, done);
        InterlockedDecrement(&g_pp_job.busy);
    }
    return 0;
}
static int pp_start_helpers(void) {
    if (g_pp_started) return g_pp_started > 0;
    g_pp_started = -1;
    if (!g_pp_helpers) return 0;
    g_pp_sem = CreateSemaphoreA(NULL, 0, 64, NULL);
    if (!g_pp_sem) return 0;
    int made = 0;
    for (int i = 0; i < g_pp_helpers; i++) {
        // stacks fully committed, as for the game's own threads (shim_thread.c): Linux code does not probe guard pages
        HANDLE h = CreateThread(NULL, (size_t)1 << 20, pp_helper, NULL, 0, NULL);
        if (h) { CloseHandle(h); made++; }
    }
    g_pp_helpers = made;
    g_pp_started = made ? 1 : -1;
    log_line("soldier list preparation: %d helper threads started", made);
    return made > 0;
}
static long pp_run_job(long k, int kind);
static int pp_ptr_cmp(const void *a, const void *b) {
    uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b;
    return x < y ? -1 : x > y;
}

// The mesh list moved together, then every soldier list of every mesh moved together and sorted, shared out.
// 0 = nothing done that changes what the game does; 1 = done (the game's function may skip its soldier sorts);
// 2 = done, and the pushing may be done in one go (pp_push_all).
static int pp_presort(uint8_t *meshlist) {
    ((ListMoveFn)(uintptr_t)0x354aca0)(meshlist);
    uint32_t n = *(uint32_t *)(meshlist + 0xc);
    uint8_t **m = *(uint8_t ***)(meshlist + 0x10);
    if (n < 2 || !m) return 0;
    int bulk = 1;
    uint8_t *target = NULL;
    for (uint32_t i = 0; i < n; i++) {
        if (!m[i] || (*(uintptr_t *const *)m[i])[0x278 / 8] != 0x3418420) {
            static int told;
            if (!told++) log_line("soldier list preparation: a mesh of another kind in the list, the game's way is used for such lists");
            return 0;
        }
        uint8_t *to = *(uint8_t **)(m[i] + 0x358);
        if (!i) target = to;
        if ((*(uintptr_t *const *)m[i])[0x1b8 / 8] != 0x3417b00 || !to || to != target) bulk = 0;
    }
    if (!bulk) { static int told; if (!told++) log_line("soldier list preparation: meshes with another sort value or render list, pushed the game's way"); }
    if (g_pp_cap < (long)n) {
        uint8_t **nm = realloc(g_pp_meshes, ((size_t)n + 64) * sizeof *nm);
        if (!nm) return 0;
        g_pp_meshes = nm; g_pp_cap = (long)n + 64;
    }
    memcpy(g_pp_meshes, m, (size_t)n * sizeof *m);
    qsort(g_pp_meshes, n, sizeof *g_pp_meshes, pp_ptr_cmp);   // each mesh once, whatever the list holds
    long k = 1;
    for (uint32_t i = 1; i < n; i++) if (g_pp_meshes[i] != g_pp_meshes[k - 1]) g_pp_meshes[k++] = g_pp_meshes[i];
    long mine = pp_run_job(k, 0);
    InterlockedIncrement64(&g_pp_runs);
    InterlockedAdd64(&g_pp_meshes_done, k);
    InterlockedAdd64(&g_pp_alone, mine);
    return bulk ? 2 : 1;
}

// Shares g_pp_meshes[0..k) out over this thread and the helpers and waits until all are done. Returns how many meshes
// this thread did.
static long pp_run_job(long k, int kind) {
    g_pp_job.mesh = g_pp_meshes;
    g_pp_job.n = k;
    g_pp_job.next = 0;
    g_pp_job.kind = kind;
    long helpers = k - 1 < g_pp_helpers ? k - 1 : g_pp_helpers;
    if (helpers > 0 && !pp_start_helpers()) helpers = 0;
    if (helpers > g_pp_helpers) helpers = g_pp_helpers;
    g_pp_job.busy = helpers;
    MemoryBarrier();
    if (helpers > 0) ReleaseSemaphore(g_pp_sem, helpers, NULL);
    long mine = pp_take_work();
    // helpers that have not woken up yet are not needed any more: take their turn back, then wait for those at work
    while (g_pp_job.busy > 0 && WaitForSingleObject(g_pp_sem, 0) == WAIT_OBJECT_0) InterlockedDecrement(&g_pp_job.busy);
    for (unsigned spins = 0; g_pp_job.busy > 0; spins++) { if (spins < 50000) YieldProcessor(); else SwitchToThread(); }
    return mine;
}

// Rigid parts: the mesh list moved together, its RIGID_MESH_V5<RIGID_INSTANCE_V2> meshes (each once) shared out.
static void pp_presort_rigid(uint8_t *meshlist) {
    ((ListMoveFn)(uintptr_t)0x354aca0)(meshlist);
    uint32_t n = *(uint32_t *)(meshlist + 0xc);
    uint8_t **m = *(uint8_t ***)(meshlist + 0x10);
    if (n < 2 || !m) return;
    if (g_pp_cap < (long)n) {
        uint8_t **nm = realloc(g_pp_meshes, ((size_t)n + 64) * sizeof *nm);
        if (!nm) return;
        g_pp_meshes = nm; g_pp_cap = (long)n + 64;
    }
    long k = 0, others = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (m[i] && (*(uintptr_t *const *)m[i])[0x278 / 8] == 0x35122f0) g_pp_meshes[k++] = m[i];
        else others++;
    }
    if (k < 2) return;
    qsort(g_pp_meshes, (size_t)k, sizeof *g_pp_meshes, pp_ptr_cmp);
    long u = 1;
    for (long i = 1; i < k; i++) if (g_pp_meshes[i] != g_pp_meshes[u - 1]) g_pp_meshes[u++] = g_pp_meshes[i];
    pp_run_job(u, 1);
    InterlockedIncrement64(&g_ppr_runs);
    InterlockedAdd64(&g_ppr_meshes, u);
    InterlockedAdd64(&g_ppr_others, others);
}

static int pp_key_cmp(const void *a, const void *b) {
    const PpMeshKey *x = a, *y = b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return (uintptr_t)x->mesh < (uintptr_t)y->mesh ? -1 : (uintptr_t)x->mesh > (uintptr_t)y->mesh;
}
// What the rest of the game's function does, in one go (see the top). 0 = not done (nothing changed that matters: at
// most the render list got its larger block already), the game's function must run.
static int pp_push_all(uint8_t *meshlist) {
    typedef uint32_t (SYSV *MeshSortFn)(const void *mesh);
    typedef void *(SYSV *GameMallocFn)(size_t bytes, int heap);
    typedef void (SYSV *GameFreeFn)(const void *p);
    uint32_t n = *(uint32_t *)(meshlist + 0xc);
    uint8_t **m = *(uint8_t ***)(meshlist + 0x10);
    if (n < 1 || !m) return 0;
    if (g_pp_keys_cap < (long)n) {
        PpMeshKey *nk = realloc(g_pp_keys, ((size_t)n + 64) * sizeof *nk);
        if (!nk) return 0;
        g_pp_keys = nk; g_pp_keys_cap = (long)n + 64;
    }
    for (uint32_t i = 0; i < n; i++) { g_pp_keys[i].key = ((MeshSortFn)(uintptr_t)0x3417b00)(m[i]); g_pp_keys[i].mesh = m[i]; }
    qsort(g_pp_keys, n, sizeof *g_pp_keys, pp_key_cmp);
    uint64_t add = 0;
    for (uint32_t i = 0; i < n; i++)
        for (int t = 0; t < 23; t++) add += *(const uint32_t *)(g_pp_keys[i].mesh + 0x3b4 + t * 0x30);
    uint8_t *vec = *(uint8_t **)(g_pp_keys[0].mesh + 0x358) + 0x30;
    uint32_t cap = *(uint32_t *)vec, count = *(uint32_t *)(vec + 4);
    void **data = *(void ***)(vec + 8);
    uint64_t need = (uint64_t)count + add;
    if (need > 0x40000000u) return 0;
    if (need > cap) {
        uint64_t nc = cap ? cap : 1;
        while (nc < need) nc *= 2;
        void **nd = ((GameMallocFn)(uintptr_t)0x525f10)((size_t)nc * 8, 0);
        if (!nd) return 0;
        if (count) memcpy(nd, data, (size_t)count * 8);
        if (data) ((GameFreeFn)(uintptr_t)0x525fd0)(data);
        *(void ***)(vec + 8) = nd;
        *(uint32_t *)vec = (uint32_t)nc;
        data = nd;
    }
    for (uint32_t i = 0; i < n; i++) m[i] = g_pp_keys[i].mesh;   // the mesh list in the game's order, as its function leaves it
    uint32_t total = 0;
    for (int t = 0; t < 23; t++) {
        *(uint32_t *)(meshlist + 0x40 + t * 4) = total;
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *list = m[i] + 0x3a8 + t * 0x30;
            uint32_t c = *(const uint32_t *)(list + 0xc);
            if (!c) continue;
            memcpy(data + count, *(void *const *const *)(list + 0x10), (size_t)c * 8);
            count += c; total += c;
        }
        *(uint32_t *)(meshlist + 0x9c + t * 4) = total;
    }
    *(uint32_t *)(vec + 4) = count;
    InterlockedIncrement64(&g_pp_bulk_runs);
    return 1;
}

static SYSV void pp_prepare_weighted(void *node, uint8_t *meshlist, const char *name) {
    if (!g_pp_on || InterlockedCompareExchange(&g_pp_lock, 1, 0) != 0) { g_pp_orig(node, meshlist, name); return; }
    uint64_t t0 = __builtin_ia32_rdtsc();
    int sorted = pp_presort(meshlist);
    uint64_t t1 = __builtin_ia32_rdtsc();
    if (!(sorted == 2 && g_pp_bulk && pp_push_all(meshlist))) {
        t_sort_skip_w = sorted != 0;
        g_pp_orig(node, meshlist, name);
        t_sort_skip_w = 0;
    }
    InterlockedAdd64(&g_pp_ticks_a, (LONG64)(t1 - t0));
    InterlockedAdd64(&g_pp_ticks_b, (LONG64)(__builtin_ia32_rdtsc() - t1));
    InterlockedExchange(&g_pp_lock, 0);
}

static SYSV void pp_prepare_rigid(void *node, uint8_t *meshlist, const char *name) {
    if (g_pp_rigid_on) {
        if (InterlockedCompareExchange(&g_pp_lock, 1, 0) == 0) {
            pp_presort_rigid(meshlist);
            InterlockedExchange(&g_pp_lock, 0);
        } else InterlockedIncrement64(&g_ppr_busy);
    }
    g_pp_rigid_orig(node, meshlist, name);   // as always; the shared-out lists are found in order
}

// With each fps line (shim_gfx.c): the flip test, and for tests what was shared out.
void prep_parallel_report(long frames) {
    if (g_pp_rigid_orig) {
        LONG64 runs = InterlockedExchange64(&g_ppr_runs, 0), meshes = InterlockedExchange64(&g_ppr_meshes, 0);
        LONG64 others = InterlockedExchange64(&g_ppr_others, 0), busy = InterlockedExchange64(&g_ppr_busy, 0), byh = InterlockedExchange64(&g_ppr_by_helpers, 0);
        if (g_pp_rigid_flip) {
            long fr = frames < 1 ? 1 : frames;
            log_line("rigid prepare flip test: the window just logged had the rigid part lists prepared %s (%.1f runs a frame, %.0f meshes each, %.0f other meshes each, %.0f%% by helper threads, %.1f a frame found the helpers busy)",
                     g_pp_rigid_on ? "on several cores" : "the game's way", (double)runs / (double)fr, runs ? (double)meshes / (double)runs : 0.0,
                     runs ? (double)others / (double)runs : 0.0, meshes ? 100.0 * (double)byh / (double)meshes : 0.0, (double)busy / (double)fr);
            g_pp_rigid_on = !g_pp_rigid_on;
        }
    }
    if (!g_pp_orig) return;
    LONG64 runs = InterlockedExchange64(&g_pp_runs, 0), meshes = InterlockedExchange64(&g_pp_meshes_done, 0);
    LONG64 helpers = InterlockedExchange64(&g_pp_by_helpers, 0), alone = InterlockedExchange64(&g_pp_alone, 0);
    LONG64 bulk = InterlockedExchange64(&g_pp_bulk_runs, 0), ta = InterlockedExchange64(&g_pp_ticks_a, 0), tb = InterlockedExchange64(&g_pp_ticks_b, 0);
    static LARGE_INTEGER f, last_q;
    static uint64_t last_t;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    uint64_t t = __builtin_ia32_rdtsc();
    if (!f.QuadPart) { QueryPerformanceFrequency(&f); last_q = q; last_t = t; }
    double mpt = t > last_t ? (double)(q.QuadPart - last_q.QuadPart) * 1000.0 / (double)f.QuadPart / (double)(t - last_t) : 0;
    last_q = q; last_t = t;
    if (!g_pp_flip) return;
    if (frames < 1) frames = 1;
    log_line("prepare flip test: the window just logged had the soldier lists prepared %s (%.1f runs a frame, %.0f meshes each, %.0f%% by helper threads, %lld pushed in one go; shared part %.3f ms, rest %.3f ms a run)",
             g_pp_on ? "on several cores" : "the game's way", (double)runs / (double)frames, runs ? (double)meshes / (double)runs : 0.0,
             meshes ? 100.0 * (double)helpers / (double)meshes : 0.0, (long long)bulk, runs ? (double)ta * mpt / (double)runs : 0.0, runs ? (double)tb * mpt / (double)runs : 0.0);
    (void)alone;
    g_pp_on = !g_pp_on;
}

static void patch_prep_parallel(void) {
    const char *e = getenv("ATTILA_PARALLEL_PREPARE"), *h = getenv("ATTILA_PREPARE_HELPERS");
    if (e && !strcmp(e, "0")) { log_line("setting: soldier draw lists prepared the game's way (ATTILA_PARALLEL_PREPARE=0)"); return; }
    if (!g_fast_sort_on || !g_sortw_loop_orig || !g_sortw_final_orig) { log_line("setting skipped: soldier lists on several cores need the fast list sort"); return; }
    static const uint8_t prep_old[10] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53};   // 354fd00: push rbp, r15, r14, r13, r12, rbx
    static const uint8_t move_old[11] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x49, 0x89, 0xFE};   // 354aca0 and 353df40
    static const uint8_t call_prep[5] = {0xE8, 0x37, 0x00, 0x00, 0x00};   // 354fcc4: call 354fd00 (in PREPARE_TASK::execute)
    if (memcmp((void *)0x354fd00, prep_old, 10) || memcmp((void *)0x354aca0, move_old, 11) || memcmp((void *)0x353df40, move_old, 11) ||
        memcmp((void *)0x354fcc4, call_prep, 5)) {
        log_line("patch skipped, game code differs: soldier lists on several cores"); return;
    }
    if (h) { int v = atoi(h); g_pp_helpers = v < 0 ? 0 : v > 7 ? 7 : v; }
    if (getenv("ATTILA_PREPARE_BULK") && !strcmp(getenv("ATTILA_PREPARE_BULK"), "0")) g_pp_bulk = 0;
    static const uint8_t sort_value_old[9] = {0x31, 0xC0, 0x83, 0xBF, 0x70, 0x01, 0x00, 0x00, 0x00};   // 3417b00: xor eax,eax; cmp dword [rdi+170h],0
    static const uint8_t malloc_old[1] = {0x00}, push_old[11] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x50, 0x49, 0x89, 0xF6};   // 353dc30
    (void)malloc_old;
    if (g_pp_bulk && (memcmp((void *)0x3417b00, sort_value_old, 9) || memcmp((void *)0x353dc30, push_old, 11))) {
        g_pp_bulk = 0; log_line("soldier list preparation: game code differs, pushed the game's way");
    }
    g_pp_orig = (PrepNodeFn)hook_entry(0x354fd00, prep_old, 5, (void *)pp_prepare_weighted);
    if (!g_pp_orig) { log_line("patch skipped, no memory near the game: soldier lists on several cores"); return; }
    g_pp_flip = e && !strcmp(e, "flip");
    g_pp_on = 1;
    log_line("patch applied: soldier draw lists prepared on several cores (%d helper threads)%s", g_pp_helpers, g_pp_flip ? ", switched off and on with every fps line (test)" : "");
    // rigid parts (see the top)
    // OFF unless asked for (ATTILA_PARALLEL_PREPARE_RIGID=1 or =flip): measured 2026-11-13 (auto\fliprigid) it made the
    // big clash 4-20% slower. The rigid tasks run at the same time as the soldier task; when one of them took the helpers
    // first, the much larger soldier task fell back to one thread (main thread waits 0.29 -> 1.1 ms a frame). It would need
    // its own helpers or a shared job queue, for at most about 0.2 ms a frame.
    const char *r = getenv("ATTILA_PARALLEL_PREPARE_RIGID");
    if (!r || !strcmp(r, "0")) return;
    static const uint8_t rprep_old[10] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53};   // 354fb20
    static const uint8_t rmove_old[11] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x49, 0x89, 0xFE};   // 338f690
    static const uint8_t rmesh_old[10] = {0x41, 0x57, 0x41, 0x56, 0x53, 0x49, 0x89, 0xFF, 0x89, 0xF0};   // 35122f0
    static const uint8_t rcall[5] = {0xE8, 0x37, 0x00, 0x00, 0x00};   // 354fae4: call 354fb20 (in PREPARE_TASK::execute)
    if (!g_sortr_loop_orig || !g_sortr_final_orig || memcmp((void *)0x354fb20, rprep_old, 10) || memcmp((void *)0x338f690, rmove_old, 11) ||
        memcmp((void *)0x35122f0, rmesh_old, 10) || memcmp((void *)0x354fae4, rcall, 5)) {
        log_line("patch skipped, game code differs: rigid part lists on several cores"); return;
    }
    g_pp_rigid_orig = (PrepNodeFn)hook_entry(0x354fb20, rprep_old, 5, (void *)pp_prepare_rigid);
    if (!g_pp_rigid_orig) { log_line("patch skipped, no memory near the game: rigid part lists on several cores"); return; }
    g_pp_rigid_flip = r && !strcmp(r, "flip");
    g_pp_rigid_on = 1;
    log_line("patch applied: rigid part draw lists prepared on several cores%s", g_pp_rigid_flip ? ", switched off and on with every fps line (test)" : "");
}
