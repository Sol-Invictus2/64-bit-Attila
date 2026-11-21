// Shadow refresh: redraw the shadow map only every nth frame while the camera stands still. Display only.
// Part of loader.c (included there after pass_stats.inc; the order of the includes matters).
//
// Shadows are the largest single cost of a frame. In the game's own benchmark at maximum shadows (RX 9070 XT, 9800X3D,
// 2026-11-11): 128 fps with them, 217 without; the shadow map view alone is 2.6 ms of an 8 ms frame, and most of that is not
// draw calls but going through every object again for each of up to four shadow levels. Same benchmark, this file: every
// frame 133 fps, every 2nd 150, every 4th 167; a saved replay with a still camera 233 / 246 / 249.
//
// How: every view is run by WARSCAPE::SCENE_VIEW::process_internal (30ce260), which is hooked here (view_process). On a
// frame where a shadow map view rests, the game's function is not called for it, and SHADOW_MAP_VIEW::set_scene_camera
// (30e5340, which the view's begin_render calls to place the level cameras) does nothing. So the shadow map, the level
// cameras and the shader values written at the end of SHADOW_MAP_VIEW::draw all stay exactly as the last drawn frame left
// them, and the lighting keeps reading a shadow map that matches its matrices: shadows stay in the right place in the
// world, they are only up to n-1 frames old (a moving soldier's shadow trails him by that much).
//
// A view rests only if it was drawn less than n frames ago AND the scene camera is still about where it was at that drawn
// frame (position and the point it looks at, each within g_shadow_still metres). So a still or gliding camera rests (the
// benchmark's fly-through stays under 0.25 m a frame), and a cut, a jump or a fast sweep redraws at once, because the
// shadow levels are fitted to what the camera sees. Each shadow view (battle, campaign map, unit
// portraits) is tracked by itself, so a new one is always drawn first.
//
// Chosen in the start window ("Shadow refresh", settings.txt shadow_refresh=n, picker.c); 1 = every frame = the game as it
// was, and then nothing here is hooked unless ATTILA_PASS_STATS wants the view hook. ATTILA_SHADOW_EVERY=n and
// ATTILA_SHADOW_STILL=metres are for tests.
#define SHADOW_VIEW_VTABLE 0x3becf90u   // what a SHADOW_MAP_VIEW object starts with (vtable for WARSCAPE::SHADOW_MAP_VIEW + 0x10)
extern volatile unsigned g_frame_no;   // shim_gfx.c: frames shown so far
// The same hook tells the upscaling (render_scale.inc, ATTILA_UPSCALE) when the 2D interface begins: objects that start with
// the table of functions of WARSCAPE::UI_OVERLAY_VIEW or UI_ALTERNATE_VIEW (vtable + 0x10).
#define UI_OVERLAY_VTABLE 0x3bec2f0u
#define UI_ALTERNATE_VTABLE 0x3beba20u
extern void upscale_interface_begins(void);
static int g_upscale_hook;
static unsigned g_shadow_every = 1;
static float g_shadow_still = 0.25f;
typedef struct { uint8_t *view; unsigned drawn; float camera[6]; } ShadowView;
static ShadowView g_shadow_views[8];
static volatile long g_shadow_rested, g_shadow_drawn;
typedef void (SYSV *ViewCameraFn)(uint8_t *view);
static ViewCameraFn g_shadow_camera_orig;

// The scene camera the view belongs to, asked for the way set_scene_camera itself does: position (3 numbers) then the
// point it looks at (3 numbers), at +0x10 of the camera.
static const float *shadow_scene_camera(uint8_t *view) {
    uint8_t *scene = *(uint8_t **)(view + 0x78);
    return ((SYSV const float *(*)(uint8_t *))(*(void ***)scene)[0x58 / 8])(scene) + 4;
}

static int shadow_view_rests(uint8_t *view) {
    if (g_shadow_every < 2) return 0;
    for (int i = 0; i < 8; i++) {
        ShadowView *s = &g_shadow_views[i];
        if (s->view != view) continue;
        if (g_frame_no - s->drawn >= g_shadow_every) return 0;
        const float *c = shadow_scene_camera(view);
        for (int k = 0; k < 6; k++) { float d = c[k] - s->camera[k]; if (d > g_shadow_still || d < -g_shadow_still) return 0; }
        return 1;
    }
    return 0;   // not seen before: draw it
}

static void shadow_view_drawn(uint8_t *view) {
    int slot = 0;
    for (int i = 0; i < 8; i++) {
        if (g_shadow_views[i].view == view) { slot = i; break; }
        if (g_frame_no - g_shadow_views[i].drawn > g_frame_no - g_shadow_views[slot].drawn) slot = i;   // else reuse the one drawn longest ago
    }
    g_shadow_views[slot].view = view;
    g_shadow_views[slot].drawn = g_frame_no;
    memcpy(g_shadow_views[slot].camera, shadow_scene_camera(view), sizeof g_shadow_views[slot].camera);
}

