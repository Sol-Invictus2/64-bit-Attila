// The start window's graphics choices, applied before the game starts: texture sharpness and the OpenGL / DirectX 11 /
// DirectX 12 route.
// Part of loader.c (included there after settings_copy.inc, whose settings-file helpers it uses).

// Sets one line "name value;" in the 64-bit game's own settings file and leaves the rest as it is. Nothing is written when the
// value is already there, or when the game has not made its settings file yet (first start: the game's own choice stands).
static void set_game_setting(const char *name, const char *value) {
    extern const char *attila_home_dir(void);   // shim_libc.c
    char ours[MAX_PATH], tmp[MAX_PATH + 8];
    snprintf(ours, sizeof ours, "%s\\.Creative Assembly\\Attila\\scripts\\preferences.script.txt", attila_home_dir());
    snprintf(tmp, sizeof tmp, "%s.tmp", ours);
    char *cur = read_settings_text(ours);
    if (!cur) { log_line("%s: no settings file yet, the choice applies from the next start", name); return; }
    size_t len = strlen(cur), nlen = strlen(name), vlen = strlen(value), o = 0;
    char *out = malloc(len + nlen + vlen + 16);
    int done = 0, same = 0;
    const char *nl_text = "\n";
    {
        const char *next, *e = settings_line_end(cur, &next);
        if (*e) nl_text = *e == '\r' ? (e[1] == '\n' ? "\r\n" : "\r") : "\n";
    }
    if (!out) { free(cur); return; }
    for (const char *s = cur, *next; *s; s = next) {
        const char *e = settings_line_end(s, &next), *n, *v;
        int nl, vl;
        if (!done && settings_line(s, e, &n, &nl, &v, &vl) && (size_t)nl == nlen && !memcmp(n, name, nlen)) {
            done = 1;
            if ((size_t)vl == vlen && !memcmp(v, value, vlen)) { same = 1; break; }
            log_line("%s: %.*s -> %s (chosen in the start window)", name, vl, v, value);
            memcpy(out + o, s, (size_t)(v - s)); o += (size_t)(v - s);
            memcpy(out + o, value, vlen); o += vlen;
            memcpy(out + o, v + vl, (size_t)(next - (v + vl))); o += (size_t)(next - (v + vl));
            continue;
        }
        memcpy(out + o, s, (size_t)(next - s));
        o += (size_t)(next - s);
    }
    if (!same) {
        if (!done) {
            if (o && out[o - 1] != '\n' && out[o - 1] != '\r') o += (size_t)sprintf(out + o, "%s", nl_text);
            o += (size_t)sprintf(out + o, "%s %s;%s", name, value, nl_text);
            log_line("%s: set to %s (chosen in the start window)", name, value);
        }
        FILE *f = fopen(tmp, "wb");
        int ok = f && fwrite(out, 1, o, f) == o;
        if (f && fclose(f)) ok = 0;
        if (ok) ok = MoveFileExA(tmp, ours, MOVEFILE_REPLACE_EXISTING) != 0;
        if (!ok) { DeleteFileA(tmp); log_line("%s: could not write the game's settings file", name); }
    }
    free(out);
    free(cur);
}

// Texture sharpness: the game's gfx_texture_filtering, 0 = trilinear to 4 = anisotropic 16x. ATTILA_TEXTURE_FILTER=n is for tests.
// Measured 2026-11-11 (shim_gfx.c logs it): in battle the game sends the level to the card per texture, 2 -> 4x, 4 -> 16x.
static void apply_texture_sharpness(void) {
    extern int settings_texture_filtering(void);   // picker.c
    const char *e = getenv("ATTILA_TEXTURE_FILTER");
    int level = e ? atoi(e) : settings_texture_filtering();
    if (level < 0 || level > 4) return;   // "Game setting": left alone
    char v[4];
    snprintf(v, sizeof v, "%d", level);
    set_game_setting("gfx_texture_filtering", v);
}

// Test only: ATTILA_AA=n writes the game's anti-aliasing setting (0 none, 1 MLAA, 2 to 4 = MSAA 2x, 4x, 8x) before the start.
static void apply_test_aa(void) {
    const char *e = getenv("ATTILA_AA");
    if (e && e[0] >= '0' && e[0] <= '4' && !e[1]) set_game_setting("gfx_aa", e);
}

