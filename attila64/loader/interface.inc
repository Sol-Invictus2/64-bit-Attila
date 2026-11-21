// Interface: size that follows the screen, sharp text with it, version text, and the interface test traces.
// Part of loader.c (included there, not built by itself; the order of the includes matters).

// ---- Interface size. WARSCAPE::WS_ENGINE_IMP::get_ui_restricted_screen_size(bool) (30ff560) gives the screen size the interface
// is laid out for (width in the low 32 bits, height in the high); the interface is then stretched over the real screen. The game's
// own ui_scale (float at 5256be8, its options slider is not in the layouts) pulls that size towards 1440 x 900, unevenly in width
// and height. This zooms evenly instead: the size is divided by the player's factor, never below the game's smallest layout
// (1440 x 900). The factor comes from the mod list window (settings.txt next to the exe): Auto = looks like 1080p (screen height
// / 1080), or a percentage. Display only: UI code, nothing in the shared game.
float g_ui_size;                 // 0 = auto, otherwise the factor (1 = the game's own size); set from main
static volatile float g_ui_zoom_now;   // the zoom in use (real / laid-out size), 0 while the interface is not zoomed
float ui_zoom_now(void) { return g_ui_zoom_now > 1.0f ? g_ui_zoom_now : 1.0f; }   // menu_button.c: the Discord button's size
typedef uint64_t (SYSV *UiSizeFn)(void *engine, int use_scale);
static UiSizeFn g_ui_size_orig;
static SYSV uint64_t ui_restricted_size(void *engine, int use_scale) {
    uint64_t r = g_ui_size_orig(engine, use_scale);
    // EMPIREBATTLE::BATTLE_UI::draw places the drag-selection box (a TWUI component, so laid-out units) from the mouse position
    // times the UNSCALED size (two calls with use_scale = 0, returning to 2c1417a and 2c14197): with the zoom the box sat
    // zoom times too far right and down. Those two calls get the laid-out size as well.
    uintptr_t from = (uintptr_t)__builtin_return_address(0);
    if (!(use_scale & 0xff) && from != 0x2c1417a && from != 0x2c14197) {
        // the unscaled size: with upscaling that is the window, not the game's smaller frame (the interface's root box is made this
        // large by UIDLL::COMMON_UI::load_layout, and everything outside it is cut off)
        extern int upscale_window_size(uint32_t *, uint32_t *);
        uint32_t ww, wh;
        if (upscale_window_size(&ww, &wh) && (ww > (uint32_t)r || wh > (uint32_t)(r >> 32))) return (uint64_t)wh << 32 | ww;
        return r;
    }
    uint32_t w = (uint32_t)r, h = (uint32_t)(r >> 32);
    // Upscaling (render_scale.inc): the game's frame is smaller than the window and the interface is drawn at the window's size,
    // so it is laid out for the window. Without the zoom that is simply the window's size.
    extern int upscale_window_size(uint32_t *, uint32_t *);
    uint32_t fw = w, fh = h;
    int upscaled = upscale_window_size(&w, &h) && (w > fw || h > fh);
    if (!upscaled) { w = fw; h = fh; }
    if (w < 1440 || h < 900) { g_ui_zoom_now = 0; return upscaled ? (uint64_t)h << 32 | w : r; }
    float f = g_ui_size > 0 ? g_ui_size : (float)h / 1080.0f;
    float most = (float)w / 1440.0f < (float)h / 900.0f ? (float)w / 1440.0f : (float)h / 900.0f;
    if (f > most) f = most;
    if (f < 1.01f) { g_ui_zoom_now = 0; return upscaled ? (uint64_t)h << 32 | w : r; }
    uint32_t nw = (uint32_t)((float)w / f + 0.5f), nh = (uint32_t)((float)h / f + 0.5f);
    g_ui_zoom_now = (float)h / (float)nh;
    static int told;
    if (!told++) log_line("interface size: screen %ux%u laid out as %ux%u (zoom %.2f)", w, h, nw, nh, f);
    return (uint64_t)nh << 32 | nw;
}

// ---- Test switch ATTILA_TEXT_TRACE=1: logs the first text drawn and measured (which call, text, place, measured size), to see how
// the interface sizes its text. Measuring only. (Engine +0x71ec4/c8 = real screen size, +0x71ecc/d0 = laid-out size.)
typedef void (SYSV *TextDrawFn)(void *self, const void *text, const float *pos, float depth, unsigned flags, const void *a,
                                const void *b, int c);
