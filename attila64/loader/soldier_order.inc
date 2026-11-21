// Soldier draw order (measuring: ATTILA_BATCH_STATS=1). Part of loader.c.
//
// Found 2026-11-12 with draw_callers.inc: soldiers (WEIGHTED_INSTANCE) are drawn by
//   RIGID_SCENE_NODE_V2::add_list_instances<WEIGHTED_INSTANCE>        354ba30 (this, tracker, list, ?, first, end)
//   RIGID_SCENE_NODE_V2::add_list_instances_depth<WEIGHTED_INSTANCE>  354b670 (this, tracker, list, first, end)
// Both walk list->items[first..end) (items at list+8, each item's first field is its mesh) and send a draw
// (MESH::render_mesh_instances 311c230) whenever an item's mesh differs from the mesh of the item before. So items of one
// mesh are only drawn together when they stand next to each other in the list.
// Counted here per frame: items walked, places where the mesh changes (= draws the walk must send at least), and how many
// different meshes each walked range holds (= draws needed if items of one mesh stood together).
typedef void (SYSV *ListFn)(void *, void *, void *, uint64_t, uint64_t, uint64_t);
static ListFn g_list_orig, g_list_depth_orig;
static volatile unsigned long long g_so_lists[2], g_so_items[2], g_so_changes[2], g_so_distinct[2];

static void soldier_order_count(int which, const uint8_t *list, uint32_t first, uint32_t end) {
    if (!list || !*(const uint32_t *)(list + 4) || first >= end) return;
    void **const *items = *(void **const *const *)(list + 8);
    enum { SLOTS = 8192 };
    static __thread const void *seen[SLOTS];
    static __thread uint32_t stamp[SLOTS], now;
    now++;
    const void *before = NULL;
    unsigned changes = 0, distinct = 0;
    for (uint32_t i = first; i < end; i++) {
        const void *mesh = *items[i];
        if (mesh != before) { changes++; before = mesh; }
        if (distinct >= SLOTS / 2) continue;
        unsigned at = (unsigned)(((uintptr_t)mesh >> 4) * 2654435761u) % SLOTS;
        while (stamp[at] == now && seen[at] != mesh) at = (at + 1) % SLOTS;
        if (stamp[at] != now) { stamp[at] = now; seen[at] = mesh; distinct++; }
    }
    g_so_lists[which]++; g_so_items[which] += end - first; g_so_changes[which] += changes; g_so_distinct[which] += distinct;
}
static SYSV void list_counted(void *node, void *tracker, void *list, uint64_t d, uint64_t first, uint64_t end) {
    soldier_order_count(0, list, (uint32_t)first, (uint32_t)end);
    g_list_orig(node, tracker, list, d, first, end);
}
static SYSV void list_depth_counted(void *node, void *tracker, void *list, uint64_t first, uint64_t end, uint64_t f) {
    soldier_order_count(1, list, (uint32_t)first, (uint32_t)end);
    g_list_depth_orig(node, tracker, list, first, end, f);
}
// Rigid parts (RIGID_INSTANCE_V2): RIGID_MESH::add_list_instances_depth<RIGID_INSTANCE_V2> 3391de0 (mesh, tracker, list, first, end)
// is called once per mesh with that mesh's range of the list; it sends a draw when the mesh differs from the call before.
// Measured 2026-11-12 in the melee of the kept clash: this is where most one-copy draws of the main picture come from.
// Counted per frame: calls, parts, calls with one part, different meshes among the calls, calls whose mesh was already
// seen earlier in the same frame (= the same mesh visited again, not next to its first visit).
static ListFn g_rigid_orig;
extern volatile unsigned g_frame_no;
static volatile unsigned long long g_ro_calls, g_ro_items, g_ro_single, g_ro_distinct, g_ro_again;
static SYSV void rigid_counted(void *mesh, void *tracker, void *list, uint64_t first, uint64_t end, uint64_t f) {
    enum { SLOTS = 65536 };
    static const void *seen[SLOTS];
    static uint32_t stamp[SLOTS];
    static const void *before;
    uint32_t now = g_frame_no + 1, a = (uint32_t)first, b = (uint32_t)end;
    if (list && *(const uint32_t *)((const uint8_t *)list + 4) && a < b) {
        g_ro_calls++; g_ro_items += b - a;
        if (b - a == 1) g_ro_single++;
        unsigned at = (unsigned)(((uintptr_t)mesh >> 4) * 2654435761u) % SLOTS, tries = 0;
        while (stamp[at] == now && seen[at] != mesh && ++tries < SLOTS) at = (at + 1) % SLOTS;
        if (stamp[at] != now || seen[at] != mesh) { stamp[at] = now; seen[at] = mesh; g_ro_distinct++; }
        else if (before != mesh) g_ro_again++;
        before = mesh;
    }
    g_rigid_orig(mesh, tracker, list, first, end, f);
}
static void batch_sets_window(void);
static void soldier_order_report(long frames) {
    batch_sets_window();
    if (g_rigid_orig && frames > 0 && g_ro_calls) {
        log_line("rigid part lists per frame: %.0f calls, %.0f parts, %.0f calls with one part, %.0f different meshes, %.0f calls for a mesh seen earlier in the frame",
                 (double)g_ro_calls / frames, (double)g_ro_items / frames, (double)g_ro_single / frames, (double)g_ro_distinct / frames, (double)g_ro_again / frames);
        g_ro_calls = g_ro_items = g_ro_single = g_ro_distinct = g_ro_again = 0;
    }
    if (!g_list_orig || frames <= 0) return;
    for (int w = 0; w < 2; w++) {
        if (!g_so_lists[w]) continue;
        log_line("soldier lists per frame (%s): %.0f walks, %.0f parts, %.0f mesh changes, %.0f different meshes", w ? "depth and shadow" : "colour",
                 (double)g_so_lists[w] / frames, (double)g_so_items[w] / frames, (double)g_so_changes[w] / frames, (double)g_so_distinct[w] / frames);
        g_so_lists[w] = g_so_items[w] = g_so_changes[w] = g_so_distinct[w] = 0;
    }
}
static void patch_soldier_order(void) {
    if (!getenv("ATTILA_BATCH_STATS")) return;
    static const uint8_t old[10] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53};   // push rbp, r15, r14, r13, r12, rbx
    g_list_orig = (ListFn)hook_entry(0x354ba30, old, 10, (void *)list_counted);
    g_list_depth_orig = (ListFn)hook_entry(0x354b670, old, 10, (void *)list_depth_counted);
    g_rigid_orig = (ListFn)hook_entry(0x3391de0, old, 10, (void *)rigid_counted);
    if (!g_rigid_orig) log_line("patch skipped, game code differs: rigid part counting");
    if (!g_list_orig || !g_list_depth_orig) { g_list_orig = NULL; log_line("patch skipped, game code differs: soldier list counting"); }
    else log_line("soldier list counting: on");
}

