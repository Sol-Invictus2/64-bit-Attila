// A small frame-rate number drawn by 64-bit Attila itself, on every graphics route (OpenGL, DirectX 11, DirectX 12).
// The game shows its own only in the benchmark, and outside overlays (Steam, the card maker's) do not always appear.
// On when the start window's "Show frame rate" box is ticked (settings.txt show_fps=1), or ATTILA_SHOW_FPS=1 / 0 for tests.
#pragma once
#include <stdint.h>
int fps_overlay_on(void);
// Once per shown frame: counts the frame and gives the text to show, renewed twice a second, e.g. "157 FPS".
const char *fps_overlay_tick(void);
// One character as 7 rows of 5 dots (bit 4 = the left dot); digits, F, P, S; anything else is blank.
const uint8_t *fps_overlay_glyph(char c);
// Where the box goes and how big a dot is, for a picture of this size: left edge, top edge (rows counted from the top),
// dot size in pixels. The box is (6 * characters + 1) x 9 dots.
void fps_overlay_place(int w, int h, int chars, int *x, int *y, int *dot);
