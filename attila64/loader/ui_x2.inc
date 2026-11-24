// Sharp interface at 4K (DirectX 11 route). Part of loader.c (included after dx11_device.inc).
// ATTILA_UI_X2=0 = off, =1 = on whatever the zoom; default: on when the interface is zoomed 1.5 times or more
// (interface size "Auto (like 1080p)" on a 3840x2160 screen = 2.0). Display only.
//
// Found 2026-11-13: with the zoom, every interface picture is drawn twice as large by a plain bilinear stretch and looks
// soft. Replacing the picture files with twice-as-large ones breaks frame lines (parts of the interface size themselves
// from a picture's pixel count), so the game must keep its own size for every picture. The interface packs its pictures
// into a few large textures, the cache pages (WARSCAPE::UI_TEXTURE_HANDLER): a picture is copied row by row into an
// upload texture (UI_TEXTURE_HANDLER::m_image at 5252628, its TEXTURE at +30h, pieces of up to 512x512) and then copied
// on the card into its spot on a page, up to 9 times (the picture and its repeated edges, against bleeding):
//   UI_TEXTURE_HANDLER::update_region(CACHED_TEXTURE_REGION_RESULTS*, const CA::ImageType<Pixel8888>&, bool) 30a75a0
//     (results +30h = the page's TEXTURE_CACHE_DETAILS, whose +8 is a CA::String holding the picture's file name, put
//     there by load_into_region just before; the image: vtable +8 = width, +10h = height),
//   TEXTURE_MANAGER_ENTRY::update_region(entry, TEXTURE_UPDATE_IMAGE&, Rectangle const&, x, y) 3440be0: the card copy
//     (entry->device_texture() 3440260 = the page).
// Here both the upload texture and every page are kept at twice the size on the card (dx11_device.inc, DXT_SCALE), so
// the game's own rectangles and fractions all stay valid; uploads are written in at double size: the sharp picture
// <exe folder>\ui4k\<file name>.png when there is one of exactly twice the picture's size (pictures up to 512x512),
// else the picture stretched the way the card stretched it before (same look). Hit tests use the game's own images.
// A sharp picture is only used when it matches the picture the game loaded (x2_matches: 256 points of the sharp picture
// shrunk back against the game's pixels), so a mod's own picture under the same name keeps its look (stretched).
// Pictures larger than 512 come in pieces; dx11_device.inc cuts the matching part of the sharp picture for each piece.
#define COBJMACROS
#include <wincodec.h>
static int g_x2_on = -1;   // -1 = not decided yet
typedef uint64_t (SYSV *UiUpdateRegionFn)(uint8_t *results, uint8_t *image, int flag);
typedef uint64_t (SYSV *EntryUpdateRegionFn)(uint8_t *entry, uint8_t *img, const void *rect, int x, int y);
static UiUpdateRegionFn g_x2_uur_orig;
static EntryUpdateRegionFn g_x2_eur_orig;
static IWICImagingFactory *g_x2_wic;
static int g_x2_wic_failed;
static char g_x2_dir[MAX_PATH];
static volatile long g_x2_missing, g_x2_wrong_size, g_x2_rejected, g_x2_checked;
// WIC's identifiers, written out here (no extra import library needed)
static const GUID X2_CLSID_WICImagingFactory = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
static const GUID X2_IID_IWICImagingFactory = {0xec5ec8a9, 0xc395, 0x4314, {0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70}};
static const GUID X2_GUID_32bppBGRA = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f}};

