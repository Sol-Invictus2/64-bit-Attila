// Measuring only (ATTILA_PASS_STATS=1): where a frame's drawing goes, step by step. Nothing here runs without the switch.
// Part of loader.c (included there before shadow_rate.inc, which holds the one hook on the view function).
//
// The engine draws a frame as a list of "views" (SHADOW_MAP_VIEW, ZPREFILL_VIEW, GBUFFER_VIEW, ... about 90 kinds), each run
// by WARSCAPE::SCENE_VIEW::process_internal (30ce260). With the switch on, each run goes through view_process_timed: for
// each kind of view it adds up how often it ran, the time spent in it (not counting views it runs inside itself) and the
// draw calls made meanwhile (g_gl_draw_total, counted in shim_gfx.c, which also sorts the draws of the shadow map and of
// the main picture by size). The kind's name comes from the C++ type information the Linux game file carries (object ->
// table of functions -> type record -> name). With every fps line the totals are logged per frame. With ATTILA_PROFILE=1
// as well, the profiler marks its samples with the view that was running (g_view_kind; tools\viewsum.sh).
typedef void (SYSV *ViewProcessFn)(uint8_t *view, void *ctx, void *viewport, void *target, uint32_t face, uint32_t a, uint32_t b);
static ViewProcessFn g_view_process_orig;   // set by shadow_rate.inc
static int g_pass_stats_on;
extern volatile unsigned long long g_gl_draw_total, g_draw_hist[3][4][3], g_draw_indices[3], g_draw_instances[3];
extern volatile int g_view_kind;

typedef struct { const char *name; uint64_t calls, ticks, draws; } ViewStat;
static ViewStat g_view_stats[128];
static struct { int64_t start, inner; uint64_t draws0, inner_draws; } g_view_stack[16];
static int g_view_depth;
static DWORD g_view_thread;

// ATTILA_VIEW_ORDER=n (with ATTILA_PASS_STATS=1): for frame n and again every 2000 frames, logs every view in the order it
// runs, with the framebuffer and viewport OpenGL has when it ends (0 = the window itself). Shows where the 3D picture ends
// and the interface begins.
static unsigned g_view_order_frame;
static void view_order_line(const char *name, int depth) {
    extern volatile unsigned g_frame_no;
    if (!g_view_order_frame || g_frame_no < g_view_order_frame || (g_frame_no - g_view_order_frame) % 2000) return;
    static void (__stdcall *get)(unsigned, int *);
    if (!get) get = (void (__stdcall *)(unsigned, int *))GetProcAddress(GetModuleHandleA("opengl32.dll"), "glGetIntegerv");
    int fbo = -1, vp[4] = {0, 0, 0, 0};
    if (get) { get(0x8CA6, &fbo); get(0x0BA2, vp); }   // GL_DRAW_FRAMEBUFFER_BINDING, GL_VIEWPORT
    log_line("view order: %*s%s -> framebuffer %d, viewport %d,%d %dx%d", depth * 2, "", name, fbo, vp[0], vp[1], vp[2], vp[3]);
}

static ViewStat *view_stat(uint8_t *view) {
    const char *name = "?";
    void **vtable = *(void ***)view;
    const uint8_t *type = (const uint8_t *)vtable[-1];
    if ((uintptr_t)type >= 0x400000 && (uintptr_t)type < 0x5280000) name = *(const char *const *)(type + 8);
    for (int i = 0; i < 128; i++) {
        if (g_view_stats[i].name == name) return &g_view_stats[i];
        if (!g_view_stats[i].name) { g_view_stats[i].name = name; return &g_view_stats[i]; }
    }
    return &g_view_stats[127];
}

