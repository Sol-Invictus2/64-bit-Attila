// Test automation for unattended runs (attila64\dx11\autobench.ps1): hidden + muted window, the game's own benchmark
// started by itself, pictures of the game's back buffer at set benchmark seconds, then the run ends. Nothing here is on
// unless its ATTILA_TEST_* / ATTILA_AUTO_BENCHMARK switch is set. See test_auto.c.
#pragma once
#include <stdint.h>
int test_hidden(void);                  // ATTILA_TEST_HIDDEN=1: the game window is never shown and never takes the screen
int test_mute(void);                    // ATTILA_TEST_MUTE=1: the game's sound is silenced
void test_auto_tick(void);              // from the game's event pump (main thread): starts the benchmark when asked
double test_bench_seconds(void);        // benchmark time so far in seconds, -1 while no benchmark runs
int test_shot_due(void);                // the benchmark second of a picture to take at this frame, or -1
// One picture: 4 bytes per pixel, `order` "rgba" or "bgra", rows top-down unless bottom_up. Saved half size as shot_SSS.bmp.
void test_shot_save(int second, const uint8_t *pixels, int w, int h, int row_bytes, const char *order, int bottom_up);
void test_frame_done(void);             // after each shown frame: ends the run when its time is up
void test_note_draws(unsigned draws);   // DirectX 11 route: draw calls of the frame about to be shown (for the run's summary line)
const char *test_out_dir(void);         // ATTILA_TEST_SHOT_DIR, or NULL
