// context_camera.inc - the battle "context camera" key (default Insert; Options > Controls "context_camera"), as normal
// Windows Attila has it. Part of loader.c (included there, not built by itself).
//
// Player report 2026-11-26 (Deeks, Workshop): "The built-in cinematic battle camera (focus on a unit, no UI) works in 32-bit
// Attila but does nothing in 64-bit, even with a key bound". Seen in a hidden battle: Insert reaches the game (key down 73)
// and nothing happens, while Delete (toggle_tracking) works.
// Cause: the Linux game never compares a shortcut's name with "context_camera" (the name's only users are the key list,
// KEYBOARD_SHORTCUT_DESCRIPTION::shortcut). Normal Windows Attila (32-bit empire.retail.dll, read 2026-11-26) handles the
// name in two places. Both are put back here, each as a detour on the 5-byte "mov esi,<toggle_first_person_mode>" that
// starts the game's next name compare:
//  A  EMPIREBATTLE::BATTLE_CONTROLLER::on_shortcut (9a86f0) at 9a8d24, between toggle_tracking and toggle_first_person_mode
//     (rbx = controller, r14 = name, way out 9a8f81): when BATTLE_CAMERA_MANAGER::is_functional(0x80) (special cameras are
//     allowed; cutscenes and scripts clear it), the battle's CINEMATIC_EVENT_VIEW (battle env [controller+0x38] + 0x644c8)
//     gets toggle_enabled(), or without a view the camera manager (env + 0x64408) gets toggle_context_camera().
//  B  EMPIREBATTLE::FIRST_PERSON_MANAGER::on_shortcut (7ce870) at 7ce97d, the key while first person is on (rax = name,
//     rbx = manager, env = [manager+0x28], way out 7cec8c): view ? toggle_enabled() : camera toggle_first_person().
// CINEMATIC_EVENT_VIEW::toggle_enabled (e30610) is what the game's own cinematic mode button and Escape call
// (UIDLL::BattleButtonCinematicMode, BattleRootComponentCallback::OnShortcut "escape_menu"): going on it tries first person
// first (the plain game allows that for artillery only) and else starts the context camera on the tracked unit; going off it
// leaves whichever of the two is on. So nothing here is new for multiplayer: it is the button's own call.
//
// 1.3.29 hooked the entry of on_shortcut and always called toggle_context_camera (b7e360) after the game's own handling.
// That skipped what the game does first (first person on -> the key goes to the first-person manager; the camera's own key
// handler; an interface priority lock; is_functional) and the view, so artillery got the context camera, not first person.
// ATTILA_CONTEXT_CAMERA_DIRECT=1 = that way again (A always toggle_context_camera, B not placed).
//
// Attila Cam (plugins\attila_cam64.dll) opens first person to ordinary men (FIRST_PERSON_MANAGER::valid, 7d0347 74 1A ->
// EB 1A). toggle_enabled would then take one man, like the plugin's own Y key. So while those two bytes are not 74 1A (looked
// at when the key is pressed), A keeps the plain context camera (toggle_enabled only to switch off a view that is on) and B
// leaves the key alone.
// ATTILA_NO_CONTEXT_CAMERA_FIX=1 = the game's way (the key does nothing).

static int g_ctx_direct;   // ATTILA_CONTEXT_CAMERA_DIRECT=1

static int first_person_open_to_men(void) {
    const volatile uint8_t *je = (const volatile uint8_t *)0x7d0347;
    return je[0] != 0x74 || je[1] != 0x1A;
}

static void ctx_note(const char *where, const char *what) {   // the first uses go in the log (tests, players' reports)
    static int n;
    if (n < 12) { n++; log_line("context camera key (%s): %s", where, what); }
}

