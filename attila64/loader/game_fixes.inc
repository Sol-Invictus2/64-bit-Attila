// Small fixes and settings applied to the game's code and data at start: stack checks, the table of byte patches, the web
// monitor, music track bounds, the game's own settings, far-away sprite step, effect counter.
// Part of loader.c (included there, not built by itself; the order of the includes matters).

// Stack-check reads use fs:0x28, which is not usable on Windows. gs:0x28 is a stable per-thread value there, and the check
// only compares the value with itself, so changing the one prefix byte is enough.
static int patch_stack_checks(Module *m) {
    int n = 0;
    for (uint8_t *p = m->lo; p + 9 <= m->hi; p++) {
        if (p[0] != 0x64 || (p[1] & 0xF8) != 0x48) continue;
        if (p[2] != 0x8B && p[2] != 0x33 && p[2] != 0x2B && p[2] != 0x3B) continue;
        if ((p[3] & 0xC7) != 0x04 || p[4] != 0x25) continue;
        if (p[5] != 0x28 || p[6] || p[7] || p[8]) continue;
        p[0] = 0x65;
        n++;
    }
    log_line("%s: patched %d stack checks", m->pub.name, n);
    return n;
}

// Changes to the game's own code. Each checks the original bytes first and is skipped if they differ.
typedef struct { const char *name; uintptr_t addr; uint8_t old_bytes[8]; uint8_t new_bytes[8]; int len; } GamePatch;
static const GamePatch g_game_patches[] = {
    // The game's video memory budget is a 32-bit number, so a 16 GB card counts as 4095 MB. Going over it makes
    // TEXTURE_MANAGER::downgrade_features_to_target lower the graphics settings; returning at once leaves the real card memory as the limit.
    {"no lowering of settings at the 4 GB budget", 0x311f7e0, {0x55, 0x41, 0x57}, {0x31, 0xC0, 0xC3}, 3},
    // UIDLL::BattleSetupOptions::AreLargeArmiesEnabled turns large armies off above 4 players (above 2 for some battle types).
    // Allow them up to 16 players (8v8) on all battle types. All players must have this patch.
    {"large armies with up to 16 players", 0x2d54065, {0x83, 0xF8, 0x05}, {0x83, 0xF8, 0x11}, 3},
    {"large armies with up to 16 players, other battle types", 0x2d54060, {0x83, 0xF8, 0x03}, {0x83, 0xF8, 0x11}, 3},
    // UIDLL::BattleSetupOptions::OnUpdatePulse has the same rule built in, and it is the copy that keeps switching the button off.
    {"large armies button stays on up to 16 players", 0x2cd6a72, {0x83, 0xF8, 0x05}, {0x83, 0xF8, 0x11}, 3},
    {"large armies button stays on up to 16 players, other battle types", 0x2cd6a6d, {0x83, 0xF8, 0x03}, {0x83, 0xF8, 0x11}, 3},
    // EMPIRECAMPAIGN::CAMPAIGN_ENV::autosave_end_turn, multiplayer path: saved when the last faction of the round ends its turn AND
    // a check on the session object (vtable +0x110, likely host-only or an option) says yes. Now every player saves every round
    // ("and al,cl" -> "mov al,cl"). A save is written on each PC only; it changes nothing in the shared game.
    {"multiplayer campaign: every player autosaves every round", 0x1761444, {0x20, 0xC8}, {0x88, 0xC8}, 2},
    // WARSCAPE::WS_ENGINE_IMP::downgrade_options lowers graphics settings one step at a time; one step switches screen-space
    // reflections off for good ("Disabled SSR", engine +0x7171c = 0). That step is now always passed over ("je" -> "jmp"), so
    // reflections stay as the player set them. Display only.
    {"reflections are no longer switched off by the settings-lowering step", 0x310a352,
     {0x0F, 0x84, 0xA5, 0x0B, 0x00, 0x00}, {0xE9, 0xA6, 0x0B, 0x00, 0x00, 0x90}, 6},
    // WS_ENGINE_IMP::init_graphics_options_postdevice: when the render device is the OpenGL one (get_gfx_device_type() == 2, the
    // only renderer of the Linux game we run) it set the highest allowed reflections level to 0 in every preset (engine +0x7179c
    // and the preset copies), and cap_to_available then cut the player's setting to 0 at every start. The jump past it is made
    // unconditional, so the limit stays the engine's own (2). Display only.
    {"reflections are allowed with the OpenGL renderer", 0x3104410, {0x75, 0x46}, {0xEB, 0x46}, 2},
    // OPENGL_RENDER_DEVICE::update_adapter_specific_details and ::create_device put 0 as the highest texture filtering level in the
    // device caps (+0xeac = caps +0x24), and init_graphics_options_postdevice then cut the player's setting to trilinear at every
    // start. The renderer does draw anisotropic filtering (set_sampler_state, filter 0x55 -> GL_TEXTURE_MAX_ANISOTROPY), so the
    // limit becomes 4 = anisotropic 16x, the same as Windows Attila. Display only.
    {"anisotropic texture filtering with the OpenGL renderer (1/2)", 0x33d518b, {0xC7, 0x83, 0xAC, 0x0E, 0, 0, 0, 0}, {0xC7, 0x83, 0xAC, 0x0E, 0, 0, 4, 0}, 8},
    {"anisotropic texture filtering with the OpenGL renderer (2/2)", 0x33d5410, {0xC7, 0x83, 0xAC, 0x0E, 0, 0, 0, 0}, {0xC7, 0x83, 0xAC, 0x0E, 0, 0, 4, 0}, 8},
};