// Where the sharp pictures are, out = folder ending in a backslash: <exe folder>\ui4k\ (hand-installed, tests) first, else the
// Workshop item with the pictures (shipped apart, about 1 GB, read where Steam puts it, nothing copied): any subscribed item
// of Attila (steamapps\workshop\content\325610\<id>) holding ui4k\attila64_sharp_ui.txt, so the item number need not be
// built in. 0 = none.
static int x2_is_dir(const char *p) {
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static int x2_find_dir(char *out, size_t size) {
    char exe[MAX_PATH], p[MAX_PATH * 2];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    char *s = strrchr(exe, '\\');
    if (s) *s = 0;
    snprintf(p, sizeof p, "%s\\ui4k\\", exe);
    if (x2_is_dir(p) && strlen(p) < size) { strcpy(out, p); return 1; }
    char ws[MAX_PATH * 2];
    snprintf(ws, sizeof ws, "%s\\..\\..\\..\\workshop\\content\\325610", exe);
    snprintf(p, sizeof p, "%s\\*", ws);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(p, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int found = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        snprintf(p, sizeof p, "%s\\%s\\ui4k\\attila64_sharp_ui.txt", ws, fd.cFileName);
        if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) continue;
        snprintf(p, sizeof p, "%s\\%s\\ui4k\\", ws, fd.cFileName);
        char full[MAX_PATH * 2];   // without the ..\..\..: picture paths stay short enough for the plain Windows calls
        DWORD n = GetFullPathNameA(p, sizeof full, full, NULL);
        if (n && n < size) { strcpy(out, full); found = 1; }
    } while (!found && FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

static int x2_active(void) {
    if (g_x2_on >= 0) return g_x2_on;
    if (!g_dx.d || !g_dx.dev) return 0;   // not the DirectX 11 route (yet)
    const char *e = getenv("ATTILA_UI_X2");
    float f = 0;
    if (e && *e == '0') g_x2_on = 0;
    else if (e && *e == '1') g_x2_on = 1;
    else {   // the zoom as interface.inc works it out: the player's factor, or screen height / 1080, never past what fits
        extern float g_ui_size;
        uint32_t w = DXF(g_dx.dev, 0x1610, uint32_t), h = DXF(g_dx.dev, 0x1614, uint32_t);
        f = g_ui_size > 0 ? g_ui_size : (float)h / 1080.0f;
        float most = (float)w / 1440.0f < (float)h / 1080.0f ? (float)w / 1440.0f : (float)h / 1080.0f;   // never laid out lower than 1080 (interface.inc)
        if (f > most) f = most;
        g_x2_on = f >= 1.5f;
    }
    int no_dir = 0;
    if (g_x2_on) {
        // Without the picture folder there is nothing sharp to show, and the pages at twice the size would only cost card
        // memory: off then. ATTILA_UI_X2=1 keeps it on for tests.
        if (!x2_find_dir(g_x2_dir, sizeof g_x2_dir) && !(e && *e == '1')) { g_x2_on = 0; no_dir = 1; }
    }
    log_line("sharp interface: %s (interface zoom %.2f%s; sharp pictures from %s)", g_x2_on ? "on" : no_dir ? "off, no sharp pictures (ui4k folder next to the game, or the Workshop item 64-bit Attila Sharp 4K Interface)" : "off", (double)f, e ? ", set by ATTILA_UI_X2" : "", g_x2_on ? g_x2_dir : "-");
    return g_x2_on;
}

// The sharp picture for a game file name, BGRA, exactly (2w) x (2h); NULL when there is none (or it does not fit).
// alt 0 = <name>.png; alt n = <name>.png.alt<n>.png: the same picture name in a mod with other art (ui4k\upscale_all.py
// and the staging script give each different version of a name its own number; x2_matches picks the right one).
// *exists: whether that file is there at all (alternates are numbered without gaps).
static uint8_t *x2_load_sharp(const char *name, uint32_t len, uint32_t w, uint32_t h, int alt, int *exists) {
    *exists = 0;
    if (!name || !len || len > 400 || g_x2_wic_failed) return NULL;
    char rel[560];
    uint32_t n = 0;
    for (uint32_t i = 0; i < len && n < 500; i++) { char c = name[i]; rel[n++] = c == '/' ? '\\' : (c >= 'A' && c <= 'Z' ? (char)(c + 32) : c); }
    rel[n] = 0;
    char *r0 = rel;
    if (!strncmp(r0, "data\\", 5)) { r0 += 5; n -= 5; }   // the game names some pictures data/ui/...: our files are under ui\...
    if (n < 4 || _stricmp(r0 + n - 4, ".png")) { memcpy(r0 + n, ".png", 5); n += 4; }
    if (alt) snprintf(r0 + n, sizeof rel - (size_t)(r0 - rel) - n, ".alt%d.png", alt);
    char full[MAX_PATH + 600];
    snprintf(full, sizeof full, "%s%s", g_x2_dir, r0);
    if (GetFileAttributesA(full) == INVALID_FILE_ATTRIBUTES) {
        if (!alt) {
            InterlockedIncrement(&g_x2_missing);
            static volatile long told;
            if (InterlockedIncrement(&told) <= 40) log_line("sharp interface: no sharp file for %s (%ux%u)", r0, w, h);
        }
        return NULL;
    }
    *exists = 1;
    if (!g_x2_wic) {
        HRESULT ci = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        (void)ci;   // already set up on this thread (any kind) is fine too
        if (FAILED(CoCreateInstance(&X2_CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &X2_IID_IWICImagingFactory, (void **)&g_x2_wic)) || !g_x2_wic) {
            g_x2_wic_failed = 1;
            log_line("sharp interface: the Windows picture reader (WIC) could not be started: sharp pictures not used");
            return NULL;
        }
    }
    wchar_t wfull[MAX_PATH + 512];
    MultiByteToWideChar(CP_UTF8, 0, full, -1, wfull, MAX_PATH + 512);
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *fr = NULL;
    IWICFormatConverter *cv = NULL;
    uint8_t *px = NULL;
    UINT fw = 0, fh = 0;
    if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(g_x2_wic, wfull, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &fr)) && SUCCEEDED(IWICBitmapFrameDecode_GetSize(fr, &fw, &fh))) {
        if (fw != 2 * w || fh != 2 * h) {
            if (InterlockedIncrement(&g_x2_wrong_size) <= 5) log_line("sharp interface: %s is %ux%u, the picture is %ux%u: not used", r0, fw, fh, w, h);
        } else if (SUCCEEDED(IWICImagingFactory_CreateFormatConverter(g_x2_wic, &cv)) &&
                   SUCCEEDED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)fr, &X2_GUID_32bppBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom))) {
            px = malloc((size_t)fw * fh * 4);
            if (px && FAILED(IWICFormatConverter_CopyPixels(cv, NULL, fw * 4, fw * fh * 4, px))) { free(px); px = NULL; }
        }
    }
    if (cv) IWICFormatConverter_Release(cv);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (dec) IWICBitmapDecoder_Release(dec);
    return px;
}