// A: 1 = the key is dealt with (on_shortcut leaves), 0 = not ours (the game's next compare follows).
static SYSV uint8_t context_camera_key_battle(uint8_t *controller, const void *name) {
    SYSV uint8_t (*same)(const void *, const char *) = (void *)0x51b070;    // CA::operator==(CA::String const&, char const*)
    if (!same(name, "context_camera")) return 0;
    uint8_t *env = *(uint8_t **)(controller + 0x38);
    void *cam = env ? *(void **)(env + 0x64408) : NULL;
    if (!cam) return 0;
    SYSV void (*toggle_context)(void *) = (void *)0xb7e360;                 // BATTLE_CAMERA_MANAGER::toggle_context_camera()
    if (g_ctx_direct) { ctx_note("battle", "context camera switched directly (ATTILA_CONTEXT_CAMERA_DIRECT)"); toggle_context(cam); return 1; }
    SYSV uint8_t (*is_functional)(void *, uint32_t) = (void *)0xb7c860;     // BATTLE_CAMERA_MANAGER::is_functional(CAMERA_FUNCTIONALITY)
    if (!is_functional(cam, 0x80)) { ctx_note("battle", "nothing, special cameras are switched off now"); return 0; }
    uint8_t *view = *(uint8_t **)(env + 0x644c8);                           // CINEMATIC_EVENT_VIEW; +0x7a = switched on
    if (view && (view[0x7a] || !first_person_open_to_men())) {
        ctx_note("battle", view[0x7a] ? "the game's cinematic view switched off" : "the game's cinematic view switched on");
        ((SYSV void (*)(void *))0xe30610)(view);                            // CINEMATIC_EVENT_VIEW::toggle_enabled()
    } else {
        ctx_note("battle", view ? "plain context camera switched (a plugin has opened first person to men)" : "context camera switched (no cinematic view)");
        toggle_context(cam);
    }
    return 1;
}

// B: the same answer, for the key while first person is on.
static SYSV uint8_t context_camera_key_first_person(uint8_t *manager, const void *name) {
    SYSV uint8_t (*same)(const void *, const char *) = (void *)0x51b070;
    if (!same(name, "context_camera")) return 0;
    if (first_person_open_to_men()) { ctx_note("first person", "nothing (a plugin has opened first person to men)"); return 0; }
    uint8_t *env = *(uint8_t **)(manager + 0x28);
    if (!env) return 0;
    uint8_t *view = *(uint8_t **)(env + 0x644c8);
    void *cam = *(void **)(env + 0x64408);
    if (view) {
        ctx_note("first person", view[0x7a] ? "the game's cinematic view switched off" : "the game's cinematic view switched on");
        ((SYSV void (*)(void *))0xe30610)(view);                            // CINEMATIC_EVENT_VIEW::toggle_enabled()
    } else if (cam) {
        ctx_note("first person", "first person switched (no cinematic view)");
        ((SYSV void (*)(void *))0xb80db0)(cam);                             // BATTLE_CAMERA_MANAGER::toggle_first_person()
    }
    return 1;
}