// The card's real memory in MB, from Windows: the largest dedicated figure among the adapters.
static unsigned card_memory_mb(void) {
    static const GUID iid_factory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
    HMODULE h = LoadLibraryA("dxgi.dll");
    HRESULT (WINAPI *create)(const GUID *, void **) = h ? (void *)GetProcAddress(h, "CreateDXGIFactory1") : NULL;
    IDXGIFactory1 *f = NULL;
    if (!create || FAILED(create(&iid_factory1, (void **)&f))) return 0;
    unsigned long long best = 0;
    IDXGIAdapter1 *a;
    for (UINT i = 0; f->lpVtbl->EnumAdapters1(f, i, &a) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(a->lpVtbl->GetDesc1(a, &d)) && d.DedicatedVideoMemory > best) best = d.DedicatedVideoMemory;
        a->lpVtbl->Release(a);
    }
    f->lpVtbl->Release(f);
    return (unsigned)(best >> 20);
}

// Room for small pieces of code the game can reach with its 32-bit jumps.
static uint8_t *near_game_code(size_t n) {
    static uint8_t *page;
    static size_t used;
    for (uint64_t at = 0x5300000; !page && at < 0x60000000; at += 0x10000)
        page = VirtualAlloc((void *)at, 0x2000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!page || used + n > 0x2000) return NULL;
    uint8_t *p = page + used;
    used += n;
    return p;
}

// The online services thread keeps checking for finished web requests. Curl is a stand-in here, so requests never finish and the loop
// never waits: it used a whole CPU core all the time. It now rests after releasing its lock each time round, so nobody waits on it.
static SYSV int curl_monitor_unlock(void *mutex) {
    SYSV int (*unlock)(void *) = (SYSV int (*)(void *))0x409d20;  // the game's own route to pthread_mutex_unlock
    int r = unlock(mutex);
    Sleep(10);
    return r;
}

static void patch_curl_monitor(void) {
    uint8_t *call = (uint8_t *)0x2849320;
    int32_t rel = (int32_t)((uint8_t *)0x409d20 - (call + 5));
    if (call[0] != 0xE8 || memcmp(call + 1, &rel, 4)) { log_line("patch skipped, game code differs: online thread rest"); return; }
    uint8_t *code = near_game_code(16);
    if (!code) { log_line("patch skipped, no memory near the game: online thread rest"); return; }
    uint64_t target = (uint64_t)curl_monitor_unlock;
    code[0] = 0x48; code[1] = 0xB8;           // mov rax,target
    memcpy(code + 2, &target, 8);
    code[10] = 0xFF; code[11] = 0xE0;         // jmp rax
    rel = (int32_t)(code - (call + 5));
    memcpy(call + 1, &rel, 4);
    log_line("patch applied: online thread rest");
}