// Parts with the same shader set stay in one draw (TEST, ATTILA_BATCH_SETS=1).
// Read 2026-11-12: every part carries a shader-set number (part+0x44: 0, 1 or 2; the mesh's get_technique_pass picks one of
// three technique tables with it). The walk keeps the number of the running group in tracker+0x20. Its rule: a part joins
// the running group only if the mesh is the same AND tracker+0x20 is 0; and before each part is compared the number is put
// back to 0. So a part with set 1 or 2 is always sent as a draw of its own, even between neighbours with the same set.
// Changed here in three walks (rigid parts; soldiers colour; soldiers depth/shadow): the "tracker+0x20 is not 0" jump is
// removed and the number is no longer put back to 0 for a part of the same mesh, so the game's own comparison further down
// (part+0x44 against tracker+0x20) decides: equal = same draw, different = new draw with the technique of the new set.
// Not known: why the game sent those parts alone. If sets 1 and 2 need something per part that is not in the instance
// data, the picture will be wrong; that is what the test has to show.
static int g_sets_flip, g_sets_on, g_sets_windows;
static void batch_sets_write(int on);
static void patch_batch_sets(void) {
    // On by default since 2026-11-12 (user: soldiers in a melee should be drawn grouped). ATTILA_BATCH_SETS=0 turns it off.
    const char *mode = getenv("ATTILA_BATCH_SETS");
    if (mode && !strcmp(mode, "0")) { log_line("parts with the same shader set in one draw: off (ATTILA_BATCH_SETS=0)"); return; }
    if (!mode) mode = "1";
    g_sets_flip = !strcmp(mode, "flip");   // flip: on and off in turns, two fps windows each, to compare within one run
    static const struct { uintptr_t jump; uint8_t jump_old[2]; uintptr_t zero; uint8_t zero_old[8]; int zero_len; } s[3] = {
        {0x3391ec4, {0x75, 0x1a}, 0x3391fe2, {0xc7, 0x43, 0x20, 0, 0, 0, 0}, 7},          // RIGID_MESH::add_list_instances_depth<RIGID_INSTANCE_V2>
        {0x354b728, {0x75, 0x26}, 0x354b881, {0xc7, 0x43, 0x20, 0, 0, 0, 0}, 7},          // add_list_instances_depth<WEIGHTED_INSTANCE>
        {0x354bba3, {0x75, 0x1b}, 0x354bd20, {0x41, 0xc7, 0x45, 0x20, 0, 0, 0, 0}, 8}};   // add_list_instances<WEIGHTED_INSTANCE>
    for (int i = 0; i < 3; i++)
        if (memcmp((void *)s[i].jump, s[i].jump_old, 2) || memcmp((void *)s[i].zero, s[i].zero_old, (size_t)s[i].zero_len)) { log_line("patch skipped, game code differs: parts with the same shader set in one draw"); return; }
    batch_sets_write(1);
    log_line("patch applied: parts with the same shader set stay in one draw (ATTILA_BATCH_SETS=0 turns it off%s)", g_sets_flip ? ", flipping" : "");
}
static void batch_sets_write(int on) {
    static const struct { uintptr_t at; uint8_t old[8]; int len; } w[6] = {
        {0x3391ec4, {0x75, 0x1a}, 2}, {0x3391fe2, {0xc7, 0x43, 0x20, 0, 0, 0, 0}, 7},
        {0x354b728, {0x75, 0x26}, 2}, {0x354b881, {0xc7, 0x43, 0x20, 0, 0, 0, 0}, 7},
        {0x354bba3, {0x75, 0x1b}, 2}, {0x354bd20, {0x41, 0xc7, 0x45, 0x20, 0, 0, 0, 0}, 8}};
    for (int i = 0; i < 6; i++) {
        DWORD was;
        VirtualProtect((void *)w[i].at, 8, PAGE_EXECUTE_READWRITE, &was);
        if (on) memset((void *)w[i].at, 0x90, (size_t)w[i].len); else memcpy((void *)w[i].at, w[i].old, (size_t)w[i].len);
    }
    g_sets_on = on;
}
// Called between frames, once per fps window.
static void batch_sets_window(void) {
    if (!g_sets_flip) return;
    log_line("shader sets in one draw: was %s in the window above", g_sets_on ? "ON" : "OFF");
    if (++g_sets_windows % 2 == 0) batch_sets_write(!g_sets_on);
}