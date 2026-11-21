// Faster sort of the draw lists of soldiers and rigid parts. Display only (the graphics engine's lists, nothing shared).
// Part of loader.c (included there after interface.inc, which has hook_entry).
//
// Found 2026-11-13 in a profile of the kept big clash on DirectX 11, split by view (ATTILA_PROFILE + ATTILA_PASS_STATS,
// tools\viewthreads.sh): the shadow map view takes 3.06 ms of a 10.5 ms frame, and for 46% of that time the main thread
// sleeps in tbb::ThreadTaskManager::remove_task waiting for task threads, which are busy on under 2 cores. The largest
// piece of what they do there is std::sort of the soldier lists (about a third of one core for the whole view):
//   WEIGHTED_MESH_V5::prepare_instances(VIEW_LIST_TYPE)           3418420
//   WEIGHTED_MESH_V5::prepare_instance_list(NODE_INSTANCE_LIST&)  3418380
// Both do: list.transfer_thread_lists(); std::sort(items, items + n, INSTANCE_LIST_SORTER<WEIGHTED_INSTANCE>()); then push
// every item to the node's render list. The sorter compares one unsigned 64-bit key per item (item + 0x78), read through
// the item pointer at every comparison: about 2 * n * log2(n) reads of scattered memory. Rigid parts (RIGID_INSTANCE_V2)
// are sorted the same way by the key at item + 0x50 (four places).
// In the game file std::sort is two functions: std::__introsort_loop (soldiers 353e120, rigid 3392d90; called by the
// places above and by itself) and right after it std::__final_insertion_sort on the same range (353e470 / 33931a0).
//
// Here: the first one puts the whole range in order itself. The keys are read once into (key, item) pairs, the pairs are
// sorted by key (by bytes from the lowest up, bytes that are the same in every key are skipped; equal keys keep the order
// they came in), and the items are written back. The second function is then skipped for that same range.
// The result is ascending by the same key, as before. Items with EQUAL keys can stand in another order than std::sort
// left them; std::sort promises no order for them either.
//
// ATTILA_FAST_SORT=0 leaves the game's sort alone (nothing hooked), =flip switches between the two with every fps line and
// logs the time each took (test), ATTILA_SORT_STATS=1 logs the times without switching.
typedef void (SYSV *SortLoopFn)(void **first, void **last, long depth);
typedef void (SYSV *SortFinalFn)(void **first, void **last);
typedef struct { uint64_t key; void *item; } SortPair;
static SortLoopFn g_sortw_loop_orig, g_sortr_loop_orig;
static SortFinalFn g_sortw_final_orig, g_sortr_final_orig;
static volatile int g_fast_sort_on;
static int g_fast_sort_flip, g_fast_sort_stats;
static __thread int t_sort_inside;                              // the game's own sort is running on this thread (it calls itself)
static __thread void **t_sort_done_first, **t_sort_done_last;   // the range our sort has just put in order on this thread
static __thread SortPair *t_sort_buf;                           // work space of this thread, kept for the next list
static __thread size_t t_sort_cap;
static volatile LONG64 g_sort_ticks[2], g_sort_items[2], g_sort_lists[2], g_sort_longest[2];   // [0] the game's sort, [1] ours

