// Test automation for unattended runs. Switches (all off by default):
//   ATTILA_TEST_HIDDEN=1        the game window stays hidden (never full screen, never in front, never on the desktop)
//   ATTILA_TEST_MUTE=1          the game's sound is silenced
//   ATTILA_AUTO_BENCHMARK=1     starts the game's own benchmark (benchmarks/londinium.benchmark) once the main menu is up;
//                               =<path> names another .benchmark file. ATTILA_AUTO_BENCHMARK_DELAY=s (default 10) waits
//                               that long after the front end appears.
//   ATTILA_TEST_SHOTS=5,8,13    pictures of the game's own back buffer at these benchmark seconds (DirectX 11 and OpenGL
//                               routes; taken inside the game, the desktop is never captured), half size, into
//   ATTILA_TEST_SHOT_DIR=folder as shot_SSS.bmp
//   ATTILA_TEST_CROP=x,y,w,h    with each picture also crop_SSS.bmp: that rectangle at full size (for judging edges)
//   ATTILA_TEST_END=s           ends the test process s benchmark seconds in (after the last picture)
//   ATTILA_TEST_TIMEOUT=s       ends the test process s seconds after start whatever happened (default 420 when any
//                               of the switches above is set)
// How the benchmark is started: the game's own handler for "benchmark file chosen", UIDLL::BenchmarkLoadCallbacks::OnAccept
// (2c85270), is called with the file's path; it only uses that path and the front end's own globals. Benchmark time is
// read from the engine's BENCHMARK_STATS (WS_CONTEXT::m_e_engine 52516d0 -> +0x71640; +1 paused, +0 first frame pending,
// +8 microseconds so far), which WS_ENGINE_IMP::present feeds once per frame (3107ba8).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "loader.h"
#include "test_auto.h"

static int flag(const char *name, int *cache) {
    if (*cache < 0) { const char *e = getenv(name); *cache = e && *e && strcmp(e, "0"); }
    return *cache;
}
int test_hidden(void) { static int c = -1; return flag("ATTILA_TEST_HIDDEN", &c); }
int test_mute(void) { static int c = -1; return flag("ATTILA_TEST_MUTE", &c); }
const char *test_out_dir(void) { const char *e = getenv("ATTILA_TEST_SHOT_DIR"); return e && *e ? e : NULL; }

static uint8_t *bench_stats(void) {
    uint8_t *engine = *(uint8_t **)0x52516d0;
    return engine ? *(uint8_t **)(engine + 0x71640) : NULL;
}
// A replay test (ATTILA_AUTO_REPLAY) has no benchmark clock: its seconds are the battle's own steps (the hook in
// task_wait.inc counts them; 10 a second), counted from the first step that runs beside the drawing. Replays play the
// same battle every time, so the same second shows the same moment in every run, also when the PC stalls for a while.
// (Until 2026-11-13 this was the wall clock since the first step: a run that froze for 53 s after the start then filmed
// and measured a different moment of the battle than the others.)
extern volatile long g_battle_steps_total, g_battle_steps_base;
static int g_replay;                 // 1 = this run plays a replay instead of the benchmark
static ULONGLONG g_battle_start;     // GetTickCount64 at the first battle step, 0 = not yet
// ATTILA_TEST_CLOCK=front: test seconds are wall-clock seconds since the front end (main menu) first came up, for tests of
// menus, lobbies and the campaign (scripted input, pictures, end). The local multiplayer test (steam_hub.c) uses it too.
static ULONGLONG g_front_first;
double test_front_seconds(void) { return g_front_first ? (double)(GetTickCount64() - g_front_first) / 1000.0 : -1; }
double test_bench_seconds(void) {
    static int front = -1;
    if (front < 0) { const char *c = getenv("ATTILA_TEST_CLOCK"); front = c && !strcmp(c, "front"); }
    if (front) return test_front_seconds();
    if (g_replay) {
        if (!g_battle_start) return -1;
        long base = g_battle_steps_base;
        if (base < 0) {   // no step beside the drawing yet: the opening steps, or a PC where the battle never runs beside the drawing
            if (GetTickCount64() - g_battle_start < 10000) return 0;
            base = 0;
        }
        return (double)(g_battle_steps_total - base) / 10.0;
    }
    uint8_t *st = bench_stats();
    if (!st || st[1] || st[0]) return -1;
    return *(uint32_t *)(st + 8) / 1000000.0;
}