static int g_text_trace;
static void text_name(const void *s, char *out, size_t n) {   // CA::UniString: u32 length, u32 capacity, char16_t *text
    out[0] = 0;
    if (!s) return;
    uint32_t len = *(const uint32_t *)s;
    const uint16_t *t = *(const uint16_t *const *)((const uint8_t *)s + 8);
    if (!t || len > 4000) return;
    size_t k = 0;
    for (uint32_t i = 0; i < len && k + 1 < n; i++) out[k++] = t[i] < 128 ? (char)t[i] : '?';
    out[k] = 0;
}
static TextDrawFn g_text_unscaled_orig;
static SYSV void text_unscaled_trace(void *self, const void *text, const float *pos, float depth, unsigned flags, const void *a,
                                     const void *b, int c) {
    static int n;
    if (n < 60) {
        char name[48];
        text_name(text, name, sizeof name);
        n++;
        log_line("text trace: draw_unscaled '%s' at %.1f,%.1f flags %u c %d", name, pos ? pos[0] : -1, pos ? pos[1] : -1, flags, c);
    }
    g_text_unscaled_orig(self, text, pos, depth, flags, a, b, c);
}
typedef int (SYSV *TextDetailsFn)(void *engine, const uint8_t *font, const void *text, uint8_t *details, const float *offset);
static TextDetailsFn g_text_details_orig;
static SYSV int text_details_trace(void *engine, const uint8_t *font, const void *text, uint8_t *details, const float *offset) {
    float lw = details ? *(float *)details : 0, lh = details ? *(float *)(details + 4) : 0;
    int r = g_text_details_orig(engine, font, text, details, offset);
    static int n;
    char name[48];
    text_name(text, name, sizeof name);
    if (n < 80 && details && name[0]) {
        n++;
        log_line("text trace: measured '%s' limit %.0fx%.0f -> width %d, +c %d, height %d, font size %d", name, lw, lh,
                 *(int *)(details + 8), *(int *)(details + 12), *(int *)(details + 16), font ? *(const int *)(font + 0x10) : -1);
    }
    return r;
}
static void *hook_entry(uintptr_t at, const uint8_t *old, int len, void *fn) {   // trampoline for the first len bytes, then jump in
    uint8_t *f = (uint8_t *)at;
    if (memcmp(f, old, (size_t)len)) return NULL;
    uint8_t *code = near_game_code(32);
    if (!code) return NULL;
    memcpy(code, old, (size_t)len);
    code[len] = 0xE9;
    int32_t rel = (int32_t)((f + len) - (code + len + 5));
    memcpy(code + len + 1, &rel, 4);
    uint8_t *in = code + 16;
    uint64_t target = (uint64_t)fn;
    in[0] = 0x48; in[1] = 0xB8;
    memcpy(in + 2, &target, 8);
    in[10] = 0xFF; in[11] = 0xE0;
    uint8_t jump[5] = {0xE9};
    rel = (int32_t)(in - (f + 5));
    memcpy(jump + 1, &rel, 4);
    memcpy(f, jump, 5);
    return code;
}
static void patch_text_trace(void) {
    g_text_trace = getenv("ATTILA_TEXT_TRACE") != NULL;
    if (!g_text_trace) return;
    static const uint8_t un[5] = {0x55, 0x41, 0x57, 0x41, 0x56};
    static const uint8_t det[6] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55};
    g_text_unscaled_orig = (TextDrawFn)hook_entry(0x30a2e20, un, 5, (void *)text_unscaled_trace);
    g_text_details_orig = (TextDetailsFn)hook_entry(0x3108810, det, 6, (void *)text_details_trace);
    log_line("text trace: %s / %s", g_text_unscaled_orig ? "drawing hooked" : "drawing NOT hooked",
             g_text_details_orig ? "measuring hooked" : "measuring NOT hooked");
}

