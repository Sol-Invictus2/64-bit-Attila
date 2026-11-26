// context_camera.inc - the battle "context camera" key (default Insert; Options > Controls "context_camera"), as normal
// Windows Attila has it. Part of loader.c (included there, not built by itself).
//
// Player report 2026-11-26 (Deeks, Workshop): "The built-in cinematic battle camera (focus on a unit, no UI) works in 32-bit
// Attila but does nothing in 64-bit, even with a key bound". Seen in a hidden battle: Insert reaches the game (key down 73)
// and nothing happens, while Delete (toggle_tracking) works.
// Cause: the Linux game's EMPIREBATTLE::BATTLE_CONTROLLER::on_shortcut (9a86f0) compares the shortcut name with zoom,
// bookmarks, toggle_tracking, first person ... but never with "context_camera" (the name's only users are the key list,
// KEYBOARD_SHORTCUT_DESCRIPTION::shortcut). The camera itself is all there: BATTLE_CAMERA_MANAGER::toggle_context_camera
// (b7e360; starts the context camera on the tracked unit, or ends it when it is on) has no caller in the Linux game.
// Fix: after the game's own on_shortcut, the name "context_camera" calls toggle_context_camera on the battle's camera
// manager (battle env [this+0x38] + 0x64408, the same object on_shortcut uses for toggle_tracking). Camera view only:
// nothing in the shared battle changes, so multiplayer stays in step. ATTILA_NO_CONTEXT_CAMERA_FIX=1 = the game's way.

typedef SYSV void (*ShortcutFn)(void *self, const void *name, uint32_t id, float secs);
static ShortcutFn g_battle_shortcut_orig;

static SYSV void battle_shortcut_hook(void *self, const void *name, uint32_t id, float secs) {
    g_battle_shortcut_orig(self, name, id, secs);
    SYSV uint8_t (*same)(const void *, const char *) = (void *)0x51b070;   // CA::operator==(CA::String const&, char const*)
    if (!self || !name || !same(name, "context_camera")) return;
    uint8_t *env = *(uint8_t **)((uint8_t *)self + 0x38);
    void *cam = env ? *(void **)(env + 0x64408) : NULL;
    if (!cam) return;
    SYSV void (*toggle)(void *) = (void *)0xb7e360;   // BATTLE_CAMERA_MANAGER::toggle_context_camera()
    toggle(cam);
    static int told;
    if (!told++) log_line("context camera key used in battle");
}

static void patch_context_camera(void) {
    if (getenv("ATTILA_NO_CONTEXT_CAMERA_FIX")) { log_line("test: battle context camera key left as the game has it (ATTILA_NO_CONTEXT_CAMERA_FIX)"); return; }
    static const uint8_t on_shortcut_old[6] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x53};   // push rbp; push r15; push r14; push rbx
    static const uint8_t toggle_old[7] = {0x48, 0x8B, 0x87, 0x30, 0x0A, 0x00, 0x00};   // mov rax,[rdi+0xa30]
    if (memcmp((const void *)0xb7e360, toggle_old, sizeof toggle_old)) { log_line("patch skipped, game code differs: battle context camera key"); return; }
    g_battle_shortcut_orig = (ShortcutFn)hook_entry(0x9a86f0, on_shortcut_old, 6, (void *)battle_shortcut_hook);
    if (!g_battle_shortcut_orig) { log_line("patch skipped, game code differs or no memory near the game: battle context camera key"); return; }
    log_line("patch applied: battle context camera key (context_camera, default Insert), as on Windows");
}
