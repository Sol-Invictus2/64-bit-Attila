// Upscaling (TEST, ATTILA_UPSCALE=1): 3D picture at the game's resolution, interface at the window's.
// Part of shim_gfx.c (included at its end).
//
// What the game does by itself (seen 2026-11-12): the resolution picked in its options is only the size of its frame targets.
// The window stays as large as the screen, and at the end of a frame the game copies its finished frame (its own framebuffer
// object, "the frame") to the window with glBlitFramebuffer, stretched. It also maps the mouse from window to frame. So a
// lower resolution already means "3D drawn smaller and stretched", but the interface is drawn small and stretched with it.
//
// Here, when the frame is smaller than the window:
//  1. The 3D picture and everything in it are drawn as before, into the frame.
//  2. When the 2D interface begins (the engine's UI_OVERLAY_VIEW / UI_ALTERNATE_VIEW; loader.c's view hook calls
//     upscale_interface_begins), the frame is copied, stretched, into a second frame of ours as large as the window, and from
//     then on every "draw into the frame" of the game lands in ours instead, with its viewports and scissor boxes made larger
//     by window / frame. The interface is still laid out for the frame's size but every pixel of it is drawn at full size.
//  3. At the game's copy to the window, ours is copied instead, 1:1, and everything goes back to normal for the next frame.
// The game keeps its own record of what OpenGL has bound and set and skips calls it thinks are not needed. So whatever is
// changed behind its back here (bound framebuffer, viewport, scissor box, scissor test) is put back exactly at step 3.
// Stage 3 (not here yet): sharpen the stretched 3D picture (AMD FSR 1) instead of the plain stretch in step 2.
#define UP_FRAMEBUFFER 0x8D40
#define UP_READ 0x8CA8
#define UP_DRAW 0x8CA9
static int g_up_on, g_up_trace, g_up_logged;
static unsigned g_up_frame_fbo;                 // the game's frame (learnt from its copy to the window), 0 = not known yet
static int g_up_fw, g_up_fh, g_up_ww, g_up_wh;  // frame size, window size (from that copy)
static unsigned g_up_big_fbo, g_up_big_tex;     // ours, window size
static int g_up_big_w, g_up_big_h;
static int g_up_phase;                          // 1 = the interface is being drawn into ours
static int g_up_view[4], g_up_scissor[4];       // the viewport and scissor box the game last asked for (its own numbers)
static unsigned g_up_draw, g_up_read;           // the framebuffers the game last bound for drawing and reading
static int g_up_scissor_on;                     // the scissor test as the game last set it
// None of this is asked from OpenGL while frames are drawn: a question makes the driver wait for its own drawing thread
// (measured: 211 fps with questions against 235 without the upscaling at all). Every call that changes it passes through here.
static int g_up_view_set, g_up_scissor_set;
static unsigned long long g_up_frames, g_up_plain;

static void (__stdcall *up_get)(unsigned, int *);
static void (__stdcall *up_viewport)(int, int, int, int);
static void (__stdcall *up_scissor)(int, int, int, int);
static void (__stdcall *up_bind_fbo)(unsigned, unsigned);
static void (__stdcall *up_blit)(int, int, int, int, int, int, int, int, unsigned, unsigned);
static void (__stdcall *up_gen_fbo)(int, unsigned *);
static void (__stdcall *up_gen_tex)(int, unsigned *);
static void (__stdcall *up_bind_tex)(unsigned, unsigned);
static void (__stdcall *up_tex_image)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
static void (__stdcall *up_tex_parami)(unsigned, unsigned, int);
static void (__stdcall *up_fbo_tex)(unsigned, unsigned, unsigned, unsigned, int);
static unsigned (__stdcall *up_fbo_status)(unsigned);
static unsigned char (__stdcall *up_is_enabled)(unsigned);
static void (__stdcall *up_enable)(unsigned);
static void (__stdcall *up_disable)(unsigned);
static void (__stdcall *up_delete_tex)(int, const unsigned *);

static void up_note(const char *what, int a, int b, int c, int d) {
    if (g_up_trace && g_up_logged++ < 60) log_line("upscale: %s %d,%d %d,%d", what, a, b, c, d);
}
static int up_x(int v) { return (int)((long long)v * g_up_ww / g_up_fw); }
static int up_y(int v) { return (int)((long long)v * g_up_wh / g_up_fh); }
static int up_draw_binding(void) { return (int)g_up_draw; }
static int up_read_binding(void) { return (int)g_up_read; }
static int up_real_draw_is_big;   // OpenGL really draws into our frame while the game believes it draws into its own

