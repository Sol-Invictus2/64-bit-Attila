// The frame-rate number (see fps_overlay.h). Counting and the dot letters live here; the two drawing routes are in
// shim_gfx.c (OpenGL: small clears inside a scissor box, no shaders or textures needed) and dx11_device.inc (DirectX 11:
// a small block of pixels copied onto the picture just before it is shown).
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fps_overlay.h"

int fps_overlay_on(void) {
    static int on = -1;
    if (on < 0) {
        extern int settings_show_fps(void);   // picker.c: the start window's tick box
        const char *e = getenv("ATTILA_SHOW_FPS");
        on = e && *e ? atoi(e) != 0 : settings_show_fps();
    }
    return on;
}

const char *fps_overlay_tick(void) {
    static char text[16] = "0 FPS";
    static LARGE_INTEGER freq, start;
    static unsigned frames;
    LARGE_INTEGER now;
    if (!freq.QuadPart) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&start); }
    QueryPerformanceCounter(&now);
    frames++;
    double dt = (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
    if (dt >= 0.5) {
        double fps = frames / dt;
        snprintf(text, sizeof text, "%d FPS", fps > 9999 ? 9999 : (int)(fps + 0.5));
        frames = 0;
        start = now;
    }
    return text;
}

const uint8_t *fps_overlay_glyph(char c) {
    static const uint8_t digits[10][7] = {
        {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
        {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}, {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
        {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
        {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}};
    static const uint8_t f[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}, p[7] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
                         s[7] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, blank[7] = {0};
    if (c >= '0' && c <= '9') return digits[c - '0'];
    return c == 'F' ? f : c == 'P' ? p : c == 'S' ? s : blank;
}

// Top edge, a third of the way across: clear of the game's own things in battle and on the campaign map (corner buttons,
// the adviser top left, the balance bar top middle).
void fps_overlay_place(int w, int h, int chars, int *x, int *y, int *dot) {
    int d = h >= 1800 ? 4 : h >= 1300 ? 3 : 2;
    int bw = (6 * chars + 1) * d;
    int left = w / 3;
    if (left + bw > w) left = w - bw;
    if (left < 0) left = 0;
    *x = left; *y = 2 * d; *dot = d;
}