// ---- Test switch ATTILA_LOBBY_TRACE=1: logs what the battle lobby does when a player place is added (box clicks, slot requests,
// the slot list, which box takes the new slot), to find where adding a 5th+ player on a side stops. Measuring only.
typedef struct { uint64_t a, b; } Pair;
static int (SYSV *g_lt_add_slot)(void *, unsigned, int);
static unsigned (SYSV *g_lt_add_info)(uint8_t *, const uint8_t *);
static int (SYSV *g_lt_alliance_added)(uint8_t *, unsigned, unsigned, int);
static Pair (SYSV *g_lt_openable)(uint8_t *, unsigned);
static int (SYSV *g_lt_can_add)(void *, unsigned, int);
static void (SYSV *g_lt_notify)(uint8_t *, const void *);
static void (SYSV *g_lt_setup_added)(void *, unsigned, unsigned, int);
static SYSV int lt_add_slot(void *lobby, unsigned team, int b) {
    void *from = __builtin_return_address(0);
    int r = g_lt_add_slot(lobby, team, b);
    log_line("lobby trace: add_player_slot(team %u, %d) -> %d, called from %p", team, b & 0xff, r & 0xff, from);
    return r;
}
static SYSV unsigned lt_add_info(uint8_t *slots, const uint8_t *info) {
    uint32_t flags = info ? *(const uint32_t *)(info + 0xd8) : 0;
    unsigned r = g_lt_add_info(slots, info);
    log_line("lobby trace: add_player_info(flags %x, team %u) -> slot %d of %u", flags, flags & 3, (int)r, *(uint32_t *)(slots + 0x1180));
    return r;
}
static SYSV int lt_alliance_added(uint8_t *alliance, unsigned slot, unsigned team, int b) {
    int r = g_lt_alliance_added(alliance, slot, team, b);
    log_line("lobby trace: side %u got slot-added(slot %u, team %u, %d) -> %d", *(uint32_t *)(alliance + 0x58), slot, team, b & 0xff, r & 0xff);
    return r;
}
static SYSV Pair lt_openable(uint8_t *mgr, unsigned team) {
    Pair r = g_lt_openable(mgr, team);
    log_line("lobby trace: FindOpenableSlot(team %u) -> box %p, %llu", team, (void *)r.a, (unsigned long long)(r.b & 0xffffffff));
    return r;
}
static SYSV int lt_can_add(void *lobby, unsigned team, int b) {
    int r = g_lt_can_add(lobby, team, b);
    log_line("lobby trace: can_add_player_slot(team %u, %d) -> %d", team, b & 0xff, r & 0xff);
    return r;
}
static SYSV void lt_notify(uint8_t *player, const void *ev) {
    const uint8_t *button = ev ? *(const uint8_t *const *)ev : NULL;
    static const int fields[7] = {0x68, 0x70, 0x78, 0x80, 0x88, 0x90, 0x98};
    int which = -1;
    for (int i = 0; i < 7 && player; i++) if (*(const uint8_t *const *)(player + fields[i]) == button) which = fields[i];
    char name[64] = "";
    if (button) {   // Component name: CA::String at +0xf8 (u32 length, u32 capacity, char *text)
        uint32_t len = *(const uint32_t *)(button + 0xf8);
        const char *t = *(const char *const *)(button + 0x100);
        if (t && len && len < 60) snprintf(name, sizeof name, "%.*s", (int)len, t);
    }
    if (which >= 0 || (ev && ((const uint8_t *)ev)[8]))
        log_line("lobby trace: box %p (slot %d, used %u) got click on button %p '%s' = its field %+x", (void *)player,
                 player ? *(int *)(player + 0xa4) : -9, player ? player[0xa0] : 0, (void *)button, name, which);
    g_lt_notify(player, ev);
}
static SYSV void lt_setup_added(void *setup, unsigned slot, unsigned team, int b) {
    log_line("lobby trace: lobby says open slot added: slot %u, team %u, %d", slot, team, b & 0xff);
    g_lt_setup_added(setup, slot, team, b);
}
static void patch_lobby_trace(void) {
    if (!getenv("ATTILA_LOBBY_TRACE")) return;
    static const uint8_t b_add[5] = {0x55, 0x41, 0x57, 0x41, 0x56}, b_info[5] = {0x55, 0x41, 0x57, 0x41, 0x56};
    static const uint8_t b_all[7] = {0x55, 0x41, 0x56, 0x53, 0x41, 0x89, 0xF6}, b_open[7] = {0x41, 0x56, 0x53, 0x50, 0x49, 0x89, 0xFE};
    static const uint8_t b_can[11] = {0x48, 0x8B, 0x47, 0x28, 0x48, 0x8B, 0x80, 0xB8, 0x2F, 0x0D, 0x00};
    static const uint8_t b_not[5] = {0x55, 0x41, 0x57, 0x41, 0x56};
    static const uint8_t b_set[11] = {0x53, 0x48, 0x89, 0xFB, 0x80, 0xBB, 0x33, 0x01, 0x00, 0x00, 0x00};
    *(void **)&g_lt_add_slot = hook_entry(0x2854aa0, b_add, 5, (void *)lt_add_slot);
    *(void **)&g_lt_add_info = hook_entry(0x27c8140, b_info, 5, (void *)lt_add_info);
    *(void **)&g_lt_alliance_added = hook_entry(0x2d407b0, b_all, 7, (void *)lt_alliance_added);
    *(void **)&g_lt_openable = hook_entry(0x2d3f570, b_open, 7, (void *)lt_openable);
    *(void **)&g_lt_can_add = hook_entry(0x27fe710, b_can, 11, (void *)lt_can_add);
    *(void **)&g_lt_notify = hook_entry(0x2d46900, b_not, 5, (void *)lt_notify);
    *(void **)&g_lt_setup_added = hook_entry(0x2cda320, b_set, 11, (void *)lt_setup_added);
    log_line("lobby trace: hooks %d%d%d%d%d%d%d", g_lt_add_slot != NULL, g_lt_add_info != NULL, g_lt_alliance_added != NULL,
             g_lt_openable != NULL, g_lt_can_add != NULL, g_lt_notify != NULL, g_lt_setup_added != NULL);
}