// DirectX 12 route: Mesa's opengl32.dll (with its driver file and Microsoft's dxil.dll) from the dx12 folder next to the exe is
// loaded before anything asks for OpenGL. Windows then gives that same loaded file to everyone who asks for "opengl32.dll" by
// name (SDL, the window code, shim_gfx.c), so nothing else changes: the game still draws with OpenGL calls and Mesa turns them
// into DirectX 12. Without this the card's own OpenGL driver is used. ATTILA_GRAPHICS=dx12 / opengl is for tests.
//
// DirectX 11 route (dx11_device.inc): the game's own DirectX 11 shaders drawn by a real DirectX 11 device instead of the
// OpenGL one. Since 2026-11-13 the only route offered (no choice in the start window; picker.c settings_graphics_dx11 is 1
// unless this PC fell back after a failed DirectX 11 start). ATTILA_GRAPHICS=dx11|opengl|dx12 and ATTILA_DX11=1 are for tests.
// Before it is taken, this PC is asked for a DirectX 11 device of the level the renderer needs; a PC that cannot give one
// stays on OpenGL. A start that never gets as far as a running game leaves dx11_starting.txt behind, and the start window
// then puts the choice back to OpenGL (the same safety net as for DirectX 12).
static int g_dx12_note, g_dx11_note;
static int dx11_usable(HRESULT *why) {
    static const D3D_FEATURE_LEVEL want[1] = {D3D_FEATURE_LEVEL_11_0};
    ID3D11Device *d = NULL;
    ID3D11DeviceContext *c = NULL;
    D3D_FEATURE_LEVEL got = (D3D_FEATURE_LEVEL)0;
    HRESULT hr = getenv("ATTILA_TEST_DX11_FAIL") ? E_FAIL   /* test: behave like a PC without DirectX 11 */
               : D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want, 1, D3D11_SDK_VERSION, &d, &got, &c);
    if (c) c->lpVtbl->Release(c);
    if (d) d->lpVtbl->Release(d);
    if (SUCCEEDED(hr) && !LoadLibraryA("d3dcompiler_47.dll")) hr = HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);   // reads the shaders' slot names
    *why = hr;
    return SUCCEEDED(hr);
}
static void setup_graphics_route(void) {
    extern int settings_graphics_dx12(void), settings_graphics_dx11(void);   // picker.c
    const char *e = getenv("ATTILA_GRAPHICS"), *test = getenv("ATTILA_DX11");
    if (test && !strcmp(test, "1")) { g_want_dx11 = 1; log_line("graphics: DirectX 11 (test switch ATTILA_DX11=1)"); return; }
    if (e ? !_stricmp(e, "dx11") : settings_graphics_dx11()) {
        HRESULT why = 0;
        if (!dx11_usable(&why)) {
            log_line("graphics: DirectX 11 was chosen but this PC could not make a DirectX 11 device (0x%08lx); using OpenGL, the card's own driver", (unsigned long)why);
            return;
        }
        g_want_dx11 = 1;
        log_line("graphics: DirectX 11");
        char note[MAX_PATH + 32];
        GetModuleFileNameA(NULL, note, MAX_PATH);
        char *slash = strrchr(note, '\\');
        snprintf(slash ? slash + 1 : note, 32, "dx11_starting.txt");
        FILE *nf = fopen(note, "w");
        if (nf) { fputs("64-bit Attila was starting with DirectX 11. This file goes away once the game is running.\n", nf); fclose(nf); g_dx11_note = 1; }
        return;
    }
    if (!(e ? !_stricmp(e, "dx12") : settings_graphics_dx12())) { log_line("graphics: OpenGL, the card's own driver"); return; }
    static const char *const files[] = {"libgallium_wgl.dll", "dxil.dll", "opengl32.dll"};   // opengl32 last: it needs the first
    char dir[MAX_PATH], path[MAX_PATH + 32];
    GetModuleFileNameA(NULL, dir, sizeof dir);
    char *s = strrchr(dir, '\\');
    if (s) *s = 0;
    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof path, "%s\\dx12\\%s", dir, files[i]);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
            log_line("graphics: DirectX 12 was chosen but dx12\\%s is missing; using OpenGL, the card's own driver", files[i]);
            return;
        }
    }
    // Mesa picks its driver from this name; set for Windows and for the C library copies already loaded
    SetEnvironmentVariableA("GALLIUM_DRIVER", "d3d12");
    _putenv("GALLIUM_DRIVER=d3d12");
    HMODULE crt = GetModuleHandleA("msvcrt.dll");
    int (__cdecl *crt_putenv)(const char *) = crt ? (void *)GetProcAddress(crt, "_putenv") : NULL;
    if (crt_putenv) crt_putenv("GALLIUM_DRIVER=d3d12");
    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof path, "%s\\dx12\\%s", dir, files[i]);
        if (!LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH)) {
            DWORD err = GetLastError();
            log_line("graphics: could not load dx12\\%s (error %lu)%s", files[i], err,
                     i < 2 ? "; using OpenGL, the card's own driver" : "; OpenGL comes from the card's own driver");
            return;
        }
    }
    log_line("graphics: DirectX 12 through Mesa (files in the dx12 folder)");
    // A note that a DirectX 12 start is under way. It is taken away once the game has drawn for a while (dx12_start_worked,
    // called from shim_gfx.c). If it is still there at the next start, DirectX 12 did not get that far on this PC, and the
    // start window puts the choice back to OpenGL (picker.c). On a GTX 1660 Super the game crashed while making its first
    // textures (tester log 2026-11-11); without this the player is left with a game that will not open.
    snprintf(path, sizeof path, "%s\\dx12_starting.txt", dir);
    FILE *f = fopen(path, "w");
    if (f) { fputs("64-bit Attila was starting with DirectX 12. This file goes away once the game is running.\n", f); fclose(f); g_dx12_note = 1; }
}

// Called once the game has drawn for a while (shim_gfx.c, frame 600) or ended by itself (shim_libc.c): the DirectX 12 or
// DirectX 11 start of this run got through, so its note goes away.
void dx12_start_worked(void) {
    if (!g_dx12_note && !g_dx11_note) return;   // only the run that left a note takes it away
    const char *name = g_dx11_note ? "dx11_starting.txt" : "dx12_starting.txt";
    int eleven = g_dx11_note;
    g_dx12_note = g_dx11_note = 0;
    char path[MAX_PATH + 32];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char *s = strrchr(path, '\\');
    snprintf(s ? s + 1 : path, 32, "%s", name);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES && DeleteFileA(path)) log_line("graphics: DirectX %d start worked", eleven ? 11 : 12);
}
