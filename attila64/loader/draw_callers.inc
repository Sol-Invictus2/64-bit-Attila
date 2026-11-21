// Measuring only (ATTILA_DRAW_CALLERS=1 with ATTILA_PASS_STATS=1): which game code issues the one-copy draw calls of a melee.
// Part of shim_gfx.c (included at its end).
//
// Once a frame has had more than 15,000 draw calls, the one-copy draws of the following frames are sampled (4,000 of them):
// for each, the stack is searched upwards for the first six return addresses that lie in the game's code right after a call
// instruction, i.e. the chain of game functions that led to the draw. The chains are counted and the most frequent ones are
// logged once, as addresses (tools\fn_at.sh names them). The view that was running (shadow map / main picture / other) is
// kept apart.
#define DC_DEPTH 7
#define DC_SLOTS 512
static int g_dc_on = -1, g_dc_armed, g_dc_done;
static unsigned g_dc_frame, g_dc_samples;
static unsigned long long g_dc_frame_start_total;
static struct { uint64_t chain[DC_DEPTH]; unsigned n, kind; } g_dc[DC_SLOTS];
extern volatile unsigned g_frame_no;

static int dc_is_return(uint64_t a) {
    if (a < 0x400000 + 8 || a >= 0x3700000) return 0;
    const uint8_t *p = (const uint8_t *)(uintptr_t)a;
    if (p[-5] == 0xE8) return 1;                                                  // call rel32
    if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return 1;                        // call reg / [reg]
    if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) return 1;                        // call [reg+disp8] or with a prefix
    if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return 1;                        // call [reg+disp32] / [rip+disp32]
    if (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10) return 1;                        // the same with an index byte
    return 0;
}

static void draw_caller_note(int copies, int kind) {
    if (g_dc_on < 0) g_dc_on = getenv("ATTILA_DRAW_CALLERS") != NULL;
    if (!g_dc_on || g_dc_done) return;
    if (g_frame_no != g_dc_frame) {   // a new frame: was the last one a heavy one?
        static int heavy;   // 30 heavy frames in a row: a single heavy frame is a loading frame, not the melee (second try)
        if (g_gl_draw_total - g_dc_frame_start_total > 15000) { if (++heavy >= 30) g_dc_armed = 1; } else heavy = 0;
        g_dc_frame = g_frame_no;
        g_dc_frame_start_total = g_gl_draw_total;
    }
    if (!g_dc_armed || copies != 1) return;
    uint64_t chain[DC_DEPTH] = {0};
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    uint64_t *sp = (uint64_t *)__builtin_frame_address(0), *top = (uint64_t *)tib->StackBase;
    int found = 0;
    // The slots of our own wrappers hold old values that look like return addresses (first try, 2026-11-12: set_constants over
    // and over). The chain therefore starts at the first address inside the device context's draw functions (33e1000-33e3000),
    // which is the game function that really called OpenGL; repeats of the address before are skipped.
    int started = 0;
    for (int k = 0; k < 3000 && sp + k + 1 <= top && found < DC_DEPTH; k++) {
        uint64_t a = sp[k];
        if (!started) { if (a >= 0x33e1000 && a < 0x33e3000 && dc_is_return(a)) started = 1; else continue; }
        if (dc_is_return(a) && (!found || chain[found - 1] != a)) chain[found++] = a;
    }
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < DC_DEPTH; i++) h = (h ^ chain[i]) * 1099511628211ull;
    for (unsigned i = 0, at = (unsigned)(h % DC_SLOTS); i < DC_SLOTS; i++, at = (at + 1) % DC_SLOTS) {
        if (!g_dc[at].n) { memcpy(g_dc[at].chain, chain, sizeof chain); g_dc[at].kind = (unsigned)kind; g_dc[at].n = 1; break; }
        if (g_dc[at].kind == (unsigned)kind && !memcmp(g_dc[at].chain, chain, sizeof chain)) { g_dc[at].n++; break; }
    }
    if (++g_dc_samples < 4000) return;
    g_dc_done = 1;
    log_line("one-copy draws: 4000 sampled after a frame with more than 15000 draw calls; chains of game return addresses, nearest first:");
    for (int shown = 0; shown < 24; shown++) {
        int best = -1;
        for (int i = 0; i < DC_SLOTS; i++) if (g_dc[i].n && (best < 0 || g_dc[i].n > g_dc[best].n)) best = i;
        if (best < 0) break;
        log_line("  %4u x view %u: %llx %llx %llx %llx %llx %llx %llx", g_dc[best].n, g_dc[best].kind, (unsigned long long)g_dc[best].chain[0], (unsigned long long)g_dc[best].chain[1],
                 (unsigned long long)g_dc[best].chain[2], (unsigned long long)g_dc[best].chain[3], (unsigned long long)g_dc[best].chain[4], (unsigned long long)g_dc[best].chain[5], (unsigned long long)g_dc[best].chain[6]);
        g_dc[best].n = 0;
    }
}