// Does the sharp picture (BGRA, 2w x 2h) show the picture the game loaded (its pixels at image + 18h, w a row)? 256 points:
// the sharp picture's 2x2 block against the game's pixel, colours weighed by alpha, both channel orders tried.
static int x2_matches(const uint8_t *sharp, uint32_t w, uint32_t h, const uint8_t *image, const char *name, uint32_t len) {
    const uint8_t *g = *(const uint8_t *const *)(image + 0x18);
    if (!g) return 1;
    long d[2] = {0, 0};
    int n = 0;
    for (uint32_t sy = 0; sy < 16; sy++)
        for (uint32_t sx = 0; sx < 16; sx++) {
            uint32_t x = (2 * sx + 1) * w / 32, y = (2 * sy + 1) * h / 32;
            const uint8_t *gp = g + ((size_t)y * w + x) * 4;
            const uint8_t *s0 = sharp + ((size_t)(2 * y) * (2 * w) + 2 * x) * 4, *s1 = s0 + 4, *s2 = s0 + (size_t)w * 8, *s3 = s2 + 4;
            int av[4];
            for (int c = 0; c < 4; c++) av[c] = (s0[c] + s1[c] + s2[c] + s3[c] + 2) / 4;
            int ga = gp[3], sa = av[3];
            for (int k = 0; k < 2; k++) {
                long e = labs((long)ga - sa) * 255;
                for (int c = 0; c < 3; c++) e += labs((long)gp[k ? 2 - c : c] * ga - (long)av[c] * sa);
                d[k] += e;
            }
            n++;
        }
    double m0 = (double)d[0] / (n * 4.0 * 255.0), m1 = (double)d[1] / (n * 4.0 * 255.0), m = m0 < m1 ? m0 : m1;
    int ok = m <= 24.0;
    long c = InterlockedIncrement(&g_x2_checked);
    if (!ok) InterlockedIncrement(&g_x2_rejected);
    if (c <= 8 || (!ok && g_x2_rejected <= 12))
        log_line("sharp interface: %.*s %ux%u: difference %.1f (as stored %.1f, red and blue swapped %.1f): %s", (int)(len < 120 ? len : 120), name, w, h, m, m0, m1, ok ? "sharp picture used" : "NOT the same picture, stretched instead");
    return ok;
}