// ---- starting the benchmark
typedef SYSV void (*UniCtorFn)(void *, const uint32_t *);
typedef SYSV void (*UniDtorFn)(void *);
typedef SYSV uint8_t (*OnAcceptFn)(void *, const void *, const void *);
void test_auto_tick(void) {
    static int state = -1;          // -1 not looked yet, 0 off, 1 waiting for the front end, 2 waiting out the delay, 3 done
    static ULONGLONG seen, started;
    static double delay;
    static char path[260];
    if (!g_front_first && *(void **)0x5167298) {   // UIDLL::FRONT_END_UI::s_ui: the front end is up
        g_front_first = GetTickCount64();
        if (getenv("ATTILA_TEST_CLOCK")) log_line("test clock: front end up, test seconds count from here");
    }
    if (state == 0 || state == 3) return;
    if (state < 0) {
        const char *e = getenv("ATTILA_AUTO_BENCHMARK"), *rp = getenv("ATTILA_AUTO_REPLAY");
        if (rp && *rp) {   // a saved battle replay (file name in the replays folder of the game's home) instead
            static const uint8_t rhead[8] = {0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x89, 0xfb};   // FRONT_END::battle_replay 278c4c0
            if (memcmp((void *)0x278c4c0, rhead, sizeof rhead)) { log_line("auto replay: game code differs, not started"); state = 0; return; }
            g_replay = 1;
            snprintf(path, sizeof path, "%s", rp);
            const char *d = getenv("ATTILA_AUTO_BENCHMARK_DELAY");
            delay = d ? atof(d) : 10;
            state = 1;
            started = GetTickCount64();
            return;
        }
        if (!e || !*e || !strcmp(e, "0")) { state = 0; return; }
        snprintf(path, sizeof path, "%s", strcmp(e, "1") ? e : "benchmarks/londinium.benchmark");
        const char *d = getenv("ATTILA_AUTO_BENCHMARK_DELAY");
        delay = d ? atof(d) : 10;
        static const uint8_t head[11] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x81, 0xec, 0x58, 0x04};
        if (memcmp((void *)0x2c85270, head, sizeof head)) { log_line("auto benchmark: game code differs, not started"); state = 0; return; }
        state = 1;
        started = GetTickCount64();
    }
    void *front_end_ui = *(void **)0x5167298;   // UIDLL::FRONT_END_UI::s_ui
    if (state == 1) {
        if (!front_end_ui) return;
        seen = GetTickCount64();
        state = 2;
        log_line("%s: front end up after %.1f s, starting %s in %.0f s", g_replay ? "auto replay" : "auto benchmark", (seen - started) / 1000.0, path, delay);
        return;
    }
    if (!front_end_ui) { state = 1; return; }
    if (GetTickCount64() - seen < (ULONGLONG)(delay * 1000)) return;
    state = 3;
    uint32_t wide[260];
    size_t n = 0;
    for (; path[n] && n < 259; n++) wide[n] = (uint8_t)path[n];
    wide[n] = 0;
    uint64_t uni[2] = {0, 0}, self[16] = {0};
    ((UniCtorFn)0x51b5e0)(uni, wide);
    if (g_replay) {   // what the Replays screen's "Watch Replay" does (LoadReplayCallbacks::OnAccept 2a112a0): FRONT_END::battle_replay(name)
        void *front_end = *(void **)((uint8_t *)front_end_ui + 0x2a0);
        if (front_end) ((SYSV void (*)(void *, const void *))0x278c4c0)(front_end, uni);
        ((UniDtorFn)0x51ba50)(uni);
        log_line("auto replay: asked the game to play %s (%s)", path, front_end ? "sent" : "no front end object");
        return;
    }
    uint8_t r = ((OnAcceptFn)0x2c85270)(self, uni, uni);
    ((UniDtorFn)0x51ba50)(uni);
    log_line("auto benchmark: asked the game to load %s (answer %u)", path, r);
}

