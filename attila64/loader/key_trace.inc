// key_trace.inc - TEST ONLY (switch ATTILA_KEY_TRACE=1, off by default): which key set the shortcuts use.
// data\text\default_keys.xml has two key sets, "classic_keyset" (normal keyboard shortcuts) and "controller". The game's
// UIDLL::SteamGameController::Acquire (2aa52f0) switches to "controller" when Steam reports a connected controller
// (ISteamController GetConnectedControllers > 0); every key set change goes through
// EMPIREUTILITY::SHORTCUT_HANDLER::set_current_keyset (1cd26b0). The hooks only log, then run the game's own code.

typedef SYSV uint8_t (*KtAcquireFn)(void *self);
typedef SYSV void (*KtSetKeysetFn)(void *self, const void *name);
static KtAcquireFn g_kt_acquire;
static KtSetKeysetFn g_kt_set_keyset;

static SYSV uint8_t kt_acquire(void *self) {
    uint8_t r = g_kt_acquire(self);
    const uint8_t *s = (const uint8_t *)self;   // +0x40: CA_STD::VECTOR of the controller handles Steam reported
    log_line("key trace: Steam controller Acquire -> %d (handle list words %u %u)", r, *(const uint32_t *)(s + 0x40),
             *(const uint32_t *)(s + 0x44));
    return r;
}
static SYSV void kt_set_keyset(void *self, const void *name) {
    char buf[64] = "?";
    if (name) {   // CA::UniString: +0 length, +8 2-byte letters
        uint32_t len = *(const uint32_t *)name;
        const uint16_t *t = *(const uint16_t *const *)((const uint8_t *)name + 8);
        if (t && len < sizeof buf) {
            for (uint32_t i = 0; i < len; i++) buf[i] = t[i] < 0x80 ? (char)t[i] : '?';
            buf[len] = 0;
        }
    }
    log_line("key trace: key set changed to \"%s\"", buf);
    g_kt_set_keyset(self, name);
}

// ---- TEST ONLY (switch ATTILA_MOUSE_TRACE=1): how a mouse position becomes the game's mouse position. TOTALWAR::on_mouse_event
// (575c50) turns the SDL window position into game pixels: x * get_screen_size().w / window width (CAWindow::get_display_rect
// = 0,0 .. SDL_GetWindowSize); CA::mouse_event (4f14a0) then divides by get_visible_screen_size() and keeps the result in
// n_mouse_state (4c0c7e0: x, y, 0..1), which the cameras use for edge scrolling and intelli-zoom.
typedef SYSV uint64_t (*MtMouseEventFn)(const uint8_t *ev, uint32_t w, uint32_t h, uint32_t z, uint8_t b);
static MtMouseEventFn g_mt_mouse_event;
static volatile long g_mt_lines, g_mt_edge_lines;
static SYSV uint64_t mt_mouse_event(const uint8_t *ev, uint32_t w, uint32_t h, uint32_t z, uint8_t b) {
    uint64_t r = g_mt_mouse_event(ev, w, h, z, b);
    const float *ms = (const float *)0x4c0c7e0;
    int edge = ms[0] > 0.6f || ms[1] > 0.6f;
    if (InterlockedIncrement(&g_mt_lines) <= 12 || (edge && InterlockedIncrement(&g_mt_edge_lines) <= 24)) {
        extern void test_window_size(int *, int *);
        int ww, wh;
        test_window_size(&ww, &wh);
        log_line("mouse trace: event %d, game pixels %.0f,%.0f, divided by %ux%u -> mouse %.4f,%.4f (window %dx%d)",
                 *(const int *)ev, *(const float *)(ev + 8), *(const float *)(ev + 12), w, h, ms[0], ms[1], ww, wh);
    }
    return r;
}

static void patch_key_trace(void) {
    if (getenv("ATTILA_MOUSE_TRACE")) {
        static const uint8_t me_old[5] = {0xF3, 0x0F, 0x10, 0x47, 0x10};   // CA::mouse_event 4f14a0: movss xmm0,[rdi+0x10]
        g_mt_mouse_event = (MtMouseEventFn)hook_entry(0x4f14a0, me_old, 5, (void *)mt_mouse_event);
        log_line("test: mouse trace (ATTILA_MOUSE_TRACE) %s", g_mt_mouse_event ? "on" : "NOT hooked");
    }
    if (!getenv("ATTILA_KEY_TRACE")) return;
    static const uint8_t acq_old[5] = {0x55, 0x41, 0x57, 0x41, 0x56};               // SteamGameController::Acquire 2aa52f0
    static const uint8_t set_old[7] = {0x41, 0x56, 0x53, 0x48, 0x83, 0xEC, 0x48};   // SHORTCUT_HANDLER::set_current_keyset 1cd26b0
    g_kt_acquire = (KtAcquireFn)hook_entry(0x2aa52f0, acq_old, 5, (void *)kt_acquire);
    g_kt_set_keyset = (KtSetKeysetFn)hook_entry(0x1cd26b0, set_old, 7, (void *)kt_set_keyset);
    log_line("test: key trace (ATTILA_KEY_TRACE): controller acquire %s, key set changes %s", g_kt_acquire ? "on" : "NOT hooked",
             g_kt_set_keyset ? "on" : "NOT hooked");
}