static void patch_ui_size(void) {
    if (g_ui_size > 0 && g_ui_size < 1.01f && !getenv("ATTILA_UPSCALE")) { log_line("interface size: the game's own"); return; }   // upscaling needs the hook at any size
    uint8_t *f = (uint8_t *)0x30ff560;
    static const uint8_t old[10] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x83, 0xEC, 0x18};   // push rbp, r15, r14, rbx; sub rsp,18h
    if (memcmp(f, old, 10)) { log_line("patch skipped, game code differs: interface size"); return; }
    uint8_t *code = near_game_code(32);
    if (!code) { log_line("patch skipped, no memory near the game: interface size"); return; }
    memcpy(code, old, 6);                       // trampoline: the four pushes (6 bytes), then on into the original at +6
    code[6] = 0xE9;
    int32_t rel = (int32_t)((f + 6) - (code + 11));
    memcpy(code + 7, &rel, 4);
    g_ui_size_orig = (UiSizeFn)(void *)code;
    uint8_t *in = code + 16;                    // way in: mov rax,ui_restricted_size; jmp rax
    uint64_t target = (uint64_t)ui_restricted_size;
    in[0] = 0x48; in[1] = 0xB8;
    memcpy(in + 2, &target, 8);
    in[10] = 0xFF; in[11] = 0xE0;
    uint8_t jump[5] = {0xE9};
    rel = (int32_t)(in - (f + 5));
    memcpy(jump + 1, &rel, 4);
    memcpy(f, jump, 5);
    if (g_ui_size > 0) log_line("patch applied: interface size %.0f%%", g_ui_size * 100.0f);
    else log_line("patch applied: interface size auto (looks like 1080p)");
    // WS_ENGINE_IMP::get_ui_screen_scale (30fc590) = real size / laid-out size, used by the interface code (text boxes, fonts,
    // pictures) for the game's own scaling on top of the laid-out size: with our zoom that stretched text boxes a second time
    // (210 -> 280 units for a menu button whose picture stays ~250). Our zoom is the whole scaling, so it answers 1 x 1:
    // movabs rax,(1.0f,1.0f); mov [rdi],rax; mov rax,rdi; ret.
    {
        uint8_t *g = (uint8_t *)0x30fc590;
        static const uint8_t g_old[8] = {0x41, 0x56, 0x53, 0x50, 0x48, 0x89, 0xF3, 0x49};   // push r14, rbx, rax; mov rbx,rsi; mov r14,..
        static const uint8_t g_new[17] = {0x48, 0xB8, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x80, 0x3F, 0x48, 0x89, 0x07, 0x48, 0x89, 0xF8, 0xC3};
        if (getenv("ATTILA_UI_GAME_SCALE")) log_line("interface size: the game's own scale kept (test switch)");
        else if (memcmp(g, g_old, 8)) log_line("patch skipped, game code differs: interface scale 1 x 1");
        else { memcpy(g, g_new, sizeof g_new); log_line("patch applied: interface code sees no second scale (our zoom is the scaling)"); }
    }
}

