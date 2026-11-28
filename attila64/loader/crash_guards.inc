// Crash guards (included by loader.c after interface.inc, which has hook_entry).
//
// ---- CA::UniString::substr(pos, n) with pos past the end of the text. Crash reports 2026-11-26 on 1.3.30: c0000005 at 4EA6D0 and
// 4EA720 (both copy loops of substr), on the main menu or the campaign start screen, 10 reports from at least 3 PCs, OpenGL and
// DirectX 11, with and without JGdaCaesar's engine fix. The game never checks pos: size - pos wraps round, and the copy reads from
// up to 8 GB past the text. The registers showed pos of about -0x200 and lengths 484 and 4. Which caller hands it the bad position
// is not known yet (the old stack lines were data, not calls). Now a pos past the end gives an empty text, as for pos == size, and
// the log names the caller and the start of the text, once per caller (up to 12). Off with ATTILA_NO_TEXT_GUARD=1.
// Test: ATTILA_TEST_SUBSTR=N makes the Nth call ask for a position 0x200 past the end (the guard catches it);
// ATTILA_TEST_SUBSTR=N,crash does the same without the guard (a real crash in substr, for testing crash reports and dumps).
typedef SYSV void *(*SubstrFn)(void *out, const void *self, unsigned pos, unsigned n);
static SubstrFn g_substr_orig;
static long g_substr_test_at;
static int g_substr_test_crash, g_substr_guard_on = 1;
static volatile LONG g_substr_calls;
const char *loader_where(void *addr);

__attribute__((noinline)) static void substr_note(const void *self, unsigned pos, unsigned size, void *from) {
    static void *seen[12];
    static volatile LONG nseen;
    LONG k = nseen;
    for (LONG i = 0; i < k && i < 12; i++) if (seen[i] == from) return;
    LONG i = InterlockedIncrement(&nseen) - 1;
    if (i >= 12) return;
    seen[i] = from;
    char text[44] = "";   // the start of the text, plain letters only
    const uint16_t *d = *(const uint16_t *const *)((const uint8_t *)self + 8);
    for (unsigned j = 0; d && j < size && j < 40; j++) text[j] = (d[j] >= 32 && d[j] < 127 && d[j] != '\\') ? (char)d[j] : '?', text[j + 1] = 0;
    log_line("text guard: UniString::substr asked for letter %d of a %u-letter text '%s' (called from %s): empty text given, no crash",
             (int)pos, size, text, loader_where(from));
}

static SYSV void *substr_guard(void *out, const void *self, unsigned pos, unsigned n) {
    if (g_substr_test_at && InterlockedIncrement(&g_substr_calls) == g_substr_test_at && self) {
        pos = *(const unsigned *)self + 0x200;
        log_line("text guard test: call %ld asks for letter %u%s", g_substr_test_at, pos, g_substr_test_crash ? " WITHOUT the guard (a crash follows)" : "");
        if (g_substr_test_crash) return g_substr_orig(out, self, pos, n);
    }
    unsigned size = self ? *(const unsigned *)self : 0;
    if (pos > size && g_substr_guard_on) {
        substr_note(self, pos, size, __builtin_return_address(0));
        pos = size;
    }
    return g_substr_orig(out, self, pos, n);
}