static SYSV uint64_t x2_ui_update_region(uint8_t *results, uint8_t *image, int flag) {
    if (!x2_active()) return g_x2_uur_orig(results, image, flag);
    uint8_t *holder = *(uint8_t **)(uintptr_t)0x5252628;   // UI_TEXTURE_HANDLER::m_image
    uint8_t *upload = holder ? *(uint8_t **)(holder + 0x30) : NULL;
    if (upload) dx_x2_make(upload);
    uint32_t w = 0, h = 0;
    if (image) {
        void **vt = *(void ***)image;
        w = ((SYSV uint32_t (*)(void *))vt[1])(image);
        h = ((SYSV uint32_t (*)(void *))vt[2])(image);
    }
    int whole = w && h && w <= 512 && h <= 512;   // bigger pictures come in pieces of 512: the whole upload texture is used
    g_x2_written_w = whole ? w : 0;
    g_x2_written_h = whole ? h : 0;
    uint8_t *px = NULL;
    uint8_t *details = results ? *(uint8_t **)(results + 0x30) : NULL;
    const char *name = details ? *(const char **)(details + 0x10) : NULL;
    uint32_t len = details ? *(uint32_t *)(details + 0x8) : 0;
    for (int alt = 0; w && h && w <= 4096 && h <= 4096 && name && alt <= 6; alt++) {   // the main file, then the alternates
        int exists = 0;
        px = x2_load_sharp(name, len, w, h, alt, &exists);
        if (!exists) break;
        if (px && x2_matches(px, w, h, image, name, len)) break;
        free(px);
        px = NULL;
    }
    g_x2_repl = whole ? px : NULL; g_x2_repl_w = w; g_x2_repl_h = h; g_x2_repl_used = 0;
    g_x2_big = whole ? NULL : px; g_x2_big_w = w; g_x2_big_h = h;
    uint64_t r = g_x2_uur_orig(results, image, flag);
    g_x2_repl = NULL; g_x2_big = NULL;
    g_x2_written_w = g_x2_written_h = 0;
    free(px);
    return r;
}
static SYSV uint64_t x2_entry_update_region(uint8_t *entry, uint8_t *img, const void *rect, int x, int y) {
    if (x2_active() && entry) {
        uint8_t *page = ((SYSV uint8_t *(*)(uint8_t *))(uintptr_t)0x3440260)(entry);   // TEXTURE_MANAGER_ENTRY::device_texture
        if (page) dx_x2_make(page);
    }
    return g_x2_eur_orig(entry, img, rect, x, y);
}

// With each fps line (shim_gfx.c), while on: what was done.
void ui_x2_report(void) {
    if (g_x2_on != 1) return;
    long up = InterlockedExchange(&g_x2_uploads, 0), sharp = InterlockedExchange(&g_x2_sharp, 0), miss = InterlockedExchange(&g_x2_missing, 0);
    long pieces = InterlockedExchange(&g_x2_pieces, 0);
    if (up) log_line("sharp interface: %ld uploads written at twice the size, %ld of them sharp (%ld pieces of large pictures); %ld pictures without a sharp file, %ld sharp files not matching the game's picture so far",
                     up, sharp, pieces, miss, (long)g_x2_rejected);
}

static void patch_ui_x2(void) {
    const char *e = getenv("ATTILA_UI_X2");
    if (e && *e == '0') { g_x2_on = 0; return; }
    static const uint8_t uur_old[10] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53};   // 30a75a0 and 3440be0 begin alike
    static const uint8_t page_old[8] = {0x53, 0x48, 0x89, 0xFB, 0x48, 0x8B, 0x03, 0xFF};   // 3440260: TEXTURE_MANAGER_ENTRY::device_texture
    static const uint8_t mimage_read[7] = {0x4C, 0x8B, 0x2D, 0x25, 0xAE, 0x1A, 0x02};   // 30a77fc: mov r13,[UI_TEXTURE_HANDLER::m_image]
    if (memcmp((void *)0x30a75a0, uur_old, 10) || memcmp((void *)0x3440be0, uur_old, 10) || memcmp((void *)0x30a77fc, mimage_read, 7) ||
        memcmp((void *)0x3440260, page_old, 8)) {
        log_line("patch skipped, game code differs: sharp interface"); return;
    }
    g_x2_uur_orig = (UiUpdateRegionFn)hook_entry(0x30a75a0, uur_old, 5, (void *)x2_ui_update_region);
    g_x2_eur_orig = (EntryUpdateRegionFn)hook_entry(0x3440be0, uur_old, 5, (void *)x2_entry_update_region);
    if (!g_x2_uur_orig || !g_x2_eur_orig) { log_line("patch skipped, no memory near the game: sharp interface"); g_x2_on = 0; return; }
    log_line("patch applied: sharp interface hooks (they decide at the first interface picture: on with the DirectX 11 route and an interface zoom of 1.5 or more)");
}