static void view_process_timed(uint8_t *view, void *ctx, void *viewport, void *target, uint32_t face, uint32_t a, uint32_t b) {
    DWORD me = GetCurrentThreadId();
    if (!g_view_thread) g_view_thread = me;
    if (me != g_view_thread || g_view_depth >= 16) { g_view_process_orig(view, ctx, viewport, target, face, a, b); return; }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    int d = g_view_depth++;
    g_view_stack[d].start = t.QuadPart;
    g_view_stack[d].inner = 0;
    g_view_stack[d].draws0 = g_gl_draw_total;
    g_view_stack[d].inner_draws = 0;
    ViewStat *s = view_stat(view);
    int kind_before = g_view_kind;
    g_view_kind = strstr(s->name, "15SHADOW_MAP_VIEWE") ? 1 : strstr(s->name, "12GBUFFER_VIEWE") ? 2 : 0;
    g_view_process_orig(view, ctx, viewport, target, face, a, b);
    g_view_kind = kind_before;
    view_order_line(s->name, d);
    QueryPerformanceCounter(&t);
    g_view_depth = d;
    int64_t all = t.QuadPart - g_view_stack[d].start;
    uint64_t draws = g_gl_draw_total - g_view_stack[d].draws0;
    s->calls++;
    s->ticks += (uint64_t)(all - g_view_stack[d].inner);
    s->draws += draws - g_view_stack[d].inner_draws;
    if (d > 0) { g_view_stack[d - 1].inner += all; g_view_stack[d - 1].inner_draws += draws; }
}

// Called with each fps line (shim_gfx.c): the views that took the most time, per frame, then everything starts again.
void pass_stats_report(long frames) {
    if (!g_pass_stats_on || frames <= 0) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    static uint64_t last_draws;
    uint64_t all_draws = g_gl_draw_total - last_draws, in_views = 0, ticks = 0;
    last_draws = g_gl_draw_total;
    int order[128], n = 0;
    for (int i = 0; i < 128 && g_view_stats[i].name; i++) { if (g_view_stats[i].calls) order[n++] = i; in_views += g_view_stats[i].draws; ticks += g_view_stats[i].ticks; }
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && g_view_stats[order[j]].ticks > g_view_stats[order[j - 1]].ticks; j--) { int x = order[j]; order[j] = order[j - 1]; order[j - 1] = x; }
    log_line("frame steps, per frame over %ld frames: %.2f ms in views, %.0f draw calls (%.0f inside views)", frames,
             (double)ticks * 1000.0 / (double)f.QuadPart / frames, (double)all_draws / frames, (double)in_views / frames);
    for (int k = 0; k < n && k < 14; k++) {
        ViewStat *s = &g_view_stats[order[k]];
        const char *name = s->name;
        if (!strncmp(name, "N8WARSCAPE", 10)) { name += 10; while (*name >= '0' && *name <= '9') name++; }
        log_line("  step %-44.44s %6.2f ms  %6.0f draws  %5.1f runs", name, (double)s->ticks * 1000.0 / (double)f.QuadPart / frames,
                 (double)s->draws / frames, (double)s->calls / frames);
    }
    static const char *const kinds[3] = {"other", "shadow map", "main picture"};
    for (int k = 1; k < 3; k++) {
        log_line("  %s draws by size (1 copy / 2-9 / 10+): tiny %.0f/%.0f/%.0f, small %.0f/%.0f/%.0f, medium %.0f/%.0f/%.0f, big %.0f/%.0f/%.0f; %.0f copies, %.2f million triangles", kinds[k],
                 (double)g_draw_hist[k][0][0] / frames, (double)g_draw_hist[k][0][1] / frames, (double)g_draw_hist[k][0][2] / frames,
                 (double)g_draw_hist[k][1][0] / frames, (double)g_draw_hist[k][1][1] / frames, (double)g_draw_hist[k][1][2] / frames,
                 (double)g_draw_hist[k][2][0] / frames, (double)g_draw_hist[k][2][1] / frames, (double)g_draw_hist[k][2][2] / frames,
                 (double)g_draw_hist[k][3][0] / frames, (double)g_draw_hist[k][3][1] / frames, (double)g_draw_hist[k][3][2] / frames,
                 (double)g_draw_instances[k] / frames, (double)g_draw_indices[k] / 3e6 / frames);
    }
    memset((void *)g_draw_hist, 0, sizeof g_draw_hist); memset((void *)g_draw_indices, 0, sizeof g_draw_indices); memset((void *)g_draw_instances, 0, sizeof g_draw_instances);
    for (int i = 0; i < 128; i++) g_view_stats[i].calls = g_view_stats[i].ticks = g_view_stats[i].draws = 0;
}