// A sound bank can say a music track has more clip automation records than it holds (a 1212AD track says 256 and holds 1).
// CAkMusicTrack::SetInitialValues trusts that, walks off the end of the item into whatever memory follows and sooner or later
// crashes. The list now ends at the first record that does not fit inside the item, and reading goes on from there, which is
// where the next part of the item really starts. Tracks with a correct list never reach the new exit.
static void patch_music_track_bounds(void) {
    uint8_t *entry = (uint8_t *)0x3634dec, *record = (uint8_t *)0x3635013;
    static const uint8_t entry_old[5] = {0x48, 0x89, 0x74, 0x24, 0x38};   // mov [rsp+38h],rsi
    static const uint8_t record_old[5] = {0x48, 0x8B, 0x44, 0x24, 0x38};  // mov rax,[rsp+38h]
    if (memcmp(entry, entry_old, 5) || memcmp(record, record_old, 5)) { log_line("patch skipped, game code differs: music track bounds"); return; }
    uint8_t *code = near_game_code(128);
    if (!code) { log_line("patch skipped, no memory near the game: music track bounds"); return; }

    uint8_t *p = code;
#define EMIT(...) do { static const uint8_t b_[] = {__VA_ARGS__}; memcpy(p, b_, sizeof b_); p += sizeof b_; } while (0)
#define JMP_TO(target) do { *p++ = 0xE9; int32_t r_ = (int32_t)((uint8_t *)(target) - (p + 4)); memcpy(p, &r_, 4); p += 4; } while (0)
    // At function start: keep the end of the item in a stack slot this function does not use.
    uint8_t *on_entry = p;
    EMIT(0x48, 0x89, 0x74, 0x24, 0x38);        // mov [rsp+38h],rsi        the instruction replaced
    EMIT(0x48, 0x89, 0x74, 0x24, 0x70);        // mov [rsp+70h],rsi
    EMIT(0x89, 0xD2);                          // mov edx,edx
    EMIT(0x48, 0x01, 0x54, 0x24, 0x70);        // add [rsp+70h],rdx        data + size
    JMP_TO(entry + 5);
    // Before each record: rdx and r8 are about to be overwritten by the game, so they are free here.
    uint8_t *on_record = p;
    EMIT(0x48, 0x8B, 0x44, 0x24, 0x38);        // mov rax,[rsp+38h]        the instruction replaced
    EMIT(0x48, 0x8B, 0x54, 0x24, 0x70);        // mov rdx,[rsp+70h]
    EMIT(0x4C, 0x8D, 0x40, 0x0C);              // lea r8,[rax+0Ch]
    EMIT(0x49, 0x39, 0xD0);                    // cmp r8,rdx
    EMIT(0x77, 0x17);                          // ja  stop
    EMIT(0x44, 0x8B, 0x40, 0x08);              // mov r8d,[rax+8]          points in this record
    EMIT(0x4F, 0x8D, 0x04, 0x40);              // lea r8,[r8+r8*2]
    EMIT(0x49, 0xC1, 0xE0, 0x02);              // shl r8,2
    EMIT(0x4D, 0x8D, 0x44, 0x00, 0x0C);        // lea r8,[r8+rax+0Ch]
    EMIT(0x49, 0x39, 0xD0);                    // cmp r8,rdx
    EMIT(0x77, 0x05);                          // ja  stop
    JMP_TO(record + 5);
    // stop: drop the empty entry just added, go on to the node parameters with the cursor still at this record.
    EMIT(0x4D, 0x85, 0xE4);                    // test r12,r12
    EMIT(0x74, 0x06);                          // je  +6
    EMIT(0xFF, 0x8D, 0xA8, 0x00, 0x00, 0x00);  // dec dword [rbp+0A8h]
    JMP_TO(0x3635238);
#undef EMIT
#undef JMP_TO
    uint8_t jump[5] = {0xE9};
    int32_t rel = (int32_t)(on_entry - (entry + 5));
    memcpy(jump + 1, &rel, 4);
    memcpy(entry, jump, 5);
    rel = (int32_t)(on_record - (record + 5));
    memcpy(jump + 1, &rel, 4);
    memcpy(record, jump, 5);
    log_line("patch applied: music track bounds, stubs at %p", code);
}