// Puts first[0..n) in ascending order of the unsigned 64-bit key at item + key_at. 0 = no memory, nothing changed.
static int fast_sort_range(void **first, size_t n, size_t key_at) {
    if (n < 2) return 1;
    if (n <= 32) {   // short list (the usual case: about 24 items): straight insertion, on the stack
        SortPair p[32];
        uint64_t before = 0;
        int in_order = 1;
        for (size_t i = 0; i < n; i++) {   // all keys first: these reads do not wait for each other
            uint64_t k = *(const uint64_t *)((const uint8_t *)first[i] + key_at);
            p[i].key = k; p[i].item = first[i];
            if (k < before) in_order = 0;
            before = k;
        }
        if (in_order) return 1;
        for (size_t i = 1; i < n; i++) {
            SortPair v = p[i];
            size_t j = i;
            while (j && p[j - 1].key > v.key) { p[j] = p[j - 1]; j--; }
            p[j] = v;
        }
        for (size_t i = 0; i < n; i++) first[i] = p[i].item;
        return 1;
    }
    if (n > 0x7fffffffu) return 0;
    if (t_sort_cap < 2 * n) {
        size_t cap = 2 * n + 2048;
        SortPair *nb = (SortPair *)realloc(t_sort_buf, cap * sizeof *nb);
        if (!nb) return 0;
        t_sort_buf = nb; t_sort_cap = cap;
    }
    SortPair *a = t_sort_buf, *b = a + n;
    uint32_t hist[8][256];
    memset(hist, 0, sizeof hist);
    uint64_t prev = 0;
    int sorted = 1;
    for (size_t i = 0; i < n; i++) {
        if (i + 8 < n) __builtin_prefetch((const uint8_t *)first[i + 8] + key_at);
        uint64_t k = *(const uint64_t *)((const uint8_t *)first[i] + key_at);
        a[i].key = k; a[i].item = first[i];
        if (k < prev) sorted = 0;
        prev = k;
        hist[0][k & 255]++; hist[1][k >> 8 & 255]++; hist[2][k >> 16 & 255]++; hist[3][k >> 24 & 255]++;
        hist[4][k >> 32 & 255]++; hist[5][k >> 40 & 255]++; hist[6][k >> 48 & 255]++; hist[7][k >> 56]++;
    }
    if (sorted) return 1;   // already in order: the list stays as it is
    for (int byte = 0; byte < 8; byte++) {
        uint32_t *h = hist[byte];
        int shift = 8 * byte;
        if (h[a[0].key >> shift & 255] == (uint32_t)n) continue;   // this byte is the same in every key
        uint32_t sum = 0;
        for (int v = 0; v < 256; v++) { uint32_t c = h[v]; h[v] = sum; sum += c; }
        for (size_t i = 0; i < n; i++) b[h[a[i].key >> shift & 255]++] = a[i];
        SortPair *t = a; a = b; b = t;
    }
    for (size_t i = 0; i < n; i++) first[i] = a[i].item;
    return 1;
}

static inline void sort_stat(int which, uint64_t t0, size_t n) {
    LONG64 dt = (LONG64)(__builtin_ia32_rdtsc() - t0);
    InterlockedAdd64(&g_sort_ticks[which], dt);
    InterlockedAdd64(&g_sort_items[which], (LONG64)n);
    InterlockedIncrement64(&g_sort_lists[which]);
    if (dt > g_sort_longest[which]) g_sort_longest[which] = dt;   // a rough "longest single list" for the log (no lock: test figure)
}
static void sort_loop_any(void **first, void **last, long depth, SortLoopFn orig, size_t key_at) {
    if (t_sort_inside) { orig(first, last, depth); return; }
    int timed = g_fast_sort_flip | g_fast_sort_stats;   // the clock is only read in test runs
    uint64_t t0 = timed ? __builtin_ia32_rdtsc() : 0;
    size_t n = (size_t)(last - first);
    if (g_fast_sort_on && fast_sort_range(first, n, key_at)) {
        t_sort_done_first = first; t_sort_done_last = last;
        if (timed) sort_stat(1, t0, n);
        return;
    }
    t_sort_inside = 1;
    orig(first, last, depth);
    t_sort_inside = 0;
    if (timed) sort_stat(0, t0, n);
}
static void sort_final_any(void **first, void **last, SortFinalFn orig) {
    if (t_sort_done_first == first && t_sort_done_last == last) { t_sort_done_first = t_sort_done_last = NULL; return; }
    if (!(g_fast_sort_flip | g_fast_sort_stats)) { orig(first, last); return; }
    uint64_t t0 = __builtin_ia32_rdtsc();
    orig(first, last);
    InterlockedAdd64(&g_sort_ticks[0], (LONG64)(__builtin_ia32_rdtsc() - t0));
}
static __thread int t_sort_skip_w;   // prep_parallel.inc: this thread's soldier lists were sorted already, skip their sorts
static SYSV void sortw_loop(void **f, void **l, long d) {
    if (t_sort_skip_w && !t_sort_inside) { t_sort_done_first = f; t_sort_done_last = l; return; }
    sort_loop_any(f, l, d, g_sortw_loop_orig, 0x78);
}
static SYSV void sortr_loop(void **f, void **l, long d) { sort_loop_any(f, l, d, g_sortr_loop_orig, 0x50); }
static SYSV void sortw_final(void **f, void **l) { sort_final_any(f, l, g_sortw_final_orig); }
static SYSV void sortr_final(void **f, void **l) { sort_final_any(f, l, g_sortr_final_orig); }