// ---- Sharp interface text with the zoom. The interface is laid out at the smaller size and stretched, so its letters (bitmap
// fonts font\<name>_[b_]<size>.cuf, even sizes up to 32) come from small glyph pictures blown up by the zoom: soft. Every font a
// text is drawn with gets a bigger brother (same name, size x zoom, the nearest even size that loads). WS_SPRITE_TEXT_IMP::draw
// (30a2d30, what TWUI::ComponentState::Draw uses for each text piece) then draws with the bigger font, and our hooks on
// WS_SPRITE_IMP::pixel_to_screen / dimensions_to_screen (309ded0 / 309df30, how SPRITE_FONT::render_kerned turns letter places
// and the letter box into screen space) shrink its letter places and boxes back around the text's start point: across by the
// width ratio of the two fonts (SPRITE_FONT::kerned_string_length), so the text keeps exactly the width the interface measured,
// up and down by the size ratio. The stretch then shows the bigger glyphs about 1:1 on screen. Display only (drawing code).
// ATTILA_SOFT_TEXT=1 turns it off.
typedef void *(SYSV *FontLoadFn)(void *imp, const void *name, unsigned size, int bold, const void *subdir);
typedef float *(SYSV *ToScreenFn)(float *out, void *sprite, float x, float y, int scaled);
#define SHARP_MAX 160
typedef struct { void *small, *big; int s, b; } SharpFont;
static SharpFont g_sharp[SHARP_MAX];
static volatile long g_sharp_n;
static TextDrawFn g_sharp_draw_orig;
static ToScreenFn g_p2s_orig, g_d2s_orig;
static __thread int t_sharp_on;
static __thread float t_kx, t_ky, t_ox, t_oy;
static DWORD g_sharp_thread;           // the thread the first sharp draw ran on; fonts are only loaded from that thread
#define FONT_INVALID (*(void *const *)(uintptr_t)0x5254d38)   // WARSCAPE::SPRITE_FONT_HANDLE_INVALID
static void *const g_font_load_fn = (void *)(uintptr_t)0x30a2910;   // WS_SPRITE_TEXT_IMP::font_load(name, size, bold, subdir)
static int (SYSV *const g_font_width)(const void *font, const void *text) =
    (int (SYSV *)(const void *, const void *))(uintptr_t)0x3168070;     // SPRITE_FONT::kerned_string_length(UniString const&)
static void (SYSV *const g_ca_string_new)(void *s, const char *t) = (void (SYSV *)(void *, const char *))(uintptr_t)0x512db0;
static void (SYSV *const g_ca_string_free)(void *s) = (void (SYSV *)(void *))(uintptr_t)0x512d00;