// ---- The cause of those crashes, found with the 1.3.31 call chain (and the old reports' stack lines read as addresses, not
// offsets): UTILITYDLL::LUA::StringSubReplacement (2de7e40), the game's own string.sub for Lua scripts (here a mod's script
// run through require on the main menu). It counts a negative END from the end as Lua does, but not a negative START:
// string.sub(s, -3) became substr(-4, length + 4). In 32-bit Attila the position wraps round to just before the text (a wrong
// text, no crash); in 64-bit it is 8 GB away. Its one call to substr (2de801e) now goes to lua_sub_fix, which counts a
// negative start from the end as Lua 5.1 does (-1 = the last letter; before the first letter = the first letter).
// Off with ATTILA_NO_LUA_SUB_FIX=1 (the text guard above still stops the crash).
static SYSV void *lua_sub_fix(void *out, const void *self, unsigned pos, unsigned n) {
    if ((int)pos < 0 && self) {   // pos = start - 1, n = end - pos (end already counted from the end and capped at the length)
        int len = (int)*(const unsigned *)self, end = (int)pos + (int)n, start = (int)pos + 1 + len + 1;
        if (start < 1) start = 1;
        int count = end - (start - 1);
        pos = (unsigned)(start - 1);
        n = count > 0 ? (unsigned)count : 0;
        static volatile LONG noted;
        if (InterlockedIncrement(&noted) <= 3) log_line("Lua string.sub with a start counted from the end: letters %u..%d of %d", pos + 1, end, len);
    }
    return substr_guard(out, self, pos, n);
}
static void patch_lua_sub(void) {
    if (getenv("ATTILA_NO_LUA_SUB_FIX")) return;
    uint8_t *call = (uint8_t *)0x2de801e;
    int32_t want = (int32_t)(0x4ea5c0 - (0x2de801e + 5));
    if (call[0] != 0xE8 || memcmp(call + 1, &want, 4)) { log_line("patch skipped, game code differs: Lua string.sub start"); return; }
    uint8_t *s = near_game_code(16);
    if (!s) { log_line("patch skipped, no memory near the game: Lua string.sub start"); return; }
    s[0] = 0x48; s[1] = 0xB8;                                    // mov rax,lua_sub_fix; jmp rax (arguments untouched)
    uint64_t fn = (uint64_t)lua_sub_fix;
    memcpy(s + 2, &fn, 8);
    s[10] = 0xFF; s[11] = 0xE0;
    int32_t rel = (int32_t)(s - (call + 5));
    memcpy(call + 1, &rel, 4);
    log_line("patch applied: Lua string.sub counts a negative start from the end");
    if (getenv("ATTILA_TEST_LUA_SUB")) {   // test: what the game's string.sub hands substr for ("abcdef", -3), ("abcdef", -10), ("abcdef", 2, -2)
        static const uint16_t six[7] = {'a', 'b', 'c', 'd', 'e', 'f', 0};
        struct { unsigned size, cap; const uint16_t *data; } s = {6, 6, six};
        static const int cases[3][2] = {{-4, 10}, {-11, 17}, {1, 4}};   // (start - 1, end - (start - 1)) as StringSubReplacement makes them
        for (int i = 0; i < 3; i++) {
            struct { unsigned size, cap; uint16_t *data; } r = {0, 0, NULL};
            lua_sub_fix(&r, &s, (unsigned)cases[i][0], (unsigned)cases[i][1]);
            char t[16] = "";
            for (unsigned j = 0; r.data && j < r.size && j < 15; j++) t[j] = (char)r.data[j], t[j + 1] = 0;
            log_line("Lua string.sub test %d: '%s' (%u letters)", i + 1, t, r.size);
        }
    }
}

