// The second per-soldier display list cut into smaller pieces. Display only. Part of loader.c (included after
// prep_parallel.inc, which uses near_game_code from game_fixes.inc).
// ATTILA_TRACKER_GRAIN2=n = piece size for that list (default 10; 0 = the game's), =flip = test.
//
// Found 2026-11-13 (task_waits.inc, kept clash): EMPIREBATTLE::ENTITY_DISPLAY::draw runs its per-soldier work
// (TBB_DRAW_TRACKER) twice a frame with tbb::parallel_for (a5a580), on two lists: about 10,650 soldiers (+e0), and about
// 309 (+f0) that cost about 3 times as much each (1.0 ms of work a frame between them). parallel_for cuts the range into
// len / (20 * (threads + 1)) and never less than the grain, rounded down to a multiple of the grain; the grain is the game's
// setting tbb_draw_tracker_grain (50), read at a5a1e2 for the first list and at a5a35a for the second. So the second list
// became 7 pieces for 8 threads, and the main thread spent 0.32 ms a frame in it. Here the second list's read (a5a35a,
// mov ecx,[rip+...]) reads our own 4-byte value near the game code instead: 10, so 31 pieces. The first list keeps the
// game's 50 (213 pieces). Every soldier gets the same work as before; only which thread takes which soldiers changes, as
// it does every frame anyway.
static uint32_t *g_tg_slot;
static int g_tg_flip;
static uint32_t g_tg_value = 10;

// test (ATTILA_TRACKER_GRAIN2=flip), with every fps line: our piece size and the game's, in turns
void tracker_grain_report(void) {
    if (!g_tg_flip || !g_tg_slot) return;
    uint32_t game = *(const uint32_t *)(uintptr_t)0x4e884e4;
    uint32_t now = *g_tg_slot;
    log_line("grain flip test: the window just logged had the second soldier list cut in pieces of %u (the game's setting: %u)", now, game);
    *g_tg_slot = now == g_tg_value ? game : g_tg_value;
}

static void patch_tracker_grain(void) {
    const char *e = getenv("ATTILA_TRACKER_GRAIN2");
    // OFF unless asked for (ATTILA_TRACKER_GRAIN2=n or =flip): measured 2026-11-13 (auto\flipgrain, in-run flip) the second
    // list's time fell 0.32 -> 0.20 ms a frame, but the first list got about 0.06 ms slower and the frame rate did not rise
    // (pieces of 10 against neighbours: -7 to +2%, about -1% on average).
    if (!e || !strcmp(e, "0")) return;
    if (e && strcmp(e, "flip")) { int v = atoi(e); if (v >= 1 && v <= 1000) g_tg_value = (uint32_t)v; }
    static const uint8_t read1[6] = {0x8B, 0x0D, 0xFC, 0xE2, 0x42, 0x04};   // a5a1e2: mov ecx,[tbb_draw_tracker_grain] (first list)
    static const uint8_t read2[6] = {0x8B, 0x0D, 0x84, 0xE1, 0x42, 0x04};   // a5a35a: the same read for the second list
    if (memcmp((void *)0xa5a1e2, read1, 6) || memcmp((void *)0xa5a35a, read2, 6)) {
        log_line("patch skipped, game code differs: second soldier list pieces"); return;
    }
    uint8_t *slot = near_game_code(8);
    int64_t disp = slot ? (int64_t)(slot - (uint8_t *)(uintptr_t)0xa5a360) : 0;   // from the end of the 6-byte instruction
    if (!slot || disp != (int64_t)(int32_t)disp) { log_line("patch skipped, no memory near the game: second soldier list pieces"); return; }
    g_tg_slot = (uint32_t *)slot;
    *g_tg_slot = g_tg_value;
    int32_t d = (int32_t)disp;
    memcpy((void *)(uintptr_t)0xa5a35c, &d, 4);
    g_tg_flip = e && !strcmp(e, "flip");
    log_line("patch applied: second soldier display list cut in pieces of %u%s", g_tg_value, g_tg_flip ? ", switched with the game's size with every fps line (test)" : "");
}