static void dx11_game_settings(void);   // dx11_device.inc: settings for the DirectX 11 route (battle line of sight)

// The game's own named settings that cannot be set from its command line. Set in memory after its start-up code has given them their defaults.
// Each one first checks that the code reading it is the code it was studied in. ATTILA_VAB and ATTILA_UBM override the values for tests.
void loader_game_settings(void) {
    if (getenv("ATTILA_PLAIN")) { log_line("PLAIN: game settings left at the game's defaults"); return; }
    static const uint8_t vab_read[6] = {0x8A, 0x05, 0x33, 0x41, 0xE9, 0x01};   // 33d54e1: mov al,[enable_vertex_attrib_binding]
    static const uint8_t ubm_read[6] = {0x8B, 0x05, 0x7D, 0xCF, 0xE8, 0x01};   // 33dc791: mov eax,[gl_uniform_buffer_mode]
    const char *e;
    // Measured on the same replay: 50 fps with the game's defaults, 225 fps with these (and the same picture). The modern vertex path only starts if the driver gives OpenGL 4.3.
    int vab = 1, ubm = 2;
    if ((e = getenv("ATTILA_VAB")) != NULL) vab = atoi(e);
    if ((e = getenv("ATTILA_UBM")) != NULL) ubm = atoi(e);
    if (vab >= 0) {
        if (memcmp((void *)0x33d54e1, vab_read, 6)) log_line("setting skipped, game code differs: enable_vertex_attrib_binding");
        else { *(uint8_t *)0x526961a = (uint8_t)vab; log_line("setting: enable_vertex_attrib_binding %d", vab); }
    }
    if (ubm >= 0) {
        if (memcmp((void *)0x33dc791, ubm_read, 6)) log_line("setting skipped, game code differs: gl_uniform_buffer_mode");
        else { *(int *)0x5269714 = ubm; log_line("setting: gl_uniform_buffer_mode %d", ubm); }
    }
    // Battle effects (dust, blood, fire particles) updated on all cores: the game's own VFX_PARALLEL_UPDATER tweaker (off when
    // shipped) makes WARSCAPE::VFX_IMPL::update run VFX_INSTANCE_IMPL::update through its task system instead of one by one on the
    // main thread, where effects took about a third of the time in a big clash (44 -> 67 fps measured). Alone it corrupted the
    // heap: the particle pools' free lists are shared (see patch_vfx_pool_locks, which must be in place). Display only: effects
    // are not part of the battle's shared simulation.
    // OFF BY DEFAULT since Test 18: in the user's 1212AD rain battle (Test 17) it still corrupted the heap, in
    // RIGID_SCENE_NODE_V2::add_simple_instance (VECTOR push_back) under BURST_RIGID_EMITTER_INSTANCE::update_emitter on a task
    // thread: emitters write into shared scene nodes too, not only the pools. ATTILA_PARALLEL_VFX=1 turns it on for tests until
    // every shared write is locked. Since 2026-11-10 the effect gate (patch_vfx_gate) lets only plain billboard effects run
    // at the same time and keeps every other effect to one at a time; still a test switch until it has run long battles.
    {
        static const uint8_t vfx_read[7] = {0x80, 0x3D, 0x8D, 0x20, 0xDC, 0x01, 0x00};   // 34ad1de: cmp byte [VFX_PARALLEL_UPDATER+62h],0
        extern int g_vfx_pool_locked, g_vfx_gated;
        if (!getenv("ATTILA_PARALLEL_VFX")) log_line("setting: effects updated one by one, the game's way (ATTILA_PARALLEL_VFX=1 tries all cores)");
        else if (!g_vfx_pool_locked || !g_vfx_gated) log_line("setting skipped: parallel effects need the particle pool locks and the effect gate");
        else if (memcmp((void *)0x34ad1de, vfx_read, 7)) log_line("setting skipped, game code differs: parallel effects");
        else {
            extern int g_vfx_flip;
            *(uint8_t *)0x526f272 = 1;
            g_vfx_flip = !strcmp(getenv("ATTILA_PARALLEL_VFX"), "flip");   // test: off and on again with every fps line (vfx_flip)
            log_line("setting: battle effects updated on all cores (VFX_PARALLEL_UPDATER)%s", g_vfx_flip ? ", switched off and on with every fps line (test)" : "");
        }
    }
    dx11_game_settings();
    // More of the game's own settings, logged so their start values are known (the value byte is at +0x62 of each setting).
    static const struct { const char *name; uintptr_t at; } shown[] = {
        {"enable_tbb_fill_animation_texture", 0x526e1d2},    // fill_animation_texture uses several cores when set
        {"fill_animation_prefill", 0x526a172},
        {"texture_manager_enable_async_load", 0x526a40a},
        {"texture_manager_enable_async_update", 0x526a472},
        {"create_shaders_on_demand", 0x52618ba},
        {"entity_display_enable_imposter_shadows", 0x4e887ba},       // shadows of far-away soldier sprites
        {"entity_display_use_imposter_variants_for_all", 0x4e88f0a},
    };
    char list[640];
    size_t n = 0;
    for (size_t i = 0; i < sizeof shown / sizeof shown[0] && n < sizeof list; i++)
        n += (size_t)snprintf(list + n, sizeof list - n, "%s%s %u", i ? ", " : "", shown[i].name, *(volatile uint8_t *)shown[i].at);
    log_line("game settings at start: %s, tbb_fill_animation_texture_grain %d", list, *(volatile int *)0x526e16c);
    // Test switch: ATTILA_SHOW=hexaddr,... logs single bytes of the game's data area (setting values), to see their start values.
    if ((e = getenv("ATTILA_SHOW")) != NULL) {
        char line[1600];
        size_t k = 0;
        for (const char *s = e; *s && k + 32 < sizeof line;) {
            char *end;
            unsigned long long a = strtoull(s, &end, 16);
            if (end == s) break;
            if (a >= 0x4c00000 && a < 0x5280000) k += (size_t)snprintf(line + k, sizeof line - k, " %llx=%u", a, *(volatile uint8_t *)(uintptr_t)a);
            s = *end == ',' ? end + 1 : end;
            if (end == s) break;
        }
        line[k] = 0;
        log_line("test show:%s", line);
    }
    // Test switch: ATTILA_POKE=hexaddr:value,... sets single bytes inside the game's data area (its settings live there), to measure other settings.
    if ((e = getenv("ATTILA_POKE")) != NULL) {
        const char *s = e;
        for (;;) {
            char *end;
            unsigned long long a = strtoull(s, &end, 16);
            if (end == s || *end != ':') break;
            const char *vs = end + 1;
            long v = strtol(vs, &end, 0);
            if (end == vs) break;
            if (a >= 0x4c00000 && a < 0x5280000) {
                volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)a;
                log_line("test setting: byte %llx %u -> %ld", a, *p, v);
                *p = (uint8_t)v;
            } else log_line("test setting refused, outside the game's data area: %llx", a);
            if (*end != ',') break;
            s = end + 1;
        }
    }
    { extern void plugins_game_started(void); plugins_game_started(); }   // plugins.inc: plugins that wait for the game's settings
}