// SPRITE_FONT name (CA::String at +0x50088) "la_gioconda_16.cuf" / "arialunicode_b_16.cuf" -> size, base name, bold
static int font_file_size(const uint8_t *font, char *base, size_t bn, int *bold) {
    if (!font || font == FONT_INVALID) return 0;
    uint32_t len = *(const uint32_t *)(font + 0x50088);
    const char *t = *(const char *const *)(font + 0x50088 + 8);
    if (!t || len < 6 || len > 200) return 0;
    int e = (int)len;
    if (_strnicmp(t + e - 4, ".cuf", 4) == 0) e -= 4;
    int d = e, s = 0;
    while (d > 0 && t[d - 1] >= '0' && t[d - 1] <= '9') d--;
    if (d == e || d < 2 || t[d - 1] != '_') return 0;
    for (int i = d; i < e; i++) s = s * 10 + (t[i] - '0');
    int b0 = 0, end = d - 1;                       // end: index of the '_' before the size
    if (end >= 2 && t[end - 1] == 'b' && t[end - 2] == '_') { b0 = 1; end -= 2; }
    int st = end;
    while (st > 0 && t[st - 1] != '\\' && t[st - 1] != '/') st--;
    if (base) { int n = end - st; if (n <= 0 || (size_t)n >= bn) return 0; memcpy(base, t + st, (size_t)n); base[n] = 0; }
    if (bold) *bold = b0;
    return s;
}
static SharpFont *sharp_find(const void *font) {
    long n = g_sharp_n;
    for (long i = 0; i < n; i++) if (g_sharp[i].small == font) return &g_sharp[i];
    return NULL;
}
static SharpFont *sharp_prepare(void *imp, void *font, float zoom) {
    SharpFont *e = sharp_find(font);
    if (e || g_sharp_n >= SHARP_MAX) return e;
    char base[96]; int bold = 0;
    int s = font_file_size(font, base, sizeof base, &bold);
    SharpFont add = {font, NULL, s, 0};
    if (s > 0) {
        int c0 = (int)((float)s * zoom * 0.5f + 0.5f) * 2;
        if (c0 > 32) c0 = 32;
        int cand[3] = {c0, c0 - 2, c0 + 2};
        uint8_t name[32] = {0};
        g_ca_string_new(name, base);
        for (int i = 0; i < 3 && !add.big; i++) {
            int c = cand[i];
            if (c <= s || c > 32) continue;
            void *g = ((FontLoadFn)g_font_load_fn)(imp, name, (unsigned)c, bold, NULL);
            if (g && g != FONT_INVALID && font_file_size(g, NULL, 0, NULL) == c) { add.big = g; add.b = c; }
        }
        g_ca_string_free(name);
    }
    g_sharp[g_sharp_n] = add;
    __sync_synchronize();
    g_sharp_n++;
    if (g_sharp_n <= 40) {
        if (add.big) log_line("sharp text: %s%s size %d drawn with size %d (zoom %.2f)", base, bold ? " bold" : "", s, add.b, zoom);
        else log_line("sharp text: no bigger font for %s%s size %d", s ? base : "(unknown font)", bold ? " bold" : "", s);
    }
    return &g_sharp[g_sharp_n - 1];
}
static SYSV void sharp_text_draw(uint8_t *imp, const void *text, const float *pos, float depth, unsigned flags, const void *a,
                                 const void *b, int c) {
    float zoom = g_ui_zoom_now;
    void *font = imp ? *(void **)(imp + 0x28) : NULL;
    SharpFont *e = NULL;
    if (zoom >= 1.05f && pos && text && font && font != FONT_INVALID && !t_sharp_on) {
        e = sharp_find(font);
        if (!e) {
            DWORD me = GetCurrentThreadId();
            if (!g_sharp_thread) { g_sharp_thread = me; log_line("sharp text: first text drawn on thread %lu", (unsigned long)me); }
            if (me == g_sharp_thread) e = sharp_prepare(imp, font, zoom);
        }
    }
    if (!e || !e->big) { g_sharp_draw_orig(imp, text, pos, depth, flags, a, b, c); return; }
    {   // the bigger font must still be one of this text system's fonts (count +0x1c, list +0x20, as in font_get); if the game
        // has thrown it away, draw as the game does and let the pair be made again at the next draw
        uint32_t nf = *(const uint32_t *)(imp + 0x1c);
        void *const *list = *(void *const *const *)(imp + 0x20);
        int alive = 0;
        for (uint32_t i = 0; list && i < nf && i < 4096; i++) if (list[i] == e->big) { alive = 1; break; }
        if (!alive || font_file_size(e->big, NULL, 0, NULL) != e->b || font_file_size(font, NULL, 0, NULL) != e->s) {
            e->small = NULL;
            g_sharp_draw_orig(imp, text, pos, depth, flags, a, b, c);
            return;
        }
    }
    int ws = g_font_width(font, text), wb = g_font_width(e->big, text);
    t_ky = (float)e->s / (float)e->b;
    t_kx = (ws > 0 && wb > 0) ? (float)ws / (float)wb : t_ky;
    t_ox = (float)(int)pos[0]; t_oy = (float)(int)pos[1];   // render_kerned starts at the truncated position
    // The text colour lives in the font object (SPRITE_FONT::set_font_colour, +0x50098; WS_SPRITE_TEXT_IMP::colour_set sets it
    // on the current font), and so does the tab width (+0x5009c, in the font's own pixels): hand both to the bigger font.
    // Without this, coloured text (red player names) came out in the bigger font's last colour.
    *(uint32_t *)((uint8_t *)e->big + 0x50098) = *(const uint32_t *)((const uint8_t *)font + 0x50098);
    *(uint32_t *)((uint8_t *)e->big + 0x5009c) =
        (uint32_t)((uint64_t)*(const uint32_t *)((const uint8_t *)font + 0x5009c) * (uint64_t)e->b / (uint64_t)e->s);
    *(void **)(imp + 0x28) = e->big;
    t_sharp_on = 1;
    g_sharp_draw_orig(imp, text, pos, depth, flags, a, b, c);
    t_sharp_on = 0;
    *(void **)(imp + 0x28) = font;
}
static SYSV float *sharp_pixel_to_screen(float *out, void *sprite, float x, float y, int scaled) {
    if (t_sharp_on) { x = t_ox + (x - t_ox) * t_kx; y = t_oy + (y - t_oy) * t_ky; }
    return g_p2s_orig(out, sprite, x, y, scaled);
}
static SYSV float *sharp_dimensions_to_screen(float *out, void *sprite, float w, float h, int scaled) {
    if (t_sharp_on) { w *= t_kx; h *= t_ky; }
    return g_d2s_orig(out, sprite, w, h, scaled);
}
// The game throws its fonts away when the interface's resources are registered again (battle and campaign loading:
// WS_SPRITE_TEXT_IMP::on_registration_begin 30a2840 destroys every font of that text system, which then loads them again) and
// when a text system goes (~WS_SPRITE_TEXT_IMP 30a2710). Our bigger fonts sit in the same list, so the table of pairs is
// emptied before that: kept pairs pointed at freed fonts, and a new font at an old address got a dead partner (Test 16 crash
// in SPRITE_FONT::kerned_string_length on the battle loading screen). The pairs are made again at the next draw.
typedef void (SYSV *TextImpFn)(void *imp);
static TextImpFn g_sharp_reg_orig, g_sharp_dtor_orig, g_sharp_font_dtor_orig;
// Any single font that goes (SPRITE_FONT::~SPRITE_FONT 309e940: frees the kerning table and leaves +0x20030 = NULL, which is
// what crashed when a dead bigger font was measured) takes its pairs with it.
static SYSV void sharp_font_gone(void *font) {
    long n = g_sharp_n;
    for (long i = 0; i < n; i++) if (g_sharp[i].small == font || g_sharp[i].big == font) g_sharp[i].small = NULL;
    g_sharp_font_dtor_orig(font);
}
static volatile long g_sharp_forgets;
static void sharp_forget_all(const char *why) {
    g_sharp_n = 0;
    __sync_synchronize();
    if (InterlockedIncrement(&g_sharp_forgets) <= 20) log_line("sharp text: the game reloads its fonts (%s), bigger ones made again", why);
}
static SYSV void sharp_registration_begin(void *imp) { sharp_forget_all("interface resources registered again"); g_sharp_reg_orig(imp); }
static SYSV void sharp_text_imp_gone(void *imp) { sharp_forget_all("text system closed"); g_sharp_dtor_orig(imp); }
// ---- Test switch ATTILA_GFX_TRACE=1: logs WS_ENGINE_GRAPHICS_OPTION::cap_to_available (2cb3e20), which lowers every graphics
// setting to what the engine thinks the card can do (reflections at +1c come out 0). Measuring only.
typedef void (SYSV *CapFn)(int32_t *opt, const int32_t *avail);
static CapFn g_cap_orig;
static SYSV void cap_trace(int32_t *opt, const int32_t *avail) {
    int32_t before[32];
    memcpy(before, opt, sizeof before);
    // test only, with ATTILA_AA_LIMIT=n: raise the engine's anti-aliasing limit (first number; 1 = MLAA is what the OpenGL
    // renderer reports, 4 = MSAA 8x) to see whether the renderer can draw the higher modes at all
    { const char *e = getenv("ATTILA_AA_LIMIT"); if (e && atoi(e) > avail[0]) ((int32_t *)avail)[0] = atoi(e); }
    g_cap_orig(opt, avail);
    char a[400], b[400], c[400]; int na = 0, nb = 0, nc = 0;
    for (int i = 0; i < 32; i++) {
        na += snprintf(a + na, sizeof a - na, " %x", (unsigned)before[i]);
        nb += snprintf(b + nb, sizeof b - nb, " %x", (unsigned)avail[i]);
        nc += snprintf(c + nc, sizeof c - nc, " %x", (unsigned)opt[i]);
    }
    const uint8_t *eng = *(const uint8_t *const *)(uintptr_t)0x52516d0;   // WARSCAPE::WS_CONTEXT::m_e_engine
    log_line("gfx trace: cap_to_available (options at engine%+lld, limit at engine%+lld, from %p)\n  wanted:%s\n  avail: %s\n  result:%s",
             eng ? (long long)((const uint8_t *)opt - eng) : 0LL, eng ? (long long)((const uint8_t *)avail - eng) : 0LL,
             __builtin_return_address(0), a, b, c);
}
// ---- Main menu version text ("v1.6.0  Build 9776.826737", component "version_number", top right) broke onto two lines in a
// box too narrow for it, also without our zoom. UIDLL::MainMenuCallbackComponent::OnCreate sets it with
// TWUI::Component::StateTextSet (call at 29a9871); that call now goes to version_text_set, which also widens the box to its
// text (TWUI::Component::ResizeToFitText) and docks it again (TWUI::Component::Dock) so it stays at the screen edge.
// Display only. ATTILA_NO_VERSION_FIT=1 turns it off.
static SYSV void version_text_set(void *comp, const void *text, int flag) {
    ((SYSV void (*)(void *, const void *, int))(uintptr_t)0x29182c0)(comp, text, flag);       // Component::StateTextSet
    if (!comp) return;
    float w0 = ((SYSV float (*)(void *))(uintptr_t)0x2916bd0)(comp);                          // Component::Width
    ((SYSV int (*)(void *, int))(uintptr_t)0x29170c0)(comp, 1);                                // Component::ResizeToFitText
    ((SYSV void (*)(void *, int))(uintptr_t)0x2923260)(comp, 1);                               // Component::Dock
    float w1 = ((SYSV float (*)(void *))(uintptr_t)0x2916bd0)(comp);
    log_line("version text: box %.0f -> %.0f wide", w0, w1);
}
static void patch_version_text(void) {
    if (getenv("ATTILA_NO_VERSION_FIT")) return;
    uint8_t *call = (uint8_t *)0x29a9871;
    int32_t want = (int32_t)(0x29182c0 - (0x29a9871 + 5));
    if (call[0] != 0xE8 || memcmp(call + 1, &want, 4)) { log_line("patch skipped, game code differs: version text"); return; }
    uint8_t *s = near_game_code(16);
    if (!s) { log_line("patch skipped, no memory near the game: version text"); return; }
    s[0] = 0x48; s[1] = 0xB8;                                    // mov rax,version_text_set; jmp rax (arguments untouched)
    uint64_t fn = (uint64_t)version_text_set;
    memcpy(s + 2, &fn, 8);
    s[10] = 0xFF; s[11] = 0xE0;
    int32_t rel = (int32_t)(s - (call + 5));
    memcpy(call + 1, &rel, 4);
    log_line("patch applied: main menu version text fits its box");
}

