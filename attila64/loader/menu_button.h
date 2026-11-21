// The 64-bit Attila Discord button on the game's main menu (menu_button.c).
#pragma once
#include <stdint.h>
// 1 while the front end shows its main menu (and ATTILA_NO_DISCORD_BUTTON is not set).
int menu_button_visible(void);
// The button's pixels for a picture of pw x ph: rgba = 1 gives R,G,B,A bytes (DirectX 11), 0 gives B,G,R,A (OpenGL upload).
// Rows from the top. Sets its place (left, top, rows counted from the top) and size. NULL when it does not fit.
const uint8_t *menu_button_pixels(int pw, int ph, int rgba, int *x, int *y, int *w, int *h);
// An SDL event before the game sees it: 1 = it was a click on the button (the game must not get it), 0 = pass it on.
int menu_button_event(void *sdl_event);
// DirectX 11 route: puts the button onto the picture that goes to the window (context, target texture, picture size).
void menu_button_dx11(void *context, void *target, int pw, int ph);