// ---- pictures at benchmark seconds
static int g_shots[64], g_nshots = -1, g_next;
static void shots_init(void) {
    g_nshots = 0;
    const char *e = getenv("ATTILA_TEST_SHOTS");
    while (e && *e && g_nshots < 64) {
        g_shots[g_nshots++] = atoi(e);
        e = strchr(e, ',');
        if (e) e++;
    }
}
// ATTILA_TEST_SHOT_EVERY=s: after the listed seconds, one more picture every s seconds (long unattended runs).
// test_shot_now (the test command file's "shot"): one picture at the next frame.
static volatile int g_shot_now;
void test_shot_now(void) { g_shot_now = 1; }
int test_shot_due(void) {
    static int every = -1, last;
    if (every < 0) { const char *e = getenv("ATTILA_TEST_SHOT_EVERY"); every = e ? atoi(e) : 0; if (every < 0) every = 0; }
    if (g_nshots < 0) shots_init();
    double t = test_bench_seconds();
    if (g_shot_now && t >= 0) { g_shot_now = 0; return last = (int)t; }
    if (g_next >= g_nshots) {
        if (every <= 0 || t < 0) return -1;
        int due = (g_nshots ? (last > g_shots[g_nshots - 1] ? last : g_shots[g_nshots - 1]) : last) + every;
        if (t < due) return -1;
        return last = (int)t;
    }
    if (t < 0 || t < g_shots[g_next]) return -1;
    return last = g_shots[g_next++];
}
void test_shot_save(int second, const uint8_t *pixels, int w, int h, int row_bytes, const char *order, int bottom_up) {
    const char *dir = test_out_dir();
    if (!dir || w < 2 || h < 2) return;
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\shot_%03d.bmp", dir, second);
    FILE *f = fopen(path, "wb");
    if (!f) { log_line("test picture: cannot write %s", path); return; }
    int ow = w / 2, oh = h / 2, row = ow * 3, pad = (4 - row % 4) % 4;
    int ri = order[0] == 'r' ? 0 : 2, bi = 2 - ri;
    uint8_t hdr[54] = {'B', 'M'};
    *(uint32_t *)(hdr + 2) = 54 + (uint32_t)(row + pad) * (uint32_t)oh; *(uint32_t *)(hdr + 10) = 54; *(uint32_t *)(hdr + 14) = 40;
    *(int32_t *)(hdr + 18) = ow; *(int32_t *)(hdr + 22) = oh; *(uint16_t *)(hdr + 26) = 1; *(uint16_t *)(hdr + 28) = 24;
    fwrite(hdr, 1, 54, f);
    uint8_t *line = calloc(1, (size_t)(row + pad) + 4);
    for (int oy = 0; oy < oh; oy++) {           // BMP rows go bottom-up
        int top_row = oh - 1 - oy;              // output row counted from the top of the picture
        int y0 = bottom_up ? h - 1 - (top_row * 2) : top_row * 2, y1 = bottom_up ? y0 - 1 : y0 + 1;
        const uint8_t *a = pixels + (size_t)y0 * (size_t)row_bytes, *b = pixels + (size_t)y1 * (size_t)row_bytes;
        for (int x = 0; x < ow; x++) {
            const uint8_t *p0 = a + x * 8, *p1 = p0 + 4, *p2 = b + x * 8, *p3 = p2 + 4;
            line[x * 3] = (uint8_t)((p0[bi] + p1[bi] + p2[bi] + p3[bi] + 2) / 4);
            line[x * 3 + 1] = (uint8_t)((p0[1] + p1[1] + p2[1] + p3[1] + 2) / 4);
            line[x * 3 + 2] = (uint8_t)((p0[ri] + p1[ri] + p2[ri] + p3[ri] + 2) / 4);
        }
        fwrite(line, 1, (size_t)(row + pad), f);
    }
    free(line);
    fclose(f);
    log_line("test picture: benchmark second %d saved (%dx%d)", second, ow, oh);
    // ATTILA_TEST_CROP=x,y,w,h: the same moment again as crop_SSS.bmp, that rectangle only (counted from the top left, in
    // the picture's own pixels) and at full size. For looking at edges: anti-aliasing cannot be judged at half size.
    const char *crop = getenv("ATTILA_TEST_CROP");
    int cx, cy, cw, ch;
    if (!crop || sscanf(crop, "%d,%d,%d,%d", &cx, &cy, &cw, &ch) != 4 || cx < 0 || cy < 0 || cw < 1 || ch < 1 || cx + cw > w || cy + ch > h) return;
    snprintf(path, sizeof path, "%s\\crop_%03d.bmp", dir, second);
    if (!(f = fopen(path, "wb"))) return;
    row = cw * 3; pad = (4 - row % 4) % 4;
    memset(hdr + 2, 0, 52);
    *(uint32_t *)(hdr + 2) = 54 + (uint32_t)(row + pad) * (uint32_t)ch; *(uint32_t *)(hdr + 10) = 54; *(uint32_t *)(hdr + 14) = 40;
    *(int32_t *)(hdr + 18) = cw; *(int32_t *)(hdr + 22) = ch; *(uint16_t *)(hdr + 26) = 1; *(uint16_t *)(hdr + 28) = 24;
    fwrite(hdr, 1, 54, f);
    line = calloc(1, (size_t)(row + pad) + 4);
    for (int oy = 0; oy < ch; oy++) {
        int top_row = cy + ch - 1 - oy, y = bottom_up ? h - 1 - top_row : top_row;
        const uint8_t *a = pixels + (size_t)y * (size_t)row_bytes + (size_t)cx * 4;
        for (int x = 0; x < cw; x++) { line[x * 3] = a[x * 4 + bi]; line[x * 3 + 1] = a[x * 4 + 1]; line[x * 3 + 2] = a[x * 4 + ri]; }
        fwrite(line, 1, (size_t)(row + pad), f);
    }
    free(line);
    fclose(f);
}