// Far-away soldier sprites (imposters) are drawn while a battle loads, one pass per animation frame step: IMPOSTER_SCENE_NODE::
// set_texture_defaults picks 0.1 s for quality 0-1, 0.05 s for quality 2 and 0.025 s for quality 3, and get_num_passes counts the
// passes from it. In an 8-player battle those passes are most of the loading time: 0.025 s loaded in 107-116 s, 0.1 s in 32 s
// (and the battle ran 40 -> 54 fps). Only sprites of soldiers far from the camera change; 0.1 s is the game's own step for quality
// 0-1. Quality 2 and 3 get the coarser of the game's step and ours. ATTILA_SPRITE_STEP=<seconds> for tests; 0.025 keeps the game's.
static void patch_sprite_step(void) {
    float f = 0.1f;
    const char *e = getenv("ATTILA_SPRITE_STEP");
    if (e) f = (float)atof(e);
    if (!(f >= 0.01f && f <= 0.5f)) { log_line("sprite step ignored: %s", e ? e : ""); return; }
    static const struct { uintptr_t at; uint8_t code[10]; float game; } steps[2] = {
        {0x32d44a5, {0xC7, 0x05, 0xA1, 0xF1, 0x92, 0x01, 0xCD, 0xCC, 0x4C, 0x3D}, 0.05f},    // quality 2: mov [DEFAULT_IMPOSTER_FRAME_STEP], 0.05f
        {0x32d44bb, {0xC7, 0x05, 0x8B, 0xF1, 0x92, 0x01, 0xCD, 0xCC, 0xCC, 0x3C}, 0.025f},   // quality 3: mov [DEFAULT_IMPOSTER_FRAME_STEP], 0.025f
    };
    for (int i = 0; i < 2; i++) {
        uint8_t *code = (uint8_t *)steps[i].at;
        if (memcmp(code, steps[i].code, 10)) { log_line("patch skipped, game code differs: sprite step for quality %d", i + 2); continue; }
        float use = f > steps[i].game ? f : steps[i].game;
        memcpy(code + 6, &use, 4);
        log_line("patch applied: far-away sprite step for quality %d: %.3f s (game: %.3f)", i + 2, use, steps[i].game);
    }
}