static void patch_context_camera(void) {
    if (getenv("ATTILA_NO_CONTEXT_CAMERA_FIX")) { log_line("test: battle context camera key left as the game has it (ATTILA_NO_CONTEXT_CAMERA_FIX)"); return; }
    g_ctx_direct = getenv("ATTILA_CONTEXT_CAMERA_DIRECT") != NULL;
    uint8_t *a = (uint8_t *)0x9a8d24, *b = (uint8_t *)0x7ce97d;
    static const struct { uintptr_t at; uint8_t n, old[13]; } checks[] = {
        {0x9a8d24, 13, {0xBE, 0x6E, 0x8D, 0x7D, 0x03, 0x4C, 0x89, 0xF7, 0xE8, 0x3F, 0x23, 0xB7, 0xFF}},   // A: mov esi,37d8d6e; mov rdi,r14; call 51b070
        {0x7ce97d, 13, {0xBE, 0x6E, 0x8D, 0x7D, 0x03, 0x48, 0x89, 0xC7, 0xE8, 0xE6, 0xC6, 0xD4, 0xFF}},   // B: mov esi,37d8d6e; mov rdi,rax; call 51b070
        {0x9a8f81, 11, {0x48, 0x83, 0xC4, 0x68, 0x5B, 0x41, 0x5E, 0x41, 0x5F, 0x5D, 0xC3}},               // A's way out: add rsp,68h; pop rbx, r14, r15, rbp; ret
        {0x7cec8c, 6, {0x48, 0x83, 0xC4, 0x50, 0x5B, 0xC3}},                                              // B's way out: add rsp,50h; pop rbx; ret
        {0xe30610, 7, {0x41, 0x56, 0x53, 0x50, 0x48, 0x89, 0xFB}},                                        // CINEMATIC_EVENT_VIEW::toggle_enabled
        {0xb7c860, 12, {0x53, 0x89, 0xF3, 0x48, 0x8B, 0x07, 0xFF, 0x90, 0x60, 0x03, 0x00, 0x00}},         // BATTLE_CAMERA_MANAGER::is_functional
        {0xb80db0, 8, {0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x89, 0xFB}},                                  // BATTLE_CAMERA_MANAGER::toggle_first_person
        {0xb7e360, 7, {0x48, 0x8B, 0x87, 0x30, 0x0A, 0x00, 0x00}},                                        // BATTLE_CAMERA_MANAGER::toggle_context_camera
        {0x7d033f, 8, {0xFF, 0x90, 0x38, 0x02, 0x00, 0x00, 0x84, 0xC0}},                                  // FIRST_PERSON_MANAGER::valid, just before the je a plugin may change
    };
    for (size_t i = 0; i < sizeof checks / sizeof checks[0]; i++)
        if (memcmp((const void *)checks[i].at, checks[i].old, checks[i].n)) { log_line("patch skipped, game code differs: battle context camera key (at %llx)", (unsigned long long)checks[i].at); return; }
    uint8_t *code = near_game_code(96);
    if (!code) { log_line("patch skipped, no memory near the game: battle context camera key"); return; }

    uint8_t *p = code;
#define CTX_EMIT(...) do { static const uint8_t b_[] = {__VA_ARGS__}; memcpy(p, b_, sizeof b_); p += sizeof b_; } while (0)
#define CTX_REL(target) do { int32_t r_ = (int32_t)((uint8_t *)(target) - (p + 4)); memcpy(p, &r_, 4); p += 4; } while (0)
#define CTX_CALL(fn) do { uint64_t t_ = (uint64_t)(fn); CTX_EMIT(0x48, 0xB8); memcpy(p, &t_, 8); p += 8; CTX_EMIT(0xFF, 0xD0); } while (0)
    // A. Nothing but rbx, r14 and the other kept registers is in use here (both ways in come straight from a call), and the
    // stack is 16-byte aligned (4 pushes + 68h after the return address).
    uint8_t *stub_a = p;
    CTX_EMIT(0x48, 0x89, 0xDF);                   // mov rdi,rbx            the battle controller
    CTX_EMIT(0x4C, 0x89, 0xF6);                   // mov rsi,r14            the shortcut's name
    CTX_CALL(context_camera_key_battle);          // mov rax,helper; call rax
    CTX_EMIT(0x84, 0xC0);                         // test al,al
    CTX_EMIT(0x0F, 0x85); CTX_REL(0x9a8f81);      // jne  the function's way out
    CTX_EMIT(0xBE, 0x6E, 0x8D, 0x7D, 0x03);       // mov esi,37d8d6e        the instruction replaced ("toggle_first_person_mode")
    CTX_EMIT(0xE9); CTX_REL(a + 5);               // jmp  back
    // B. rax = the name and must survive; the stack is aligned (1 push + 50h), so two pushes keep it so.
    uint8_t *stub_b = p;
    CTX_EMIT(0x50, 0x50);                         // push rax; push rax
    CTX_EMIT(0x48, 0x89, 0xDF);                   // mov rdi,rbx            the first-person manager
    CTX_EMIT(0x48, 0x89, 0xC6);                   // mov rsi,rax            the shortcut's name
    CTX_CALL(context_camera_key_first_person);
    CTX_EMIT(0x84, 0xC0);                         // test al,al
    CTX_EMIT(0x58, 0x58);                         // pop rax; pop rax       (the flags stay)
    CTX_EMIT(0x0F, 0x85); CTX_REL(0x7cec8c);      // jne  the function's way out
    CTX_EMIT(0xBE, 0x6E, 0x8D, 0x7D, 0x03);       // mov esi,37d8d6e        the instruction replaced
    CTX_EMIT(0xE9); CTX_REL(b + 5);               // jmp  back
#undef CTX_EMIT
#undef CTX_REL
#undef CTX_CALL
    uint8_t jump[5] = {0xE9};
    int32_t rel = (int32_t)(stub_a - (a + 5));
    memcpy(jump + 1, &rel, 4);
    memcpy(a, jump, 5);
    if (!g_ctx_direct) {
        rel = (int32_t)(stub_b - (b + 5));
        memcpy(jump + 1, &rel, 4);
        memcpy(b, jump, 5);
    }
    log_line("patch applied: battle context camera key (context_camera, default Insert), %s, stubs at %p",
             g_ctx_direct ? "direct (ATTILA_CONTEXT_CAMERA_DIRECT): always the plain context camera" : "as on Windows: the game's cinematic view, also in first person", code);
}