// With each fps line (shim_gfx.c), tests only: what the sorting of the window just logged cost, and the flip.
void fast_sort_report(long frames) {
    if (!g_fast_sort_flip && !g_fast_sort_stats) return;
    static LARGE_INTEGER f, last_q;
    static uint64_t last_t;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    uint64_t t = __builtin_ia32_rdtsc();
    if (!f.QuadPart) { QueryPerformanceFrequency(&f); last_q = q; last_t = t; }
    double ms_per_tick = t > last_t ? (double)(q.QuadPart - last_q.QuadPart) * 1000.0 / (double)f.QuadPart / (double)(t - last_t) : 0;
    last_q = q; last_t = t;
    if (frames < 1) frames = 1;
    for (int w = 0; w < 2; w++) {
        LONG64 ticks = InterlockedExchange64(&g_sort_ticks[w], 0), items = InterlockedExchange64(&g_sort_items[w], 0);
        LONG64 lists = InterlockedExchange64(&g_sort_lists[w], 0), longest = InterlockedExchange64(&g_sort_longest[w], 0);
        if (!lists) continue;
        log_line("list sort, %s: %.3f ms a frame on all threads together (%lld lists and %lld items a frame, longest single list %.3f ms)",
                 w ? "the fast sort" : "the game's sort", (double)ticks * ms_per_tick / (double)frames, (long long)(lists / frames), (long long)(items / frames), (double)longest * ms_per_tick);
    }
    if (g_fast_sort_flip) {
        log_line("sort flip test: the window just logged had %s", g_fast_sort_on ? "the fast sort" : "the game's sort");
        g_fast_sort_on = !g_fast_sort_on;
    }
}

static void patch_fast_sort(void) {
    const char *e = getenv("ATTILA_FAST_SORT");
    if (e && !strcmp(e, "0")) { log_line("setting: draw lists sorted the game's way (ATTILA_FAST_SORT=0)"); return; }
    static const uint8_t loop_old[16] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x49, 0x89, 0xD6, 0x49, 0x89, 0xFF, 0x48};
    static const uint8_t final_old[16] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x49, 0x89, 0xF6, 0x49, 0x89, 0xFF, 0x4C};
    static const uint8_t w_key[4] = {0x49, 0x8B, 0x4D, 0x78};   // 353e4a9: mov rcx,[r13+78h]  (the soldier key)
    static const uint8_t r_key[4] = {0x49, 0x8B, 0x4D, 0x50};   // 33931d9: mov rcx,[r13+50h]  (the rigid part key)
    static const uint8_t call_w1[5] = {0xE8, 0x5C, 0x5D, 0x12, 0x00};   // 34183bf: call 353e120 (prepare_instance_list)
    static const uint8_t call_w2[5] = {0xE8, 0x93, 0x5C, 0x12, 0x00};   // 3418488: call 353e120 (prepare_instances)
    if (memcmp((void *)0x353e120, loop_old, 16) || memcmp((void *)0x353e470, final_old, 16) || memcmp((void *)0x353e4a9, w_key, 4) ||
        memcmp((void *)0x3392d90, loop_old, 16) || memcmp((void *)0x33931a0, final_old, 16) || memcmp((void *)0x33931d9, r_key, 4) ||
        memcmp((void *)0x34183bf, call_w1, 5) || memcmp((void *)0x3418488, call_w2, 5)) {
        log_line("patch skipped, game code differs: fast list sort"); return;
    }
    g_sortw_loop_orig = (SortLoopFn)hook_entry(0x353e120, loop_old, 6, (void *)sortw_loop);
    g_sortw_final_orig = (SortFinalFn)hook_entry(0x353e470, final_old, 6, (void *)sortw_final);
    g_sortr_loop_orig = (SortLoopFn)hook_entry(0x3392d90, loop_old, 6, (void *)sortr_loop);
    g_sortr_final_orig = (SortFinalFn)hook_entry(0x33931a0, final_old, 6, (void *)sortr_final);
    if (!g_sortw_loop_orig || !g_sortw_final_orig || !g_sortr_loop_orig || !g_sortr_final_orig) {
        log_line("patch skipped, no memory near the game: fast list sort (the game's sort stays in use)"); return;
    }
    g_fast_sort_flip = e && !strcmp(e, "flip");
    g_fast_sort_stats = getenv("ATTILA_SORT_STATS") != NULL;
    g_fast_sort_on = 1;
    log_line("patch applied: fast sort of the soldier and rigid part draw lists%s", g_fast_sort_flip ? ", switched off and on with every fps line (test)" : "");
}