static void patch_gfx_trace(void) {
    if (!getenv("ATTILA_GFX_TRACE")) return;
    static const uint8_t old[8] = {0x53, 0x48, 0x89, 0xFB, 0xF3, 0x0F, 0x6F, 0x06};   // push rbx; mov rbx,rdi; movdqu xmm0,[rsi]
    g_cap_orig = (CapFn)hook_entry(0x2cb3e20, old, 8, (void *)cap_trace);
    log_line("gfx trace: %s", g_cap_orig ? "cap_to_available hooked" : "cap_to_available NOT hooked");
}
static void patch_sharp_text(void) {
    if (getenv("ATTILA_SOFT_TEXT")) { log_line("sharp text off (ATTILA_SOFT_TEXT)"); return; }
    static const uint8_t draw_old[5] = {0x55, 0x41, 0x57, 0x41, 0x56};                       // push rbp; push r15; push r14
    static const uint8_t conv_old[8] = {0x53, 0x48, 0x89, 0xFB, 0x48, 0x8B, 0x76, 0x18};     // push rbx; mov rbx,rdi; mov rsi,[rsi+18h]
    static const uint8_t load_old[5] = {0x55, 0x41, 0x57, 0x41, 0x56};
    static const uint8_t reg_old[7] = {0x53, 0x48, 0x89, 0xFB, 0x8B, 0x43, 0x1C};             // push rbx; mov rbx,rdi; mov eax,[rbx+1Ch]
    static const uint8_t dtor_old[11] = {0x53, 0x48, 0x89, 0xFB, 0x48, 0xC7, 0x03, 0x30, 0x70, 0xBE, 0x03};   // ...; mov qword [rbx],vtable
    static const uint8_t font_dtor_old[11] = {0x53, 0x48, 0x89, 0xFB, 0x48, 0xC7, 0x03, 0x70, 0x6F, 0xBE, 0x03};
    if (memcmp((void *)0x30a2d30, draw_old, 5) || memcmp((void *)0x309ded0, conv_old, 8) || memcmp((void *)0x309df30, conv_old, 8) ||
        memcmp(g_font_load_fn, load_old, 5) || memcmp((void *)0x30a2840, reg_old, 7) || memcmp((void *)0x30a2710, dtor_old, 11) ||
        memcmp((void *)0x309e940, font_dtor_old, 11)) {
        log_line("patch skipped, game code differs: sharp text"); return;
    }
    // the font-going hooks first: without them the bigger fonts must not be used at all
    g_sharp_reg_orig = (TextImpFn)hook_entry(0x30a2840, reg_old, 7, (void *)sharp_registration_begin);
    g_sharp_dtor_orig = g_sharp_reg_orig ? (TextImpFn)hook_entry(0x30a2710, dtor_old, 11, (void *)sharp_text_imp_gone) : NULL;
    g_sharp_font_dtor_orig = g_sharp_dtor_orig ? (TextImpFn)hook_entry(0x309e940, font_dtor_old, 11, (void *)sharp_font_gone) : NULL;
    if (!g_sharp_reg_orig || !g_sharp_dtor_orig || !g_sharp_font_dtor_orig) {
        log_line("patch skipped, no memory near the game: sharp text"); return;
    }
    g_p2s_orig = (ToScreenFn)hook_entry(0x309ded0, conv_old, 8, (void *)sharp_pixel_to_screen);
    g_d2s_orig = (ToScreenFn)hook_entry(0x309df30, conv_old, 8, (void *)sharp_dimensions_to_screen);
    if (!g_p2s_orig || !g_d2s_orig) { log_line("patch skipped, no memory near the game: sharp text"); return; }   // (inactive hooks)
    g_sharp_draw_orig = (TextDrawFn)hook_entry(0x30a2d30, draw_old, 5, (void *)sharp_text_draw);
    if (!g_sharp_draw_orig) { log_line("patch skipped, no memory near the game: sharp text"); return; }
    log_line("patch applied: sharp interface text with the zoom (bigger fonts drawn at the laid-out size)");
}