void upscale_start(void) {
    g_up_trace = getenv("ATTILA_RENDER_TRACE") != NULL;
    g_up_on = getenv("ATTILA_UPSCALE") != NULL;
    if (g_up_on) log_line("upscale: on (test, ATTILA_UPSCALE): interface drawn at the window's size when the game's resolution is lower");
}

static int up_ready(void) {
    if (up_get) return 1;
    up_get = gl_real("glGetIntegerv"); up_gen_fbo = gl_real("glGenFramebuffers");
    up_gen_tex = gl_real("glGenTextures"); up_bind_tex = gl_real("glBindTexture"); up_tex_image = gl_real("glTexImage2D");
    up_tex_parami = gl_real("glTexParameteri"); up_fbo_tex = gl_real("glFramebufferTexture2D"); up_fbo_status = gl_real("glCheckFramebufferStatus");
    up_is_enabled = gl_real("glIsEnabled"); up_delete_tex = gl_real("glDeleteTextures");
    if (!up_enable) up_enable = gl_real("glEnable");
    if (!up_disable) up_disable = gl_real("glDisable");
    if (!up_bind_fbo) up_bind_fbo = gl_real("glBindFramebuffer");
    if (!up_viewport) up_viewport = gl_real("glViewport");
    if (!up_scissor) up_scissor = gl_real("glScissor");
    if (!up_blit) up_blit = gl_real("glBlitFramebuffer");
    return up_get && up_bind_fbo && up_gen_fbo && up_gen_tex && up_bind_tex && up_tex_image && up_tex_parami && up_fbo_tex && up_fbo_status &&
           up_is_enabled && up_enable && up_disable && up_delete_tex && up_viewport && up_scissor && up_blit;
}

// Our window-sized frame, made or remade when the window's size changes. The texture binding the game believes in is put back.
static int up_big_frame(void) {
    if (g_up_big_fbo && g_up_big_w == g_up_ww && g_up_big_h == g_up_wh) return 1;
    int tex_before = 0, draw_before = up_draw_binding(), read_before = up_read_binding();
    lazy_unit_flush();   // the texture binding below belongs to the unit the game selected last
    up_get(0x8069, &tex_before);   // GL_TEXTURE_BINDING_2D
    if (g_up_big_tex) up_delete_tex(1, &g_up_big_tex);
    if (!g_up_big_fbo) up_gen_fbo(1, &g_up_big_fbo);
    up_gen_tex(1, &g_up_big_tex);
    up_bind_tex(0x0DE1, g_up_big_tex);
    up_tex_image(0x0DE1, 0, 0x8058 /* RGBA8 */, g_up_ww, g_up_wh, 0, 0x1908, 0x1401, NULL);
    up_tex_parami(0x0DE1, 0x2801, 0x2601); up_tex_parami(0x0DE1, 0x2800, 0x2601);   // linear
    up_tex_parami(0x0DE1, 0x2802, 0x812F); up_tex_parami(0x0DE1, 0x2803, 0x812F);   // clamp to edge
    up_bind_tex(0x0DE1, (unsigned)tex_before);
    up_bind_fbo(UP_FRAMEBUFFER, g_up_big_fbo);
    up_fbo_tex(UP_FRAMEBUFFER, 0x8CE0, 0x0DE1, g_up_big_tex, 0);
    unsigned status = up_fbo_status(UP_FRAMEBUFFER);
    up_bind_fbo(UP_DRAW, (unsigned)draw_before); up_bind_fbo(UP_READ, (unsigned)read_before);   // not in the phase here: belief = real
    g_up_big_w = g_up_ww; g_up_big_h = g_up_wh;
    log_line("upscale: full-size frame %dx%d for a game frame of %dx%d, status %x%s", g_up_ww, g_up_wh, g_up_fw, g_up_fh, status, status == 0x8CD5 ? "" : " (NOT complete: upscale off)");
    if (status != 0x8CD5) { g_up_on = 0; return 0; }
    return 1;
}

