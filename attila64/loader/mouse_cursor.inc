// mouse_cursor.inc - the mouse cursor's place for camera dragging (middle mouse button), as normal Windows Attila does it.
// Part of loader.c (included there, not built by itself).
//
// The Linux game's CA::CURSOR::get_cursor_position (4f34a0) is a stub that returns false, and CA::CURSOR::set_cursor_position
// (4f3500) does nothing. The camera controllers use them through EMPIREUTILITY::HARDWARE_CURSOR (1c776d0 / 1c776f0):
//   - battle: BATTLE_CONTROLLER::mouse_event (9a9810) keeps the cursor's place and hides it when the middle button goes down,
//     and puts it back when the button goes up; process_mouse_shortcuts (9a9320) puts it back whenever it comes within 0.002 of
//     a screen edge while the button is held, and starts the drag again from there (drag_middle with "not dragging");
//   - campaign: CAMERA_CONTROLLER::mouse_event (163bba0) keeps the place, update_controller (163bae0) puts it back at an edge;
//   - TWUI::RadialMenu::UpdateRadialCursorPosition (29609bb) moves the cursor to window corner + offset: screen pixels.
// Without the two the hidden cursor stays wherever the drag takes it: a drag stops dead at a screen edge, and after a few drags
// the cursor sits near an edge, so turning or moving the camera towards that edge does nothing (player reports 2026-11-15:
// "can't pan the camera right toward the 1 o'clock direction, and when I scroll down the screen doesn't move").
// Screen pixels both ways (GetCursorPos / SetCursorPos), as the radial menu shows the game means. Camera view only, nothing
// in the shared game. In a hidden test the scripted test mouse is read and moved instead (shim_gfx.c), never the real cursor.
// Test switch ATTILA_NO_CURSOR_FIX=1 = the game's stubs.

int test_mouse_get(int *x, int *y);   // shim_gfx.c: 1 = a hidden test, the scripted mouse was used
int test_mouse_warp(int x, int y);

static SYSV uint8_t cursor_get(float *x, float *y, void *window) {
    (void)window;
    int tx, ty;
    if (test_mouse_get(&tx, &ty)) { *x = (float)tx; *y = (float)ty; return 1; }
    POINT p;
    if (!GetCursorPos(&p)) return 0;
    *x = (float)p.x;
    *y = (float)p.y;
    return 1;
}
static SYSV void cursor_set(int x, int y) {
    if (test_mouse_warp(x, y)) return;
    SetCursorPos(x, y);
}

static void patch_mouse_cursor(void) {
    if (getenv("ATTILA_NO_CURSOR_FIX")) { log_line("test: the game's cursor read and move stubs kept (ATTILA_NO_CURSOR_FIX)"); return; }
    static const uint8_t get_old[5] = {0x31, 0xC0, 0xC3, 0x66, 0x66};   // xor eax,eax; ret (then padding)
    static const uint8_t set_old[5] = {0xC3, 0x66, 0x66, 0x66, 0x66};   // ret (then padding)
    if (memcmp((const void *)0x4f34a0, get_old, 5) || memcmp((const void *)0x4f3500, set_old, 5)) {
        log_line("patch skipped, game code differs: mouse cursor for camera dragging");
        return;
    }
    if (!hook_entry(0x4f34a0, get_old, 5, (void *)cursor_get)) { log_line("patch skipped, no memory near the game: mouse cursor for camera dragging"); return; }
    if (!hook_entry(0x4f3500, set_old, 5, (void *)cursor_set)) {
        memcpy((void *)0x4f34a0, get_old, 5);   // all or nothing
        log_line("patch skipped, no memory near the game: mouse cursor for camera dragging");
        return;
    }
    log_line("patch applied: mouse cursor read and moved for camera dragging with the middle mouse button, as on Windows");
}
