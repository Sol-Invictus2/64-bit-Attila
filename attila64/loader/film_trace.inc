// film_trace.inc - TEST ONLY (switch ATTILA_FILM_TRACE=1, off by default): how evenly a film's frames advance.
// WARSCAPE::VP8::MOVIE::calculate_target_frame (342c4a0) runs once per game frame from VIDEO_CONTROLLER::update_video and
// sets the film frame to show (+0x74) from the sound engine's film position, or from gettimeofday when there is no sound.
// MOVIE: +0xc frame time (float), +0x64 frame count, +0x74 wanted frame, +0x78 last decoded, +0x7c shown, +0x8a.. flags.
// The hook only logs, then the game's own code has run.
typedef SYSV int (*FtTargetFn)(void *movie);
static FtTargetFn g_ft_target;
static float g_pos; static uint8_t g_run;
static SYSV int ft_target(void *movie) {
    static LARGE_INTEGER f, last_change, last_call;
    static int last_frame = -1, calls, lines, n_steps, n_skip, n_late;
    static double worst_gap, sum_gap, worst_call;
    static void *last_movie;
    LARGE_INTEGER t0, t1;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    int r = g_ft_target(movie);
    QueryPerformanceCounter(&t1);
    const uint8_t *m = (const uint8_t *)movie;
    int frame = *(const int32_t *)(m + 0x74), count = *(const int32_t *)(m + 0x64);
    float ft = *(const float *)(m + 0xc);
    static int film_lines;
    if (movie != last_movie || frame < last_frame) {
        if (last_movie && n_steps) log_line("film trace: film ended: %d frame steps, average %.1f ms, worst %.1f ms, %d steps jumped frames, %d steps over 1.5 frame times, longest game frame %.1f ms", n_steps, sum_gap / n_steps, worst_gap, n_skip, n_late, worst_call);
        last_movie = movie; last_frame = -1; film_lines = 0; calls = 0; n_steps = n_skip = n_late = 0; worst_gap = sum_gap = worst_call = 0; last_call.QuadPart = 0;
        log_line("film trace: new film: %d frames, frame time %.3f, flags %u %u %u %u %u %u", count, ft, m[0x88], m[0x89], m[0x8a], m[0x8b], m[0x8c], m[0x8d]);
    }
    if (last_call.QuadPart) { double c = (double)(t0.QuadPart - last_call.QuadPart) * 1000.0 / (double)f.QuadPart; if (c > worst_call) worst_call = c; }
    last_call = t0;
    calls++;
    {   // the sound engine's film clock: engine (52516d0) -> virtual +0x750 -> object; its +0x30 = running?, +0x38 = position (float)
        static int told; static float last_pos = -1; static LARGE_INTEGER last_pos_t; static int pos_lines;
        void **eng = *(void ***)(uintptr_t)0x52516d0;
        if (eng) {
            typedef SYSV void *(*GetFn)(void *); typedef SYSV uint8_t (*RunFn)(void *); typedef SYSV float (*PosFn)(void *);
            void **obj = (void **)((GetFn)(*(void ***)eng)[0x750 / 8])(eng);
            if (obj) {
                void **vt = *(void ***)obj;
                if (!told++) log_line("film trace: clock object vtable %p: +0x10 %p, +0x18 %p, +0x20 %p, +0x30 %p, +0x38 %p", (void *)vt, vt[2], vt[3], vt[4], vt[6], vt[7]);
                uint8_t run = ((RunFn)vt[6])(obj);
                float pos = run ? ((PosFn)vt[7])(obj) : -1;
                g_pos = pos; g_run = run;
                if (pos != last_pos) {
                    double gap = last_pos_t.QuadPart ? (double)(t1.QuadPart - last_pos_t.QuadPart) * 1000.0 / (double)f.QuadPart : 0;
                    if (pos_lines < 0) { pos_lines++; log_line("film trace: clock %.4f -> %.4f (step %.4f) after %.1f ms, running %u", last_pos, pos, pos - last_pos, gap, run); }
                    last_pos = pos; last_pos_t = t1;
                }
            }
        }
    }
    if (frame != last_frame) {
        double gap = last_frame < 0 ? 0 : (double)(t1.QuadPart - last_change.QuadPart) * 1000.0 / (double)f.QuadPart;
        if (last_frame >= 0) {
            n_steps++; sum_gap += gap; if (gap > worst_gap) worst_gap = gap;
            if (frame - last_frame != 1) n_skip++;
            if (ft > 0 && gap > 1.5 * ft * (frame - last_frame > 0 ? frame - last_frame : 1) && gap > 50) n_late++;
            if (lines < 600 && (frame - last_frame != 1 || gap > 50 || gap < 20 || film_lines < 12)) { lines++; film_lines++; log_line("film trace: frame %d -> %d after %.1f ms, %d game frames, clock %.3f running %u; decoded %d shown %d limit %d paused %u us flags %u %u %u %u", last_frame, frame, gap, calls, g_pos, g_run, *(const int32_t *)(m + 0x78), *(const int32_t *)(m + 0x7c), *(const int32_t *)(m + 0x48), *(const uint32_t *)(m + 0xc8), m[0x8a], m[0x8b], m[0x8c], m[0x8d]); }
        }
        last_frame = frame; last_change = t1; calls = 0;
    }
    return r;
}
static void patch_film_trace(void) {
    if (!getenv("ATTILA_FILM_TRACE")) return;
    static const uint8_t old[6] = {0x55, 0x53, 0x48, 0x83, 0xEC, 0x28};   // push rbp; push rbx; sub rsp,0x28
    g_ft_target = (FtTargetFn)hook_entry(0x342c4a0, old, 6, (void *)ft_target);
    log_line("test: film trace (ATTILA_FILM_TRACE) %s", g_ft_target ? "on" : "NOT hooked");
}