// The window's real size, as SDL gives it to the game (before the first frame is drawn).
static int g_up_sdl_w, g_up_sdl_h;
SHIM(void, SDL_GetWindowSize, (void *w, int *x, int *y)) {
    static void (__cdecl *r)(void *, int *, int *);
    if (!r) r = (void *)GetProcAddress(sdl_dll(), "SDL_GetWindowSize");
    int rw = 0, rh = 0;
    r(w, &rw, &rh);
    if (rw > 0 && rh > 0) { if (g_up_trace && (rw != g_up_sdl_w || rh != g_up_sdl_h)) log_line("upscale: SDL says the window is %dx%d", rw, rh); g_up_sdl_w = rw; g_up_sdl_h = rh; }
    if (x) *x = rw;
    if (y) *y = rh;
}

// For the interface size (interface.inc): with upscaling on, the interface is laid out for the window, not for the game's
// smaller frame, so it looks exactly as it does without upscaling. 1 = sizes given.
int upscale_window_size(uint32_t *w, uint32_t *h) {
    if (!g_up_on || g_up_sdl_w <= 0 || g_up_sdl_h <= 0) return 0;
    *w = (uint32_t)g_up_sdl_w; *h = (uint32_t)g_up_sdl_h;
    return 1;
}

// loader.c's view hook: the 2D interface is about to be drawn. Called on the thread that draws, before the view runs.
void upscale_interface_begins(void) {
    if (!g_up_on || g_up_phase || !g_up_frame_fbo || g_up_fw >= g_up_ww || !up_ready() || !up_big_frame()) return;
    int draw_before = up_draw_binding(), read_before = up_read_binding();
    int scissor_on = g_up_scissor_on;
    if (scissor_on) up_disable(0x0C11);   // a copy obeys the scissor box
    up_bind_fbo(UP_READ, g_up_frame_fbo);
    up_bind_fbo(UP_DRAW, g_up_big_fbo);
    up_blit(0, 0, g_up_fw, g_up_fh, 0, 0, g_up_ww, g_up_wh, 0x4000, 0x2601);   // colour, linear: the 3D picture, stretched
    if (scissor_on) up_enable(0x0C11);
    up_bind_fbo(UP_READ, (unsigned)read_before);
    up_bind_fbo(UP_DRAW, (unsigned)draw_before == g_up_frame_fbo ? g_up_big_fbo : (unsigned)draw_before);
    up_real_draw_is_big = (unsigned)draw_before == g_up_frame_fbo;
    g_up_view_set = g_up_scissor_set = 0;
    g_up_phase = 1;
    if ((unsigned)draw_before == g_up_frame_fbo) {
        up_viewport(up_x(g_up_view[0]), up_y(g_up_view[1]), up_x(g_up_view[2]), up_y(g_up_view[3]));
        up_scissor(up_x(g_up_scissor[0]), up_y(g_up_scissor[1]), up_x(g_up_scissor[2]), up_y(g_up_scissor[3]));
        g_up_view_set = g_up_scissor_set = 1;
    }
    up_note("interface begins; frame bound / viewport width", draw_before, g_up_view[2], g_up_view[3], scissor_on);
}

// The end of the phase: OpenGL gets back what the game believes it has.
static void up_phase_ends(void) {
    g_up_phase = 0;
    if (up_real_draw_is_big) up_bind_fbo(UP_DRAW, g_up_draw);
    up_real_draw_is_big = 0;
    if (g_up_view_set) up_viewport(g_up_view[0], g_up_view[1], g_up_view[2], g_up_view[3]);
    if (g_up_scissor_set) up_scissor(g_up_scissor[0], g_up_scissor[1], g_up_scissor[2], g_up_scissor[3]);
}

