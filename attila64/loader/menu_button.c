// menu_button.c - the 64-bit Attila Discord button: a small button of our own in the bottom right corner of the game's main
// menu that opens the 64-bit Attila Discord invite in the browser. User request 2026-11-16 ("my discord link like dawnless
// days mod"). 64-bit Attila draws it onto the picture that goes to the window (DirectX 11: dx11_device.inc calls
// menu_button_dx11; OpenGL: shim_gfx.c) and takes its clicks before the game sees them (shim_gfx.c SDL_PollEvent), so it
// works with any mod's main menu and no screen file of the game or of a mod is changed (The Dawnless Days and 1212AD bring
// main menus of their own). It shows only while the front end shows its main menu (frontend_extras.inc watches the screen
// changes; the front end object UIDLL::FRONT_END_UI::s_ui must exist). Mouse moves still reach the game; a left click on
// the button does not. Its size follows the interface zoom (40 pixels high at the game's own size). In hidden test runs a
// click is logged but no browser is opened. settings.txt discord_button=off (picker.c) or ATTILA_NO_DISCORD_BUTTON=1 hides it.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "loader.h"
#include "test_auto.h"
#include "menu_button.h"

#define DISCORD_INVITE "https://discord.gg/3TUEGTMshP"
#define BUTTON_TEXT "64-bit Attila Discord"

extern volatile int g_menu_main;   // frontend_extras.inc: the front end shows its main menu
extern float ui_zoom_now(void);    // interface.inc: the interface zoom in use, 1 when not zoomed

static int g_x, g_y, g_w, g_h, g_for_w, g_for_h;   // the button's place in the picture it was made for
static volatile int g_hover, g_pressed;
static uint8_t *g_px[2][2];                         // [0 BGRA / 1 RGBA][normal / under the mouse]

int menu_button_visible(void) {
    static int off = -1;
    if (off < 0) { extern int settings_discord_button_off(void); off = getenv("ATTILA_NO_DISCORD_BUTTON") != NULL || settings_discord_button_off(); }
    if (off || !g_menu_main) return 0;
    return *(void *volatile *)(uintptr_t)0x5167298 != NULL;   // UIDLL::FRONT_END_UI::s_ui: the front end is up
}