// Battle effects are asked for by name through VFX_SCENE_NODE::register_vfx (32fdb40, a jump to register_vfx_internal at 32fdb50:
// scene node in rdi, the name in rsi, returns the effect or NULL when there is none). spawn_vfx_instance_internal (effects started
// by other effects' particles) goes through it as well. A CA::String is u32 length, u32 capacity, char *text.
// (VFX_IMPL_BASE::create_instance at 34a5e50 made only one copy in a whole battle: not the battle's way in.)
// ATTILA_COUNT_VFX=1 counts the requests per name and lists the most asked for at each fps line (measuring only; display code).
typedef void *(__attribute__((sysv_abi)) *VfxRegisterFn)(void *self, const void *name);
static VfxRegisterFn g_vfx_register_orig;
#define VFX_SLOTS 2048u
static struct { uint64_t hash; uint64_t made; char name[100]; } g_vfx[VFX_SLOTS];
static SRWLOCK g_vfx_lock = SRWLOCK_INIT;
static int g_vfx_count;

static const char *g_vfx_skip;   // test switch ATTILA_TEST_NO_VFX=name,name: those effects are not made (measuring only)
__attribute__((sysv_abi)) static void *vfx_register(void *self, const void *name) {
    uint32_t len = name ? *(const uint32_t *)name : 0;
    const char *t = name ? *(const char *const *)((const uint8_t *)name + 8) : NULL;
    if (g_vfx_skip && t && len && len < 100) {
        for (const char *s = g_vfx_skip; *s;) {
            const char *e = strchr(s, ',');
            size_t n = e ? (size_t)(e - s) : strlen(s);
            if (n == len && !_strnicmp(s, t, n)) return NULL;
            s += n + (e ? 1 : 0);
        }
    }
    if (g_vfx_count && t && len && len <= 400) {
        uint64_t h = 1469598103934665603ull;   // FNV-1a, case folded (the game lower-cases names too)
        for (uint32_t k = 0; k < len; k++) { h ^= (uint8_t)(t[k] | 0x20); h *= 1099511628211ull; }
        if (!h) h = 1;
        AcquireSRWLockExclusive(&g_vfx_lock);
        uint32_t i = (uint32_t)h & (VFX_SLOTS - 1);
        for (int probe = 0; probe < 64; probe++, i = (i + 1) & (VFX_SLOTS - 1)) {
            if (g_vfx[i].hash == h) { g_vfx[i].made++; break; }
            if (!g_vfx[i].hash) { g_vfx[i].hash = h; g_vfx[i].made = 1; snprintf(g_vfx[i].name, sizeof g_vfx[i].name, "%.*s", (int)len, t); break; }
        }
        ReleaseSRWLockExclusive(&g_vfx_lock);
    }
    return g_vfx_register_orig(self, name);
}