// ---- AI personality crash (2 reports 2026-11-26 from one player, 1.3.30, 1212AD + perffix, starting a new campaign: c0000005
// at 18F1334 in CAI_PERSONALITY::CAI_PERSONALITY reading [rbx+0x70] with rbx = 4C8_00000005, an address whose low half was
// overwritten with 5). CAI_PERSONALITY_DEAL_GENERATION_COMPONENT's constructor (1b342a0) fills three tables with room for 20
// deal generator types (CAI_DIPLOMATIC_DEAL_GENERATOR_TYPE 0..19: 9 floats at +0, 3 values at +0x2d0, one int at +0x3c0, object
// 0x410 bytes) from the generator priorities table, using each row's generator type as the index WITHOUT a check. A row with
// type 20 writes its priority (here 5) at +0x410, 4 bytes past the object: INDEXED_STORAGE<...>::get (1928960) builds it at
// rsp+0x420 of a 0x830 frame, so +0x410 is the saved rbx of CAI_PERSONALITY's constructor. In 32-bit Attila those 4 bytes land
// on something harmless. Now a row whose type is not 0..19 is skipped (it has no slot anyway) and logged.
// The type number is read at 1b343dc; the 10 bytes at 1b343d8 go to a stub that checks it. ATTILA_NO_DEAL_GEN_FIX=1: off.
typedef SYSV void (*DealGenCtorFn)(void *self, const void *record, void *manager);
static DealGenCtorFn g_dealgen_orig;
static volatile LONG *g_dealgen_skipped, *g_dealgen_last;
static SYSV void dealgen_ctor(void *self, const void *record, void *manager) {
    LONG before = *g_dealgen_skipped;
    g_dealgen_orig(self, record, manager);
    if (*g_dealgen_skipped != before) {
        static volatile LONG noted;
        if (InterlockedIncrement(&noted) <= 5)
            log_line("AI personality: a deal generator row with type %ld (only 0..19 exist) was skipped; before 1.3.31 it overwrote memory "
                     "and crashed starting a campaign. It comes from the AI personality data of a mod or the game.", (long)*g_dealgen_last);
    }
}
static void patch_deal_generation(void) {
    if (getenv("ATTILA_NO_DEAL_GEN_FIX")) return;
    uint8_t *at = (uint8_t *)0x1b343d8;
    static const uint8_t old[10] = {0x48, 0x8B, 0x4A, 0x08, 0x8B, 0x71, 0x10, 0x48, 0x63, 0xCE};   // mov rcx,[rdx+8]; mov esi,[rcx+0x10]; movsxd rcx,esi
    static const uint8_t ctor_old[5] = {0x55, 0x41, 0x57, 0x41, 0x56};                              // push rbp; push r15; push r14
    if (memcmp(at, old, sizeof old) || memcmp((void *)0x1b342a0, ctor_old, 5)) { log_line("patch skipped, game code differs: AI deal generator check"); return; }
    uint8_t *s = near_game_code(64);
    if (!s) { log_line("patch skipped, no memory near the game: AI deal generator check"); return; }
    const char *lim = getenv("ATTILA_TEST_DEAL_GEN_LIMIT");   // test only: a lower limit, so real rows take the skip path
    uint8_t limit = lim ? (uint8_t)atoi(lim) : 20;
    int n = 0;
    memcpy(s + n, old, 7); n += 7;                            // mov rcx,[rdx+8]; mov esi,[rcx+0x10]
    s[n++] = 0x83; s[n++] = 0xFE; s[n++] = limit;            // cmp esi,20
    s[n++] = 0x73; int jae = n++;                             // jae skip (a negative type counts as too big)
    s[n++] = 0x48; s[n++] = 0x63; s[n++] = 0xCE;              // movsxd rcx,esi
    s[n++] = 0xE9; int32_t r1 = (int32_t)(0x1b343e2 - (intptr_t)(s + n + 4)); memcpy(s + n, &r1, 4); n += 4;   // jmp back
    s[jae] = (uint8_t)(n - (jae + 1));
    uint8_t *cnt = s + 48, *last = s + 52;
    s[n++] = 0xF0; s[n++] = 0xFF; s[n++] = 0x05; int32_t r2 = (int32_t)(cnt - (s + n + 4)); memcpy(s + n, &r2, 4); n += 4;   // lock inc dword [cnt]
    s[n++] = 0x89; s[n++] = 0x35; int32_t r3 = (int32_t)(last - (s + n + 4)); memcpy(s + n, &r3, 4); n += 4;            // mov [last],esi
    s[n++] = 0xE9; int32_t r4 = (int32_t)(0x1b3445e - (intptr_t)(s + n + 4)); memcpy(s + n, &r4, 4); n += 4;             // jmp to the next row
    if (n > 48) { log_line("patch skipped, stub too long: AI deal generator check"); return; }
    memset(cnt, 0, 8);
    g_dealgen_skipped = (volatile LONG *)cnt;
    g_dealgen_last = (volatile LONG *)last;
    g_dealgen_orig = (DealGenCtorFn)hook_entry(0x1b342a0, ctor_old, 5, (void *)dealgen_ctor);
    if (!g_dealgen_orig) { log_line("patch skipped, constructor hook refused: AI deal generator check"); return; }
    uint8_t jump[10] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90};
    int32_t rj = (int32_t)(s - (at + 5));
    memcpy(jump + 1, &rj, 4);
    memcpy(at, jump, sizeof jump);
    log_line("patch applied: AI deal generator types outside 0..%d are skipped (no memory overwrite)%s", limit - 1, lim ? " (TEST LIMIT)" : "");
}

