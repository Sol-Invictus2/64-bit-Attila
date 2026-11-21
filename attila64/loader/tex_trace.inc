// Texture uploads from the game's memory: only the rows that changed, and (ATTILA_TEX_TRACE=1) how much is uploaded.
// Part of shim_gfx.c (included at its end).
//
// OPENGL_TEXTURE_2D::update_subresource (33db8b0) is given a box (left, top, front, right, bottom, back: the part of the
// texture that changed) and ignores it: it always sends the whole level with glTexSubImage2D / 3D, from the pointer it was
// given. The busiest caller is the particle effects' table, WARSCAPE::EMITTER_INSTANCE_BUFFER::update_texture (34da7f0):
// six layers of 256 x 256 four-number entries, 1 MB each, every frame, although it asks for the rows between its first and
// last changed emitter only (rows 0 to 9 in a large battle). The driver copies all 6 MB during the calls, on the main
// thread: 0.3 to 0.4 ms a frame (measured 2026-11-12).
// loader.c hooks update_subresource and leaves the box's row count here while the game's function runs (upload_rows_begin /
// upload_rows_end); the upload call that follows then sends only those rows. Only boxes that start at the top left corner
// and span the level's full width are used, so the data pointer needs no change; anything else uploads as before.
// The untouched rows keep what an earlier upload put there, exactly as on DirectX. ATTILA_FULL_UPLOADS=1 turns it off.
//
// Trace: uploads take either a pointer into the game's memory (the driver copies during the call) or, while a pixel buffer
// object is bound, an offset into it (small number; the copy happens later inside the driver). Logged with each fps line
// for both ways: calls and texels per frame, time in the calls, the largest upload.
static int g_tt_on, g_upload_box_on = -1;
static int g_upload_rows, g_upload_width;   // set only while update_subresource runs, on the thread that draws
static unsigned long long g_upload_rows_saved, g_upload_rows_sent;
static void (__stdcall *r_tt_sub2d)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
static void (__stdcall *r_tt_sub3d)(unsigned, int, int, int, int, int, int, int, unsigned, unsigned, const void *);
static struct { unsigned long long calls, texels, ticks; unsigned biggest_w, biggest_h, format, type; } g_tt[2];   // 0 = pointer, 1 = buffer offset

void upload_rows_begin(const uint32_t *box) {
    if (g_upload_box_on < 0) {
        extern int settings_full_uploads(void);
        g_upload_box_on = getenv("ATTILA_FULL_UPLOADS") == NULL && !settings_full_uploads();
        log_line(g_upload_box_on ? "texture uploads: only the changed rows" : "texture uploads: whole textures (texture_uploads=full or ATTILA_FULL_UPLOADS)");
    }
    if (g_upload_box_on && box && !box[0] && !box[1] && !box[2] && box[5] == 1 && box[4] > 0 && box[3] > 0) { g_upload_rows = (int)box[4]; g_upload_width = (int)box[3]; }
}
void upload_rows_end(void) { g_upload_rows = 0; }

// The height to send: the box's rows when the call is the whole-level upload that box belongs to.
static int upload_height(int x, int y, int w, int h) {
    if (g_upload_rows <= 0 || x || y || w != g_upload_width || g_upload_rows >= h) return h;
    g_upload_rows_saved += (unsigned long long)(h - g_upload_rows);
    g_upload_rows_sent += (unsigned long long)g_upload_rows;
    return g_upload_rows;
}

static void tt_add(const void *data, int w, int h, int d, unsigned format, unsigned type, unsigned long long ticks) {
    int k = (uintptr_t)data < 0x10000000u;
    unsigned long long n = (unsigned long long)w * (unsigned long long)h * (unsigned long long)(d > 0 ? d : 1);
    g_tt[k].calls++; g_tt[k].texels += n; g_tt[k].ticks += ticks;
    if ((unsigned long long)g_tt[k].biggest_w * g_tt[k].biggest_h < n) { g_tt[k].biggest_w = (unsigned)w; g_tt[k].biggest_h = (unsigned)(h * (d > 0 ? d : 1)); g_tt[k].format = format; g_tt[k].type = type; }
}
SYSV static void w_tt_sub2d(unsigned target, int level, int x, int y, int w, int h, unsigned format, unsigned type, const void *data) {
    h = upload_height(x, y, w, h);
    if (!g_tt_on) { r_tt_sub2d(target, level, x, y, w, h, format, type, data); return; }
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    r_tt_sub2d(target, level, x, y, w, h, format, type, data);
    QueryPerformanceCounter(&b);
    tt_add(data, w, h, 1, format, type, (unsigned long long)(b.QuadPart - a.QuadPart));
}
SYSV static void w_tt_sub3d(unsigned target, int level, int x, int y, int z, int w, int h, int d, unsigned format, unsigned type, const void *data) {
    if (d == 1) h = upload_height(x, y, w, h);
    if (!g_tt_on) { r_tt_sub3d(target, level, x, y, z, w, h, d, format, type, data); return; }
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    r_tt_sub3d(target, level, x, y, z, w, h, d, format, type, data);
    QueryPerformanceCounter(&b);
    tt_add(data, w, h, d, format, type, (unsigned long long)(b.QuadPart - a.QuadPart));
}

static void *tex_trace_wrap(const char *name, void *real) {
    static int asked;
    if (!asked) { asked = 1; g_tt_on = getenv("ATTILA_TEX_TRACE") != NULL; }
    if (!strcmp(name, "glTexSubImage2D")) { r_tt_sub2d = real; return (void *)w_tt_sub2d; }
    if (!strcmp(name, "glTexSubImage3D")) { r_tt_sub3d = real; return (void *)w_tt_sub3d; }
    return NULL;
}

void tex_trace_report(long frames) {
    if (frames <= 0) return;
    if (g_tt_on && (g_upload_rows_saved || g_upload_rows_sent))
        log_line("texture uploads cut to their changed rows, per frame: %.0f rows sent, %.0f rows left out", (double)g_upload_rows_sent / frames, (double)g_upload_rows_saved / frames);
    g_upload_rows_saved = g_upload_rows_sent = 0;
    if (!g_tt_on) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    static const char *const how[2] = {"from the game's memory", "from a pixel buffer"};
    for (int k = 0; k < 2; k++) {
        if (!g_tt[k].calls) continue;
        log_line("texture uploads %s, per frame: %.1f calls, %.0f thousand texels, %.3f ms in the calls; largest %ux%u format %x type %x", how[k],
                 (double)g_tt[k].calls / frames, (double)g_tt[k].texels / 1000.0 / frames, (double)g_tt[k].ticks * 1000.0 / (double)f.QuadPart / frames,
                 g_tt[k].biggest_w, g_tt[k].biggest_h, g_tt[k].format, g_tt[k].type);
        memset(&g_tt[k], 0, sizeof g_tt[k]);
    }
}