// One look of the button, B,G,R,A rows from the top: Discord's colour, a darker edge, the text in white.
static uint8_t *draw_button(int w, int h, int hover) {
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bm = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    if (!bm || !bits) { if (bm) DeleteObject(bm); if (dc) DeleteDC(dc); return NULL; }
    HGDIOBJ old_bm = SelectObject(dc, bm);
    RECT all = {0, 0, w, h};
    int e = h / 20 + 1;
    RECT face = {e, e, w - e, h - e};
    HBRUSH edge_b = CreateSolidBrush(RGB(64, 73, 176)), face_b = CreateSolidBrush(hover ? RGB(121, 134, 247) : RGB(88, 101, 242));
    FillRect(dc, &all, edge_b);
    FillRect(dc, &face, face_b);
    DeleteObject(edge_b);
    DeleteObject(face_b);
    HFONT font = CreateFontA(-(h * 46 / 100), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HGDIOBJ old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextA(dc, BUTTON_TEXT, -1, &all, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();
    uint8_t *px = malloc((size_t)w * (size_t)h * 4);
    if (px) {
        memcpy(px, bits, (size_t)w * (size_t)h * 4);
        for (int i = 0; i < w * h; i++) px[i * 4 + 3] = 255;
    }
    SelectObject(dc, old_font);
    DeleteObject(font);
    SelectObject(dc, old_bm);
    DeleteObject(bm);
    DeleteDC(dc);
    return px;
}

const uint8_t *menu_button_pixels(int pw, int ph, int rgba, int *x, int *y, int *w, int *h) {
    if (pw != g_for_w || ph != g_for_h || !g_px[0][0]) {
        float z = ui_zoom_now();
        int bh = (int)(40.0f * z + 0.5f), bw = bh * 6, m = (int)(18.0f * z + 0.5f);
        g_w = 0;   // no clicks while it is remade
        for (int o = 0; o < 2; o++) for (int k = 0; k < 2; k++) { free(g_px[o][k]); g_px[o][k] = NULL; }
        g_for_w = pw; g_for_h = ph;
        if (bw + 2 * m > pw || bh + 2 * m > ph) return NULL;
        for (int k = 0; k < 2; k++) {
            g_px[0][k] = draw_button(bw, bh, k);
            if (!g_px[0][k]) return NULL;
            g_px[1][k] = malloc((size_t)bw * (size_t)bh * 4);
            if (!g_px[1][k]) return NULL;
            for (int i = 0; i < bw * bh; i++) {   // B,G,R,A -> R,G,B,A
                g_px[1][k][i * 4] = g_px[0][k][i * 4 + 2];
                g_px[1][k][i * 4 + 1] = g_px[0][k][i * 4 + 1];
                g_px[1][k][i * 4 + 2] = g_px[0][k][i * 4];
                g_px[1][k][i * 4 + 3] = 255;
            }
        }
        g_x = pw - bw - m; g_y = ph - bh - m; g_h = bh; g_w = bw;
        static int told;
        if (told++ < 3) log_line("Discord button: %dx%d at %d,%d in a %dx%d picture (interface zoom %.2f)", bw, bh, g_x, g_y, pw, ph, z);
    }
    if (!g_w) return NULL;
    *x = g_x; *y = g_y; *w = g_w; *h = g_h;
    return g_px[rgba ? 1 : 0][g_hover ? 1 : 0];
}

static DWORD WINAPI open_invite(void *unused) {
    (void)unused;
    HMODULE sh = LoadLibraryA("shell32.dll");
    HINSTANCE (WINAPI *open_fn)(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT) =
        sh ? (HINSTANCE (WINAPI *)(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT))GetProcAddress(sh, "ShellExecuteA") : NULL;
    if (open_fn) open_fn(NULL, "open", DISCORD_INVITE, NULL, NULL, SW_SHOWNORMAL);
    return 0;
}

// SDL_MOUSEMOTION 400h, SDL_MOUSEBUTTONDOWN 401h, SDL_MOUSEBUTTONUP 402h: button at +10h (1 = left), x at +14h, y at +18h
int menu_button_event(void *sdl_event) {
    const uint8_t *e = sdl_event;
    uint32_t type = *(const uint32_t *)e;
    if (type < 0x400 || type > 0x402) return 0;
    int x = *(const int32_t *)(e + 20), y = *(const int32_t *)(e + 24);
    int on = g_w && menu_button_visible() && x >= g_x && x < g_x + g_w && y >= g_y && y < g_y + g_h;
    if (type == 0x400) { g_hover = on; return 0; }
    if (e[16] != 1) return 0;
    if (type == 0x401) {
        if (!on) return 0;
        g_pressed = 1;
        return 1;
    }
    if (!g_pressed) return 0;
    g_pressed = 0;
    if (on) {
        static int told;
        if (test_hidden()) { if (told++ < 5) log_line("Discord button: clicked (hidden test run: no browser opened)"); }
        else {
            if (told++ < 5) log_line("Discord button: invite opened in the browser");
            HANDLE t = CreateThread(NULL, 0, open_invite, NULL, 0, NULL);
            if (t) CloseHandle(t);
        }
    }
    return 1;
}

void menu_button_dx11(void *context, void *target, int pw, int ph) {
    ID3D11DeviceContext *c = context;
    int x, y, w, h;
    const uint8_t *px = menu_button_pixels(pw, ph, 1, &x, &y, &w, &h);
    if (!c || !target || !px) return;
    D3D11_BOX box = {(UINT)x, (UINT)y, 0, (UINT)(x + w), (UINT)(y + h), 1};
    c->lpVtbl->UpdateSubresource(c, (ID3D11Resource *)target, 0, &box, px, (UINT)w * 4, 0);
}