static SYSV void view_process(uint8_t *view, void *ctx, void *viewport, void *target, uint32_t face, uint32_t a, uint32_t b) {
    if (g_shadow_every >= 2 && *(uintptr_t *)view == SHADOW_VIEW_VTABLE) {
        if (shadow_view_rests(view)) { g_shadow_rested++; return; }
        shadow_view_drawn(view);
        g_shadow_drawn++;
    }
    if (g_upscale_hook && (*(uintptr_t *)view == UI_OVERLAY_VTABLE || *(uintptr_t *)view == UI_ALTERNATE_VTABLE)) upscale_interface_begins();
    if (g_pass_stats_on) view_process_timed(view, ctx, viewport, target, face, a, b);
    else g_view_process_orig(view, ctx, viewport, target, face, a, b);
}

static SYSV void shadow_camera_or_rest(uint8_t *view) {
    if (!shadow_view_rests(view)) g_shadow_camera_orig(view);
}

// With each fps line (shim_gfx.c): how often the shadow map rested, so a log shows the setting at work.
// test (ATTILA_SHADOW_FLIP=1): the setting is switched off and on again with every fps line, so one run of a steady
// scene shows both next to each other (run-to-run comparisons drown in noise when the PC is in use).
static unsigned g_shadow_flip_to;
void shadow_rate_report(long frames) {
    if (g_shadow_flip_to) {
        log_line("shadow flip test: the window just logged had the shadow map redrawn every %u frames (%ld drawn, %ld left, %ld frames)", g_shadow_every < 2 ? 1u : g_shadow_every, g_shadow_drawn, g_shadow_rested, frames);
        g_shadow_every = g_shadow_every >= 2 ? 1 : g_shadow_flip_to;
        g_shadow_rested = g_shadow_drawn = 0;
        return;
    }
    if (g_shadow_every < 2 || !(g_shadow_rested + g_shadow_drawn)) return;
    log_line("shadow map: drawn %ld times, left as it was %ld times, in %ld frames", g_shadow_drawn, g_shadow_rested, frames);
    g_shadow_rested = g_shadow_drawn = 0;
}

static void patch_shadow_rate(void) {
    extern int settings_shadow_every(void);   // picker.c: the start window's "Shadow refresh" choice
    const char *e = getenv("ATTILA_SHADOW_EVERY"), *still = getenv("ATTILA_SHADOW_STILL");
    unsigned every = e ? (unsigned)atoi(e) : (unsigned)settings_shadow_every();
    if (every < 1 || every > 8) every = 1;
    if (still && atof(still) >= 0) g_shadow_still = (float)atof(still);
    g_pass_stats_on = getenv("ATTILA_PASS_STATS") != NULL;
    if (getenv("ATTILA_VIEW_ORDER")) g_view_order_frame = (unsigned)atoi(getenv("ATTILA_VIEW_ORDER"));
    g_upscale_hook = getenv("ATTILA_UPSCALE") != NULL;
    if (every < 2 && !g_pass_stats_on && !g_upscale_hook) return;
    static const uint8_t frame5[5] = {0x55, 0x41, 0x57, 0x41, 0x56};   // both functions begin: push rbp; push r15; push r14
    // a shadow view's table of functions must be where it was studied: its set_scene_camera entry is the function hooked below
    if (every >= 2 && *(const uint64_t *)(uintptr_t)(SHADOW_VIEW_VTABLE + 0xd8) != 0x30e5340) {
        log_line("patch skipped, game code differs: shadow refresh");
        every = 1;
        if (!g_pass_stats_on && !g_upscale_hook) return;
    }
    g_view_process_orig = (ViewProcessFn)hook_entry(0x30ce260, frame5, 5, (void *)view_process);
    if (!g_view_process_orig) { g_pass_stats_on = 0; g_upscale_hook = 0; log_line("patch skipped, game code differs: view hook (shadow refresh, frame step measuring)"); return; }
    if (g_pass_stats_on) log_line("frame step measuring: on");
    if (every < 2) return;
    g_shadow_camera_orig = (ViewCameraFn)hook_entry(0x30e5340, frame5, 5, (void *)shadow_camera_or_rest);
    if (!g_shadow_camera_orig) { log_line("patch skipped, game code differs: shadow refresh"); return; }
    g_shadow_every = every;
    if (getenv("ATTILA_SHADOW_FLIP")) g_shadow_flip_to = every;
    log_line("patch applied: shadow map redrawn every %u frames while the camera is still (within %.2f m)%s", every, (double)g_shadow_still, g_shadow_flip_to ? ", switched off and on with every fps line (test)" : "");
}