// ---- Missing campaign map terrain (1.3.30 crash report 2026-11-26 22:56 UTC, a mod's grand campaign, Charlemagne faction
// cha_fact_cordoba): c0000005 at 32CCF10 TILE_DATABASE::TILE_VARIATION::tile() on NULL. CAMPAIGN_VIEW ->
// CAMPAIGN_TERRAIN_DISPLAY -> create_terrain_setup -> TERRAIN_SETUP ctor -> build_terrain -> create_single_tile_battle
// (3473060): at 3473086 TILE_DATABASE::tile_variation_from_location(this=[r14+0x10], name=r14) gives NULL when the map's
// terrain is not in the loaded packs, and 34730e3 calls tile() on it unchecked. The call now goes through terrain_lookup,
// which logs the map's name once when it is not found. The game still crashes after that (there is no terrain to build),
// but the log says which map a mod failed to supply. ATTILA_NO_TERRAIN_NAME=1: off.
typedef SYSV void *(*TileVarFn)(void *db, const void *name);
static TileVarFn g_tilevar = (TileVarFn)0x32d01a0;
static SYSV void *terrain_lookup(void *db, const void *name) {
    void *r = g_tilevar(db, name);
    if (!r) {
        static volatile LONG noted;
        if (InterlockedIncrement(&noted) <= 3) {
            // CA::String::get_temporary_chars (4e8c60) is "mov rax,[rdi+8]": the text pointer at +8, 8-bit letters
            char shown[200] = "?";
            const char *text = name ? *(const char *const *)((const uint8_t *)name + 8) : NULL;
            MEMORY_BASIC_INFORMATION mb;
            if (text && VirtualQuery(text, &mb, sizeof mb) && mb.State == MEM_COMMIT && !(mb.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                size_t room = (size_t)((const uint8_t *)mb.BaseAddress + mb.RegionSize - (const uint8_t *)text), i = 0;
                for (; i < sizeof shown - 1 && i < room && text[i]; i++) shown[i] = (text[i] >= 32 && text[i] < 127) ? text[i] : '?';
                shown[i] = 0;
            }
            log_line("campaign map terrain not found: '%s' - a mod's map files are missing or not loaded (the game will crash building the map)", shown);
            log_go_sync();
        }
    }
    return r;
}
static void patch_terrain_name(void) {
    if (getenv("ATTILA_NO_TERRAIN_NAME")) return;
    uint8_t *call = (uint8_t *)0x3473086;
    int32_t want = (int32_t)(0x32d01a0 - (0x3473086 + 5));
    if (call[0] != 0xE8 || memcmp(call + 1, &want, 4)) { log_line("patch skipped, game code differs: campaign map terrain name"); return; }
    uint8_t *s = near_game_code(16);
    if (!s) { log_line("patch skipped, no memory near the game: campaign map terrain name"); return; }
    s[0] = 0x48; s[1] = 0xB8;                                    // mov rax,terrain_lookup; jmp rax (arguments untouched)
    uint64_t fn = (uint64_t)terrain_lookup;
    memcpy(s + 2, &fn, 8);
    s[10] = 0xFF; s[11] = 0xE0;
    int32_t rel = (int32_t)(s - (call + 5));
    memcpy(call + 1, &rel, 4);
    log_line("patch applied: a campaign map terrain that is not found is named in the log");
}

static void patch_crash_guards(void) {
    patch_deal_generation();
    patch_terrain_name();
    if (getenv("ATTILA_NO_TEXT_GUARD")) { log_line("text guard: off (ATTILA_NO_TEXT_GUARD)"); return; }
    const char *t = getenv("ATTILA_TEST_SUBSTR");
    if (t) { g_substr_test_at = strtol(t, NULL, 10); g_substr_test_crash = strstr(t, "crash") != NULL; }
    static const uint8_t old[5] = {0x55, 0x41, 0x57, 0x41, 0x56};   // push rbp; push r15; push r14
    g_substr_orig = (SubstrFn)hook_entry(0x4ea5c0, old, 5, (void *)substr_guard);
    log_line(g_substr_orig ? "patch applied: text guard (UniString::substr past the end gives an empty text)"
                           : "patch skipped, game code differs: text guard");
    if (g_substr_orig) patch_lua_sub();   // lua_sub_fix goes through substr_guard
}