// Called at each fps line: the effects made most since the last one.
void vfx_count_report(void) {
    if (!g_vfx_count) return;
    static struct { uint64_t made; const char *name; } top[20];
    int n = 0, kinds = 0;
    uint64_t total = 0;
    AcquireSRWLockExclusive(&g_vfx_lock);
    for (uint32_t i = 0; i < VFX_SLOTS; i++) {
        if (!g_vfx[i].hash || !g_vfx[i].made) continue;
        kinds++;
        total += g_vfx[i].made;
        int at;
        if (n < 20) at = n++;
        else if (top[19].made >= g_vfx[i].made) continue;
        else at = 19;
        top[at].made = g_vfx[i].made; top[at].name = g_vfx[i].name;
        while (at > 0 && top[at - 1].made < top[at].made) { __typeof__(top[0]) t = top[at]; top[at] = top[at - 1]; top[at - 1] = t; at--; }
    }
    log_line("effects made since the last line: %llu copies of %d effects", (unsigned long long)total, kinds);
    for (int i = 0; i < n; i++) log_line("  effect %7llu  %s", (unsigned long long)top[i].made, top[i].name[0] ? top[i].name : "(no name)");
    for (uint32_t i = 0; i < VFX_SLOTS; i++) g_vfx[i].made = 0;
    ReleaseSRWLockExclusive(&g_vfx_lock);
}

static void patch_vfx_register(void) {
    g_vfx_count = getenv("ATTILA_COUNT_VFX") != NULL;
    g_vfx_skip = getenv("ATTILA_TEST_NO_VFX");
    if (!g_vfx_count && !g_vfx_skip) return;
    uint8_t *f = (uint8_t *)0x32fdb50;
    static const uint8_t old[11] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x50, 0x48, 0x89, 0xF3};   // push r15, r14, r12, rbx, rax; mov rbx,rsi
    if (memcmp(f, old, 11)) { log_line("patch skipped, game code differs: effect counter"); return; }
    uint8_t *code = near_game_code(32);
    if (!code) { log_line("patch skipped, no memory near the game: effect counter"); return; }
    // trampoline: push r15, r14, r12 (6 bytes, covering the 5 the jump needs), then on into the original at +6
    memcpy(code, old, 6);
    code[6] = 0xE9;
    int32_t rel = (int32_t)((f + 6) - (code + 11));
    memcpy(code + 7, &rel, 4);
    g_vfx_register_orig = (VfxRegisterFn)(void *)code;
    // way in: mov rax,vfx_register; jmp rax (rax is free at a function's start)
    uint8_t *in = code + 16;
    uint64_t target = (uint64_t)vfx_register;
    in[0] = 0x48; in[1] = 0xB8;
    memcpy(in + 2, &target, 8);
    in[10] = 0xFF; in[11] = 0xE0;
    uint8_t jump[5] = {0xE9};
    rel = (int32_t)(in - (f + 5));
    memcpy(jump + 1, &rel, 4);
    memcpy(f, jump, 5);
    log_line("patch applied: effect counter (measuring)");
}