// ---- ending the run
static unsigned g_frame_draws;
void test_note_draws(unsigned draws) { g_frame_draws = draws; }
void test_frame_done(void) {
    static double end = -2, timeout;
    static ULONGLONG start;
    if (end == -2) {
        const char *e = getenv("ATTILA_TEST_END"), *t = getenv("ATTILA_TEST_TIMEOUT");
        end = e ? atof(e) : -1;
        int any = e || getenv("ATTILA_TEST_SHOTS") || getenv("ATTILA_AUTO_BENCHMARK") || test_hidden();
        timeout = t ? atof(t) : any ? 420 : 0;
        start = GetTickCount64();
    }
    if (g_replay && !g_battle_start && g_battle_steps_total > 0) { g_battle_start = GetTickCount64(); log_line("auto replay: the battle is running"); }
    if (g_replay && g_battle_start) {
        static int told;
        if (!told && g_battle_steps_base >= 0) { told = 1; log_line("auto replay: battle clock started after %ld opening steps, %.1f s after the first step", g_battle_steps_base, (GetTickCount64() - g_battle_start) / 1000.0); }
    }
    double b = test_bench_seconds();
    {   // The run's own speed figure: frames shown between benchmark seconds 3 and 60, timed by the clock, plus the
        // longest single frame and how many frames took over 100 ms (hitches). One log line when second 60 is reached.
        static LARGE_INTEGER f, t0, t1;
        static unsigned frames, hitches;
        static unsigned long long draws;
        static double longest, from = -1, to = 60;
        static int done;
        if (from < 0) {   // ATTILA_TEST_FPS_WINDOW=a,b: other seconds than 3 to 60 (a replay test measures while the camera sits in the fight)
            const char *w = getenv("ATTILA_TEST_FPS_WINDOW");
            from = 3;
            if (w) { double a2 = 0, b2 = 0; if (sscanf(w, "%lf,%lf", &a2, &b2) == 2 && b2 > a2) { from = a2; to = b2; } }
        }
        if (!done && b >= from) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            if (!frames) { QueryPerformanceFrequency(&f); t0 = t1 = now; }
            if (b < to) {
                double ms = (double)(now.QuadPart - t1.QuadPart) * 1000.0 / (double)f.QuadPart;
                if (ms > longest) longest = ms;
                if (ms > 100) hitches++;
                frames++; t1 = now; draws += g_frame_draws;
            } else {
                done = 1;
                double s = (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
                if (frames > 1 && s > 0) log_line("test run: average frame rate between %s seconds %.0f and %.0f: %.1f fps (%u frames in %.1f s; longest frame %.0f ms, %u frames over 100 ms; %.0f draw calls a frame)", g_replay ? "battle" : "benchmark", from, to, (frames - 1) / s, frames, s, longest, hitches, frames ? (double)draws / frames : 0.0);
            }
        }
    }
    {   // ATTILA_TEST_QUIT=s: at that benchmark second the game is asked to close the normal way (a window-close event), so
        // its own shutdown runs; ATTILA_TEST_END / the time limit still end the process if it does not.
        static double quit = -2;
        static int asked;
        if (quit == -2) { const char *q = getenv("ATTILA_TEST_QUIT"); quit = q ? atof(q) : -1; }
        if (quit >= 0 && b >= quit && !asked) {
            asked = 1;
            HMODULE sdl = GetModuleHandleA("SDL2.dll");
            int (__cdecl *push)(void *) = sdl ? (void *)GetProcAddress(sdl, "SDL_PushEvent") : NULL;
            uint32_t ev[14] = {0x100 /* SDL_QUIT */, 0};
            log_line("test run: benchmark second %.1f, asking the game to close (%s)", b, push ? "sent" : "SDL_PushEvent not found");
            if (push) push(ev);
        }
    }
    if (end >= 0 && b >= end) { log_go_sync(); log_line("test run: benchmark second %.1f reached, ending", b); TerminateProcess(GetCurrentProcess(), 0); }
    if (timeout > 0 && GetTickCount64() - start > (ULONGLONG)(timeout * 1000)) { log_go_sync(); log_line("test run: time limit of %.0f s reached, ending", timeout); TerminateProcess(GetCurrentProcess(), 3); }
}