SYSV static void w_up_bind_fbo(unsigned target, unsigned id) {
    if (target != UP_READ) g_up_draw = id;
    if (target != UP_DRAW) g_up_read = id;
    if (g_up_phase && id == g_up_frame_fbo && target != UP_READ) {
        up_bind_fbo(UP_DRAW, g_up_big_fbo);
        if (target == UP_FRAMEBUFFER) up_bind_fbo(UP_READ, id);
        up_real_draw_is_big = 1;
        // the game may not repeat a viewport it believes is set: make the real one fit ours now
        up_viewport(up_x(g_up_view[0]), up_y(g_up_view[1]), up_x(g_up_view[2]), up_y(g_up_view[3]));
        up_scissor(up_x(g_up_scissor[0]), up_y(g_up_scissor[1]), up_x(g_up_scissor[2]), up_y(g_up_scissor[3]));
        g_up_view_set = g_up_scissor_set = 1;
        return;
    }
    if (g_up_phase && target != UP_READ && (g_up_view_set || g_up_scissor_set)) {
        // drawing somewhere else during the phase (an interface texture): the game's own numbers again
        up_viewport(g_up_view[0], g_up_view[1], g_up_view[2], g_up_view[3]);
        up_scissor(g_up_scissor[0], g_up_scissor[1], g_up_scissor[2], g_up_scissor[3]);
        g_up_view_set = g_up_scissor_set = 0;
    }
    if (target != UP_READ) up_real_draw_is_big = 0;
    up_bind_fbo(target, id);
}

SYSV static void w_up_viewport(int x, int y, int w, int h) {
    g_up_view[0] = x; g_up_view[1] = y; g_up_view[2] = w; g_up_view[3] = h;
    if (g_up_phase && up_real_draw_is_big) { up_viewport(up_x(x), up_y(y), up_x(w), up_y(h)); g_up_view_set = 1; return; }
    up_viewport(x, y, w, h);
}

SYSV static void w_up_scissor(int x, int y, int w, int h) {
    g_up_scissor[0] = x; g_up_scissor[1] = y; g_up_scissor[2] = w; g_up_scissor[3] = h;
    if (g_up_phase && up_real_draw_is_big) { up_scissor(up_x(x), up_y(y), up_x(w), up_y(h)); g_up_scissor_set = 1; return; }
    up_scissor(x, y, w, h);
}

SYSV static void w_up_blit(int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1, unsigned mask, unsigned filter) {
    if (up_ready() && up_draw_binding() == 0) {   // the game's copy of its finished frame to the window
        g_up_frame_fbo = (unsigned)up_read_binding();
        g_up_fw = sx1 - sx0; g_up_fh = sy1 - sy0; g_up_ww = dx1 - dx0; g_up_wh = dy1 - dy0;
        up_note("copy to the window: frame size, window size", g_up_fw, g_up_fh, g_up_ww, g_up_wh);
        g_up_frames++;
        if (g_up_phase) {
            int scissor_on = g_up_scissor_on;
            if (scissor_on) up_disable(0x0C11);
            up_bind_fbo(UP_READ, g_up_big_fbo);
            up_blit(0, 0, g_up_big_w, g_up_big_h, dx0, dy0, dx1, dy1, 0x4000, 0x2600);   // ours, 1:1
            up_bind_fbo(UP_READ, g_up_read);
            if (scissor_on) up_enable(0x0C11);
            up_phase_ends();
            return;
        }
        g_up_plain++;
    }
    up_blit(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
}

SYSV static void w_up_enable(unsigned what) { if (what == 0x0C11) g_up_scissor_on = 1; up_enable(what); }
SYSV static void w_up_disable(unsigned what) { if (what == 0x0C11) g_up_scissor_on = 0; up_disable(what); }

// For gl_resolve: our wrapper for an OpenGL function when upscaling or its trace is on, else NULL.
static void *render_scale_wrap(const char *name, void *real) {
    if (!g_up_on && !g_up_trace) return NULL;
    if (!strcmp(name, "glViewport")) { up_viewport = real; return (void *)w_up_viewport; }
    if (!strcmp(name, "glScissor")) { up_scissor = real; return (void *)w_up_scissor; }
    if (!strcmp(name, "glBlitFramebuffer")) { up_blit = real; return (void *)w_up_blit; }
    if (!strcmp(name, "glBindFramebuffer")) { up_bind_fbo = real; return (void *)w_up_bind_fbo; }
    if (!strcmp(name, "glEnable")) { up_enable = real; return (void *)w_up_enable; }
    if (!strcmp(name, "glDisable")) { up_disable = real; return (void *)w_up_disable; }
    return NULL;
}

// With each fps line: how many frames had the interface drawn at full size.
void upscale_report(void) {
    if (!g_up_on || !g_up_frames) return;
    log_line("upscale: %llu frames, %llu with the interface at full size (frame %dx%d, window %dx%d)", g_up_frames, g_up_frames - g_up_plain, g_up_fw, g_up_fh, g_up_ww, g_up_wh);
    g_up_frames = g_up_plain = 0;
}
