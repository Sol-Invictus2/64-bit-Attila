// DirectX 11 renderer for the 64-bit game (work in progress, dev only; switch ATTILA_DX11=1). Part of loader.c.
//
// The game draws only through WARSCAPE::RENDER_DEVICE, whose Linux build is OPENGL_RENDER_DEVICE (attila64\dx11\NOTES.md has
// the whole map). The game makes it in exactly one place, create_opengl_render_device (33d4220). With ATTILA_DX11=1 that
// function is replaced: the game's own OpenGL device constructor still runs (it only fills fields, no OpenGL), then the
// device's and its immediate context's vtables are swapped for ours. Our tables start as copies of the OpenGL ones; every
// slot that would reach OpenGL is either done here with DirectX 11 or points at a trap that logs "not done yet: <slot>" once
// and returns 0, so each test run lists what is left. Slot names: attila64\dx11\vtables.txt (device = OPENGL_RENDER_DEVICE,
// context = OPENGL_DEVICE_CONTEXT).

#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <math.h>
#include "test_auto.h"
#include "fps_overlay.h"
#include "menu_button.h"

static int g_want_dx11;                 // 1 = this run draws with DirectX 11 (decided in graphics_choice.inc before the game starts)
static uint32_t g_dx_sm = 3;            // shader model level told to the game (caps +8), see dx_create_device and patch_dx11
static ID3D11DeviceContext1 *g_dx_c1;   // the same context seen as DirectX 11.1 (NULL on a system without it)
#define DXREL(p) do { if (p) { ((IUnknown *)(p))->lpVtbl->Release((IUnknown *)(p)); (p) = NULL; } } while (0)
static D3D11_RECT g_dx_scissor;         // the scissor rectangle as last set (starts as the whole window, like OpenGL's)
typedef SYSV void *(*GameMallocFn)(size_t, int);
typedef SYSV void (*GameCtorFn)(void *);
typedef SYSV void (*GameRectFn)(void *, void *);
#define GAME_MALLOC ((GameMallocFn)0x525f10)
#define GL_DEVICE_CTOR ((GameCtorFn)0x33d42d0)
#define CAWINDOW_DISPLAY_RECT ((GameRectFn)0x4f0db0)

enum { DX_DEV_SLOTS = 0x308 / 8, DX_CTX_SLOTS = 0x190 / 8 };
static const uint64_t *const GL_DEV_VTBL = (const uint64_t *)0x3c533e0;   // "vtable for OPENGL_RENDER_DEVICE" + 0x10
static const uint64_t *const GL_CTX_VTBL = (const uint64_t *)0x3c53900;   // "vtable for OPENGL_DEVICE_CONTEXT" + 0x10

static struct {
    int on;
    uint8_t *dev;                     // the game's device object (0x16d8 bytes, OpenGL layout kept)
    ID3D11Device *d;
    ID3D11DeviceContext *c;
    IDXGISwapChain1 *sc;
    ID3D11RenderTargetView *bb_rtv;   // back buffer
    HWND hwnd;
    UINT w, h, vsync;
    uint64_t dev_vtbl[DX_DEV_SLOTS], ctx_vtbl[DX_CTX_SLOTS];
    volatile LONG trap_seen[8][128];
    uint64_t frames;
} g_dx;

// DirectX 11's drawing context is for one thread at a time. The game draws from one thread; this counts the buffer and
// texture fill calls that reach us from any other thread (the game's parallel effects updater runs on the task threads),
// so a test run shows whether such calls exist at all. Reported with a line in the log when there were any.
static DWORD g_dx_thread;
static volatile long g_dx_other_thread[4];   // buffer map, buffer unmap, texture map / unmap / update, compute buffer
static inline void dx_thread_note(int what) { if (GetCurrentThreadId() != g_dx_thread) InterlockedIncrement(&g_dx_other_thread[what]); }
enum { DXT_RES = 0x68, DXT_SRV = 0x70, DXT_SRV4 = 0x78, DXT_SRV5 = 0x80, DXT_BIND = 0x88, DXT_VIEWS = 0x90, DXT_MAP_PTR = 0x110, DXT_MAP_SUB = 0x118, DXT_MAP_KIND = 0x11c, DXT_SAMPLES = 0x120 /* samples per pixel, 0 or 1 = plain */, DXT_SCALE = 0x124 /* 2 = kept at twice the game's size on the card (ui_x2.inc) */, DXT_FLAGS = 0x128 /* DXTF_* */, DXT_SIZE = 0x130 };   // our texture fields
enum { DXTF_PLAIN = 1 };   // DXT_FLAGS: the game made it dynamic, the card has it as a plain (default) texture (dx_tex_writable_by_card)
#define DXF(obj, off, T) (*(T *)((uint8_t *)(obj) + (off)))
static int dx_create_back_buffer(uint8_t *dev, uint32_t w, uint32_t h);
static void dx_depth_frame_end(uint32_t draws);
static uint32_t g_dx_next_array;
static struct {
    uint32_t draws, no_vs, no_ps, to_bb, no_layout, miss_cb, miss_tex, clears;
    uint32_t shaders, textures, views, dispatches;                // made / run in this frame
    double shader_ms, texture_ms, map_ms, update_ms;              // time spent in this frame making shaders, textures, in maps and uploads
} g_dxs;
static double dx_now_ms(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
// test: ATTILA_DX11_CPU=1 times every call the game makes into this device, by kind, and keeps the longest single one;
// the slow-frame report then says where the game's thread sat. (Two clock reads per call, so only for tests.)
static int g_dx_cpu_on;
static struct { double draw, targets, clear, copy, query, compute, map, worst; const char *worst_what; const void *worst_vs, *worst_ps; } g_dxcpu;
static const void *dx_cpu_shader(int stage);
#define DX_CPU_T0() (g_dx_cpu_on ? dx_now_ms() : 0.0)
static void dx_cpu_add(double *sum, double t0, const char *what) {
    if (!g_dx_cpu_on) return;
    double dt = dx_now_ms() - t0;
    *sum += dt;
    if (dt > g_dxcpu.worst) { g_dxcpu.worst = dt; g_dxcpu.worst_what = what; g_dxcpu.worst_vs = dx_cpu_shader(0); g_dxcpu.worst_ps = dx_cpu_shader(1); }
}


// ---- traps: one tiny piece of code per slot ("mov edi, id; jmp dx_trap") so the log can name the slot.
SYSV static uint64_t dx_trap(uint32_t id) {
    uint32_t table = id >> 8, slot = id & 0xff;
    static const char *const names[8] = {"device", "context", "texture 2D", "?", "?", "?", "?", "?"};
    if (table < 8 && slot < 128 && !InterlockedExchange(&g_dx.trap_seen[table][slot], 1))
        log_line("DirectX 11: not done yet: %s slot +0x%03x (attila64\\dx11\\vtables.txt)", names[table], slot * 8);
    return 0;
}
static uint8_t *g_dx_thunks;
static void *dx_trap_thunk(uint32_t table, uint32_t slot) {
    if (!g_dx_thunks) g_dx_thunks = VirtualAlloc(NULL, 8 * 128 * 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_dx_thunks) return NULL;
    uint8_t *t = g_dx_thunks + (table * 128 + slot) * 32;
    uint32_t id = table << 8 | slot;
    uint64_t target = (uint64_t)dx_trap;
    t[0] = 0xBF; memcpy(t + 1, &id, 4);                    // mov edi, id
    t[5] = 0x48; t[6] = 0xB8; memcpy(t + 7, &target, 8);  // mov rax, dx_trap
    t[15] = 0xFF; t[16] = 0xE0;                            // jmp rax
    return t;
}

// ---- device slots done so far
static HWND dx_hwnd_of_sdl_window(void *sdl_window) {
    typedef struct { uint8_t major, minor, patch; } SdlVer;
    typedef struct { SdlVer v; int subsystem; HWND window; HDC hdc; HINSTANCE inst; uint8_t pad[64]; } SdlWmInfo;
    typedef int (__cdecl *GetWmInfoFn)(void *, SdlWmInfo *);
    HMODULE sdl = GetModuleHandleA("SDL2.dll");
    GetWmInfoFn get = sdl ? (GetWmInfoFn)(void *)GetProcAddress(sdl, "SDL_GetWindowWMInfo") : NULL;
    if (!get || !sdl_window) return NULL;
    SdlWmInfo info;
    memset(&info, 0, sizeof info);
    info.v.major = 2; info.v.minor = 0; info.v.patch = 0;
    return get(sdl_window, &info) ? info.window : NULL;
}

static int dx_make_back_buffer_view(void) {
    ID3D11Texture2D *bb = NULL;
    if (FAILED(g_dx.sc->lpVtbl->GetBuffer(g_dx.sc, 0, &IID_ID3D11Texture2D, (void **)&bb))) return 0;
    HRESULT hr = g_dx.d->lpVtbl->CreateRenderTargetView(g_dx.d, (ID3D11Resource *)bb, NULL, &g_dx.bb_rtv);
    bb->lpVtbl->Release(bb);
    return SUCCEEDED(hr);
}

// A new screen size or window mode (options screen Apply; found 2026-11-24 from a Workshop report: "When I change resolution
// to 4k, the screen goes white"). The game applies it as WS_ENGINE_IMP::set_graphics_options -> res_destroy(2) ->
// pr_mode_change -> WS_ENGINE_SCREEN::mode_change -> destroy_device, then pr_create_device -> create_device. On OpenGL that
// keeps the context, so every texture, buffer and shader the game still holds stays valid; it only rebuilds the screen.
// Before this the DirectX 11 route released the whole device there and made a new one: everything the game kept pointed into
// the old device (white screen; in a hidden test a crash right after). Now destroy_device only lets go of the screen
// (dx_destroy_screen) and create_device, when the device is already there, resizes the swap chain and makes the game's back
// buffer objects at the new size. destroy (+0x128, game end) still releases everything.
static int dx_create_back_buffer(uint8_t *dev, uint32_t w, uint32_t h);
static void dx_unbind_all(void);
static int dx_resize_screen(uint8_t *dev, HWND old_hwnd, uint32_t w, uint32_t h, uint8_t vsync) {
    HRESULT hr = S_OK;
    DXREL(g_dx.bb_rtv);
    if (g_dx.sc && g_dx.hwnd == old_hwnd) {
        hr = g_dx.sc->lpVtbl->ResizeBuffers(g_dx.sc, 0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    } else {   // another window: a new swap chain for it
        DXREL(g_dx.sc);
        IDXGIDevice *xd = NULL; IDXGIAdapter *xa = NULL; IDXGIFactory2 *xf = NULL;
        g_dx.d->lpVtbl->QueryInterface(g_dx.d, &IID_IDXGIDevice, (void **)&xd);
        if (xd) xd->lpVtbl->GetAdapter(xd, &xa);
        if (xa) xa->lpVtbl->GetParent(xa, &IID_IDXGIFactory2, (void **)&xf);
        DXGI_SWAP_CHAIN_DESC1 sd;
        memset(&sd, 0, sizeof sd);
        sd.Width = w; sd.Height = h;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        hr = xf ? xf->lpVtbl->CreateSwapChainForHwnd(xf, (IUnknown *)g_dx.d, g_dx.hwnd, &sd, NULL, NULL, &g_dx.sc) : E_FAIL;
        if (xf) xf->lpVtbl->MakeWindowAssociation(xf, g_dx.hwnd, DXGI_MWA_NO_ALT_ENTER);
        if (xf) xf->lpVtbl->Release(xf);
        if (xa) xa->lpVtbl->Release(xa);
        if (xd) xd->lpVtbl->Release(xd);
    }
    if (FAILED(hr) || !dx_make_back_buffer_view()) { log_line("DirectX 11: new screen size %ux%u failed 0x%08lx", w, h, (unsigned long)hr); return 0; }
    g_dx.w = w; g_dx.h = h; g_dx.vsync = vsync;
    if (!dx_create_back_buffer(dev, w, h)) { log_line("DirectX 11: back buffer objects at %ux%u failed", w, h); return 0; }
    { D3D11_RECT all = {0, 0, (LONG)w, (LONG)h}; g_dx.c->lpVtbl->RSSetScissorRects(g_dx.c, 1, &all); g_dx_scissor = all; }
    log_line("DirectX 11: screen now %ux%u (device kept)", w, h);
    return 1;
}
// destroy_device (+0x100): only the screen (see above). The game's back buffer objects go; the device and the game's
// resources stay.
SYSV static void dx_destroy_screen(uint8_t *dev) {
    if (!g_dx.d) return;
    dx_unbind_all();
    static const uint16_t objs[3] = {0x1628, 0x1618, 0x1620};
    for (int i = 0; i < 3; i++) {
        void *o = DXF(dev, objs[i], void *);
        if (o) ((SYSV void (*)(void *))(*(void ***)o)[1])(o);
        DXF(dev, objs[i], void *) = NULL;
    }
    DXF(dev, 0x1630, void *) = NULL;
    DXREL(g_dx.bb_rtv);
    g_dx.c->lpVtbl->Flush(g_dx.c);
    log_line("DirectX 11: screen let go for a new mode (device kept)");
}

// create_device(this, adapter, CAWindow&, windowed, width, height, vsync (stack), anti-aliasing (stack)) -> bool. The field
// writes copy what the OpenGL create_device (33d52f0) leaves for the rest of the game: window rect, caps, sizes.
SYSV static uint8_t dx_create_device(uint8_t *dev, uint32_t adapter, uint8_t *win, uint8_t windowed, uint32_t w, uint32_t h,
                                     uint8_t vsync, uint32_t aa) {
    (void)adapter; (void)aa;
    dev[0xec4] = windowed;
    *(uint32_t *)(dev + 0x1610) = w;
    *(uint32_t *)(dev + 0x1614) = h;
    CAWINDOW_DISPLAY_RECT(win, dev + 0xec8);
    void *sdl_window = *(void **)(win + 0x8);
    *(void **)(dev + 0x78) = sdl_window;
    *(uint32_t *)(dev + 0x80) = 0;
    dev[0x1638] = 1;
    g_dx_thread = GetCurrentThreadId();   // the drawing thread
    HWND old_hwnd = g_dx.hwnd;
    g_dx.hwnd = dx_hwnd_of_sdl_window(sdl_window);
    if (!g_dx.hwnd) { log_line("DirectX 11: no window handle from SDL"); return 0; }
    if (g_dx.d) return dx_resize_screen(dev, old_hwnd, w, h, vsync);   // a new screen size (options Apply): keep the device

    UINT flags = getenv("ATTILA_DX11_DEBUG") ? D3D11_CREATE_DEVICE_DEBUG : 0;
    // test: ATTILA_DX11_WARP=1 draws with Windows' own software DirectX 11 (WARP), to check our use of DirectX against a second
    // implementation besides this PC's graphics card
    D3D_DRIVER_TYPE kind = getenv("ATTILA_DX11_WARP") ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
    D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0}, got;
    HRESULT hr = D3D11CreateDevice(NULL, kind, NULL, flags, want, 2, D3D11_SDK_VERSION, &g_dx.d, &got, &g_dx.c);
    if (FAILED(hr)) hr = D3D11CreateDevice(NULL, kind, NULL, flags, want + 1, 1, D3D11_SDK_VERSION, &g_dx.d, &got, &g_dx.c);
    if (FAILED(hr)) { log_line("DirectX 11: D3D11CreateDevice failed 0x%08lx", (unsigned long)hr); return 0; }

    IDXGIDevice *xd = NULL; IDXGIAdapter *xa = NULL; IDXGIFactory2 *xf = NULL;
    g_dx.d->lpVtbl->QueryInterface(g_dx.d, &IID_IDXGIDevice, (void **)&xd);
    if (xd) xd->lpVtbl->GetAdapter(xd, &xa);
    if (xa) xa->lpVtbl->GetParent(xa, &IID_IDXGIFactory2, (void **)&xf);
    if (xa) {
        DXGI_ADAPTER_DESC ad;
        if (SUCCEEDED(xa->lpVtbl->GetDesc(xa, &ad))) {
            *(uint32_t *)(dev + 0x12ec) = ad.DeviceId;
            *(uint32_t *)(dev + 0x12f0) = ad.VendorId;
            // the game's own card brand code (gather_device_info 33e1ef0: amd/ati 0, intel 1, nvidia 2, other 3) and the video
            // memory in bytes, 32-bit (+0x1320, copied to caps +0xe94): the texture streaming budget. 0 here = nothing big ever loads.
            *(uint32_t *)(dev + 0x12f4) = ad.VendorId == 0x1002 ? 0 : ad.VendorId == 0x8086 ? 1 : ad.VendorId == 0x10de ? 2 : 3;
            *(uint32_t *)(dev + 0x1320) = ad.DedicatedVideoMemory > 0xffffffffull ? 0xffffffffu : (uint32_t)ad.DedicatedVideoMemory;
            dev[0xe89] = 1;
            char *desc = (char *)dev + 0xee9;   // driver description text (OpenGL keeps its renderer string here)
            WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, desc, 256, NULL, NULL);
            log_line("DirectX 11: %s, feature level %x, %ux%u %s", desc, (unsigned)got, w, h, windowed ? "windowed" : "full screen");
        }
    }
    DXGI_SWAP_CHAIN_DESC1 sd;
    memset(&sd, 0, sizeof sd);
    sd.Width = w; sd.Height = h;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = xf ? xf->lpVtbl->CreateSwapChainForHwnd(xf, (IUnknown *)g_dx.d, g_dx.hwnd, &sd, NULL, NULL, &g_dx.sc) : E_FAIL;
    if (xf) xf->lpVtbl->MakeWindowAssociation(xf, g_dx.hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (xf) xf->lpVtbl->Release(xf);
    if (xa) xa->lpVtbl->Release(xa);
    if (xd) xd->lpVtbl->Release(xd);
    if (FAILED(hr) || !dx_make_back_buffer_view()) { log_line("DirectX 11: swap chain failed 0x%08lx", (unsigned long)hr); return 0; }
    g_dx.w = w; g_dx.h = h; g_dx.vsync = vsync;
    if (!dx_create_back_buffer(dev, w, h)) { log_line("DirectX 11: back buffer objects failed"); return 0; }
    { D3D11_RECT all = {0, 0, (LONG)w, (LONG)h}; g_dx.c->lpVtbl->RSSetScissorRects(g_dx.c, 1, &all); g_dx_scissor = all; }   // as OpenGL starts: the whole window
    if (FAILED(g_dx.c->lpVtbl->QueryInterface(g_dx.c, &IID_ID3D11DeviceContext1, (void **)&g_dx_c1))) g_dx_c1 = NULL;   // DirectX 11.1: clears of a rectangle
    {   // slot 15: zeros for shader inputs the game's vertex descriptions do not give (see dx_make_layout)
        static const float zeros[16];
        D3D11_BUFFER_DESC bd = {sizeof zeros, D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA sd = {zeros, 0, 0};
        ID3D11Buffer *zb = NULL;
        UINT stride = 0, off = 0;
        if (SUCCEEDED(g_dx.d->lpVtbl->CreateBuffer(g_dx.d, &bd, &sd, &zb))) g_dx.c->lpVtbl->IASetVertexBuffers(g_dx.c, 15, 1, &zb, &stride, &off);
    }

    // Caps, same places and meanings as the OpenGL device fills them (texture units, max size, render targets, feature flags).
    *(uint32_t *)(dev + 0xe8c) = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
    // caps +8: the shader model level the game may use (its own numbers: 0 = SM3, 1 = SM4, 2 = SM4.1, 3 = SM5; OpenGL says
    // 1). The game allows MSAA 2x / 4x / 8x only at level 3 (init_graphics_options_postdevice 3104458), where it also
    // takes its SM4.1 / SM5 shader variants (the "_ms" ones read multisampled textures), sorts particles with compute
    // shaders and stops copying the depth buffer. Chosen in patch_dx11 (default 3).
    *(uint32_t *)(dev + 0xe90) = g_dx_sm;
    *(uint32_t *)(dev + 0xe94) = *(uint32_t *)(dev + 0x1320);
    *(uint32_t *)(dev + 0xe9c) = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    *(uint32_t *)(dev + 0xea0) = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    dev[0xea4] = 0; dev[0xea5] = 0;
    *(uint32_t *)(dev + 0xea8) = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
    // caps +0x24 (device +0xeac): the highest texture filtering level the game may use (0 = trilinear .. 4 = anisotropic
    // 16x). 0 here made cap_to_available cut texture sharpness to trilinear at every start (the game's own option and the
    // start window's both; found 2026-11-13 with ATTILA_GFX_TRACE). DirectX 11 draws 16x on every level 11 card, and the
    // samplers are made from the game's own description (dx_make_sampler), so 4 = the same as Windows Attila.
    *(uint32_t *)(dev + 0xeac) = 4;
    dev[0xeb1] = 1; dev[0xeb2] = 1; dev[0xeb4] = 1; dev[0xeb3] = 0; dev[0xeb5] = 0; dev[0xeb6] = 1; dev[0xeb7] = 0;
    dev[0xeb9] = 0; dev[0xeb8] = 1; dev[0xe88] = 1; *(uint32_t *)(dev + 0xe98) = 0; dev[0xeba] = 1; dev[0xebc] = 0;
    dev[0xebd] = 0; dev[0xe89] = 1; dev[0xebb] = 0; dev[0xec0] = 1;
    // caps +0x36 (device +0xebe): "a copy may change the texture format" (OpenGL lets the driver compress on upload).
    // IMPOSTER_SCENE_NODE::convert (32d5c43) then copies its freshly drawn RGBA sprite page straight into the DXT1 page;
    // DirectX 11 refuses such a copy without a word and the far-away unit sprites stay empty (teal blocks). Off = the
    // game's own way for DirectX: copy to a staging texture, read it, compress on the CPU, copy the DXT1 result.
    dev[0xebe] = 0;
    // caps +0x37 (device +0xebf): "clip space depth runs -1..1" (OpenGL). With it on, set_shared_camera (30c84be, 30c9f41)
    // turns every projection it hands to the shaders into OpenGL's form (z' = 2z - w), while the inverse matrices and the
    // shadow look-up matrices stay in DirectX form. DirectX 11 keeps depth 0..1, so the flag must be off: otherwise every
    // position worked out from the depth buffer lands at half its distance (sliding ground, thin fog, wrong shadows).
    dev[0xebf] = 0;
    // The other flags, as far as the game's code shows: +0x2b half-pixel offset (DirectX 9 only, off), +0x2e depth-only
    // render targets allowed (on), +0x38 depth textures can be copied directly (on; GBUFFER_VIEW::copy_depth_buffer).
    *(uint32_t *)(dev + 0x1680) = 46;   // OpenGL keeps its version here (major * 10 + minor); 4.6 = everything on
    dev[0x1684] = 1;
    return 1;
}

// Test only, ATTILA_DX11_SHOT=n: frame n of the game's back buffer saved as dx11_frame.bmp next to the log (proof of what
// DirectX 11 drew; window captures cannot see flip-model swap chains).
static void dx_save_frame(ID3D11Resource *src) {
    D3D11_TEXTURE2D_DESC d;
    ((ID3D11Texture2D *)src)->lpVtbl->GetDesc((ID3D11Texture2D *)src, &d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D *st = NULL;
    if (FAILED(g_dx.d->lpVtbl->CreateTexture2D(g_dx.d, &d, NULL, &st))) return;
    g_dx.c->lpVtbl->CopyResource(g_dx.c, (ID3D11Resource *)st, src);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_dx.c->lpVtbl->Map(g_dx.c, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        char path[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);   // next to attila64.exe, never in the game folder
        while (n && path[n - 1] != '\\') n--;
        snprintf(path + n, sizeof path - n, "dx11_frame.bmp");
        FILE *f = fopen(path, "wb");
        if (f) {
            uint32_t w = d.Width, h = d.Height, row = w * 3, pad = (4 - row % 4) % 4, size = (row + pad) * h;
            uint8_t hdr[54] = {'B', 'M'};
            *(uint32_t *)(hdr + 2) = 54 + size; *(uint32_t *)(hdr + 10) = 54; *(uint32_t *)(hdr + 14) = 40;
            *(int32_t *)(hdr + 18) = (int32_t)w; *(int32_t *)(hdr + 22) = (int32_t)h; *(uint16_t *)(hdr + 26) = 1; *(uint16_t *)(hdr + 28) = 24;
            *(uint32_t *)(hdr + 34) = size;
            fwrite(hdr, 1, 54, f);
            uint8_t *line = malloc(row + pad + 4);
            for (uint32_t y = h; y-- > 0;) {
                const uint8_t *s = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
                for (uint32_t x = 0; x < w; x++) { line[x * 3] = s[x * 4 + 2]; line[x * 3 + 1] = s[x * 4 + 1]; line[x * 3 + 2] = s[x * 4]; }
                memset(line + row, 0, pad);
                fwrite(line, 1, row + pad, f);
            }
            free(line);
            fclose(f);
            log_line("DirectX 11: frame %llu saved to dx11_frame.bmp", (unsigned long long)g_dx.frames);
        }
        g_dx.c->lpVtbl->Unmap(g_dx.c, (ID3D11Resource *)st, 0);
    }
    st->lpVtbl->Release(st);
}

// Unattended test runs (test_auto.c): a picture of the game's back buffer at a set benchmark second.
static void dx_test_shot(int second, ID3D11Resource *src) {
    D3D11_TEXTURE2D_DESC d;
    ((ID3D11Texture2D *)src)->lpVtbl->GetDesc((ID3D11Texture2D *)src, &d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D *st = NULL;
    if (FAILED(g_dx.d->lpVtbl->CreateTexture2D(g_dx.d, &d, NULL, &st))) return;
    g_dx.c->lpVtbl->CopyResource(g_dx.c, (ID3D11Resource *)st, src);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_dx.c->lpVtbl->Map(g_dx.c, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        test_shot_save(second, m.pData, (int)d.Width, (int)d.Height, (int)m.RowPitch, "rgba", 0);
        g_dx.c->lpVtbl->Unmap(g_dx.c, (ID3D11Resource *)st, 0);
    }
    st->lpVtbl->Release(st);
}

// The frame-rate number on the DirectX 11 route (fps_overlay.c): a small block of pixels put onto the picture that goes
// to the window, not into the game's own back buffer, so the game's screenshots and the test pictures stay clean.
static void dx_fps_overlay(ID3D11Texture2D *target) {
    static uint8_t px[264 * 40 * 4];
    static char last[16];
    static int bw, bh, bx, by;
    const char *text = fps_overlay_tick();
    if (strcmp(text, last) || !bw) {
        int chars = (int)strlen(text), dot;
        fps_overlay_place((int)g_dx.w, (int)g_dx.h, chars, &bx, &by, &dot);
        bw = (6 * chars + 1) * dot; bh = 9 * dot;
        if (bw > 264 || bh > 40) { bw = 0; return; }
        for (int i = 0; i < bw * bh; i++) { px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = 0; px[i * 4 + 3] = 255; }
        for (int ci = 0; ci < chars; ci++) {
            const uint8_t *g = fps_overlay_glyph(text[ci]);
            for (int row = 0; row < 7; row++) for (int col = 0; col < 5; col++) if (g[row] & (0x10 >> col))
                for (int yy = 0; yy < dot; yy++) for (int xx = 0; xx < dot; xx++) {
                    uint8_t *q = px + (((row + 1) * dot + yy) * bw + (1 + ci * 6 + col) * dot + xx) * 4;
                    q[0] = q[1] = q[2] = 255;
                }
        }
        snprintf(last, sizeof last, "%s", text);
    }
    if (!bw || bx + bw > (int)g_dx.w || by + bh > (int)g_dx.h) return;
    D3D11_BOX box = {(UINT)bx, (UINT)by, 0, (UINT)(bx + bw), (UINT)(by + bh), 1};
    g_dx.c->lpVtbl->UpdateSubresource(g_dx.c, (ID3D11Resource *)target, 0, &box, px, (UINT)bw * 4, 0);
}

// Hidden test windows: Windows does not hold a hidden window's frames back (Present comes back at once), so nothing
// stops the game from sending frames faster than the card draws them. The pile-up then shows as freezes of seconds, and
// the frame rate counts frames SENT, not frames drawn (seen 2026-11-12 with MSAA, where the card is the limit). A
// visible window is held to a few frames by Windows itself. So a hidden run lets at most 2 frames be on their way.
// ATTILA_DX11_NO_LIMIT=1 switches this off (the old behaviour).
static void dx_hidden_frame_limit(void) {
    static ID3D11Query *q[3];
    static int on = -1;
    static unsigned n;
    if (on < 0) {
        on = test_hidden() && !getenv("ATTILA_DX11_NO_LIMIT");
        if (on) {
            D3D11_QUERY_DESC d = {D3D11_QUERY_EVENT, 0};
            for (int i = 0; i < 3; i++) if (FAILED(g_dx.d->lpVtbl->CreateQuery(g_dx.d, &d, &q[i]))) on = 0;
            log_line(on ? "DirectX 11: hidden test window: at most 2 frames on their way to the card" : "DirectX 11: no event queries, hidden runs are not held back");
        }
    }
    if (!on) return;
    unsigned cur = n % 3, old = (n + 1) % 3;   // old = the one sent two frames ago (reused next frame)
    g_dx.c->lpVtbl->End(g_dx.c, (ID3D11Asynchronous *)q[cur]);
    g_dx.c->lpVtbl->Flush(g_dx.c);
    if (n >= 2) { BOOL done = FALSE; while (g_dx.c->lpVtbl->GetData(g_dx.c, (ID3D11Asynchronous *)q[old], &done, sizeof done, 0) == S_FALSE) Sleep(0); }
    n++;
}
static void dx_pass_frame_end(void);static void dx_gpu_frame_end(void);
static void dx_q_stats_flush(void);
static void dx_gpu_frame_start(void);
static const char *dx_short_name_v(const void *shader);
#define dx_short_name(s) dx_short_name_v(s)
SYSV static uint8_t dx_present(uint8_t *dev, void *src, void *dst) {
    static int shot_final = -1;   // test: ATTILA_TEST_SHOT_FINAL=1 takes the test pictures from what goes to the window (with the frame-rate number)
    if (shot_final < 0) shot_final = getenv("ATTILA_TEST_SHOT_FINAL") != NULL;
    dx_pass_frame_end();
    int second = test_shot_due();
    {
        uint8_t *bbt = DXF(g_dx.dev, 0x1618, uint8_t *);
        if (second >= 0 && bbt && !shot_final) dx_test_shot(second, DXF(bbt, DXT_RES, ID3D11Resource *));
    }
    {
        static long long shot = -2;
        if (shot == -2) { const char *e = getenv("ATTILA_DX11_SHOT"); shot = e ? atoll(e) : -1; }
        uint8_t *bbt = DXF(g_dx.dev, 0x1618, uint8_t *);
        if (shot >= 0 && (long long)g_dx.frames == shot && bbt) dx_save_frame(DXF(bbt, DXT_RES, ID3D11Resource *));
    }
    (void)dev; (void)src; (void)dst;
    if (!g_dx.sc) return 0;
    uint8_t *bb = DXF(g_dx.dev, 0x1618, uint8_t *);   // the game drew into its back buffer texture: copy it to the window
    ID3D11Texture2D *out = NULL;
    if (bb && SUCCEEDED(g_dx.sc->lpVtbl->GetBuffer(g_dx.sc, 0, &IID_ID3D11Texture2D, (void **)&out))) {
        g_dx.c->lpVtbl->CopyResource(g_dx.c, (ID3D11Resource *)out, DXF(bb, DXT_RES, ID3D11Resource *));
        if (fps_overlay_on()) dx_fps_overlay(out);
        if (menu_button_visible()) menu_button_dx11(g_dx.c, out, (int)g_dx.w, (int)g_dx.h);   // the Discord button (menu_button.c)
        if (second >= 0 && shot_final) dx_test_shot(second, (ID3D11Resource *)out);
        out->lpVtbl->Release(out);
    }
    dx_gpu_frame_end();
    double t_present = dx_now_ms();
    // test: ATTILA_DX11_NO_PRESENT=1 hands nothing to Windows (the frame is only sent to the card). For telling apart
    // what the hidden test window's hand-over costs from what the drawing itself costs.
    static int no_present = -1;
    if (no_present < 0) { no_present = getenv("ATTILA_DX11_NO_PRESENT") != NULL; if (no_present) log_line("DirectX 11: frames are not handed to Windows (test)"); }
    HRESULT hr = S_OK;
    if (no_present) g_dx.c->lpVtbl->Flush(g_dx.c);
    else hr = g_dx.sc->lpVtbl->Present(g_dx.sc, g_dx.vsync ? 1 : 0, 0);
    dx_hidden_frame_limit();
    double present_ms = dx_now_ms() - t_present;
    dx_gpu_frame_start();
    {   // a frame that took very long: say what our side did in it (shaders and textures made, waits), for hitch hunting
        static double last; static int told;
        double now = dx_now_ms(), frame = last ? now - last : 0;
        last = now;
        if (frame > 300 && g_dx.frames > 300 && told++ < 60)
            log_line("DirectX 11: slow frame %.0f ms: %u draws, %u shaders made (%.0f ms), %u textures made (%.0f ms), %u views made, texture maps %.0f ms, uploads %.0f ms, %u compute runs, present %.0f ms",
                     frame, g_dxs.draws, g_dxs.shaders, g_dxs.shader_ms, g_dxs.textures, g_dxs.texture_ms, g_dxs.views, g_dxs.map_ms, g_dxs.update_ms, g_dxs.dispatches, present_ms);
        if (g_dx_cpu_on && frame > 300 && g_dx.frames > 300 && told < 61)
            log_line("   time inside our calls: draws %.0f ms, target changes %.0f, clears %.0f, copies %.0f, query answers %.0f, compute %.0f, texture maps %.0f; longest single call %.0f ms: %s (%s + %s)",
                     g_dxcpu.draw, g_dxcpu.targets, g_dxcpu.clear, g_dxcpu.copy, g_dxcpu.query, g_dxcpu.compute, g_dxcpu.map, g_dxcpu.worst, g_dxcpu.worst_what ? g_dxcpu.worst_what : "-",
                     dx_short_name(g_dxcpu.worst_vs), dx_short_name(g_dxcpu.worst_ps));
        memset(&g_dxcpu, 0, sizeof g_dxcpu);
    }
    g_dx_thread = GetCurrentThreadId();
    if ((g_dx.frames & 255) == 0 && (g_dx_other_thread[0] | g_dx_other_thread[1] | g_dx_other_thread[2] | g_dx_other_thread[3])) {
        static int told;
        if (told++ < 40)
            log_line("DirectX 11: calls from other threads than the drawing thread in the last 256 frames: buffer map %ld, buffer unmap %ld, texture %ld, compute buffer %ld",
                     InterlockedExchange(&g_dx_other_thread[0], 0), InterlockedExchange(&g_dx_other_thread[1], 0), InterlockedExchange(&g_dx_other_thread[2], 0), InterlockedExchange(&g_dx_other_thread[3], 0));
    }
    dx_depth_frame_end(g_dxs.draws);
    dx_q_stats_flush();
    test_note_draws(g_dxs.draws);
    { extern SYSV void sh_SDL_GL_SwapWindow(void *); sh_SDL_GL_SwapWindow(DXF(g_dx.dev, 0x78, void *)); }   // frame count + cap only
    if (g_dx.frames < 400 && g_dx.frames % 50 == 0) log_line("DirectX 11: frame %llu: %u draws (%u to back buffer), no vs %u, no ps %u, no layout %u", (unsigned long long)g_dx.frames, g_dxs.draws, g_dxs.to_bb, g_dxs.no_vs, g_dxs.no_ps, g_dxs.no_layout);
    memset(&g_dxs, 0, sizeof g_dxs);
    if (++g_dx.frames == 1 || FAILED(hr)) log_line("DirectX 11: present #%llu: 0x%08lx", (unsigned long long)g_dx.frames, (unsigned long)hr);
    return SUCCEEDED(hr);
}
SYSV static uint8_t dx_end_scene(uint8_t *dev) { (void)dev; return 1; }

// ---- texture formats: the game's TEXTURE_TEXEL_FORMAT (names in its table at 4c02fa0, e.g. 0x07 TTF_A8R8G8B8, 0x0f TTF_DXT1,
// 0x1c TTF_D24S8, 0x2b TTF_R8G8B8A8) -> DXGI. Depth formats get a typeless resource so they can be read by shaders too.
typedef struct { DXGI_FORMAT res, srv, rtv, dsv; } DxFmt;
static DxFmt dx_format(uint32_t ttf) {
    DxFmt f = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT x = DXGI_FORMAT_UNKNOWN;
    switch (ttf) {
    case 0x01: x = DXGI_FORMAT_A8_UNORM; break;
    case 0x02: case 0x03: case 0x30: x = DXGI_FORMAT_R8_UNORM; break;
    case 0x06: x = DXGI_FORMAT_B5G6R5_UNORM; break;
    case 0x07: case 0x35: x = DXGI_FORMAT_B8G8R8A8_UNORM; break;
    case 0x08: case 0x2b: x = DXGI_FORMAT_R8G8B8A8_UNORM; break;
    case 0x09: x = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; break;
    case 0x0a: x = DXGI_FORMAT_B8G8R8X8_UNORM; break;
    case 0x0b: x = DXGI_FORMAT_B8G8R8X8_UNORM_SRGB; break;
    case 0x0c: x = DXGI_FORMAT_R16G16_UNORM; break;
    case 0x0d: x = DXGI_FORMAT_R16_FLOAT; break;
    case 0x0e: case 0x34: x = DXGI_FORMAT_R32_FLOAT; break;
    case 0x0f: x = DXGI_FORMAT_BC1_UNORM; break;
    case 0x10: x = DXGI_FORMAT_BC2_UNORM; break;
    case 0x11: x = DXGI_FORMAT_BC3_UNORM; break;
    case 0x12: x = DXGI_FORMAT_BC1_UNORM_SRGB; break;
    case 0x13: x = DXGI_FORMAT_BC2_UNORM_SRGB; break;
    case 0x14: x = DXGI_FORMAT_BC3_UNORM_SRGB; break;
    case 0x15: case 0x16: x = DXGI_FORMAT_R16G16B16A16_UNORM; break;
    case 0x17: case 0x18: x = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
    case 0x19: case 0x1a: x = DXGI_FORMAT_R32G32B32A32_FLOAT; break;
    case 0x1b: x = DXGI_FORMAT_R32G32B32A32_UINT; break;
    case 0x2a: x = DXGI_FORMAT_R16_UINT; break;
    case 0x2c: x = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; break;
    case 0x2d: case 0x33: x = DXGI_FORMAT_R16_UNORM; break;
    case 0x2e: x = DXGI_FORMAT_R11G11B10_FLOAT; break;
    case 0x2f: x = DXGI_FORMAT_R8G8_UNORM; break;
    case 0x31: x = DXGI_FORMAT_R16G16_FLOAT; break;
    case 0x37: f.res = DXGI_FORMAT_R8G8B8A8_TYPELESS; f.srv = f.rtv = DXGI_FORMAT_R8G8B8A8_UNORM; return f;
    case 0x38: x = DXGI_FORMAT_R32_UINT; break;
    case 0x39: x = DXGI_FORMAT_R8_UINT; break;
    case 0x3a: x = DXGI_FORMAT_R8G8B8A8_SNORM; break;
    case 0x3b: x = DXGI_FORMAT_R32G32_FLOAT; break;
    case 0x3d: x = DXGI_FORMAT_R16G16B16A16_UINT; break;
    case 0x3e: x = DXGI_FORMAT_R32G32_UINT; break;
    // Typeless textures: one texture, read by shaders through one format and drawn into through another.
    case 0x25: f.res = DXGI_FORMAT_R32_TYPELESS; f.srv = f.rtv = DXGI_FORMAT_R32_FLOAT; f.dsv = DXGI_FORMAT_D32_FLOAT; return f;
    case 0x26: f.res = DXGI_FORMAT_R16_TYPELESS; f.srv = f.rtv = DXGI_FORMAT_R16_UNORM; f.dsv = DXGI_FORMAT_D16_UNORM; return f;
    case 0x28: f.res = DXGI_FORMAT_R24G8_TYPELESS; f.srv = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; f.dsv = DXGI_FORMAT_D24_UNORM_S8_UINT; return f;
    case 0x29: f.res = DXGI_FORMAT_R24G8_TYPELESS; f.srv = DXGI_FORMAT_X24_TYPELESS_G8_UINT; f.dsv = DXGI_FORMAT_D24_UNORM_S8_UINT; return f;
    case 0x1c: case 0x20: case 0x21: case 0x22: case 0x23: case 0x27:   // D24S8, RAWZ, INTZ, DF24, D24X8, R24G8_TYPELESS
        f.res = DXGI_FORMAT_R24G8_TYPELESS; f.srv = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; f.dsv = DXGI_FORMAT_D24_UNORM_S8_UINT; return f;
    case 0x1d: f.res = DXGI_FORMAT_R16_TYPELESS; f.srv = DXGI_FORMAT_R16_UNORM; f.dsv = DXGI_FORMAT_D16_UNORM; return f;
    case 0x1e: case 0x24: f.res = DXGI_FORMAT_R32_TYPELESS; f.srv = DXGI_FORMAT_R32_FLOAT; f.dsv = DXGI_FORMAT_D32_FLOAT; return f;
    default: break;   // 0x04/0x05/0x32/0x36 (24-bit, no DXGI twin), 0x1f NULL, 0x3c: not done
    }
    f.res = f.srv = f.rtv = x;
    return f;
}

// ---- textures. Our object = the OpenGL texture's first 0x68 bytes (same fields, the game reads them) + ours.
// +0x08 dimension (3 = 2D, like D3D11_RESOURCE_DIMENSION), +0x0c levels, +0x10 TTF format, +0x14 width, +0x18 height,
// +0x1c depth/array, +0x20 device, +0x28 misc, +0x2c usage, +0x30 cpu access, +0x34 created.
typedef SYSV void (*GameFreeFn)(const void *);
#define GAME_FREE ((GameFreeFn)0x525fd0)
static const uint64_t *const GL_TEX2D_VTBL = (const uint64_t *)0x3c53700;
enum { DX_TEX_SLOTS = 0xb0 / 8 };
static uint64_t g_dx_tex_vtbl[DX_TEX_SLOTS];
#define DXF(obj, off, T) (*(T *)((uint8_t *)(obj) + (off)))

static void dx_tex_release(uint8_t *t) {
    DXF(t, DXT_SRV, void *) = DXF(t, DXT_SRV4, void *) = DXF(t, DXT_SRV5, void *) = NULL;   // quick views (owned by the list below)
    for (int i = 0; i < 8; i++) { uint64_t *v = (uint64_t *)(t + DXT_VIEWS) + i * 2; if (v[1]) ((IUnknown *)v[1])->lpVtbl->Release((IUnknown *)v[1]); v[0] = v[1] = 0; }
    DXREL(DXF(t, DXT_RES, ID3D11Resource *));
}
SYSV static void dx_tex_dtor(uint8_t *t) { dx_tex_release(t); }
SYSV static void dx_tex_delete(uint8_t *t) { dx_tex_release(t); GAME_FREE(t); }

// test: ATTILA_DX11_CUBE_LOG=1 writes down how cube textures are made and filled (the sky reflection map), first 60 lines.
static int dx_cube_log(const uint8_t *t) {
    static int on = -1, lines;
    if (on < 0) on = getenv("ATTILA_DX11_CUBE_LOG") != NULL;
    return on && t && *(uint64_t **)t == g_dx_tex_vtbl && DXF(t, 0x28, uint32_t) && lines++ < 60;
}
// create_texture_2D(dev, w, h, levels, format, aa, misc, pool, usage, bind, cpu, cube, b2, bool &failed, b3): usage and bind
// use DirectX 11's own numbers (staging = 3, render target | shader resource = 0x28).
SYSV static void *dx_create_texture_2d(uint8_t *dev, uint32_t w, uint32_t h, uint32_t levels, uint32_t fmt, uint32_t aa,
                                       uint32_t misc, uint32_t pool, uint32_t usage, uint32_t bind, uint32_t cpu, uint8_t b2,
                                       uint8_t cube, uint8_t *failed, uint8_t b3)   /* cube = 2nd flag (OpenGL reads [rsp+0xc0]) */ {
    (void)aa; (void)misc; (void)pool; (void)b2; (void)b3;
    DxFmt f = dx_format(fmt);
    if (failed) *failed = 0;
    if (!w || !h || !fmt) {   // nothing to make yet (seen on the campaign map: 0x0, format 0). OpenGL's version (33d7d30) hands back an
        // empty texture object and reports success; so do we: an object without a DirectX texture behind it.
        uint8_t *e = GAME_MALLOC(DXT_SIZE, 0);
        if (!e) { if (failed) *failed = 1; return NULL; }
        memset(e, 0, DXT_SIZE);
        *(uint64_t **)e = g_dx_tex_vtbl;
        DXF(e, 0x08, uint32_t) = 3; DXF(e, 0x0c, uint32_t) = levels; DXF(e, 0x10, uint32_t) = fmt; DXF(e, 0x14, uint32_t) = w; DXF(e, 0x18, uint32_t) = h;
        DXF(e, 0x1c, uint32_t) = 1; DXF(e, 0x20, uint8_t *) = dev; DXF(e, 0x28, uint32_t) = (uint32_t)cube * 2; DXF(e, 0x2c, uint32_t) = usage;
        DXF(e, 0x30, uint32_t) = cpu; DXF(e, 0x34, uint8_t) = 1;
        { static int told; if (told++ < 3) log_line("DirectX 11: empty texture object made (%ux%u, format 0x%x), as OpenGL does", w, h, fmt); }
        return e;
    }
    if (f.res == DXGI_FORMAT_UNKNOWN) {
        log_line("DirectX 11: texture format 0x%x not done (%ux%u)", fmt, w, h);
        if (failed) *failed = 1;
        return NULL;
    }
    // 0 levels = the whole chain down to 1x1. OpenGL's version (33d7d30) works the number out (log2 of the larger side + 1)
    // and keeps it at +0x0c, and the game reads it back from there: TEXTURE_MANAGER_ENTRY::fill_texture_in_stages (34421f0)
    // uploads a texture from its file level by level up to that count. With 0 kept there it uploaded nothing, so the sky
    // reflection map (a 1024x1024 cube made this way) stayed black on DirectX 11: a dark sea with glitter and no shallow
    // colour, and no sky light in the reflections (tester report, found 2026-11-14).
    if (!levels) { uint32_t m = w > h ? w : h; levels = 1; while (m >>= 1) levels++; }
    D3D11_TEXTURE2D_DESC d;
    memset(&d, 0, sizeof d);
    d.Width = w; d.Height = h; d.MipLevels = levels; d.ArraySize = cube ? 6 : g_dx_next_array; d.Format = f.res; d.SampleDesc.Count = 1;
    if (f.res >= DXGI_FORMAT_BC1_TYPELESS && f.res <= DXGI_FORMAT_BC3_UNORM_SRGB) { d.Width = (w + 3) & ~3u; d.Height = (h + 3) & ~3u; }   // DirectX 11: block-compressed sizes in multiples of 4
    d.Usage = usage <= 3 ? (D3D11_USAGE)usage : D3D11_USAGE_DEFAULT;
    if (d.Usage != D3D11_USAGE_STAGING) {
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (bind & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_DEPTH_STENCIL));
        // a depth format the game gave no "render target" wish for is a depth buffer (the OpenGL-style calls say nothing)
        if (f.dsv != DXGI_FORMAT_UNKNOWN && !(bind & D3D11_BIND_RENDER_TARGET)) d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
        if (d.BindFlags & D3D11_BIND_DEPTH_STENCIL) d.BindFlags &= ~(UINT)(D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS);
    }
    if (d.Usage == D3D11_USAGE_DYNAMIC) d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (d.Usage == D3D11_USAGE_STAGING) d.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
    if (cube) d.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;
    if (levels != 1 && (d.BindFlags & D3D11_BIND_RENDER_TARGET)) d.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
    // Multisampling: the game's WS_ANTI_ALIASING 2 / 3 / 4 = 2 / 4 / 8 samples a pixel (0 none, 1 MLAA: both plain textures).
    UINT samples = aa >= 2 && aa <= 4 ? 1u << (aa - 1) : 1;
    if (samples > 1 && !cube && d.ArraySize == 1 && (d.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL))) {
        d.SampleDesc.Count = samples; d.MipLevels = 1; d.MiscFlags &= ~(UINT)D3D11_RESOURCE_MISC_GENERATE_MIPS;
    } else samples = 1;
    if (d.Usage == D3D11_USAGE_IMMUTABLE) d.Usage = D3D11_USAGE_DEFAULT;   // no data at creation here; filled later by update/map
    ID3D11Texture2D *tex = NULL;
    double t_made = dx_now_ms();
    HRESULT hr = g_dx.d->lpVtbl->CreateTexture2D(g_dx.d, &d, NULL, &tex);
    g_dxs.textures++; g_dxs.texture_ms += dx_now_ms() - t_made;
    if (FAILED(hr)) {
        log_line("DirectX 11: CreateTexture2D %ux%u (made %ux%u) fmt 0x%x levels %u array %u usage %u bind 0x%x/%x cpu %x misc %x failed 0x%08lx", w, h, d.Width, d.Height, fmt, d.MipLevels, d.ArraySize, usage, bind, d.BindFlags, d.CPUAccessFlags, d.MiscFlags, (unsigned long)hr);
        if (failed) *failed = 1;
        return NULL;
    }
    uint8_t *t = GAME_MALLOC(DXT_SIZE, 0);
    memset(t, 0, DXT_SIZE);
    *(uint64_t **)t = g_dx_tex_vtbl;
    DXF(t, 0x08, uint32_t) = 3;
    DXF(t, 0x0c, uint32_t) = levels;
    DXF(t, 0x10, uint32_t) = fmt;
    DXF(t, 0x14, uint32_t) = w;
    DXF(t, 0x18, uint32_t) = h;
    DXF(t, 0x1c, uint32_t) = cube ? 1 : g_dx_next_array;
    DXF(t, 0x20, uint8_t *) = dev;
    DXF(t, 0x28, uint32_t) = (uint32_t)cube * 2;
    DXF(t, 0x2c, uint32_t) = usage;
    DXF(t, 0x30, uint32_t) = cpu;
    DXF(t, 0x34, uint8_t) = 1;
    DXF(t, DXT_RES, ID3D11Resource *) = (ID3D11Resource *)tex;
    DXF(t, DXT_BIND, uint32_t) = d.BindFlags;
    DXF(t, DXT_SAMPLES, uint32_t) = samples;
    if (samples > 1) { static int told; if (told++ < 6) log_line("DirectX 11: multisampled texture made: %ux%u, format 0x%x, %u samples", w, h, fmt, samples); }
    if (dx_cube_log(t)) log_line("cube log: made %p %ux%u format 0x%x, %u levels, usage %u, bind 0x%x (DirectX bind 0x%x), cpu %u, misc %u, pool %u, b2 %u, b3 %u, asked from %p", (void *)t, w, h, fmt, levels, usage, bind, d.BindFlags, cpu, misc, pool, b2, b3, __builtin_return_address(0));
    return t;
}

// Bytes per row of one mip level (block rows for BC formats) and number of rows.
static void dx_pitch(DXGI_FORMAT f, uint32_t w, uint32_t h, uint32_t *row, uint32_t *rows) {
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    switch (f) {
    case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: *row = bw * 8; *rows = bh; return;
    case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
        *row = bw * 16; *rows = bh; return;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT: *row = w * 16; break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT: *row = w * 8; break;
    case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_A8_UNORM: case DXGI_FORMAT_R8_UINT: *row = w; break;
    case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_B5G6R5_UNORM: *row = w * 2; break;
    default: *row = w * 4; break;
    }
    *rows = h;
}

// create_texture_from_memory_2D(dev, data, size, skip, IMAGE_INFO&, u3 | stack: u4, u5, u6, filter, format, filter, pool,
// usage, bind, cpu, b1, cube, bool &failed, TTF &fmt_out, uint &w_out, uint &h_out). Sizes and the first mip come from the
// game's own TEXTURE_MANAGER::calculate_texture_details (3121980), level data like the OpenGL upload (33d9210):
// level entry = info+0x20 [face * levels_total + level] (16 bytes, +8 -> desc, desc+8 = offset), data = base + info+0x2c +
// offset - skip. Made as one immutable texture with all levels at once.
typedef SYSV void (*CalcDetailsFn)(uint32_t, uint32_t, const void *, uint32_t, uint32_t, uint32_t *, uint32_t *, uint32_t *, uint32_t *);
#define CALC_TEXTURE_DETAILS ((CalcDetailsFn)0x3121980)
SYSV static void *dx_create_texture_from_memory_2d(uint8_t *dev, const uint8_t *data, uint32_t size, uint32_t skip, const uint8_t *info,
        uint32_t u3, uint32_t u4, uint32_t u5, uint32_t u6, uint32_t filter1, uint32_t fmt, uint32_t filter2, uint32_t pool,
        uint32_t usage, uint32_t bind, uint32_t cpu, uint8_t b1, uint8_t cube, uint8_t *failed, uint32_t *fmt_out,
        uint32_t *w_out, uint32_t *h_out) {
    (void)size; (void)filter1; (void)filter2; (void)pool; (void)b1;
    uint32_t levels_more = 0, first = 0;
    if (failed) *failed = 0;
    CALC_TEXTURE_DETAILS(u3, u4, info, DXF(dev, 0xe9c, uint32_t), DXF(dev, 0xea0, uint32_t), &levels_more, &first, w_out, h_out);
    uint32_t levels = u6 ? u6 : u4 + levels_more - u5;
    uint32_t w = *w_out, h = *h_out, faces = cube ? 6 : 1;
    uint32_t levels_total = levels_more;   // entries per face in the image's level table
    DxFmt f = dx_format(fmt);
    if (fmt_out) *fmt_out = fmt;
    if (f.res == DXGI_FORMAT_UNKNOWN || !levels || levels > 16 || !w || !h) {
        log_line("DirectX 11: texture from memory not made: format 0x%x, %ux%u, %u levels (first %u, table %u, u3-u6 %u %u %u %u)",
                 fmt, w, h, levels, first, levels_more, u3, u4, u5, u6);
        if (failed) *failed = 1;
        return NULL;
    }
    D3D11_SUBRESOURCE_DATA sd[6 * 16];
    const uint8_t *table = *(const uint8_t *const *)(info + 0x20);
    for (uint32_t face = 0; face < faces; face++)
        for (uint32_t l = 0; l < levels; l++) {
            uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1, row, rows;
            const uint8_t *desc = *(const uint8_t *const *)(table + (size_t)(face * levels_total + first + l) * 16 + 8);
            dx_pitch(f.res, lw, lh, &row, &rows);
            sd[face * levels + l].pSysMem = data + *(const uint32_t *)(info + 0x2c) + *(const uint32_t *)(desc + 8) - skip;
            sd[face * levels + l].SysMemPitch = row;
            sd[face * levels + l].SysMemSlicePitch = row * rows;
        }
    D3D11_TEXTURE2D_DESC d;
    memset(&d, 0, sizeof d);
    d.Width = w; d.Height = h; d.MipLevels = levels; d.ArraySize = faces;
    if (f.res >= DXGI_FORMAT_BC1_TYPELESS && f.res <= DXGI_FORMAT_BC3_UNORM_SRGB) { d.Width = (w + 3) & ~3u; d.Height = (h + 3) & ~3u; } d.Format = f.res; d.SampleDesc.Count = 1;
    d.Usage = usage == 2 ? D3D11_USAGE_DYNAMIC : usage == 3 ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
    d.BindFlags = d.Usage == D3D11_USAGE_STAGING ? 0 : D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = d.Usage == D3D11_USAGE_DYNAMIC ? D3D11_CPU_ACCESS_WRITE : d.Usage == D3D11_USAGE_STAGING ? D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE : 0;
    d.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    ID3D11Texture2D *tex = NULL;
    HRESULT hr = g_dx.d->lpVtbl->CreateTexture2D(g_dx.d, &d, sd, &tex);
    if (FAILED(hr)) {
        log_line("DirectX 11: texture from memory %ux%u fmt 0x%x levels %u failed 0x%08lx", w, h, fmt, levels, (unsigned long)hr);
        if (failed) *failed = 1;
        return NULL;
    }
    uint8_t *t = GAME_MALLOC(DXT_SIZE, 0);
    memset(t, 0, DXT_SIZE);
    *(uint64_t **)t = g_dx_tex_vtbl;
    DXF(t, 0x08, uint32_t) = 3; DXF(t, 0x0c, uint32_t) = levels; DXF(t, 0x10, uint32_t) = fmt;
    DXF(t, 0x14, uint32_t) = w; DXF(t, 0x18, uint32_t) = h; DXF(t, 0x1c, uint32_t) = 1; DXF(t, 0x20, uint8_t *) = dev;
    DXF(t, 0x28, uint32_t) = (uint32_t)cube * 2; DXF(t, 0x2c, uint32_t) = usage; DXF(t, 0x30, uint32_t) = cpu; DXF(t, 0x34, uint8_t) = 1;
    DXF(t, DXT_RES, ID3D11Resource *) = (ID3D11Resource *)tex;
    DXF(t, DXT_BIND, uint32_t) = d.BindFlags; (void)bind;
    if (dx_cube_log(t)) log_line("cube log: made from memory %p %ux%u format 0x%x, %u levels (first %u, table %u), usage %u, face 0 starts %08x %08x, face 1 starts %08x %08x", (void *)t, w, h, fmt, levels, first, levels_total, usage,
                                 ((const uint32_t *)sd[0].pSysMem)[0], ((const uint32_t *)sd[0].pSysMem)[1], ((const uint32_t *)sd[levels].pSysMem)[0], ((const uint32_t *)sd[levels].pSysMem)[1]);
    return t;
}

// ---- vertex / index buffers. Our object = OpenGL's layout (vertex 0x48, index 0x50) + ours at 0x50:
// +0x08 1, +0x0c byte size, +0x10 usage (DirectX 11 numbers), +0x14 cpu access, +0x18 device, +0x30 first data (may be 0),
// index buffer +0x48 index format. Map flags are DirectX 11's (1 read, 2 write, 3 read/write, 4 discard, 5 no overwrite).
enum { DXB_BUF = 0x50, DXB_SHADOW = 0x58, DXB_MAP_OFF = 0x60, DXB_MAP_SIZE = 0x64, DXB_MAP_KIND = 0x68, DXB_SIZE = 0x70 };
static const uint64_t *const GL_VB_VTBL = (const uint64_t *)0x3c86530, *const GL_IB_VTBL = (const uint64_t *)0x3c863d0;
static uint64_t g_dx_vb_vtbl[6], g_dx_ib_vtbl[6];

static ID3D11Buffer *dx_buf_get(uint8_t *b) {   // made on first use (the game may or may not call create_resource first)
    ID3D11Buffer *buf = DXF(b, DXB_BUF, ID3D11Buffer *);
    if (buf || !g_dx.d) return buf;
    int is_index = *(uint64_t **)b == g_dx_ib_vtbl;
    uint32_t size = DXF(b, 0x0c, uint32_t), usage = DXF(b, 0x10, uint32_t);
    void *data = DXF(b, 0x30, void *);
    D3D11_BUFFER_DESC d;
    memset(&d, 0, sizeof d);
    d.ByteWidth = (size + 15) & ~15u;
    d.BindFlags = is_index ? D3D11_BIND_INDEX_BUFFER : D3D11_BIND_VERTEX_BUFFER;
    if (usage == 1 && data) d.Usage = D3D11_USAGE_IMMUTABLE;
    else if (usage == 2) { d.Usage = D3D11_USAGE_DYNAMIC; d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE; }
    else d.Usage = D3D11_USAGE_DEFAULT;
    void *init = data;
    if (data && d.ByteWidth != size) { init = calloc(1, d.ByteWidth); if (init) memcpy(init, data, size); }
    D3D11_SUBRESOURCE_DATA sd = {init, 0, 0};
    HRESULT hr = g_dx.d->lpVtbl->CreateBuffer(g_dx.d, &d, init ? &sd : NULL, &buf);
    if (init != data) free(init);
    if (FAILED(hr)) { log_line("DirectX 11: %s buffer of %u bytes (usage %u) failed 0x%08lx", is_index ? "index" : "vertex", size, usage, (unsigned long)hr); return NULL; }
    DXF(b, DXB_BUF, ID3D11Buffer *) = buf;
    return buf;
}
SYSV static uint8_t dx_buf_create_resource(uint8_t *b) { return dx_buf_get(b) != NULL; }
SYSV static void dx_buf_destroy_resource(uint8_t *b) {
    DXREL(DXF(b, DXB_BUF, ID3D11Buffer *));
    free(DXF(b, DXB_SHADOW, void *)); DXF(b, DXB_SHADOW, void *) = NULL;
}
SYSV static void dx_buf_dtor(uint8_t *b) { dx_buf_destroy_resource(b); }
SYSV static void dx_buf_delete(uint8_t *b) { dx_buf_destroy_resource(b); GAME_FREE(b); }

// map(this, context, offset, size, flags, void **out, wait). Dynamic buffers map for real (discard / no overwrite); every
// other case writes into a CPU copy that unmap sends with UpdateSubresource.
SYSV static uint8_t dx_buf_map(uint8_t *b, void *ctx, uint32_t off, uint32_t size, uint32_t flags, void **out, uint32_t wait) {
    (void)ctx; (void)wait;
    dx_thread_note(0);
    ID3D11Buffer *buf = dx_buf_get(b);
    uint32_t total = DXF(b, 0x0c, uint32_t);
    if (!buf || !out) return 0;
    if (!size || off + size > total) size = total - off;

    DXF(b, DXB_MAP_OFF, uint32_t) = off; DXF(b, DXB_MAP_SIZE, uint32_t) = size;
    if (DXF(b, 0x10, uint32_t) == 2 && flags != 1 && flags != 3) {
        D3D11_MAPPED_SUBRESOURCE m;
        D3D11_MAP kind = flags == 4 ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE;
        HRESULT hr = g_dx.c->lpVtbl->Map(g_dx.c, (ID3D11Resource *)buf, 0, kind, 0, &m);
        if (FAILED(hr)) { log_line("DirectX 11: buffer map failed 0x%08lx", (unsigned long)hr); return 0; }
        DXF(b, DXB_MAP_KIND, uint32_t) = 1;
        *out = (uint8_t *)m.pData + off;
        return 1;
    }
    uint8_t *sh = DXF(b, DXB_SHADOW, uint8_t *);
    if (!sh) { sh = calloc(1, total ? total : 16); DXF(b, DXB_SHADOW, uint8_t *) = sh; }
    if (!sh) return 0;
    DXF(b, DXB_MAP_KIND, uint32_t) = 2;
    *out = sh + off;
    return 1;
}
SYSV static void dx_buf_unmap(uint8_t *b, void *ctx) {
    (void)ctx;
    dx_thread_note(1);
    ID3D11Buffer *buf = DXF(b, DXB_BUF, ID3D11Buffer *);
    uint32_t kind = DXF(b, DXB_MAP_KIND, uint32_t);
    DXF(b, DXB_MAP_KIND, uint32_t) = 0;
    if (!buf) return;
    if (kind == 1) { g_dx.c->lpVtbl->Unmap(g_dx.c, (ID3D11Resource *)buf, 0); return; }
    if (kind == 2) {
        uint32_t off = DXF(b, DXB_MAP_OFF, uint32_t), size = DXF(b, DXB_MAP_SIZE, uint32_t);
        D3D11_BOX box = {off, 0, 0, off + size, 1, 1};
        if (DXF(b, 0x10, uint32_t) == 2) {   // dynamic buffer read back or partly rewritten: send it all again
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(g_dx.c->lpVtbl->Map(g_dx.c, (ID3D11Resource *)buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                memcpy(m.pData, DXF(b, DXB_SHADOW, uint8_t *), DXF(b, 0x0c, uint32_t));
                g_dx.c->lpVtbl->Unmap(g_dx.c, (ID3D11Resource *)buf, 0);
            }
        } else if (DXF(b, 0x10, uint32_t) != 1) {
            g_dx.c->lpVtbl->UpdateSubresource(g_dx.c, (ID3D11Resource *)buf, 0, &box, DXF(b, DXB_SHADOW, uint8_t *) + off, 0, 0);
        }
    }
}
static uint8_t *dx_new_buffer(uint64_t *vtbl, uint32_t size, uint32_t usage, uint32_t cpu, void *data) {
    uint8_t *b = GAME_MALLOC(DXB_SIZE, 0);
    if (!b) return NULL;
    memset(b, 0, DXB_SIZE);
    *(uint64_t **)b = vtbl;
    DXF(b, 0x08, uint32_t) = 1;
    DXF(b, 0x0c, uint32_t) = size;
    DXF(b, 0x10, uint32_t) = usage;
    DXF(b, 0x14, uint32_t) = cpu;
    DXF(b, 0x18, void *) = g_dx.dev;
    DXF(b, 0x30, void *) = data;
    return b;
}
SYSV static void *dx_create_vertex_buffer(uint8_t *dev, uint32_t size, uint32_t usage, uint32_t cpu, void *data) {
    (void)dev;
    return dx_new_buffer(g_dx_vb_vtbl, size, usage, cpu, data);
}
SYSV static void *dx_create_index_buffer(uint8_t *dev, uint32_t size, uint32_t fmt, uint32_t usage, uint32_t cpu, void *data) {
    (void)dev;
    uint8_t *b = dx_new_buffer(g_dx_ib_vtbl, size, usage, cpu, data);
    if (b) DXF(b, 0x48, uint32_t) = fmt;
    return b;
}
static void dx_build_buffer_tables(void) {
    uint64_t *t[2] = {g_dx_vb_vtbl, g_dx_ib_vtbl};
    for (int i = 0; i < 2; i++) {
        t[i][0] = (uint64_t)dx_buf_dtor; t[i][1] = (uint64_t)dx_buf_delete;
        t[i][2] = (uint64_t)dx_buf_create_resource; t[i][3] = (uint64_t)dx_buf_destroy_resource;
        t[i][4] = (uint64_t)dx_buf_map; t[i][5] = (uint64_t)dx_buf_unmap;
    }
    (void)GL_VB_VTBL; (void)GL_IB_VTBL;
}

// ---- constant buffers. Our object = OpenGL's (0x40) + ID3D11Buffer at 0x40. +0x18 device, +0x20 SHADER_CONSTANT_BUFFER
// (game side, +0x38 = change counter), +0x28 CPU data the game's constants write straight into, +0x34 made (OpenGL: -1 =
// not yet), +0x38 byte size, +0x3c change counter last sent. Size and layout come from the game's own
// OPENGL_CONSTANT_BUFFER::calc_buffer_size (33dc9b0, no OpenGL in it): HLSL cbuffer packing, so the DXBC shaders match.
enum { DXC_BUF = 0x40, DXC_SIZE = 0x48 };
typedef SYSV uint32_t (*CalcBufferSizeFn)(uint8_t *, uint8_t);
typedef SYSV void *(*MallocAlignedFn)(size_t, uint32_t, int);
#define CALC_BUFFER_SIZE ((CalcBufferSizeFn)0x33dc9b0)
#define GAME_MALLOC_ALIGNED ((MallocAlignedFn)0x526000)
static uint64_t g_dx_cb_vtbl[6];

SYSV static uint8_t dx_cb_create_resource(uint8_t *cb) {
    if (DXF(cb, DXC_BUF, ID3D11Buffer *)) return 1;
    if (!DXF(cb, 0x28, void *)) {
        uint32_t size = CALC_BUFFER_SIZE(cb, 0);
        if (!size) size = 16;
        DXF(cb, 0x38, uint32_t) = size;
        void *data = GAME_MALLOC_ALIGNED(size, 4, 0);
        if (!data) return 0;
        memset(data, 0, size);
        DXF(cb, 0x28, void *) = data;
        CALC_BUFFER_SIZE(cb, 1);   // points every constant's storage into the data block
    }
    D3D11_BUFFER_DESC d;
    memset(&d, 0, sizeof d);
    d.ByteWidth = (DXF(cb, 0x38, uint32_t) + 15) & ~15u;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Buffer *buf = NULL;
    HRESULT hr = g_dx.d->lpVtbl->CreateBuffer(g_dx.d, &d, NULL, &buf);
    if (FAILED(hr)) { log_line("DirectX 11: constant buffer of %u bytes failed 0x%08lx", d.ByteWidth, (unsigned long)hr); return 0; }
    DXF(cb, DXC_BUF, ID3D11Buffer *) = buf;
    DXF(cb, 0x34, uint32_t) = 1;
    DXF(cb, 0x3c, uint32_t) = 0xffffffff;
    return 1;
}
// The buffer to bind for a SHADER_CONSTANT_BUFFER, sent to the card when the game changed it since the last time.
static ID3D11Buffer *dx_cb_for_draw(uint8_t *cb) {
    if (!cb || (!DXF(cb, DXC_BUF, void *) && !dx_cb_create_resource(cb))) return NULL;
    uint8_t *scb = DXF(cb, 0x20, uint8_t *);
    uint32_t version = scb ? DXF(scb, 0x38, uint32_t) : 0;
    if (DXF(cb, 0x3c, uint32_t) != version) {
        g_dx.c->lpVtbl->UpdateSubresource(g_dx.c, (ID3D11Resource *)DXF(cb, DXC_BUF, ID3D11Buffer *), 0, NULL, DXF(cb, 0x28, void *), 0, 0);
        DXF(cb, 0x3c, uint32_t) = version;
    }
    return DXF(cb, DXC_BUF, ID3D11Buffer *);
}
SYSV static void dx_cb_destroy_resource(uint8_t *cb) {
    typedef SYSV void (*FreeAlignedFn)(void *);
    DXREL(DXF(cb, DXC_BUF, ID3D11Buffer *));
    (void)sizeof(FreeAlignedFn);   // the CPU block stays: the game's constants still point into it
}
SYSV static void dx_cb_dtor(uint8_t *cb) { dx_cb_destroy_resource(cb); }
SYSV static void dx_cb_delete(uint8_t *cb) { dx_cb_destroy_resource(cb); GAME_FREE(cb); }
SYSV static uint8_t dx_cb_map(uint8_t *cb, void *ctx, uint32_t off, uint32_t size, uint32_t flags, void **out, uint32_t wait) {
    (void)ctx; (void)size; (void)flags; (void)wait;
    if (!dx_cb_create_resource(cb) || !out) return 0;
    *out = DXF(cb, 0x28, uint8_t *) + off;
    return 1;
}
SYSV static void dx_cb_unmap(uint8_t *cb, void *ctx) { (void)ctx; DXF(cb, 0x3c, uint32_t) = 0xfffffffe; }   // send again at next use
SYSV static void *dx_create_constant_buffer(uint8_t *dev, uint8_t *scb) {
    (void)dev;
    uint8_t *cb = GAME_MALLOC(DXC_SIZE, 0);
    if (!cb) return NULL;
    memset(cb, 0, DXC_SIZE);
    *(uint64_t **)cb = g_dx_cb_vtbl;
    DXF(cb, 0x08, uint32_t) = 1;
    DXF(cb, 0x14, uint32_t) = 0x10000;
    DXF(cb, 0x18, void *) = g_dx.dev;
    DXF(cb, 0x20, uint8_t *) = scb;
    DXF(cb, 0x34, uint32_t) = 0xffffffff;
    DXF(cb, 0x3c, uint32_t) = 0xffffffff;
    return cb;
}
static void dx_build_cb_table(void) {
    g_dx_cb_vtbl[0] = (uint64_t)dx_cb_dtor; g_dx_cb_vtbl[1] = (uint64_t)dx_cb_delete;
    g_dx_cb_vtbl[2] = (uint64_t)dx_cb_create_resource; g_dx_cb_vtbl[3] = (uint64_t)dx_cb_destroy_resource;
    g_dx_cb_vtbl[4] = (uint64_t)dx_cb_map; g_dx_cb_vtbl[5] = (uint64_t)dx_cb_unmap;
    (void)dx_cb_for_draw;
}

// ---- shaders: the game's own compiled DirectX 11 shaders (fxc\dx11_*.fxc in its packs), read through its file system.
// CaString (CA::String = {len, cap, text}) comes from multiplayer.inc
typedef SYSV void (*StrCtorFn)(CaString *, const char *);
typedef SYSV void (*StrVoidFn)(void *);
typedef SYSV void (*FxcNameFn)(const CaString *, const void *, const void *, const void *, uint32_t, uint32_t, uint32_t,
                               CaString *, CaString *, void *, void *);
typedef SYSV void *(*VfsFn)(void);
typedef SYSV void (*FileNameFn)(void *, const CaString *);
typedef struct { void *obj; uint32_t a, b; } VfsFilePtr;
typedef SYSV void (*VfsOpenFn)(VfsFilePtr *, void *, void *, void *, uint64_t);
#define CA_STRING_CTOR ((StrCtorFn)0x512db0)
#define CA_STRING_EMPTY ((StrVoidFn)0x512b80)
#define CA_STRING_DTOR ((StrVoidFn)0x512d00)
#define CA_USTRING_EMPTY ((StrVoidFn)0x51b480)
#define CA_USTRING_DTOR ((StrVoidFn)0x51ba50)
#define CREATE_FXC_NAME ((FxcNameFn)0x30f7940)
#define GAME_VFS ((VfsFn)0x2dbac60)
#define FILENAME_CTOR ((FileNameFn)0x50e9f0)
#define VFS_SKIP_FLAG (*(volatile uint8_t *)0x52694e2)

// The whole file "name" from the game's file system into malloc'd memory, or NULL.
static uint8_t *dx_read_game_file(const char *name, uint32_t *size_out) {
    if (VFS_SKIP_FLAG) return NULL;
    uint8_t *v = GAME_VFS();
    if (!v) return NULL;
    void *fs = v + 8;
    VfsOpenFn open = (VfsOpenFn)(*(void ***)fs)[0];
    CaString s;
    uint64_t file_name[4] = {0};
    struct { uint32_t a, b; uint8_t c; } opts = {0, 0, 0};
    VfsFilePtr fp = {0};
    CA_STRING_CTOR(&s, name);
    FILENAME_CTOR(file_name, &s);
    open(&fp, fs, file_name, &opts, 0);
    CA_STRING_DTOR(&s);
    uint8_t *out = NULL;
    if (fp.obj) {
        void **vt = *(void ***)fp.obj;
        if (((SYSV uint8_t (*)(void *))vt[0x30 / 8])(fp.obj)) {
            const void *data = ((SYSV const void *(*)(void *))vt[0x60 / 8])(fp.obj);
            uint32_t size = ((SYSV uint32_t (*)(void *))vt[0x28 / 8])(fp.obj);
            if (data && size && (out = malloc(size))) { memcpy(out, data, size); *size_out = size; }
        }
        ((SYSV void (*)(void *))vt[0x50 / 8])(fp.obj);
    }
    return out;
}

// Name -> slot tables from the shader's own reflection data (D3DReflect, d3dcompiler_47).
enum { DXR_CB = 0, DXR_TEX = 1, DXR_SAMP = 2, DXR_UAV = 3, DXR_MAX = 48 };   // TEX = anything read through a t register (textures, buffers), UAV = written (u register)
typedef struct { char name[56]; uint8_t kind, slot, cube, dim; uint32_t size; } DxBind;   // dim = D3D_SRV_DIMENSION, size = cbuffer bytes
typedef struct {
    uint64_t *vtbl;
    uint32_t stage;                 // 0 vertex, 1 pixel, 2 geometry, 3 compute
    ID3D11DeviceChild *sh;
    uint8_t *blob; uint32_t blob_size;
    uint32_t nbind;
    DxBind bind[DXR_MAX];
    uint8_t tex_dim[32];            // per texture slot: the dimension the shader declares (D3D_SRV_DIMENSION)
    char file[96];
} DxShader;
static int g_dx_audit_on;           // ATTILA_DX11_AUDIT=1: slow checks at every bind and draw
static int g_dx_test_no_filter;     // test: ATTILA_DX11_NO_FILTER=1 sets every binding again even when it is the same
static int g_dx_test_map_wait;      // test: ATTILA_DX11_MAP_WAIT=1 always waits in texture maps (ignores "do not wait")
static int g_dx_test_no_plan;       // test: ATTILA_DX11_NO_PLAN=1 works every binding list out afresh
static int g_dx_test_no_srvfast;    // test: ATTILA_DX11_NO_SRVFAST=1 looks every texture view up the long way
static int g_dx_test_no_memo;       // test: ATTILA_DX11_NO_MEMO=1 looks every name up the long way
static int g_dx_old_bind;           // test: ATTILA_DX11_OLD_BIND=1 = the binding code as it was before 2026-11-13 (generic compares, every state sent every time), to compare speed in one build
static uint64_t g_dx_shader_vtbl[2];
// Views of our own making (compute buffers: read and write view; textures: write view), see the compute section.
enum { DXV_OF = 0x10, DXV_VIEW = 0x18, DXV_SIZE = 0x20 };   // what it is a view of, the DirectX view
static uint64_t g_dx_ub_vtbl[9], g_dx_view_vtbl[3];
static uint32_t g_dx_slot_epoch;
SYSV static void dx_shader_dtor(DxShader *s) { DXREL(s->sh); free(s->blob); s->blob = NULL; g_dx_slot_epoch++; }
SYSV static void dx_shader_delete(DxShader *s) { dx_shader_dtor(s); GAME_FREE(s); }

static const GUID DX_IID_REFLECT = {0x8d536ca1, 0x0cca, 0x4956, {0xa8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84}};
typedef HRESULT (WINAPI *ReflectFn)(const void *, SIZE_T, const GUID *, void **);
typedef struct { const char *Name; UINT Type, BindPoint, BindCount, uFlags, ReturnType, Dimension, NumSamples; } DxBindDesc;
static void dx_reflect(DxShader *s) {
    static ReflectFn reflect;
    if (!reflect) { HMODULE m = LoadLibraryA("d3dcompiler_47.dll"); reflect = m ? (ReflectFn)(void *)GetProcAddress(m, "D3DReflect") : NULL; }
    if (!reflect) { log_line("DirectX 11: d3dcompiler_47.dll / D3DReflect not found"); return; }
    void **r = NULL;   // ID3D11ShaderReflection: GetDesc = 3, GetResourceBindingDesc = 6
    if (FAILED(reflect(s->blob, s->blob_size, &DX_IID_REFLECT, (void **)&r)) || !r) return;
    typedef HRESULT (WINAPI *GetDescFn)(void *, void *);
    typedef HRESULT (WINAPI *GetBindFn)(void *, UINT, DxBindDesc *);
    uint8_t desc[0x100];   // D3D11_SHADER_DESC: BoundResources at +0x0c... read below by field name order
    memset(desc, 0, sizeof desc);
    ((GetDescFn)(*(void ***)r)[3])(r, desc);
    UINT bound = *(UINT *)(desc + 0x14);   // Version, Creator(ptr), Flags, ConstantBuffers, BoundResources: x64 offsets 0,8,0x10,0x14,0x18
    bound = *(UINT *)(desc + 0x18);
    for (UINT i = 0; i < bound && s->nbind < DXR_MAX; i++) {
        DxBindDesc b;
        if (FAILED(((GetBindFn)(*(void ***)r)[6])(r, i, &b)) || !b.Name) continue;
        DxBind *o = &s->bind[s->nbind];
        // D3D_SHADER_INPUT_TYPE: 0 cbuffer, 2 texture, 3 sampler, 5 structured buffer, 7 byte address buffer (all read);
        // 4 RW typed (RWTexture2D...), 6 RW structured, 8 RW byte address, 9 append, 10 consume, 11 RW structured + counter.
        o->kind = b.Type == 0 ? DXR_CB : b.Type == 3 ? DXR_SAMP : (b.Type == 2 || b.Type == 5 || b.Type == 7) ? DXR_TEX
                : (b.Type == 4 || b.Type == 6 || (b.Type >= 8 && b.Type <= 11)) ? DXR_UAV : 0xff;
        if (o->kind == 0xff) continue;
        snprintf(o->name, sizeof o->name, "%s", b.Name);
        o->slot = (uint8_t)b.BindPoint;
        o->cube = b.Dimension == 9;   // D3D_SRV_DIMENSION_TEXTURECUBE
        o->dim = (uint8_t)b.Dimension;
        if (o->kind == DXR_TEX && b.BindPoint < 32) s->tex_dim[b.BindPoint] = (uint8_t)b.Dimension;
        o->size = 0;
        if (o->kind == DXR_CB) {   // ID3D11ShaderReflection::GetConstantBufferByName = 5 -> ID3D11ShaderReflectionConstantBuffer::GetDesc = 0
            void **cbr = ((void **(WINAPI *)(void *, const char *))(*(void ***)r)[5])(r, b.Name);
            struct { const char *name; UINT type, vars, size, flags; } cd;
            memset(&cd, 0, sizeof cd);
            if (cbr && SUCCEEDED(((HRESULT (WINAPI *)(void *, void *))(*(void ***)cbr)[0])(cbr, &cd))) o->size = cd.size;
        }
        s->nbind++;
    }
    ((ULONG (WINAPI *)(void *))(*(void ***)r)[2])(r);
}
static int dx_find_slot_slow(const DxShader *s, int kind, const char *name) {
    for (uint32_t i = 0; i < s->nbind; i++)
        if (s->bind[i].kind == kind && !strcmp(s->bind[i].name, name)) return s->bind[i].slot;
    return -1;
}
// The same question comes thousands of times a frame (every constant block and texture of every draw, for each shader
// stage), so answers are kept: (shader, kind, name pointer) -> slot. The name text is kept too and compared on a hit,
// because the game may reuse a name's memory; shaders that are freed empty the whole table (g_dx_slot_epoch).
typedef struct { const DxShader *s; const char *name; uint32_t epoch; int16_t slot; uint8_t kind, used; char text[48]; } DxSlotMemo;
static DxSlotMemo g_dx_slot_memo[16384];
static int dx_find_slot(const DxShader *s, int kind, const char *name) {
    if (!s || !name) return -1;
    uint64_t h = ((uint64_t)(uintptr_t)s * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)(uintptr_t)name * 0xC2B2AE3D27D4EB4Full) ^ ((uint64_t)kind << 7);
    DxSlotMemo *e = &g_dx_slot_memo[(h >> 40) & 16383];
    if (!g_dx_test_no_memo && e->used && e->s == s && e->name == name && e->kind == kind && e->epoch == g_dx_slot_epoch && !strcmp(e->text, name)) return e->slot;
    int slot = dx_find_slot_slow(s, kind, name);
    size_t len = strlen(name);
    if (len < sizeof e->text) { e->s = s; e->name = name; e->kind = (uint8_t)kind; e->slot = (int16_t)slot; e->epoch = g_dx_slot_epoch; memcpy(e->text, name, len + 1); e->used = 1; }
    return slot;
}

// Loads + makes one shader. stage: 0 vertex, 1 pixel, 2 geometry, 3 compute (the game's SHADER_TYPE values, 0 VS 1 PS 2 HS 3 DS 4 GS 5 CS, are passed to create_fxc_name
// as they come from the OpenGL caller: same numbers as in its own create_*_shader calls).
static DxShader *dx_make_shader(uint32_t stage, uint32_t game_type, uint32_t model, const CaString *file, const void *macros,
                                const CaString *entry, uint32_t flags) {
    CaString tag, a, name;
    uint64_t u1[2], u2[2];
    double t_made = dx_now_ms();
    CA_STRING_CTOR(&tag, "dx11");
    CA_STRING_EMPTY(&a); CA_STRING_EMPTY(&name); CA_USTRING_EMPTY(u1); CA_USTRING_EMPTY(u2);
    CREATE_FXC_NAME(&tag, macros, entry, file, game_type, model, flags, &a, &name, u1, u2);
    char path[512];
    snprintf(path, sizeof path, "fxc/%.*s.fxc", (int)name.len, name.text ? name.text : "");
    CA_STRING_DTOR(&tag); CA_STRING_DTOR(&a); CA_STRING_DTOR(&name); CA_USTRING_DTOR(u1); CA_USTRING_DTOR(u2);
    uint32_t size = 0;
    uint8_t *blob = dx_read_game_file(path, &size);
    if (!blob) { log_line("DirectX 11: shader file missing: %s", path); return NULL; }
    {   // the game's .fxc = its own header (magic 0x075bcd15, model, list of source files) + the DXBC container
        uint32_t at = 0;
        while (at + 32 <= size && memcmp(blob + at, "DXBC", 4)) at++;
        if (at + 32 <= size) {
            uint32_t total = *(uint32_t *)(blob + at + 0x18);
            if (!total || total > size - at) total = size - at;
            memmove(blob, blob + at, total);
            size = total;
        }
    }
    ID3D11DeviceChild *sh = NULL;
    HRESULT hr = stage == 0 ? g_dx.d->lpVtbl->CreateVertexShader(g_dx.d, blob, size, NULL, (ID3D11VertexShader **)&sh)
               : stage == 1 ? g_dx.d->lpVtbl->CreatePixelShader(g_dx.d, blob, size, NULL, (ID3D11PixelShader **)&sh)
               : stage == 3 ? g_dx.d->lpVtbl->CreateComputeShader(g_dx.d, blob, size, NULL, (ID3D11ComputeShader **)&sh)
               : g_dx.d->lpVtbl->CreateGeometryShader(g_dx.d, blob, size, NULL, (ID3D11GeometryShader **)&sh);
    if (FAILED(hr)) {
        static int dumped;
        if (dumped++ < 2) {
            char hex[200]; int o = 0;
            for (uint32_t i = 0; i < 48 && i < size; i++) o += snprintf(hex + o, sizeof hex - (size_t)o, "%02x", blob[i]);
            const uint8_t *m = NULL;
            for (uint32_t i = 0; i + 4 <= size && !m; i++) if (!memcmp(blob + i, "DXBC", 4)) m = blob + i;
            log_line("DirectX 11: %s: %u bytes, starts %s, DXBC at %d", path, size, hex, m ? (int)(m - blob) : -1);
        }
        log_line("DirectX 11: shader %s refused 0x%08lx", path, (unsigned long)hr); free(blob); return NULL;
    }
    DxShader *s = GAME_MALLOC(sizeof(DxShader), 0);
    memset(s, 0, sizeof *s);
    s->vtbl = g_dx_shader_vtbl; s->stage = stage; s->sh = sh; s->blob = blob; s->blob_size = size;
    snprintf(s->file, sizeof s->file, "%s", path + 4);
    dx_reflect(s);
    g_dxs.shaders++; g_dxs.shader_ms += dx_now_ms() - t_made;
    {   // test: ATTILA_DX11_DUMP=<part of a name> saves the DirectX 11 blob and the game's OpenGL text twin into "dump"
        // under the test picture folder (ATTILA_TEST_SHOT_DIR). No folder of this PC is written into the program.
        const char *want = getenv("ATTILA_DX11_DUMP"), *dir = test_out_dir();
        if (want && dir && strstr(path, want)) {
            char out[MAX_PATH], gl[512], dump[MAX_PATH];
            snprintf(dump, sizeof dump, "%s\\dump", dir);
            CreateDirectoryA(dump, NULL);
            snprintf(out, sizeof out, "%s\\%s.dxbc", dump, path + 4);
            FILE *f = fopen(out, "wb");
            if (f) { fwrite(blob, 1, size, f); fclose(f); }
            const char *d = strstr(path, "dx11_");
            snprintf(gl, sizeof gl, "fxc/glsl_%.*s", (int)(strlen(d + 5) - 4), d + 5);
            strcat(gl, ".glsl");
            if (getenv("ATTILA_DX11_DUMP_GL")) snprintf(gl, sizeof gl, "%s", getenv("ATTILA_DX11_DUMP_GL"));
            uint32_t gsize = 0;
            uint8_t *g = dx_read_game_file(gl, &gsize);
            snprintf(out, sizeof out, "%s\\%s", dump, gl + 4);
            if (g && (f = fopen(out, "wb"))) { fwrite(g, 1, gsize, f); fclose(f); }
            log_line("DirectX 11: dumped %s (+ %s: %s)", path, gl, g ? "found" : "missing");
            free(g);
        }
    }
    static int told;
    if (told++ < 3) log_line("DirectX 11: shader made: %s (%u bytes, %u bindings)", path, size, s->nbind);
    return s;
}

// ---- input layouts (made from the vertex shader's blob). Object: vtable (dtor pair, create/destroy resource), +8 device,
// +0x10 ID3D11InputLayout.
static uint64_t g_dx_layout_vtbl[4];
SYSV static void dx_layout_dtor(uint8_t *l) { DXREL(DXF(l, 0x10, ID3D11InputLayout *)); }
SYSV static void dx_layout_delete(uint8_t *l) { dx_layout_dtor(l); GAME_FREE(l); }
SYSV static uint8_t dx_layout_true(uint8_t *l) { (void)l; return 1; }
SYSV static void dx_layout_none(uint8_t *l) { (void)l; }
static void *dx_make_layout(const uint8_t *desc, const DxShader *vs) {
    static const char *const sem[4] = {"POSITION", "NORMAL", "COLOR", "TEXCOORD"};
    static const DXGI_FORMAT fmt[8] = {DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT,
        DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT};
    // Format 5 (4 bytes) is the old D3DCOLOR order: blue, green, red, alpha in memory. The game's OpenGL shaders are a
    // separate set built with VAF_BYTE_4_SWIZZLE (they read .zyxw); the DirectX 11 shaders read .xyzw and rely on BGRA here.
    static const uint32_t bytes[8] = {4, 4, 8, 12, 16, 4, 4, 8};
    D3D11_INPUT_ELEMENT_DESC e[32];
    UINT n = 0;
    uint32_t nstreams = DXF(desc, 4, uint32_t);
    const uint8_t *streams = DXF(desc, 8, const uint8_t *);
    for (uint32_t si = 0; si < nstreams && si < 8; si++) {
        const uint8_t *st = streams + si * 0x28;
        uint32_t na = DXF(st, 0x14, uint32_t), off = 0;
        const uint32_t *attr = DXF(st, 0x18, const uint32_t *);
        for (uint32_t ai = 0; ai < na && n < 32; ai++, attr += 3) {
            uint32_t s = attr[0] < 4 ? attr[0] : 0, f = attr[2] < 8 ? attr[2] : 1;
            e[n].SemanticName = sem[s]; e[n].SemanticIndex = attr[1]; e[n].Format = fmt[f];
            e[n].InputSlot = si; e[n].AlignedByteOffset = off;
            e[n].InputSlotClass = si ? D3D11_INPUT_PER_INSTANCE_DATA : D3D11_INPUT_PER_VERTEX_DATA;
            e[n].InstanceDataStepRate = si ? 1 : 0;
            off += bytes[f]; n++;
        }
    }
    // Every input the shader reads must be in the layout (DirectX 11 rule). Inputs the game's description lacks read zeros
    // from slot 15 (a 64-byte zero buffer bound with stride 0 at device creation). Seen 2026-11-12: weighted 4-bone
    // shaders want POSITION0-6, the description arriving here has 6 of them.
    static char names[32][24];
    UINT added = 0;
    if (vs) {
        static ReflectFn reflect;
        if (!reflect) { HMODULE m = LoadLibraryA("d3dcompiler_47.dll"); reflect = m ? (ReflectFn)(void *)GetProcAddress(m, "D3DReflect") : NULL; }
        void **r = NULL;
        if (reflect && SUCCEEDED(reflect(vs->blob, vs->blob_size, &DX_IID_REFLECT, (void **)&r)) && r) {
            uint8_t sdesc[0x100];
            memset(sdesc, 0, sizeof sdesc);
            ((HRESULT (WINAPI *)(void *, void *))(*(void ***)r)[3])(r, sdesc);
            UINT inputs = *(UINT *)(sdesc + 0x1c);
            for (UINT i = 0; i < inputs && n < 32; i++) {
                struct { const char *name; UINT index, reg, sysval, type; BYTE mask, rw; UINT stream, prec; } p;
                memset(&p, 0, sizeof p);
                if (FAILED(((HRESULT (WINAPI *)(void *, UINT, void *))(*(void ***)r)[7])(r, i, &p)) || !p.name || p.sysval) continue;
                UINT k;
                for (k = 0; k < n; k++) if (!_stricmp(e[k].SemanticName, p.name) && e[k].SemanticIndex == p.index) break;
                if (k < n) continue;
                snprintf(names[added % 32], sizeof names[0], "%s", p.name);
                e[n].SemanticName = names[added % 32]; e[n].SemanticIndex = p.index;
                e[n].Format = p.type == 1 ? DXGI_FORMAT_R32G32B32A32_UINT : p.type == 2 ? DXGI_FORMAT_R32G32B32A32_SINT : DXGI_FORMAT_R32G32B32A32_FLOAT;
                e[n].InputSlot = 15; e[n].AlignedByteOffset = 0; e[n].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA; e[n].InstanceDataStepRate = 0;
                n++; added++;
            }
            ((ULONG (WINAPI *)(void *))(*(void ***)r)[2])(r);
        }
        if (added) { static int told; if (told++ < 5) log_line("DirectX 11: %s: %u shader inputs not in the game's description, fed zeros", vs->file, added); }
    }
    ID3D11InputLayout *il = NULL;
    HRESULT hr = vs ? g_dx.d->lpVtbl->CreateInputLayout(g_dx.d, e, n, vs->blob, vs->blob_size, &il) : E_FAIL;
    if (FAILED(hr)) {
        static int told;
        if (told++ < 10) log_line("DirectX 11: input layout (%u elements) for %s refused 0x%08lx", n, vs ? vs->file : "?", (unsigned long)hr);
    }
    {   // test log: every new vertex shader's layout, game side vs shader side (first 40 shaders)
        static char seen[400][96]; static int nseen;
        int known = 0;
        for (int i = 0; vs && i < nseen; i++) if (!strcmp(seen[i], vs->file)) known = 1;
        if (vs && !known && nseen < 400 && (strstr(vs->file, "rigid") || strstr(vs->file, "weighted") || strstr(vs->file, "terrain") || strstr(vs->file, "tree"))) {
            snprintf(seen[nseen++], sizeof seen[0], "%s", vs->file);
            static int told = 0; told = 1;
        if (told <= 2 && vs) {   // what the shader wants vs what the game gives (ID3D11ShaderReflection: GetInputParameterDesc = 7)
            char line[1024]; int o = 0;
            for (UINT i = 0; i < n; i++) o += snprintf(line + o, sizeof line - (size_t)o, " %s%u:%u/%u@%u", e[i].SemanticName, e[i].SemanticIndex, e[i].Format, e[i].InputSlot, e[i].AlignedByteOffset);
            log_line("  %s %s: game gives:%s", vs->file, FAILED(hr) ? "REFUSED" : "ok", line);
            static ReflectFn reflect;
            if (!reflect) { HMODULE m = LoadLibraryA("d3dcompiler_47.dll"); reflect = m ? (ReflectFn)(void *)GetProcAddress(m, "D3DReflect") : NULL; }
            void **r = NULL;
            if (reflect && SUCCEEDED(reflect(vs->blob, vs->blob_size, &DX_IID_REFLECT, (void **)&r)) && r) {
                uint8_t desc[0x100];
                memset(desc, 0, sizeof desc);
                ((HRESULT (WINAPI *)(void *, void *))(*(void ***)r)[3])(r, desc);
                UINT inputs = *(UINT *)(desc + 0x1c);   // D3D11_SHADER_DESC.InputParameters
                o = 0;
                for (UINT i = 0; i < inputs && i < 32; i++) {
                    struct { const char *name; UINT index, reg, sysval, type; BYTE mask, rw; UINT stream, prec; } p;
                    memset(&p, 0, sizeof p);
                    if (SUCCEEDED(((HRESULT (WINAPI *)(void *, UINT, void *))(*(void ***)r)[7])(r, i, &p)) && p.name)
                        o += snprintf(line + o, sizeof line - (size_t)o, " %s%u(type %u mask %x)", p.name, p.index, p.type, p.mask);
                }
                log_line("  shader wants:%s", line);
                ((ULONG (WINAPI *)(void *))(*(void ***)r)[2])(r);
            }
        }
        }
    }
    uint8_t *l = GAME_MALLOC(0x18, 0);
    *(uint64_t **)l = g_dx_layout_vtbl;
    DXF(l, 0x08, void *) = g_dx.dev;
    DXF(l, 0x10, ID3D11InputLayout *) = il;
    return l;
}

// create_vertex_shader(dev, TECHNIQUE&, model, file, MACROS&, entry | flags, char const*, uint, VS **out, cbufs, samplers,
// resources, INPUT_LAYOUT_DESC const&, INPUT_LAYOUT **layout_out) -> bool. OpenGL's passes SHADER_TYPE 0 for vertex shaders.
SYSV static uint8_t dx_create_vertex_shader(uint8_t *dev, void *tech, uint32_t model, const CaString *file, void *macros,
        const CaString *entry, uint32_t flags, const char *c, uint32_t u, void **out, void *cbs, void *samps, void *res,
        const uint8_t *layout_desc, void **layout_out) {
    (void)dev; (void)tech; (void)c; (void)u; (void)cbs; (void)samps; (void)res;
    if (layout_out && *layout_out) { ((SYSV void (*)(void *))((*(void ***)*layout_out)[1]))(*layout_out); *layout_out = NULL; }
    DxShader *s = dx_make_shader(0, 0, model, file, macros, entry, flags);
    if (layout_out && layout_desc) *layout_out = dx_make_layout(layout_desc, s);
    if (!s) return 0;
    *out = s;
    return 1;
}
SYSV static uint8_t dx_create_pixel_shader(uint8_t *dev, void *tech, uint32_t model, const CaString *file, void *macros,
        const CaString *entry, uint32_t flags, const char *c, uint32_t u, void **out) {
    (void)dev; (void)tech; (void)c; (void)u;
    DxShader *s = dx_make_shader(1, 1, model, file, macros, entry, flags);
    if (!s) return 0;
    *out = s;
    return 1;
}
SYSV static uint8_t dx_create_geometry_shader(uint8_t *dev, void *tech, uint32_t model, const CaString *file, void *macros,
        const CaString *entry, uint32_t flags, void **out) {
    (void)dev; (void)tech;
    DxShader *s = dx_make_shader(2, 4, model, file, macros, entry, flags);
    if (!s) return 0;
    *out = s;
    return 1;
}
// create_compute_shader(dev, TECHNIQUE&, model, file, MACROS&, entry | flags, CS **out, cbufs, samplers, resources): the
// game's call in TECHNIQUE::create_resource (331c45d). Only shader model level 3 (SM5) has compute shaders: three for
// the particle sorter, two for the depth-of-field blur.
SYSV static uint8_t dx_create_compute_shader(uint8_t *dev, void *tech, uint32_t model, const CaString *file, void *macros,
        const CaString *entry, uint32_t flags, void **out) {
    (void)dev; (void)tech;
    DxShader *s = dx_make_shader(3, 5, model, file, macros, entry, flags);
    if (!s) return 0;
    *out = s;
    return 1;
}
SYSV static uint8_t dx_true_tech(uint8_t *dev, void *tech) { (void)dev; (void)tech; return 1; }
SYSV static void dx_none_tech(uint8_t *dev, void *tech) { (void)dev; (void)tech; }
SYSV static void dx_none_defines(uint8_t *dev, void *tech, void *macros) { (void)dev; (void)tech; (void)macros; }
static void dx_build_shader_tables(void) {
    g_dx_shader_vtbl[0] = (uint64_t)dx_shader_dtor; g_dx_shader_vtbl[1] = (uint64_t)dx_shader_delete;
    g_dx_layout_vtbl[0] = (uint64_t)dx_layout_dtor; g_dx_layout_vtbl[1] = (uint64_t)dx_layout_delete;
    g_dx_layout_vtbl[2] = (uint64_t)dx_layout_true; g_dx_layout_vtbl[3] = (uint64_t)dx_layout_none;
    g_dx.dev_vtbl[0x160 / 8] = (uint64_t)dx_create_vertex_shader;
    g_dx.dev_vtbl[0x168 / 8] = (uint64_t)dx_create_pixel_shader;
    g_dx.dev_vtbl[0x180 / 8] = (uint64_t)dx_create_geometry_shader;
    g_dx.dev_vtbl[0x188 / 8] = (uint64_t)dx_create_compute_shader;
    g_dx.dev_vtbl[0x190 / 8] = (uint64_t)dx_true_tech;     // create_shader_program: nothing to link in DirectX 11
    g_dx.dev_vtbl[0x198 / 8] = (uint64_t)dx_none_tech;     // destroy_shader_program
    g_dx.dev_vtbl[0x1a0 / 8] = (uint64_t)dx_none_defines;  // set_vertex_shader_defines: OpenGL-only switches (vaf_byte_4_swizzle)
}

// ---- the immediate context: what the game binds, turned into DirectX 11 calls. `ctx` = device + 0x88.
typedef struct { const void *key; uint32_t variant; void *obj; uint8_t desc[64]; } DxCacheEntry;
static DxCacheEntry g_dx_cache[8192];
// One DirectX object per (game object, variant); `make` builds it from the game object. Samplers compare their desc too,
// because the game changes a SAMPLER_STATE's filtering in place (texture sharpness setting).
static void *dx_cached(const void *key, uint32_t variant, const void *desc, size_t desc_len, void *(*make)(const void *, uint32_t)) {
    uint64_t h = ((uint64_t)key * 0x9E3779B97F4A7C15ull ^ variant) >> 51;
    for (uint32_t i = 0; i < 8192; i++) {
        DxCacheEntry *e = &g_dx_cache[(h + i) & 8191];
        if (e->key == key && e->variant == variant) {
            if (!desc || !memcmp(e->desc, desc, desc_len)) return e->obj;
            DXREL(e->obj);
            e->obj = make(key, variant);
            memcpy(e->desc, desc, desc_len);
            return e->obj;
        }
        if (!e->key) {
            e->key = key; e->variant = variant; e->obj = make(key, variant);
            if (desc) memcpy(e->desc, desc, desc_len);
            return e->obj;
        }
    }
    return NULL;
}
static void *dx_make_raster(const void *st, uint32_t v) {
    (void)v;
    D3D11_RASTERIZER_DESC d = *(const D3D11_RASTERIZER_DESC *)((const uint8_t *)st + 0x10);
    ID3D11RasterizerState *s = NULL;
    g_dx.d->lpVtbl->CreateRasterizerState(g_dx.d, &d, &s);
    return s;
}
static void *dx_make_blend(const void *st, uint32_t v) {
    (void)v;
    ID3D11BlendState *s = NULL;
    g_dx.d->lpVtbl->CreateBlendState(g_dx.d, (const D3D11_BLEND_DESC *)((const uint8_t *)st + 0x10), &s);
    return s;
}
static void *dx_make_depth(const void *st, uint32_t v) {
    (void)v;
    ID3D11DepthStencilState *s = NULL;
    g_dx.d->lpVtbl->CreateDepthStencilState(g_dx.d, (const D3D11_DEPTH_STENCIL_DESC *)((const uint8_t *)st + 0x10), &s);
    return s;
}
// Tests, by the sampler's name (the game's SAMPLER_STATE keeps it at +0x448, e.g. "sea_surface_sampler"):
// ATTILA_DX11_SAMPLERS=1 logs every named sampler's description once; ATTILA_DX11_TRILINEAR=<part of a name>[|<another>]
// makes such samplers plain trilinear instead of anisotropic.
static int dx_name_in_list(const char *name, const char *list) {
    char part[96];
    for (const char *p = list; name && p && *p;) {
        const char *e = strchr(p, '|');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n && n < sizeof part) { memcpy(part, p, n); part[n] = 0; if (strstr(name, part)) return 1; }
        p += n + (e ? 1 : 0);
    }
    return 0;
}
static void *dx_make_sampler(const void *ss, uint32_t v) {
    (void)v;
    D3D11_SAMPLER_DESC d = *(const D3D11_SAMPLER_DESC *)((const uint8_t *)ss + 0x0c);
    {
        static const char *tri = (const char *)1; static int log_all = -1;
        if (tri == (const char *)1) { tri = getenv("ATTILA_DX11_TRILINEAR"); log_all = getenv("ATTILA_DX11_SAMPLERS") != NULL; }
        const char *name = *(const char *const *)((const uint8_t *)ss + 0x448);
        if (tri && name && dx_name_in_list(name, tri) && (d.Filter == D3D11_FILTER_ANISOTROPIC || d.Filter == D3D11_FILTER_COMPARISON_ANISOTROPIC)) {
            d.Filter = d.Filter == D3D11_FILTER_ANISOTROPIC ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
            d.MaxAnisotropy = 1;
        }
        if (log_all && name) {
            static uint64_t seen[512]; static uint32_t nseen;
            uint64_t k = 1469598103934665603ull;
            for (const char *c = name; *c; c++) k = (k ^ (uint8_t)*c) * 1099511628211ull;
            k ^= ((uint64_t)d.Filter << 40) ^ ((uint64_t)d.MaxAnisotropy << 32) ^ ((uint64_t)d.AddressU << 24) ^ (uint64_t)(uint32_t)(d.MaxLOD * 16);
            int known = 0;
            for (uint32_t i = 0; i < nseen; i++) if (seen[i] == k) known = 1;
            if (!known && nseen < 512) {
                seen[nseen++] = k;
                log_line("DirectX 11 sampler '%s': filter 0x%x, address %u/%u/%u, bias %.2f, anisotropy %u, compare %u, lod %.1f..%.1f", name, d.Filter, d.AddressU, d.AddressV, d.AddressW, d.MipLODBias, d.MaxAnisotropy, d.ComparisonFunc, d.MinLOD, d.MaxLOD);
            }
        }
    }
    if (d.MaxAnisotropy < 1) d.MaxAnisotropy = 1;
    if (d.MaxAnisotropy > 16) d.MaxAnisotropy = 16;
    if (d.Filter == D3D11_FILTER_ANISOTROPIC || d.Filter == D3D11_FILTER_COMPARISON_ANISOTROPIC) {
        static volatile LONG seen;   // one log line per level, the same words as the OpenGL route (shim_gfx.c)
        LONG bit = 1L << (d.MaxAnisotropy & 31);
        if (!(_InterlockedOr(&seen, bit) & bit)) log_line("texture sharpness sent to the graphics card: %ux (DirectX 11 sampler)", d.MaxAnisotropy);
    }
    ID3D11SamplerState *s = NULL;
    HRESULT hr = g_dx.d->lpVtbl->CreateSamplerState(g_dx.d, &d, &s);
    if (FAILED(hr)) { static int told; if (told++ < 5) log_line("DirectX 11: sampler refused 0x%08lx (filter %x)", (unsigned long)hr, d.Filter); }
    return s;
}

// Views of our textures, kept on the texture (DXT_VIEWS: 8 x {key, view}), freed with it.
static uint64_t dx_hash(const void *p, size_t n) { uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t *)p)[i]) * 1099511628211ull; return h | 1; }
static void *dx_tex_view(uint8_t *t, int kind, const uint32_t *desc, size_t desc_len) {
    if (!t || *(uint64_t **)t != g_dx_tex_vtbl || !DXF(t, DXT_RES, void *)) return NULL;
    uint64_t key = dx_hash(desc, desc_len) * 4 + (uint64_t)kind;
    uint64_t *slots = (uint64_t *)(t + DXT_VIEWS);
    for (int i = 0; i < 8; i++) if (slots[i * 2] == key) return (void *)slots[i * 2 + 1];
    DxFmt f = dx_format(DXF(t, 0x10, uint32_t));
    ID3D11Resource *r = DXF(t, DXT_RES, ID3D11Resource *);
    uint32_t array = DXF(t, 0x28, uint32_t) ? 6 : DXF(t, 0x08, uint32_t) == 4 ? 1 : DXF(t, 0x1c, uint32_t) ? DXF(t, 0x1c, uint32_t) : 1;
    void *view = NULL;
    g_dxs.views++;
    HRESULT hr = E_FAIL;
    if (kind == 0) {   // render target: desc = {format, dimension, mip, first slice, slice count}
        D3D11_RENDER_TARGET_VIEW_DESC d;
        memset(&d, 0, sizeof d);
        d.Format = f.rtv;
        if (desc_len >= 4 && desc[0] && desc[0] != DXF(t, 0x10, uint32_t)) { DxFmt vf = dx_format(desc[0]); if (vf.rtv != DXGI_FORMAT_UNKNOWN && vf.res == f.res) d.Format = vf.rtv; }
        if (DXF(t, DXT_SAMPLES, uint32_t) > 1) d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
        else if (array > 1 || desc[1] == 5) { d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY; d.Texture2DArray.MipSlice = desc[2]; d.Texture2DArray.FirstArraySlice = desc[3]; d.Texture2DArray.ArraySize = desc[4] ? desc[4] : 1; }
        else { d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D; d.Texture2D.MipSlice = desc[2]; }
        hr = g_dx.d->lpVtbl->CreateRenderTargetView(g_dx.d, r, &d, (ID3D11RenderTargetView **)&view);
        if (DXF(t, 0x28, uint32_t)) { static int told; if (told++ < 16) log_line("DirectX 11: view for drawing into a cube texture %ux%u format 0x%x: game asks {format 0x%x, kind %u, level %u, first face %u, faces %u} (%u bytes) -> 0x%08lx",
            DXF(t, 0x14, uint32_t), DXF(t, 0x18, uint32_t), DXF(t, 0x10, uint32_t), desc_len >= 4 ? desc[0] : 0, desc_len >= 8 ? desc[1] : 0, desc_len >= 12 ? desc[2] : 0, desc_len >= 16 ? desc[3] : 0, desc_len >= 20 ? desc[4] : 0, (unsigned)desc_len, (unsigned long)hr); }
    } else if (kind == 1) {   // depth: desc = {format, dimension, flags, mip, first slice, slice count}
        D3D11_DEPTH_STENCIL_VIEW_DESC d;
        memset(&d, 0, sizeof d);
        d.Format = f.dsv;
        if (desc_len >= 4 && desc[0] && desc[0] != DXF(t, 0x10, uint32_t)) { DxFmt vf = dx_format(desc[0]); if (vf.dsv != DXGI_FORMAT_UNKNOWN && vf.res == f.res) d.Format = vf.dsv; }
        // desc[2] = DirectX 11's own view flags: 1 read-only depth, 2 read-only stencil. With them the same texture may be
        // read by a shader while it is the depth buffer (the game does that when it believes it is on DirectX 11; without
        // the flag DirectX unbinds the texture from the shader without a word).
        if (desc_len >= 12) d.Flags = desc[2] & (D3D11_DSV_READ_ONLY_DEPTH | (d.Format == DXGI_FORMAT_D24_UNORM_S8_UINT ? D3D11_DSV_READ_ONLY_STENCIL : 0));
        { static uint32_t seen; if (desc_len >= 12 && desc[2] && !(seen & (1u << (desc[2] & 31)))) { seen |= 1u << (desc[2] & 31); log_line("DirectX 11: depth view with flags %u in use (texture format 0x%x)", desc[2], DXF(t, 0x10, uint32_t)); } }
        if (DXF(t, DXT_SAMPLES, uint32_t) > 1) d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
        else if (array > 1 || desc[1] == 4) { d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY; d.Texture2DArray.MipSlice = desc[3]; d.Texture2DArray.FirstArraySlice = desc[4]; d.Texture2DArray.ArraySize = desc[5] ? desc[5] : 1; }
        else { d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D; d.Texture2D.MipSlice = desc[3]; }
        hr = g_dx.d->lpVtbl->CreateDepthStencilView(g_dx.d, r, &d, (ID3D11DepthStencilView **)&view);
    } else {           // whole texture for shaders
        D3D11_SHADER_RESOURCE_VIEW_DESC d;
        memset(&d, 0, sizeof d);
        d.Format = f.srv;
        uint32_t want = desc_len >= 4 ? desc[0] : 0;   // the kind the shader declares (D3D_SRV_DIMENSION), 0 = decide here
        // desc[1]: another way to read the same texture, asked for by the game's own view (its stencil views: format 0x29)
        if (desc_len >= 8 && desc[1]) { DxFmt vf = dx_format(desc[1]); if (vf.res == f.res && vf.srv != DXGI_FORMAT_UNKNOWN) d.Format = vf.srv; }
        UINT real_slices = 1;
        if (DXF(t, 0x08, uint32_t) != 4) { D3D11_TEXTURE2D_DESC td; ((ID3D11Texture2D *)r)->lpVtbl->GetDesc((ID3D11Texture2D *)r, &td); real_slices = td.ArraySize; }
        if (DXF(t, DXT_SAMPLES, uint32_t) > 1) d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;   // read sample by sample (the game's "_ms" shaders)
        else if (want == 5 && DXF(t, 0x08, uint32_t) != 4) { d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY; d.Texture2DArray.MipLevels = (UINT)-1; d.Texture2DArray.ArraySize = real_slices; }
        else if (want == 4 && DXF(t, 0x08, uint32_t) != 4) { d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; d.Texture2D.MipLevels = (UINT)-1; }
        else if (DXF(t, 0x08, uint32_t) == 4) { d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D; d.Texture3D.MipLevels = (UINT)-1; }
        else if (array == 6) { d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE; d.TextureCube.MipLevels = (UINT)-1; }
        else if (array > 1 || DXF(t, 0x3c, uint8_t)) { d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY; d.Texture2DArray.MipLevels = (UINT)-1; d.Texture2DArray.ArraySize = array; }
        else { d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; d.Texture2D.MipLevels = (UINT)-1; }
        hr = g_dx.d->lpVtbl->CreateShaderResourceView(g_dx.d, r, &d, (ID3D11ShaderResourceView **)&view);
        if (FAILED(hr) && DXF(t, 0x08, uint32_t) != 4) {   // what DirectX made, then the plain whole-resource view
            D3D11_TEXTURE2D_DESC td;
            ((ID3D11Texture2D *)r)->lpVtbl->GetDesc((ID3D11Texture2D *)r, &td);
            static int told;
            if (told++ < 3) log_line("DirectX 11: SRV refused for %ux%u fmt %u array %u levels %u misc 0x%x bind 0x%x usage %u (asked dim %u fmt %u)", td.Width, td.Height, td.Format, td.ArraySize, td.MipLevels, td.MiscFlags, td.BindFlags, td.Usage, d.ViewDimension, d.Format);
            hr = g_dx.d->lpVtbl->CreateShaderResourceView(g_dx.d, r, NULL, (ID3D11ShaderResourceView **)&view);
        }
    }
    if (FAILED(hr)) {
        static int told;
        if (told++ < 10) log_line("DirectX 11: view kind %d of texture fmt 0x%x %ux%u (bind 0x%x, slices %u, levels %u) desc %u %u %u %u %u %u refused 0x%08lx", kind, DXF(t, 0x10, uint32_t), DXF(t, 0x14, uint32_t), DXF(t, 0x18, uint32_t), DXF(t, DXT_BIND, uint32_t), array, DXF(t, 0x0c, uint32_t), desc[0], desc_len > 4 ? desc[1] : 0, desc_len > 8 ? desc[2] : 0, desc_len > 12 ? desc[3] : 0, desc_len > 16 ? desc[4] : 0, desc_len > 20 ? desc[5] : 0, (unsigned long)hr);
        return NULL;
    }
    for (int i = 0; i < 8; i++) if (!slots[i * 2]) { slots[i * 2] = key; slots[i * 2 + 1] = (uint64_t)view; return view; }
    DXREL(view);   // more than 8 different views of one texture: not kept (rare)
    return NULL;
}

static struct {
    DxShader *sh[3];                  // vertex, pixel, geometry
    uint8_t *cb[3][14];               // our constant buffer objects bound per stage (uploaded again before each draw)
    uint8_t *rt0;                     // texture of render target 0 (viewport size)
    uint8_t *depth;
    uint8_t scissor;
    void *raster_state;               // the game's state objects last set
    void *blend_state, *depth_state;
    uint8_t *rt[8]; uint32_t nrt;     // textures of the colour targets bound now
    uint8_t *tex[3][32];              // texture bound per stage and slot
    uint32_t cb_mask[3];              // which of cb[stage][] are set
    D3D11_VIEWPORT vp;
} g_dxc;
static void dx_pass_targets(void);
static void dx_pass_draw(uint32_t elems, uint32_t inst);
static void dx_pass_clear(int colour, const float *rgba, int depth, float z);
static void dx_pass_event(const char *what, uint8_t *dst, uint8_t *src, const char *more);
static const void *dx_cpu_shader(int stage) { return g_dxc.sh[stage]; }

// ---- test: where the card's time goes (ATTILA_DX11_GPU=1; ATTILA_DX11_GPU=<benchmark second> also lists that whole frame).
// A timestamp query at every change of render targets and before every compute run ("step"), read back when the next
// frame starts (waits for the card, so only for tests). A frame the card needed over 250 ms for is reported with its
// six longest steps, each named by the target size, samples and shaders of its first draw.
enum { DXG_MAX = 600 };
static struct {
    int on, started, pending, listed; double list_at, bench;
    ID3D11Query *q[DXG_MAX + 2], *disjoint;
    uint32_t n;                              // steps so far in this frame: step i runs from q[i] to q[i + 1]
    char label[DXG_MAX + 1][120];
    uint32_t draws[DXG_MAX + 1];
} g_dxg;
static const char *dx_short_name_v(const void *shader) {
    const DxShader *s = shader;
    if (!s) return "-";
    const char *m = strstr(s->file, "SM_");
    return m && strlen(m) > 7 ? m + 7 : s->file;
}
static void dx_gpu_mark(const char *what) {
    if (!g_dxg.on || !g_dxg.started || g_dxg.n >= DXG_MAX) return;
    g_dxg.n++;
    g_dx.c->lpVtbl->End(g_dx.c, (ID3D11Asynchronous *)g_dxg.q[g_dxg.n]);
    snprintf(g_dxg.label[g_dxg.n], sizeof g_dxg.label[0], "%s", what ? what : "");
    g_dxg.draws[g_dxg.n] = 0;
}
static void dx_gpu_draw(void) {
    if (!g_dxg.on || !g_dxg.started) return;
    uint32_t i = g_dxg.n;
    if (g_dxg.draws[i]++ || g_dxg.label[i][0]) return;
    uint8_t *rt = g_dxc.rt0 ? g_dxc.rt0 : g_dxc.depth;
    uint32_t samples = rt ? DXF(rt, DXT_SAMPLES, uint32_t) : 0;
    snprintf(g_dxg.label[i], sizeof g_dxg.label[0], "%ux%u x%u | %.42s + %.48s", rt ? DXF(rt, 0x14, uint32_t) : 0, rt ? DXF(rt, 0x18, uint32_t) : 0,
             samples ? samples : 1, dx_short_name(g_dxc.sh[0]), dx_short_name(g_dxc.sh[1]));
}
static void dx_gpu_frame_end(void) {
    if (!g_dxg.on || !g_dxg.started) return;
    g_dx.c->lpVtbl->End(g_dx.c, (ID3D11Asynchronous *)g_dxg.q[g_dxg.n + 1]);
    g_dx.c->lpVtbl->End(g_dx.c, (ID3D11Asynchronous *)g_dxg.disjoint);
    g_dxg.pending = 1;
    g_dxg.bench = test_bench_seconds();
}
static void dx_gpu_frame_start(void) {
    if (!g_dxg.on) return;
    if (!g_dxg.disjoint) {
        D3D11_QUERY_DESC dj = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, ts = {D3D11_QUERY_TIMESTAMP, 0};
        g_dx.d->lpVtbl->CreateQuery(g_dx.d, &dj, &g_dxg.disjoint);
        for (int i = 0; i < DXG_MAX + 2; i++) g_dx.d->lpVtbl->CreateQuery(g_dx.d, &ts, &g_dxg.q[i]);
        if (!g_dxg.disjoint || !g_dxg.q[DXG_MAX + 1]) { g_dxg.on = 0; log_line("DirectX 11: no timestamp queries, card timing off"); return; }
    }
    if (g_dxg.pending) {
        static UINT64 ts[DXG_MAX + 2];
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {0, 1};
        uint32_t steps = g_dxg.n + 1;
        while (g_dx.c->lpVtbl->GetData(g_dx.c, (ID3D11Asynchronous *)g_dxg.disjoint, &dj, sizeof dj, 0) == S_FALSE) Sleep(0);
        for (uint32_t i = 0; i <= steps; i++) while (g_dx.c->lpVtbl->GetData(g_dx.c, (ID3D11Asynchronous *)g_dxg.q[i], &ts[i], sizeof ts[i], 0) == S_FALSE) Sleep(0);
        g_dxg.pending = 0;
        if (!dj.Disjoint && dj.Frequency) {
            double k = 1000.0 / (double)dj.Frequency, total = (double)(ts[steps] - ts[0]) * k;
            int list = g_dxg.list_at > 1 && !g_dxg.listed && g_dxg.bench >= g_dxg.list_at;
            static int told;
            if (list) {
                g_dxg.listed = 1;
                log_line("DirectX 11 card time: frame at benchmark second %.1f: %.2f ms in %u steps; steps of 0.05 ms or more:", g_dxg.bench, total, steps);
                for (uint32_t i = 0; i < steps; i++) {
                    double ms = (double)(ts[i + 1] - ts[i]) * k;
                    if (ms >= 0.05) log_line("   step %3u: %7.2f ms, %4u draws | %s", i, ms, g_dxg.draws[i], g_dxg.label[i]);
                }
            } else if (total > 250 && g_dx.frames > 300 && told++ < 40) {
                log_line("DirectX 11 card time: slow frame at benchmark second %.1f: the card needed %.0f ms for %u steps; the longest:", g_dxg.bench, total, steps);
                uint8_t used[DXG_MAX + 1] = {0};
                for (int top = 0; top < 6; top++) {
                    int best = -1; double best_ms = 0;
                    for (uint32_t i = 0; i < steps; i++) { double ms = (double)(ts[i + 1] - ts[i]) * k; if (!used[i] && ms > best_ms) { best_ms = ms; best = (int)i; } }
                    if (best < 0 || best_ms < 1) break;
                    used[best] = 1;
                    log_line("   step %3d: %7.0f ms, %4u draws | %s", best, best_ms, g_dxg.draws[best], g_dxg.label[best]);
                }
            }
        }
    }
    g_dx.c->lpVtbl->Begin(g_dx.c, (ID3D11Asynchronous *)g_dxg.disjoint);
    g_dx.c->lpVtbl->End(g_dx.c, (ID3D11Asynchronous *)g_dxg.q[0]);
    g_dxg.n = 0; g_dxg.label[0][0] = 0; g_dxg.draws[0] = 0; g_dxg.started = 1;
}


// What was last sent to DirectX through the setters below, so the same value is not sent twice (2026-11-13; emptied in
// dx_destroy). TECHNIQUE::set_shaders and commit_changes send everything for every technique, mostly unchanged.
static struct {
    ID3D11DeviceChild *sh[3];
    ID3D11InputLayout *layout;
    uint32_t topology;
    const void *raster, *blend, *depth;
    uint64_t blend_factor[2]; uint8_t blend_has_factor, depth_ref;
} g_dxset;
SYSV static void dx_ctx_topology(void *ctx, uint32_t t) {
    (void)ctx;
    if (t == g_dxset.topology && !g_dx_old_bind) return;
    g_dxset.topology = t;
    g_dx.c->lpVtbl->IASetPrimitiveTopology(g_dx.c, (D3D11_PRIMITIVE_TOPOLOGY)t);
}
SYSV static void dx_ctx_index_buffer(void *ctx, uint8_t *ib, uint32_t fmt, uint32_t off) {
    (void)ctx;
    g_dx.c->lpVtbl->IASetIndexBuffer(g_dx.c, ib ? dx_buf_get(ib) : NULL, fmt ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, off);
}
static ID3D11InputLayout *g_dx_cur_layout;
SYSV static void dx_ctx_input_layout(void *ctx, uint8_t *l) {
    (void)ctx;
    g_dx_cur_layout = l ? DXF(l, 0x10, ID3D11InputLayout *) : NULL;
    if (g_dx_cur_layout == g_dxset.layout && !g_dx_old_bind) return;
    g_dxset.layout = g_dx_cur_layout;
    g_dx.c->lpVtbl->IASetInputLayout(g_dx.c, g_dx_cur_layout);
}
SYSV static uint8_t dx_ctx_vertex_buffers(void *ctx, uint32_t start, uint32_t n, uint8_t *const *vbs, const uint32_t *strides, const uint32_t *offs) {
    (void)ctx;
    ID3D11Buffer *b[16];
    UINT s[16], o[16];
    if (n > 16) n = 16;
    for (uint32_t i = 0; i < n; i++) { b[i] = vbs && vbs[i] ? dx_buf_get(vbs[i]) : NULL; s[i] = strides ? strides[i] : 0; o[i] = offs ? offs[i] : 0; }
    g_dx.c->lpVtbl->IASetVertexBuffers(g_dx.c, start, n, b, s, o);
    return 1;
}
SYSV static uint8_t dx_ctx_vs(void *ctx, DxShader *s) {
    (void)ctx;
    ID3D11DeviceChild *d = s ? s->sh : NULL;
    g_dxc.sh[0] = s;
    if (d == g_dxset.sh[0] && !g_dx_old_bind) return 1;
    g_dxset.sh[0] = d;
    g_dx.c->lpVtbl->VSSetShader(g_dx.c, (ID3D11VertexShader *)d, NULL, 0);
    return 1;
}
SYSV static uint8_t dx_ctx_ps(void *ctx, DxShader *s) {
    (void)ctx;
    ID3D11DeviceChild *d = s ? s->sh : NULL;
    g_dxc.sh[1] = s;
    if (d == g_dxset.sh[1] && !g_dx_old_bind) return 1;
    g_dxset.sh[1] = d;
    g_dx.c->lpVtbl->PSSetShader(g_dx.c, (ID3D11PixelShader *)d, NULL, 0);
    return 1;
}
SYSV static uint8_t dx_ctx_gs(void *ctx, DxShader *s) {
    (void)ctx;
    ID3D11DeviceChild *d = s ? s->sh : NULL;
    g_dxc.sh[2] = s;
    if (d == g_dxset.sh[2] && !g_dx_old_bind) return 1;
    g_dxset.sh[2] = d;
    g_dx.c->lpVtbl->GSSetShader(g_dx.c, (ID3D11GeometryShader *)d, NULL, 0);
    return 1;
}
SYSV static uint8_t dx_ctx_samplers(void *ctx, uint32_t n, void *const *s) { (void)ctx; (void)n; (void)s; return 1; }   // samplers come with the textures

// ---- binding constants and textures by name. The game hands over lists (a technique's constant blocks or its textures);
// each entry goes to the slot of the same NAME in every shader stage in use (OpenGL binds to the whole linked program,
// whichever call the game uses; so do we). Looking names up costs far too much per draw (measured: over half of the main
// thread), so the answer for a whole list is kept as a "plan": (list, shaders in use) -> slot per entry and stage.
// What DirectX has bound right now is remembered too, so the same thing is never set twice.
static struct {
    ID3D11ShaderResourceView *srv[3][32];
    ID3D11SamplerState *samp[3][16];
    ID3D11Buffer *cb[3][14];
} g_dxb;
static inline int dx_same_items(const void *const *a, const void *const *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}
// The DirectX sampler for one of the game's SAMPLER_STATE objects. The long way (dx_cached: a hash table and a compare
// of the whole description through memcmp) ran for every texture of every draw; this remembers the last answer per
// object and compares the 52-byte description as seven numbers. The game changes a description in place (texture
// sharpness), so the compare stays.
typedef struct { const uint8_t *samp; ID3D11SamplerState *ss; uint64_t d[7]; } DxSampMemo;
static DxSampMemo g_dx_samp_memo[1024];
static inline ID3D11SamplerState *dx_sampler_for(const uint8_t *samp) {
    if (g_dx_old_bind) return dx_cached(samp, 100, samp + 0x0c, sizeof(D3D11_SAMPLER_DESC), dx_make_sampler);
    DxSampMemo *e = &g_dx_samp_memo[((uintptr_t)samp >> 4) & 1023];
    uint64_t d[7];
    d[6] = 0;
    memcpy(d, samp + 0x0c, sizeof(D3D11_SAMPLER_DESC));
    if (e->samp == samp && e->d[0] == d[0] && e->d[1] == d[1] && e->d[2] == d[2] && e->d[3] == d[3] && e->d[4] == d[4] && e->d[5] == d[5] && e->d[6] == d[6]) return e->ss;
    e->ss = dx_cached(samp, 100, samp + 0x0c, sizeof(D3D11_SAMPLER_DESC), dx_make_sampler);
    e->samp = samp;
    memcpy(e->d, d, sizeof d);
    return e->ss;
}
// The texture behind one of the game's read views. Its class (OPENGL_SHADER_RESOURCE_VIEW, vtable 3c862c0) answers
// get_resource (355d9e0) with the field at +0x28: read it directly instead of calling through the table.
static inline uint8_t *dx_holder_tex(uint8_t *holder) {
    if (*(uint64_t *)holder == 0x3c862c0 && !g_dx_old_bind) return DXF(holder, 0x28, uint8_t *);
    return ((SYSV uint8_t *(*)(void *))((*(void ***)holder)[2]))(holder);
}
#define DX_UNKNOWN ((void *)(uintptr_t)1)
// DirectX itself drops a texture binding that clashes with a newly bound render target, so after a target change nothing
// is taken for granted.
static void dx_bound_forget_textures(void) {
    for (int st = 0; st < 3; st++) for (int i = 0; i < 32; i++) g_dxb.srv[st][i] = DX_UNKNOWN;
}
static void dx_set_cb(int stage, UINT slot, ID3D11Buffer *b) {
    if (g_dxb.cb[stage][slot] == b && !g_dx_test_no_filter) return;
    g_dxb.cb[stage][slot] = b;
    if (stage == 0) g_dx.c->lpVtbl->VSSetConstantBuffers(g_dx.c, slot, 1, &b);
    else if (stage == 1) g_dx.c->lpVtbl->PSSetConstantBuffers(g_dx.c, slot, 1, &b);
    else g_dx.c->lpVtbl->GSSetConstantBuffers(g_dx.c, slot, 1, &b);
}
static void dx_set_srv(int stage, UINT slot, ID3D11ShaderResourceView *v) {
    if (g_dxb.srv[stage][slot] == v && !g_dx_test_no_filter) return;
    g_dxb.srv[stage][slot] = v;
    if (stage == 0) g_dx.c->lpVtbl->VSSetShaderResources(g_dx.c, slot, 1, &v);
    else if (stage == 1) g_dx.c->lpVtbl->PSSetShaderResources(g_dx.c, slot, 1, &v);
    else g_dx.c->lpVtbl->GSSetShaderResources(g_dx.c, slot, 1, &v);
}
static void dx_set_samp(int stage, UINT slot, ID3D11SamplerState *s) {
    if (g_dxb.samp[stage][slot] == s && !g_dx_test_no_filter) return;
    g_dxb.samp[stage][slot] = s;
    if (stage == 0) g_dx.c->lpVtbl->VSSetSamplers(g_dx.c, slot, 1, &s);
    else if (stage == 1) g_dx.c->lpVtbl->PSSetSamplers(g_dx.c, slot, 1, &s);
    else g_dx.c->lpVtbl->GSSetSamplers(g_dx.c, slot, 1, &s);
}
// The game's own view object (made by create_shader_resource_view 33dc5e0, kept from the OpenGL device: +0x08 the
// description as the game gave it, +0x28 the texture) may ask for the stencil half of a depth texture: format 0x29, made
// by TEXTURE_MANAGER_ENTRY::device_create_depth_stencil (3445e70) and used through SHADER_RESOURCE's stencil flag. OpenGL
// never had it (it offers no MSAA, the only user: the "antiresolve with fixed stencil" pass that copies the terrain's
// depth into the multisampled depth buffer). Any other format wish is left alone: the texture's own format is read.
static ID3D11ShaderResourceView *dx_tex_srv(uint8_t *t, uint32_t dim);
static ID3D11ShaderResourceView *dx_holder_srv(const uint8_t *holder, uint8_t *tex, uint32_t dim) {
    if (DXF(holder, 0x08, uint32_t) == 0x29 && tex && *(uint64_t **)tex == g_dx_tex_vtbl && DXF(tex, 0x10, uint32_t) != 0x29) {
        uint32_t k[2] = {dim, 0x29};
        return (ID3D11ShaderResourceView *)dx_tex_view(tex, 2, k, sizeof k);
    }
    return dx_tex_srv(tex, dim);
}
// The view a shader reads a texture through: 0 = as the texture is, 4 = plain 2D, 5 = 2D array (what the shader declares).
// Kept on the texture (not owned: the view list at DXT_VIEWS owns it).
static ID3D11ShaderResourceView *dx_tex_srv(uint8_t *t, uint32_t dim) {
    if (!t || *(uint64_t **)t != g_dx_tex_vtbl) return NULL;
    int off = dim == 4 ? DXT_SRV4 : dim == 5 ? DXT_SRV5 : DXT_SRV;
    ID3D11ShaderResourceView *v = DXF(t, off, ID3D11ShaderResourceView *);
    if (!v || g_dx_test_no_srvfast) { v = (ID3D11ShaderResourceView *)dx_tex_view(t, 2, &dim, 4); DXF(t, off, ID3D11ShaderResourceView *) = v; }
    return v;
}

enum { DXP_MAX = 32 };
typedef struct { int8_t slot[3]; } DxPlanCb;
typedef struct { int8_t tex[3], samp[3]; uint8_t dim[3]; uint8_t tex_stages, samp_stages; const char *rname, *sname; } DxPlanRes;   // *_stages: bit per stage that has a slot
typedef struct {
    const void *list; uint32_t n, epoch; const DxShader *sh[3]; uint8_t used, kind;
    const void *item[DXP_MAX];
    union { DxPlanCb cb[DXP_MAX]; DxPlanRes res[DXP_MAX]; } u;
} DxPlan;
static DxPlan g_dx_plan[4096];
static void dx_plan_build(DxPlan *p, int kind, uint32_t n, const void *const *list) {
    p->list = list; p->n = n; p->epoch = g_dx_slot_epoch; p->kind = (uint8_t)kind; p->used = 1;
    for (int st = 0; st < 3; st++) p->sh[st] = g_dxc.sh[st];
    memcpy(p->item, list, n * sizeof(void *));
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *it = list[i];
        if (kind == 0) {
            const char *name = it ? DXF(it, 0x40, const char *) : NULL;
            for (int st = 0; st < 3; st++) {
                int slot = it && p->sh[st] ? dx_find_slot(p->sh[st], DXR_CB, name) : -1;
                p->u.cb[i].slot[st] = (int8_t)(slot >= 0 && slot < 14 ? slot : -1);
            }
        } else {
            const uint8_t *samp = it ? DXF(it, 0x28, const uint8_t *) : NULL;
            const char *rname = it ? DXF(it, 0x20, const char *) : NULL, *sname = samp ? DXF(samp, 0x448, const char *) : NULL;
            p->u.res[i].rname = rname; p->u.res[i].sname = sname;
            p->u.res[i].tex_stages = p->u.res[i].samp_stages = 0;
            for (int st = 0; st < 3; st++) {
                const DxShader *s = p->sh[st];
                int t = -1, sm = -1;
                if (it && s) {
                    t = dx_find_slot(s, DXR_TEX, sname);
                    if (t < 0) t = dx_find_slot(s, DXR_TEX, rname);
                    sm = dx_find_slot(s, DXR_SAMP, sname);
                    if (sm < 0) sm = dx_find_slot(s, DXR_SAMP, rname);
                }
                uint8_t dim = t >= 0 && t < 32 ? s->tex_dim[t] : 0;
                p->u.res[i].tex[st] = (int8_t)(t >= 0 && t < 32 ? t : -1);
                p->u.res[i].samp[st] = (int8_t)(sm >= 0 && sm < 16 ? sm : -1);
                p->u.res[i].dim[st] = dim == 4 || dim == 5 ? dim : 0;   // 2D or 2D array on request; cube / 3D decided by the texture
                if (p->u.res[i].tex[st] >= 0) p->u.res[i].tex_stages |= (uint8_t)(1u << st);
                if (p->u.res[i].samp[st] >= 0) p->u.res[i].samp_stages |= (uint8_t)(1u << st);
            }
        }
    }
}
static DxPlan *dx_plan(int kind, uint32_t n, const void *const *list) {
    uint64_t h = ((uint64_t)(uintptr_t)list * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)(uintptr_t)g_dxc.sh[0] * 0xC2B2AE3D27D4EB4Full) ^
                 ((uint64_t)(uintptr_t)g_dxc.sh[1] * 0x165667B19E3779F9ull) ^ ((uint64_t)(uintptr_t)g_dxc.sh[2] * 0x27D4EB2F165667C5ull) ^ ((uint64_t)kind << 13) ^ n;
    DxPlan *p = &g_dx_plan[(h >> 40) & 4095];
    if (!g_dx_test_no_plan && p->used && p->list == list && p->n == n && p->kind == kind && p->epoch == g_dx_slot_epoch && p->sh[0] == g_dxc.sh[0] &&
        p->sh[1] == g_dxc.sh[1] && p->sh[2] == g_dxc.sh[2] &&
        (g_dx_old_bind ? !memcmp(p->item, list, n * sizeof(void *)) : dx_same_items(p->item, list, n))) return p;
    dx_plan_build(p, kind, n, list);
    return p;
}

// Lists longer than one plan holds are taken in pieces (seen: the grass shader's texture list has more than 32 entries).
static uint8_t dx_constants_all(uint32_t n, uint8_t **list) {
    if (!n || !list) return 1;
    if (n > DXP_MAX) { dx_constants_all(n - DXP_MAX, list + DXP_MAX); n = DXP_MAX; }
    DxPlan *p = dx_plan(0, n, (const void *const *)list);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *scb = list[i];
        if (!scb) continue;
        uint8_t *cb = DXF(scb, 0x30, uint8_t *);
        ID3D11Buffer *b = NULL;
        int ready = 0;
        for (int st = 0; st < 3; st++) {
            int slot = p->u.cb[i].slot[st];
            if (slot < 0) continue;
            g_dxc.cb[st][slot] = cb;
            if (cb) g_dxc.cb_mask[st] |= 1u << slot; else g_dxc.cb_mask[st] &= ~(1u << slot);
            if (!ready) { b = dx_cb_for_draw(cb); ready = 1; }
            dx_set_cb(st, (UINT)slot, b);
        }
    }
    return 1;
}
SYSV static uint8_t dx_ctx_vs_cb(void *ctx, uint32_t n, uint8_t **l) { (void)ctx; return dx_constants_all(n, l); }
SYSV static uint8_t dx_ctx_ps_cb(void *ctx, uint32_t n, uint8_t **l) { (void)ctx; return dx_constants_all(n, l); }
SYSV static uint8_t dx_ctx_gs_cb(void *ctx, uint32_t n, uint8_t **l) { (void)ctx; return dx_constants_all(n, l); }

static uint32_t dx_resources_all(uint32_t n, uint8_t *const *list) {
    uint32_t bound = 0;
    if (!n || !list) return 0;
    if (n > DXP_MAX) {
        { static uint32_t most; if (n > most) { most = n; log_line("DirectX 11: a texture list with %u entries (taken in pieces of %d)", n, DXP_MAX); } }
        bound = dx_resources_all(n - DXP_MAX, list + DXP_MAX); n = DXP_MAX;
    }
    DxPlan *p = dx_plan(1, n, (const void *const *)list);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *res = list[i];
        if (!res) continue;
        uint8_t *holder = DXF(res, 0x10, uint8_t *), *samp = DXF(res, 0x28, uint8_t *);
        if (DXF(res, 0x20, const char *) != p->u.res[i].rname || (samp ? DXF(samp, 0x448, const char *) : NULL) != p->u.res[i].sname) {
            dx_plan_build(p, 1, n, (const void *const *)list);   // the entry was given other names since the plan was made
        }
        const DxPlanRes *r = &p->u.res[i];
        uint8_t *tex = NULL;
        ID3D11SamplerState *ss = NULL;
        int have_tex = 0, have_ss = 0;
        if (!g_dx_old_bind) {   // only the stages that read this entry; nothing at all for entries no shader reads
            if (r->tex_stages && holder) {
                int buffer_view = *(uint64_t **)holder == g_dx_view_vtbl;
                if (!buffer_view) tex = dx_holder_tex(holder);
                for (uint32_t m = r->tex_stages; m; m &= m - 1) {
                    int st = __builtin_ctz(m), t = r->tex[st];
                    ID3D11ShaderResourceView *v = buffer_view ? DXF(holder, DXV_VIEW, ID3D11ShaderResourceView *) : dx_holder_srv(holder, tex, r->dim[st]);
                    dx_set_srv(st, (UINT)t, v);
                    g_dxc.tex[st][t] = v ? tex : NULL;
                    bound++;
                }
            }
            if (r->samp_stages && samp) {
                ss = dx_sampler_for(samp);
                for (uint32_t m = r->samp_stages; m; m &= m - 1) { int st = __builtin_ctz(m); dx_set_samp(st, (UINT)r->samp[st], ss); }
            }
            continue;
        }
        for (int st = 0; st < 3; st++) {
            int t = r->tex[st], sm = r->samp[st];
            if (t >= 0 && holder) {
                ID3D11ShaderResourceView *v;
                if (*(uint64_t **)holder == g_dx_view_vtbl) v = DXF(holder, DXV_VIEW, ID3D11ShaderResourceView *);   // a compute buffer read by a drawing shader
                else {
                    if (!have_tex) { tex = ((SYSV uint8_t *(*)(void *))((*(void ***)holder)[2]))(holder); have_tex = 1; }
                    v = dx_holder_srv(holder, tex, r->dim[st]);
                }
                dx_set_srv(st, (UINT)t, v);
                g_dxc.tex[st][t] = v ? tex : NULL;
                bound++;
            }
            if (sm >= 0 && samp) {
                if (!have_ss) { ss = dx_cached(samp, 100, samp + 0x0c, sizeof(D3D11_SAMPLER_DESC), dx_make_sampler); have_ss = 1; }
                dx_set_samp(st, (UINT)sm, ss);
            }
        }
    }
    return bound;
}
SYSV static uint8_t dx_ctx_vs_res(uint8_t *ctx, uint32_t n, uint8_t *const *l) { DXF(ctx, 0x5c, uint32_t) = DXF(ctx, 0x64, uint32_t) = dx_resources_all(n, l); return 1; }
SYSV static uint8_t dx_ctx_ps_res(uint8_t *ctx, uint32_t n, uint8_t *const *l) { DXF(ctx, 0x5c, uint32_t) = DXF(ctx, 0x64, uint32_t) = dx_resources_all(n, l); return 1; }
SYSV static uint8_t dx_ctx_gs_res(uint8_t *ctx, uint32_t n, uint8_t *const *l) { (void)ctx; dx_resources_all(n, l); return 1; }
// ---- compute shaders (shader model level 3 = SM5 only). The game has five: three sort the particles on the card
// (VFX_BITONIC_SORTER_CS 34df1f0: prepare, sort, transpose; the particle vertex shader then reads the sorted list as
// "g_sorted_data_buffer") and two blur for depth of field. What they need beyond textures:
//  - DEVICE_UAV_BUFFER (create_uav_buffer, device slot 0x220): BUFFER::create_resource 3115140 asks for (bytes, element
//    size, usage 0 card only / 2 written by the CPU / 3 read back by the CPU, cpu flag, 1 = not structured). The game
//    calls map (+0x20), unmap (+0x28), get_details (+0x30) and update (+0x40: ctx, 0, 0, data, bytes) on it and reads
//    +0x10 = usage. Our object = the vertex buffer layout + element size.
//  - a read view and a write view of such a buffer (0x250, 0x270) and a write view of a texture (0x268). The game's
//    descriptions use DirectX 11's own numbers (dimension 1 = buffer, 4 = 2D texture; flags 1 raw, 2 append, 4 counter).
//  - a SHADER_RESOURCE carries the read view at +0x10 and the write view at +0x18 (SHADER_RESOURCE::operator= 3318000,
//    32b1a90); which of the two a shader gets is decided by the kind of the same NAME in the shader.
enum { DXU_STRIDE = 0x70, DXU_SIZE = 0x78 };

SYSV static uint8_t dx_ubuf_create_resource(uint8_t *b) { return DXF(b, DXB_BUF, void *) != NULL; }
SYSV static uint8_t dx_ubuf_map(uint8_t *b, void *ctx, uint32_t off, uint32_t size, uint32_t flags, void **out, uint32_t wait) {
    if (DXF(b, 0x10, uint32_t) != 3) return dx_buf_map(b, ctx, off, size, flags, out, wait);
    dx_thread_note(3);
    ID3D11Buffer *buf = DXF(b, DXB_BUF, ID3D11Buffer *);   // read-back kind: the real memory
    D3D11_MAPPED_SUBRESOURCE m;
    if (!buf || !out) return 0;
    D3D11_MAP kind = flags == 1 ? D3D11_MAP_READ : flags == 3 ? D3D11_MAP_READ_WRITE : D3D11_MAP_WRITE;
    if (FAILED(g_dx.c->lpVtbl->Map(g_dx.c, (ID3D11Resource *)buf, 0, kind, 0, &m))) return 0;
    DXF(b, DXB_MAP_KIND, uint32_t) = 1;
    *out = (uint8_t *)m.pData + off;
    return 1;
}
SYSV static void dx_ubuf_details(uint8_t *b, uint32_t *out) {
    static int told;
    if (out) *out = DXF(b, 0x0c, uint32_t);
    if (!told++) log_line("DirectX 11 check: get_details asked of a compute buffer (answered with its size in bytes)");
}
SYSV static uint64_t dx_ubuf_unknown(uint8_t *b) {
    static int told;
    (void)b;
    if (!told++) log_line("DirectX 11: not done yet: compute buffer function +0x38");
    return 0;
}
SYSV static uint8_t dx_ubuf_update(uint8_t *b, void *ctx, uint32_t off, uint32_t other, const void *data, uint32_t bytes) {
    (void)ctx;
    dx_thread_note(3);
    ID3D11Buffer *buf = DXF(b, DXB_BUF, ID3D11Buffer *);
    uint32_t total = DXF(b, 0x0c, uint32_t), usage = DXF(b, 0x10, uint32_t);
    if (off || other) { static int told; if (!told++) log_line("DirectX 11 check: compute buffer update with arguments %u, %u (only 0, 0 seen in the game's code)", off, other); }
    if (!buf || !data || off >= total) return 0;
    if (bytes > total - off) bytes = total - off;
    if (usage == 2 || usage == 3) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(g_dx.c->lpVtbl->Map(g_dx.c, (ID3D11Resource *)buf, 0, usage == 2 ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE, 0, &m))) return 0;
        memcpy((uint8_t *)m.pData + off, data, bytes);
        g_dx.c->lpVtbl->Unmap(g_dx.c, (ID3D11Resource *)buf, 0);
        return 1;
    }
    D3D11_BOX box = {off, 0, 0, off + bytes, 1, 1};
    g_dx.c->lpVtbl->UpdateSubresource(g_dx.c, (ID3D11Resource *)buf, 0, off == 0 && bytes == total ? NULL : &box, data, 0, 0);
    return 1;
}
SYSV static void *dx_create_uav_buffer(uint8_t *dev, uint32_t bytes, uint32_t stride, uint32_t usage, uint32_t cpu, uint32_t plain) {
    (void)dev;
    if (!bytes || !g_dx.d) return NULL;
    int structured = !plain && stride;
    D3D11_BUFFER_DESC d;
    memset(&d, 0, sizeof d);
    d.ByteWidth = bytes;
    if (usage == 3) { d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE; }
    else if (usage == 2) { d.Usage = D3D11_USAGE_DYNAMIC; d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; }
    else { d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS; }
    if (structured && usage != 3) { d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; d.StructureByteStride = stride; }
    ID3D11Buffer *buf = NULL;
    HRESULT hr = g_dx.d->lpVtbl->CreateBuffer(g_dx.d, &d, NULL, &buf);
    if (FAILED(hr)) { log_line("DirectX 11: compute buffer of %u bytes (element %u, usage %u, %s) failed 0x%08lx", bytes, stride, usage, structured ? "structured" : "plain", (unsigned long)hr); return NULL; }
    uint8_t *b = GAME_MALLOC(DXU_SIZE, 0);
    if (!b) { buf->lpVtbl->Release(buf); return NULL; }
    memset(b, 0, DXU_SIZE);
    *(uint64_t **)b = g_dx_ub_vtbl;
    DXF(b, 0x08, uint32_t) = 1;
    DXF(b, 0x0c, uint32_t) = bytes;
    DXF(b, 0x10, uint32_t) = usage;
    DXF(b, 0x14, uint32_t) = cpu;
    DXF(b, 0x18, void *) = g_dx.dev;
    DXF(b, DXB_BUF, ID3D11Buffer *) = buf;
    DXF(b, DXU_STRIDE, uint32_t) = structured ? stride : 0;
    { static int told; if (told++ < 4) log_line("DirectX 11: compute buffer made: %u bytes, element %u, usage %u, %s", bytes, stride, usage, structured ? "structured" : "plain"); }
    return b;
}

static void dx_bound_forget_textures(void);
SYSV static void dx_view_dtor(uint8_t *v) { DXREL(DXF(v, DXV_VIEW, IUnknown *)); dx_bound_forget_textures(); }
SYSV static void dx_view_delete(uint8_t *v) { dx_view_dtor(v); GAME_FREE(v); }
SYSV static void *dx_view_of(uint8_t *v) { return DXF(v, DXV_OF, void *); }
static void *dx_new_view(void *of, void *view) {
    uint8_t *v = GAME_MALLOC(DXV_SIZE, 0);
    if (!v) { ((IUnknown *)view)->lpVtbl->Release((IUnknown *)view); return NULL; }
    memset(v, 0, DXV_SIZE);
    *(uint64_t **)v = g_dx_view_vtbl;
    DXF(v, DXV_OF, void *) = of;
    DXF(v, DXV_VIEW, void *) = view;
    return v;
}
// create_shader_resource_view(DEVICE_UAV_BUFFER&, desc): desc = {format, dimension 1, ..., +0x18 first element, +0x1c count}
SYSV static void *dx_create_buffer_srv(uint8_t *dev, uint8_t *b, const uint32_t *desc) {
    (void)dev;
    if (!b || *(uint64_t **)b != g_dx_ub_vtbl || !DXF(b, DXB_BUF, void *) || !desc) return NULL;
    D3D11_SHADER_RESOURCE_VIEW_DESC d;
    memset(&d, 0, sizeof d);
    d.Format = DXF(b, DXU_STRIDE, uint32_t) ? DXGI_FORMAT_UNKNOWN : dx_format(desc[0]).srv;
    d.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    d.Buffer.FirstElement = desc[6]; d.Buffer.NumElements = desc[7];
    ID3D11ShaderResourceView *v = NULL;
    HRESULT hr = g_dx.d->lpVtbl->CreateShaderResourceView(g_dx.d, DXF(b, DXB_BUF, ID3D11Resource *), &d, &v);
    if (FAILED(hr)) { log_line("DirectX 11: read view of a compute buffer (elements %u..+%u, game format 0x%x) failed 0x%08lx", desc[6], desc[7], desc[0], (unsigned long)hr); return NULL; }
    return dx_new_view(b, v);
}
// create_unordered_access_view(DEVICE_UAV_BUFFER&, desc): desc = {format, dimension 1, 0, first element, count, flags}
SYSV static void *dx_create_buffer_uav(uint8_t *dev, uint8_t *b, const uint32_t *desc) {
    (void)dev;
    if (!b || *(uint64_t **)b != g_dx_ub_vtbl || !DXF(b, DXB_BUF, void *) || !desc) return NULL;
    D3D11_UNORDERED_ACCESS_VIEW_DESC d;
    memset(&d, 0, sizeof d);
    d.Format = DXF(b, DXU_STRIDE, uint32_t) ? DXGI_FORMAT_UNKNOWN : (desc[5] & D3D11_BUFFER_UAV_FLAG_RAW) ? DXGI_FORMAT_R32_TYPELESS : dx_format(desc[0]).srv;
    d.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    d.Buffer.FirstElement = desc[3]; d.Buffer.NumElements = desc[4]; d.Buffer.Flags = desc[5];
    ID3D11UnorderedAccessView *v = NULL;
    HRESULT hr = g_dx.d->lpVtbl->CreateUnorderedAccessView(g_dx.d, DXF(b, DXB_BUF, ID3D11Resource *), &d, &v);
    if (FAILED(hr)) { log_line("DirectX 11: write view of a compute buffer (elements %u..+%u, flags %u, game format 0x%x) failed 0x%08lx", desc[3], desc[4], desc[5], desc[0], (unsigned long)hr); return NULL; }
    return dx_new_view(b, v);
}
// create_unordered_access_view(DEVICE_TEXTURE&, desc): desc = {format, dimension 4, mip level}. The game makes one for
// every texture it flagged for compute output (TEXTURE_MANAGER_ENTRY::device_create_texture 34447d9, level 3 only).
SYSV static void *dx_create_texture_uav(uint8_t *dev, uint8_t *t, const uint32_t *desc) {
    (void)dev;
    if (!t || *(uint64_t **)t != g_dx_tex_vtbl || !DXF(t, DXT_RES, void *) || !desc) return NULL;
    if (!(DXF(t, DXT_BIND, uint32_t) & D3D11_BIND_UNORDERED_ACCESS)) {
        static int told;
        if (told++ < 4) log_line("DirectX 11 check: write view asked of a texture made without that use (game format 0x%x, %ux%u)", DXF(t, 0x10, uint32_t), DXF(t, 0x14, uint32_t), DXF(t, 0x18, uint32_t));
        return NULL;
    }
    DxFmt f = dx_format(desc[0] ? desc[0] : DXF(t, 0x10, uint32_t));
    D3D11_UNORDERED_ACCESS_VIEW_DESC d;
    memset(&d, 0, sizeof d);
    d.Format = f.rtv != DXGI_FORMAT_UNKNOWN ? f.rtv : f.srv;
    d.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    d.Texture2D.MipSlice = desc[2];
    ID3D11UnorderedAccessView *v = NULL;
    HRESULT hr = g_dx.d->lpVtbl->CreateUnorderedAccessView(g_dx.d, DXF(t, DXT_RES, ID3D11Resource *), &d, &v);
    if (FAILED(hr)) { log_line("DirectX 11: write view of a texture (game format 0x%x, %ux%u, mip %u) failed 0x%08lx", DXF(t, 0x10, uint32_t), DXF(t, 0x14, uint32_t), DXF(t, 0x18, uint32_t), desc[2], (unsigned long)hr); return NULL; }
    { static int told; if (told++ < 4) log_line("DirectX 11: texture write view made (game format 0x%x, %ux%u)", DXF(t, 0x10, uint32_t), DXF(t, 0x14, uint32_t), DXF(t, 0x18, uint32_t)); }
    return dx_new_view(t, v);
}

// The compute stage of the context. TECHNIQUE::set_shaders (32eb6b0) sets all six stages for every technique, so
// CSSetShader arrives thousands of times a frame with "none"; set_bindings (32eb880) then hands over the compute lists.
static DxShader *g_dx_cs;
static uint8_t *g_dx_cs_cb[14];
static uint32_t g_dx_cs_cb_mask, g_dx_cs_srv_mask, g_dx_cs_uav_mask;
static void dx_cs_unbind(void) {
    if (g_dx_cs_uav_mask) { ID3D11UnorderedAccessView *none[8] = {0}; g_dx.c->lpVtbl->CSSetUnorderedAccessViews(g_dx.c, 0, 8, none, NULL); g_dx_cs_uav_mask = 0; }
    if (g_dx_cs_srv_mask) { ID3D11ShaderResourceView *none[16] = {0}; g_dx.c->lpVtbl->CSSetShaderResources(g_dx.c, 0, 16, none); g_dx_cs_srv_mask = 0; }
}
SYSV static uint8_t dx_ctx_cs(void *ctx, DxShader *s) {
    (void)ctx;
    if (s == g_dx_cs) return 1;
    g_dx_cs = s;
    g_dx_cs_cb_mask = 0;
    g_dx.c->lpVtbl->CSSetShader(g_dx.c, s ? (ID3D11ComputeShader *)s->sh : NULL, NULL, 0);
    return 1;
}
SYSV static uint8_t dx_ctx_cs_cb(void *ctx, uint32_t n, uint8_t **list) {
    (void)ctx;
    for (uint32_t i = 0; g_dx_cs && list && i < n; i++) {
        uint8_t *scb = list[i];
        if (!scb) continue;
        int slot = dx_find_slot(g_dx_cs, DXR_CB, DXF(scb, 0x40, const char *));
        if (slot < 0 || slot >= 14) continue;
        uint8_t *cb = DXF(scb, 0x30, uint8_t *);
        ID3D11Buffer *b = dx_cb_for_draw(cb);
        g_dx.c->lpVtbl->CSSetConstantBuffers(g_dx.c, (UINT)slot, 1, &b);
        g_dx_cs_cb[slot] = cb;
        if (cb) g_dx_cs_cb_mask |= 1u << slot; else g_dx_cs_cb_mask &= ~(1u << slot);
    }
    return 1;
}
SYSV static uint8_t dx_ctx_cs_res(void *ctx, uint32_t n, uint8_t *const *list) {
    (void)ctx;
    for (uint32_t i = 0; g_dx_cs && list && i < n; i++) {
        uint8_t *res = list[i];
        if (!res) continue;
        uint8_t *holder = DXF(res, 0x10, uint8_t *), *writer = DXF(res, 0x18, uint8_t *), *samp = DXF(res, 0x28, uint8_t *);
        const char *rname = DXF(res, 0x20, const char *), *sname = samp ? DXF(samp, 0x448, const char *) : NULL;
        int slot = writer && *(uint64_t **)writer == g_dx_view_vtbl ? dx_find_slot(g_dx_cs, DXR_UAV, rname) : -1;
        if (slot >= 0 && slot < 8) {
            ID3D11UnorderedAccessView *v = DXF(writer, DXV_VIEW, ID3D11UnorderedAccessView *);
            g_dx.c->lpVtbl->CSSetUnorderedAccessViews(g_dx.c, (UINT)slot, 1, &v, NULL);
            g_dx_cs_uav_mask |= 1u << slot;
            dx_bound_forget_textures();   // DirectX takes the same thing off every stage that was reading it
        }
        slot = holder ? dx_find_slot(g_dx_cs, DXR_TEX, sname) : -1;
        if (holder && slot < 0) slot = dx_find_slot(g_dx_cs, DXR_TEX, rname);
        if (slot >= 0 && slot < 16) {
            ID3D11ShaderResourceView *v;
            if (*(uint64_t **)holder == g_dx_view_vtbl) v = DXF(holder, DXV_VIEW, ID3D11ShaderResourceView *);
            else {
                uint8_t dim = g_dx_cs->tex_dim[slot];
                v = dx_holder_srv(holder, ((SYSV uint8_t *(*)(void *))((*(void ***)holder)[2]))(holder), dim == 4 || dim == 5 ? dim : 0);
            }
            g_dx.c->lpVtbl->CSSetShaderResources(g_dx.c, (UINT)slot, 1, &v);
            g_dx_cs_srv_mask |= 1u << slot;
        }
        slot = samp ? dx_find_slot(g_dx_cs, DXR_SAMP, sname) : -1;
        if (samp && slot < 0) slot = dx_find_slot(g_dx_cs, DXR_SAMP, rname);
        if (slot >= 0 && slot < 16) {
            ID3D11SamplerState *ss = dx_cached(samp, 100, samp + 0x0c, sizeof(D3D11_SAMPLER_DESC), dx_make_sampler);
            g_dx.c->lpVtbl->CSSetSamplers(g_dx.c, (UINT)slot, 1, &ss);
        }
    }
    return 1;
}
SYSV static void dx_ctx_dispatch(void *ctx, uint32_t x, uint32_t y, uint32_t z) {
    (void)ctx;
    if (!g_dx_cs) { static int told; if (!told++) log_line("DirectX 11 check: Dispatch with no compute shader set"); return; }
    for (uint32_t m = g_dx_cs_cb_mask; m; m &= m - 1) dx_cb_for_draw(g_dx_cs_cb[__builtin_ctz(m)]);
    if (g_dxg.on) { char what[120]; snprintf(what, sizeof what, "compute %u x %u x %u | %.80s", x, y, z, dx_short_name(g_dx_cs)); dx_gpu_mark(what); }
    g_dx.c->lpVtbl->Dispatch(g_dx.c, x, y, z);
    g_dxs.dispatches++;
    { static int told; if (told++ < 4) log_line("DirectX 11: compute shader run: %s (%u x %u x %u groups)", g_dx_cs->file, x, y, z); }
}
SYSV static void dx_ctx_clear_uav_uint(void *ctx, uint8_t *v, const uint32_t *values) {
    (void)ctx;
    if (v && *(uint64_t **)v == g_dx_view_vtbl && values) g_dx.c->lpVtbl->ClearUnorderedAccessViewUint(g_dx.c, DXF(v, DXV_VIEW, ID3D11UnorderedAccessView *), values);
}
SYSV static void dx_ctx_clear_uav_float(void *ctx, uint8_t *v, const float *values) {
    (void)ctx;
    if (v && *(uint64_t **)v == g_dx_view_vtbl && values) g_dx.c->lpVtbl->ClearUnorderedAccessViewFloat(g_dx.c, DXF(v, DXV_VIEW, ID3D11UnorderedAccessView *), values);
}
static void dx_build_compute_tables(void) {
    g_dx_ub_vtbl[0] = (uint64_t)dx_buf_dtor; g_dx_ub_vtbl[1] = (uint64_t)dx_buf_delete;
    g_dx_ub_vtbl[2] = (uint64_t)dx_ubuf_create_resource; g_dx_ub_vtbl[3] = (uint64_t)dx_buf_destroy_resource;
    g_dx_ub_vtbl[4] = (uint64_t)dx_ubuf_map; g_dx_ub_vtbl[5] = (uint64_t)dx_buf_unmap;
    g_dx_ub_vtbl[6] = (uint64_t)dx_ubuf_details; g_dx_ub_vtbl[7] = (uint64_t)dx_ubuf_unknown; g_dx_ub_vtbl[8] = (uint64_t)dx_ubuf_update;
    g_dx_view_vtbl[0] = (uint64_t)dx_view_dtor; g_dx_view_vtbl[1] = (uint64_t)dx_view_delete; g_dx_view_vtbl[2] = (uint64_t)dx_view_of;
    g_dx.dev_vtbl[0x220 / 8] = (uint64_t)dx_create_uav_buffer;
    g_dx.dev_vtbl[0x250 / 8] = (uint64_t)dx_create_buffer_srv;
    g_dx.dev_vtbl[0x268 / 8] = (uint64_t)dx_create_texture_uav;
    g_dx.dev_vtbl[0x270 / 8] = (uint64_t)dx_create_buffer_uav;
}

SYSV static void dx_ctx_clear_res(void *ctx) {
    (void)ctx;
    dx_cs_unbind();    ID3D11ShaderResourceView *none[16] = {0};
    g_dx.c->lpVtbl->VSSetShaderResources(g_dx.c, 0, 16, none);
    g_dx.c->lpVtbl->PSSetShaderResources(g_dx.c, 0, 16, none);
    g_dx.c->lpVtbl->GSSetShaderResources(g_dx.c, 0, 16, none);
    for (int st = 0; st < 3; st++) for (int i = 0; i < 16; i++) { g_dxb.srv[st][i] = NULL; g_dxc.tex[st][i] = NULL; }
}
// Scissor test on/off comes from the rasterizer state alone (OpenGL 33e11b0: desc.ScissorEnable -> GL_SCISSOR_TEST); the
// rectangle is whatever set_scissor last set.
SYSV static void dx_ctx_raster(void *ctx, void *st) {
    (void)ctx;
    g_dxc.raster_state = st;
    if (st == g_dxset.raster && st && !g_dx_old_bind) return;
    g_dxset.raster = st;
    g_dx.c->lpVtbl->RSSetState(g_dx.c, st ? dx_cached(st, 1, NULL, 0, dx_make_raster) : NULL);
}
// OpenGL (33e1330) uses neither the blend factor (its factor blend modes fall back to ONE) nor the sample mask; a mask
// of 0 from the game would switch every pixel off in DirectX 11, so all samples are always on.
SYSV static void dx_ctx_blend(void *ctx, void *st, const float *factor, uint32_t mask) {
    (void)ctx;
    { static uint32_t seen; if (mask != 0xffffffffu && seen++ < 3) log_line("DirectX 11 check: blend state set with sample mask 0x%x (OpenGL ignores it; all samples kept on)", mask); }
    g_dxc.blend_state = st;
    uint64_t f[2] = {0, 0};
    if (factor) memcpy(f, factor, sizeof f);
    if (st == g_dxset.blend && st && (factor != NULL) == g_dxset.blend_has_factor && f[0] == g_dxset.blend_factor[0] && f[1] == g_dxset.blend_factor[1] && !g_dx_old_bind) return;
    g_dxset.blend = st; g_dxset.blend_has_factor = factor != NULL; g_dxset.blend_factor[0] = f[0]; g_dxset.blend_factor[1] = f[1];
    g_dx.c->lpVtbl->OMSetBlendState(g_dx.c, st ? dx_cached(st, 3, NULL, 0, dx_make_blend) : NULL, factor, 0xffffffffu);
}
SYSV static void dx_ctx_depth(void *ctx, void *st, uint8_t ref) {
    (void)ctx;
    g_dxc.depth_state = st;
    if (st == g_dxset.depth && st && ref == g_dxset.depth_ref && !g_dx_old_bind) return;
    g_dxset.depth = st; g_dxset.depth_ref = ref;
    g_dx.c->lpVtbl->OMSetDepthStencilState(g_dx.c, st ? dx_cached(st, 4, NULL, 0, dx_make_depth) : NULL, ref);
}
// OMSetRenderTargets(ctx, n, RTV objects, n uav, uavs, DSV object). Game view objects: RTV desc at +8 (5 x u32), texture +0x20;
// DSV desc at +8 (6 x u32), texture +0x20.
// Test (ATTILA_DX11_DUMP_DEPTH=seconds): that many seconds after the first big frame (> 500 draws), every depth texture
// drawn into during one frame is saved as a grey picture next to attila64.exe (dx11_depth_<n>_<w>x<h>.bmp, slice 0).
static uint8_t *g_dx_depth_seen[8];
static int g_dx_depth_n, g_dx_depth_armed;
static char g_dx_shadow_vs[48][96]; static uint32_t g_dx_shadow_cnt[48]; static int g_dx_shadow_n;
static void dx_note_shadow_draw(void) {   // during the dump frame: which vertex shaders draw into a 2048x2048 depth texture
    extern uint8_t *g_dx_cur_depth;
    if (!g_dx_depth_armed || !g_dx_cur_depth || DXF(g_dx_cur_depth, 0x14, uint32_t) != 2048 || !g_dxc.sh[0]) return;
    for (int i = 0; i < g_dx_shadow_n; i++) if (!strcmp(g_dx_shadow_vs[i], g_dxc.sh[0]->file)) { g_dx_shadow_cnt[i]++; return; }
    if (g_dx_shadow_n < 48) { snprintf(g_dx_shadow_vs[g_dx_shadow_n], 96, "%s", g_dxc.sh[0]->file); g_dx_shadow_cnt[g_dx_shadow_n++] = 1; }
}
uint8_t *g_dx_cur_depth;
static void dx_note_depth(uint8_t *tex) {
    if (!g_dx_depth_armed || !tex) return;
    for (int i = 0; i < g_dx_depth_n; i++) if (g_dx_depth_seen[i] == tex) return;
    if (g_dx_depth_n < 8) g_dx_depth_seen[g_dx_depth_n++] = tex;
}
static void dx_save_depth(uint8_t *t, int idx) {
    ID3D11Texture2D *src = DXF(t, DXT_RES, ID3D11Texture2D *);
    D3D11_TEXTURE2D_DESC d;
    if (!src) return;
    src->lpVtbl->GetDesc(src, &d);
    uint32_t bpp = d.Format == DXGI_FORMAT_R24G8_TYPELESS || d.Format == DXGI_FORMAT_R32_TYPELESS ? 4 : d.Format == DXGI_FORMAT_R16_TYPELESS ? 2 : 0;
    if (!bpp) return;
    D3D11_TEXTURE2D_DESC sd = d;
    sd.MipLevels = 1; sd.ArraySize = 1; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Texture2D *st = NULL;
    if (FAILED(g_dx.d->lpVtbl->CreateTexture2D(g_dx.d, &sd, NULL, &st))) return;
    g_dx.c->lpVtbl->CopySubresourceRegion(g_dx.c, (ID3D11Resource *)st, 0, 0, 0, 0, (ID3D11Resource *)src, 0, NULL);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_dx.c->lpVtbl->Map(g_dx.c, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        char path[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
        while (n && path[n - 1] != '\\') n--;
        snprintf(path + n, sizeof path - n, "dx11_depth_%d_%ux%u.bmp", idx, d.Width, d.Height);
        FILE *f = fopen(path, "wb");
        uint32_t w = d.Width > 2048 ? 2048 : d.Width, h = d.Height > 2048 ? 2048 : d.Height, row = w * 3, pad = (4 - row % 4) % 4;
        if (f) {
            uint8_t hdr[54] = {'B', 'M'};
            *(uint32_t *)(hdr + 2) = 54 + (row + pad) * h; *(uint32_t *)(hdr + 10) = 54; *(uint32_t *)(hdr + 14) = 40;
            *(int32_t *)(hdr + 18) = (int32_t)w; *(int32_t *)(hdr + 22) = (int32_t)h; *(uint16_t *)(hdr + 26) = 1; *(uint16_t *)(hdr + 28) = 24;
            fwrite(hdr, 1, 54, f);
            uint8_t *line = calloc(1, row + pad + 4);
            uint32_t mn = 255, mx = 0;
            for (uint32_t y = h; y-- > 0;) {
                const uint8_t *s = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
                for (uint32_t x = 0; x < w; x++) {
                    float v = bpp == 2 ? *(const uint16_t *)(s + x * 2) / 65535.0f
                            : d.Format == DXGI_FORMAT_R32_TYPELESS ? *(const float *)(s + x * 4) : (*(const uint32_t *)(s + x * 4) & 0xffffff) / 16777215.0f;
                    uint8_t g8 = (uint8_t)(v < 0 ? 0 : v > 1 ? 255 : v * 255);
                    if (g8 < mn) mn = g8;
                    if (g8 > mx) mx = g8;
                    line[x * 3] = line[x * 3 + 1] = line[x * 3 + 2] = g8;
                }
                fwrite(line, 1, row + pad, f);
            }
            free(line);
            fclose(f);
            log_line("DirectX 11: depth texture %d (%ux%u, %u slices, format %u) saved: values %u..%u of 255", idx, d.Width, d.Height, d.ArraySize, d.Format, mn, mx);
        }
        g_dx.c->lpVtbl->Unmap(g_dx.c, (ID3D11Resource *)st, 0);
    }
    st->lpVtbl->Release(st);
}

static void dx_depth_frame_end(uint32_t draws) {
    static double secs = -2; static ULONGLONG t0; static int state;
    if (secs == -2) { const char *e = getenv("ATTILA_DX11_DUMP_DEPTH"); secs = e ? atof(e) : -1; }
    if (secs < 0 || state == 2) return;
    if (state == 1) { for (int i = 0; i < g_dx_depth_n; i++) dx_save_depth(g_dx_depth_seen[i], i); for (int i = 0; i < g_dx_shadow_n; i++) log_line("DirectX 11: shadow map draws: %u x %s", g_dx_shadow_cnt[i], g_dx_shadow_vs[i]); g_dx_depth_armed = 0; state = 2; return; }
    if (!t0 && draws > 500) t0 = GetTickCount64();
    if (t0 && GetTickCount64() - t0 >= (ULONGLONG)(secs * 1000)) { g_dx_depth_armed = 1; g_dx_depth_n = 0; state = 1; }
}

// What OpenGL's version does (set_render_targets 33ded20 + OMSetRenderTargets 33e1650), kept the same here:
//  - nothing at all, or exactly the back buffer's view: the back buffer with ITS OWN depth buffer (the depth view passed
//    in is not used);
//  - otherwise the views that are there are packed into outputs 0, 1, 2... (a NULL in the list leaves no gap);
//  - device +0x1630 = the view that gives the target size (first colour view, else the depth view);
//  - the viewport is set to the whole target afterwards.
SYSV static uint8_t dx_ctx_viewport(uint8_t *ctx, const int32_t *vp);
static void dx_pass_cube_target(void);
SYSV static uint8_t dx_ctx_targets(uint8_t *ctx, uint32_t n, uint8_t **rtvs, uint32_t nuav, void **uavs, uint8_t *dsv) {
    (void)nuav; (void)uavs;
    dx_pass_targets();   // the pass that ends here is written down with the targets it had
    dx_gpu_mark(NULL);
    uint8_t *dev = g_dx.dev;
    uint8_t *bb_view = ((SYSV uint8_t *(*)(uint8_t *))g_dx.dev_vtbl[0x1f8 / 8])(dev);
    ID3D11RenderTargetView *r[8] = {0};
    ID3D11DepthStencilView *d = NULL;
    uint32_t count = 0;
    int nothing = dsv == NULL;
    if (n > 8) n = 8;
    for (uint32_t i = 0; i < n; i++) if (rtvs && rtvs[i]) nothing = 0;
    g_dxc.rt0 = NULL; g_dxc.depth = NULL;
    if (nothing || (n == 1 && rtvs[0] == bb_view && bb_view)) {
        static const uint32_t depth_desc[6] = {0x1c, 3, 0, 0, 0, 1};
        uint8_t *tex = bb_view ? DXF(bb_view, 0x20, uint8_t *) : NULL;
        if (tex) { r[0] = dx_tex_view(tex, 0, (const uint32_t *)(bb_view + 8), 20); count = 1; }
        g_dxc.rt0 = tex; g_dxc.rt[0] = tex;
        g_dxc.depth = DXF(dev, 0x1620, uint8_t *);
        if (g_dxc.depth) d = dx_tex_view(g_dxc.depth, 1, depth_desc, 24);
        DXF(dev, 0x1630, uint8_t *) = bb_view;
    } else {
        for (uint32_t i = 0; i < n; i++) {
            uint8_t *tex = rtvs[i] ? DXF(rtvs[i], 0x20, uint8_t *) : NULL;
            if (!tex || !DXF(tex, 0x34, uint8_t)) continue;
            r[count] = dx_tex_view(tex, 0, (const uint32_t *)(rtvs[i] + 8), 20);
            if (!count) g_dxc.rt0 = tex;
            g_dxc.rt[count] = tex;
            if (count != i) { static int told; if (told++ < 5) log_line("DirectX 11 check: render target list has a gap (view %u moved to output %u)", i, count); }
            count++;
        }
        if (dsv) { g_dxc.depth = DXF(dsv, 0x20, uint8_t *); d = dx_tex_view(g_dxc.depth, 1, (const uint32_t *)(dsv + 8), 24); }
        if (n && rtvs[0]) DXF(dev, 0x1630, uint8_t *) = rtvs[0];
        else if (dsv) DXF(dev, 0x1630, uint8_t *) = dsv;
    }
    if (g_dxc.depth) dx_note_depth(g_dxc.depth);
    g_dx_cur_depth = g_dxc.depth;
    if (!g_dxc.rt0) g_dxc.rt0 = g_dxc.depth;
    // no unbinding of textures here: OpenGL keeps texture bindings across target changes, and DirectX 11 itself drops a
    // view that conflicts with a target being bound.
    g_dx.c->lpVtbl->OMSetRenderTargets(g_dx.c, count, r, d);
    g_dxc.nrt = count;
    dx_bound_forget_textures();
    dx_ctx_viewport(ctx, NULL);
    dx_pass_cube_target();
    return 1;
}
// set_viewport(ctx, {left, top, right, bottom} or NULL = whole render target)
SYSV static uint8_t dx_ctx_viewport(uint8_t *ctx, const int32_t *vp) {
    D3D11_VIEWPORT v = {0, 0, 0, 0, 0, 1};
    if (vp) { v.TopLeftX = (float)vp[0]; v.TopLeftY = (float)vp[1]; v.Width = (float)(vp[2] - vp[0]); v.Height = (float)(vp[3] - vp[1]); }
    else if (g_dxc.rt0) { v.Width = (float)DXF(g_dxc.rt0, 0x14, uint32_t); v.Height = (float)DXF(g_dxc.rt0, 0x18, uint32_t); }
    else { v.Width = (float)g_dx.w; v.Height = (float)g_dx.h; }
    g_dx.c->lpVtbl->RSSetViewports(g_dx.c, 1, &v);
    g_dxc.vp = v;
    if (ctx) { DXF(ctx, 0xde8, int32_t) = (int32_t)v.TopLeftX; DXF(ctx, 0xdec, int32_t) = (int32_t)v.TopLeftY; DXF(ctx, 0xdf0, int32_t) = (int32_t)v.Width; DXF(ctx, 0xdf4, int32_t) = (int32_t)v.Height; }
    return 1;
}
// set_scissor(rect or NULL): only the rectangle; NULL = the whole target (OpenGL 33e1790). Whether it is used is the
// rasterizer state's business.
static D3D11_RECT g_dx_scissor;
SYSV static uint8_t dx_ctx_scissor(void *ctx, const int32_t *r) {
    (void)ctx;
    D3D11_RECT rc = {0, 0, (LONG)g_dx.w, (LONG)g_dx.h};
    if (r) { rc.left = r[0]; rc.top = r[1]; rc.right = r[2]; rc.bottom = r[3]; }
    else if (g_dxc.rt0) { rc.right = (LONG)DXF(g_dxc.rt0, 0x14, uint32_t); rc.bottom = (LONG)DXF(g_dxc.rt0, 0x18, uint32_t); }
    g_dx_scissor = rc;
    g_dxc.scissor = r != NULL;
    g_dx.c->lpVtbl->RSSetScissorRects(g_dx.c, 1, &rc);
    return 1;
}
// clear_surfaces(ctx, rect, colour?, colour, depth?, depth, stencil?, stencil): clears what is bound now.
SYSV static uint8_t dx_ctx_clear(void *ctx, const void *rect, uint8_t colour, const float *rgba, uint8_t depth, float z, uint8_t stencil, uint32_t s) {
    (void)ctx; (void)rect;
    ID3D11RenderTargetView *r[8] = {0};
    ID3D11DepthStencilView *d = NULL;
    dx_pass_clear(colour, rgba, depth, z);
    // OpenGL's clear (33e1e10, glClear) stays inside the scissor rectangle while the rasterizer state has the scissor
    // test on; DirectX 11's own clears always take the whole target. The game uses this on the campaign map (a colour
    // clear of 417x619 pixels of a full-size target, seen 2026-11-12). DirectX 11.1's ClearView clears a rectangle; it
    // cannot do a depth buffer that also holds stencil, so that case still clears everything and says so in the log.
    D3D11_RECT part = g_dx_scissor;
    int scissored = 0;
    if (g_dxc.raster_state && DXF((uint8_t *)g_dxc.raster_state, 0x2c, uint32_t) && g_dxc.rt0) {
        LONG tw = (LONG)DXF(g_dxc.rt0, 0x14, uint32_t), th = (LONG)DXF(g_dxc.rt0, 0x18, uint32_t);
        if (part.left < 0) part.left = 0;
        if (part.top < 0) part.top = 0;
        if (part.right > tw) part.right = tw;
        if (part.bottom > th) part.bottom = th;
        scissored = part.left > 0 || part.top > 0 || part.right < tw || part.bottom < th;
        if (scissored && (part.right <= part.left || part.bottom <= part.top)) return 1;   // an empty rectangle: nothing to clear
    }
    g_dx.c->lpVtbl->OMGetRenderTargets(g_dx.c, 8, r, &d);
    for (int i = 0; i < 8; i++) if (r[i]) {
        if (colour) {
            if (scissored && g_dx_c1) g_dx_c1->lpVtbl->ClearView(g_dx_c1, (ID3D11View *)r[i], rgba, &part, 1);
            else g_dx.c->lpVtbl->ClearRenderTargetView(g_dx.c, r[i], rgba);
        }
        r[i]->lpVtbl->Release(r[i]);
    }
    if (d) {
        UINT f = (depth ? D3D11_CLEAR_DEPTH : 0) | (stencil ? D3D11_CLEAR_STENCIL : 0);
        if (f && scissored) {
            D3D11_DEPTH_STENCIL_VIEW_DESC dd;
            d->lpVtbl->GetDesc(d, &dd);
            if (g_dx_c1 && depth && !stencil && (dd.Format == DXGI_FORMAT_D32_FLOAT || dd.Format == DXGI_FORMAT_D16_UNORM)) {
                const float zc[4] = {z, 0, 0, 0};
                g_dx_c1->lpVtbl->ClearView(g_dx_c1, (ID3D11View *)d, zc, &part, 1);
                f = 0;
            } else {
                static int told;
                if (told++ < 8) log_line("DirectX 11 check: depth clear with the scissor test on (rectangle %ld,%ld-%ld,%ld, view format %u): whole buffer cleared", part.left, part.top, part.right, part.bottom, dd.Format);
            }
        }
        if (f) g_dx.c->lpVtbl->ClearDepthStencilView(g_dx.c, d, f, z, (UINT8)s);
        d->lpVtbl->Release(d);
    }
    return 1;
}
static ID3D11InputLayout *g_dx_cur_layout;
// Test (ATTILA_DX11_AUDIT=1): at each draw, every texture / constant slot the current shaders read must have something
// bound; what is missing is logged once per (shader, name).
static void dx_audit(void) {
    static int on = -1;
    if (on < 0) on = getenv("ATTILA_DX11_AUDIT") != NULL;
    if (!on) return;
    static uint64_t seen[4096];
    for (int st = 0; st < 3; st++) {
        DxShader *s = g_dxc.sh[st];
        if (!s) continue;
        for (uint32_t i = 0; i < s->nbind; i++) {
            const DxBind *b = &s->bind[i];
            void *v = NULL;
            if (b->kind == DXR_UAV) continue;
            if (b->kind == DXR_TEX) {
                ID3D11ShaderResourceView *x = NULL;
                if (st == 0) g_dx.c->lpVtbl->VSGetShaderResources(g_dx.c, b->slot, 1, &x);
                else if (st == 1) g_dx.c->lpVtbl->PSGetShaderResources(g_dx.c, b->slot, 1, &x);
                else g_dx.c->lpVtbl->GSGetShaderResources(g_dx.c, b->slot, 1, &x);
                v = x;
            } else if (b->kind == DXR_CB) {
                ID3D11Buffer *x = NULL;
                if (st == 0) g_dx.c->lpVtbl->VSGetConstantBuffers(g_dx.c, b->slot, 1, &x);
                else if (st == 1) g_dx.c->lpVtbl->PSGetConstantBuffers(g_dx.c, b->slot, 1, &x);
                else g_dx.c->lpVtbl->GSGetConstantBuffers(g_dx.c, b->slot, 1, &x);
                v = x;
            } else {
                ID3D11SamplerState *x = NULL;
                if (st == 0) g_dx.c->lpVtbl->VSGetSamplers(g_dx.c, b->slot, 1, &x);
                else if (st == 1) g_dx.c->lpVtbl->PSGetSamplers(g_dx.c, b->slot, 1, &x);
                else g_dx.c->lpVtbl->GSGetSamplers(g_dx.c, b->slot, 1, &x);
                v = x;
            }
            if (v) { ((IUnknown *)v)->lpVtbl->Release((IUnknown *)v); continue; }
            uint64_t k = dx_hash(s->file, strlen(s->file)) ^ dx_hash(b->name, strlen(b->name)) * 31;
            uint32_t h = (uint32_t)(k % 4096), n = 0;
            while (seen[h] && seen[h] != k && n++ < 4096) h = (h + 1) % 4096;
            if (seen[h] == k) continue;
            seen[h] = k;
            log_line("DirectX 11 audit: %s needs %s '%s' (slot %u), nothing bound", s->file, b->kind == DXR_TEX ? "texture" : b->kind == DXR_CB ? "constants" : "sampler", b->name, b->slot);
        }
    }
}
static void dx_note_shadow_draw(void);
// ---- test: ATTILA_DX11_SKIP=<part of a shader file name>[|<another part>]: draws by such a vertex or pixel shader are
// left out, to see which shader makes a fault on screen. Decided once per shader pair.
static int dx_skip_draw(void) {
    static const char *list = (const char *)1;
    static const DxShader *last_vs, *last_ps;
    static int last;
    if (list == (const char *)1) list = getenv("ATTILA_DX11_SKIP");
    if (!list || !*list) return 0;
    if (g_dxc.sh[0] == last_vs && g_dxc.sh[1] == last_ps) return last;
    last_vs = g_dxc.sh[0]; last_ps = g_dxc.sh[1]; last = 0;
    char part[160];
    for (const char *p = list; *p && !last;) {
        const char *e = strchr(p, '|');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n && n < sizeof part) {
            memcpy(part, p, n); part[n] = 0;
            for (int st = 0; st < 2; st++) if (g_dxc.sh[st] && strstr(g_dxc.sh[st]->file, part)) last = 1;
        }
        p += n + (e ? 1 : 0);
    }
    if (last) { static int told; if (told++ < 20) log_line("DirectX 11 test: draws by %s + %s left out (ATTILA_DX11_SKIP)", last_vs ? last_vs->file : "-", last_ps ? last_ps->file : "-"); }
    return last;
}
static void dx_before_draw(uint32_t elems, uint32_t inst) {   // constants the game changed after binding them
    dx_gpu_draw();    dx_pass_draw(elems, inst);
    dx_audit();
    dx_note_shadow_draw();
    g_dxs.draws++; if (!g_dxc.sh[0]) g_dxs.no_vs++; if (!g_dxc.sh[1]) g_dxs.no_ps++; if (!g_dx_cur_layout) g_dxs.no_layout++;
    if (g_dxc.rt0 && g_dxc.rt0 == DXF(g_dx.dev, 0x1618, uint8_t *)) g_dxs.to_bb++;
    for (int st = 0; st < 3; st++)
        for (uint32_t m = g_dxc.cb_mask[st]; m; m &= m - 1) dx_cb_for_draw(g_dxc.cb[st][__builtin_ctz(m)]);
}
SYSV static void dx_ctx_draw(void *ctx, uint32_t n, uint32_t first) { (void)ctx; if (dx_skip_draw()) return; dx_before_draw(n, 1); g_dx.c->lpVtbl->Draw(g_dx.c, n, first); }
SYSV static void dx_ctx_draw_inst(void *ctx, uint32_t n, uint32_t inst, uint32_t first, uint32_t first_inst) { (void)ctx; if (dx_skip_draw()) return; dx_before_draw(n, inst); (void)first_inst; g_dx.c->lpVtbl->DrawInstanced(g_dx.c, n, inst, first, 0); }   /* OpenGL ignores the first instance: the buffer offset already points at it */
SYSV static void dx_ctx_draw_idx(void *ctx, uint32_t n, uint32_t first, int32_t base) { (void)ctx; if (dx_skip_draw()) return; dx_before_draw(n, 1); g_dx.c->lpVtbl->DrawIndexed(g_dx.c, n, first, base); }
SYSV static void dx_ctx_draw_idx_inst(void *ctx, uint32_t n, uint32_t inst, uint32_t first, int32_t base, uint32_t first_inst) { (void)ctx; if (dx_skip_draw()) return; dx_before_draw(n, inst); (void)first_inst; g_dx.c->lpVtbl->DrawIndexedInstanced(g_dx.c, n, inst, first, base, 0); }   /* as OpenGL (DrawElementsInstancedBaseVertex): no base instance */
SYSV static void dx_ctx_flush(void *ctx) { (void)ctx; g_dx.c->lpVtbl->Flush(g_dx.c); }

// ---- test: frame recorder (ATTILA_DX11_PASSES=<benchmark second>, needs ATTILA_TEST_SHOT_DIR). For the first frame at or
// after that benchmark second, every "pass" (the draws between two render target changes) is written to
// <shot dir>\passes\passes.txt: targets, clears, and per shader pair the number of draws, the states of its first draw and
// which texture sits in each of the pixel shader's slots (named by the pass that drew into it). The colour targets of
// every pass that drew something are saved as pictures (pNNN_rtK.bmp; level 0, slice 0; depth for depth-only passes).
typedef struct {
    const DxShader *vs, *ps;
    uint32_t draws, elems, inst;
    uint32_t blend[5], depth[3], cull, scissor;
    D3D11_VIEWPORT vp;
    uint8_t *tex[16];
    float cam[144], shadow[180]; uint8_t has_cam, has_shadow;   // camera_VS_PS / shadowmap_PS as they were at the first draw
} DxPassRow;
static struct {
    int state;                       // 0 waiting, 1 recording, 2 done
    double at;
    FILE *f;
    uint32_t pass, draws;
    char clears[160];
    DxPassRow rows[64]; uint32_t nrows;
    struct { uint8_t *tex; uint32_t pass, slot; } made[512]; uint32_t nmade;   // which pass drew into which texture
    char dir[MAX_PATH];
} g_dxp = {.at = -2};

static float dx_half(uint16_t h) {
    uint32_t e = (h >> 10) & 0x1f, m = h & 0x3ff;
    float v = e == 0 ? ldexpf((float)m, -24) : e == 31 ? (m ? 0.0f : 65504.0f) : ldexpf((float)(m | 0x400), (int)e - 25);
    return (h & 0x8000) ? -v : v;
}
static float dx_ufloat(uint32_t bits, int mbits) {   // the unsigned small floats of R11G11B10
    uint32_t e = bits >> mbits, m = bits & ((1u << mbits) - 1);
    if (e == 0) return ldexpf((float)m, -14 - mbits);
    if (e == 31) return 65000.0f;
    return ldexpf((float)(m | (1u << mbits)), (int)e - 15 - mbits);
}
static uint8_t dx_to8(float v) { return v <= 0 ? 0 : v >= 1 ? 255 : (uint8_t)(powf(v, 1 / 2.2f) * 255.0f + 0.5f); }
// One texture (level 0, slice 0) as a picture; box-filtered down to at most 1280 wide. Returns a short text about it.
static void dx_dump_tex(uint8_t *t, const char *path, char *note, size_t note_len) {
    note[0] = 0;
    if (!t || *(uint64_t **)t != g_dx_tex_vtbl || DXF(t, 0x08, uint32_t) == 4) return;
    ID3D11Texture2D *src = DXF(t, DXT_RES, ID3D11Texture2D *);
    D3D11_TEXTURE2D_DESC d;
    if (!src) return;
    src->lpVtbl->GetDesc(src, &d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.MipLevels = 1; sd.ArraySize = 1; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Texture2D *st = NULL;
    if (FAILED(g_dx.d->lpVtbl->CreateTexture2D(g_dx.d, &sd, NULL, &st))) { snprintf(note, note_len, "(no copy, format %u)", d.Format); return; }
    g_dx.c->lpVtbl->CopySubresourceRegion(g_dx.c, (ID3D11Resource *)st, 0, 0, 0, 0, (ID3D11Resource *)src, 0, NULL);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_dx.c->lpVtbl->Map(g_dx.c, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) { st->lpVtbl->Release(st); return; }
    uint32_t w = d.Width, h = d.Height, step = (w + 1279) / 1280, ow = w / step, oh = h / step;
    if (!ow || !oh) { ow = w; oh = h; step = 1; }
    float *px = malloc((size_t)w * h * 4 * sizeof(float));   // everything as 4 floats first
    int single = 0, known = 1, direct = 0;
    float mn[4] = {1e30f, 1e30f, 1e30f, 1e30f}, mx[4] = {-1e30f, -1e30f, -1e30f, -1e30f};
    for (uint32_t y = 0; px && y < h; y++) {
        const uint8_t *s = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
        float *o = px + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; x++, o += 4) {
            o[0] = o[1] = o[2] = 0; o[3] = 1;
            switch (d.Format) {
            case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                direct = 1; o[0] = s[x * 4] / 255.0f; o[1] = s[x * 4 + 1] / 255.0f; o[2] = s[x * 4 + 2] / 255.0f; o[3] = s[x * 4 + 3] / 255.0f; break;
            case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_UNORM:
                direct = 1; o[2] = s[x * 4] / 255.0f; o[1] = s[x * 4 + 1] / 255.0f; o[0] = s[x * 4 + 2] / 255.0f; o[3] = s[x * 4 + 3] / 255.0f; break;
            case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS: {
                const uint16_t *q = (const uint16_t *)(s + x * 8); o[0] = dx_half(q[0]); o[1] = dx_half(q[1]); o[2] = dx_half(q[2]); o[3] = dx_half(q[3]); break; }
            case DXGI_FORMAT_R16G16B16A16_UNORM: {
                const uint16_t *q = (const uint16_t *)(s + x * 8); direct = 1; for (int k = 0; k < 4; k++) o[k] = q[k] / 65535.0f; break; }
            case DXGI_FORMAT_R32G32B32A32_FLOAT: { const float *q = (const float *)(s + x * 16); o[0] = q[0]; o[1] = q[1]; o[2] = q[2]; o[3] = q[3]; break; }
            case DXGI_FORMAT_R11G11B10_FLOAT: { uint32_t q = *(const uint32_t *)(s + x * 4); o[0] = dx_ufloat(q & 0x7ff, 6); o[1] = dx_ufloat((q >> 11) & 0x7ff, 6); o[2] = dx_ufloat(q >> 22, 5); break; }
            case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS: { uint32_t q = *(const uint32_t *)(s + x * 4); direct = 1; o[0] = (q & 0x3ff) / 1023.0f; o[1] = ((q >> 10) & 0x3ff) / 1023.0f; o[2] = ((q >> 20) & 0x3ff) / 1023.0f; o[3] = (q >> 30) / 3.0f; break; }
            case DXGI_FORMAT_R16G16_FLOAT: { const uint16_t *q = (const uint16_t *)(s + x * 4); o[0] = dx_half(q[0]); o[1] = dx_half(q[1]); break; }
            case DXGI_FORMAT_R16G16_UNORM: { const uint16_t *q = (const uint16_t *)(s + x * 4); direct = 1; o[0] = q[0] / 65535.0f; o[1] = q[1] / 65535.0f; break; }
            case DXGI_FORMAT_R32G32_FLOAT: { const float *q = (const float *)(s + x * 8); o[0] = q[0]; o[1] = q[1]; break; }
            case DXGI_FORMAT_R8G8_UNORM: direct = 1; o[0] = s[x * 2] / 255.0f; o[1] = s[x * 2 + 1] / 255.0f; break;
            case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: single = 1; o[0] = *(const float *)(s + x * 4); break;
            case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
                single = 1; o[0] = (*(const uint32_t *)(s + x * 4) & 0xffffff) / 16777215.0f; break;
            case DXGI_FORMAT_R16_FLOAT: single = 1; o[0] = dx_half(*(const uint16_t *)(s + x * 2)); break;
            case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: single = 1; o[0] = *(const uint16_t *)(s + x * 2) / 65535.0f; break;
            case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_A8_UNORM: single = 1; o[0] = s[x] / 255.0f; break;
            default: known = 0; break;
            }
            for (int k = 0; k < 4; k++) { if (o[k] < mn[k]) mn[k] = o[k]; if (o[k] > mx[k]) mx[k] = o[k]; }
        }
        if (!known) break;
    }
    g_dx.c->lpVtbl->Unmap(g_dx.c, (ID3D11Resource *)st, 0);
    st->lpVtbl->Release(st);
    if (!px || !known) { free(px); snprintf(note, note_len, "(format %u not shown)", d.Format); return; }
    FILE *f = fopen(path, "wb");
    if (f) {
        uint32_t row = ow * 3, pad = (4 - row % 4) % 4;
        uint8_t hdr[54] = {'B', 'M'};
        *(uint32_t *)(hdr + 2) = 54 + (row + pad) * oh; *(uint32_t *)(hdr + 10) = 54; *(uint32_t *)(hdr + 14) = 40;
        *(int32_t *)(hdr + 18) = (int32_t)ow; *(int32_t *)(hdr + 22) = (int32_t)oh; *(uint16_t *)(hdr + 26) = 1; *(uint16_t *)(hdr + 28) = 24;
        fwrite(hdr, 1, 54, f);
        uint8_t *line = calloc(1, row + pad + 4);
        float range = mx[0] - mn[0] > 1e-12f ? mx[0] - mn[0] : 1;
        for (uint32_t oy = oh; oy-- > 0;) {
            for (uint32_t ox = 0; ox < ow; ox++) {
                float a[3] = {0, 0, 0};
                for (uint32_t yy = 0; yy < step; yy++) for (uint32_t xx = 0; xx < step; xx++) {
                    const float *q = px + ((size_t)(oy * step + yy) * w + ox * step + xx) * 4;
                    a[0] += q[0]; a[1] += q[1]; a[2] += q[2];
                }
                float k = 1.0f / (float)(step * step);
                uint8_t r8, g8, b8;
                if (single) { float v = (a[0] * k - mn[0]) / range; r8 = g8 = b8 = (uint8_t)(v <= 0 ? 0 : v >= 1 ? 255 : v * 255.0f + 0.5f); }
                else if (direct) { r8 = (uint8_t)(a[0] * k * 255.0f + 0.5f); g8 = (uint8_t)(a[1] * k * 255.0f + 0.5f); b8 = (uint8_t)(a[2] * k * 255.0f + 0.5f); }
                else { r8 = dx_to8(a[0] * k); g8 = dx_to8(a[1] * k); b8 = dx_to8(a[2] * k); }
                line[ox * 3] = b8; line[ox * 3 + 1] = g8; line[ox * 3 + 2] = r8;
            }
            fwrite(line, 1, row + pad, f);
        }
        free(line);
        fclose(f);
    }
    free(px);
    snprintf(note, note_len, "dxgi %u, r %.4g..%.4g g %.4g..%.4g b %.4g..%.4g a %.4g..%.4g%s", d.Format, mn[0], mx[0], mn[1], mx[1], mn[2], mx[2], mn[3], mx[3], single ? " (grey = min..max)" : "");
}
static const char *dx_pass_tex_name(uint8_t *t, char *buf, size_t len) {
    if (!t) return "-";
    for (uint32_t i = g_dxp.nmade; i-- > 0;) if (g_dxp.made[i].tex == t) { snprintf(buf, len, "p%03u.rt%u", g_dxp.made[i].pass, g_dxp.made[i].slot); return buf; }
    if (*(uint64_t **)t != g_dx_tex_vtbl) return "?";
    snprintf(buf, len, "%ux%u/f%x%s", DXF(t, 0x14, uint32_t), DXF(t, 0x18, uint32_t), DXF(t, 0x10, uint32_t), t == DXF(g_dx.dev, 0x1618, uint8_t *) ? "(backbuffer)" : "");
    return buf;
}
static void dx_pass_flush(void) {
    if (g_dxp.state != 1 || !g_dxp.f) return;
    FILE *f = g_dxp.f;
    char nm[64], note[200], path[MAX_PATH];
    if (g_dxp.draws || g_dxp.clears[0]) {
        fprintf(f, "\npass %03u: %u draws", g_dxp.pass, g_dxp.draws);
        for (uint32_t i = 0; i < g_dxc.nrt; i++) fprintf(f, " | rt%u %s %ux%u/f%x", i, g_dxc.rt[i] == DXF(g_dx.dev, 0x1618, uint8_t *) ? "BACKBUFFER" : dx_pass_tex_name(g_dxc.rt[i], nm, sizeof nm), g_dxc.rt[i] ? DXF(g_dxc.rt[i], 0x14, uint32_t) : 0, g_dxc.rt[i] ? DXF(g_dxc.rt[i], 0x18, uint32_t) : 0, g_dxc.rt[i] ? DXF(g_dxc.rt[i], 0x10, uint32_t) : 0);
        if (g_dxc.depth) fprintf(f, " | depth %ux%u/f%x %s", DXF(g_dxc.depth, 0x14, uint32_t), DXF(g_dxc.depth, 0x18, uint32_t), DXF(g_dxc.depth, 0x10, uint32_t), dx_pass_tex_name(g_dxc.depth, nm, sizeof nm));
        if (g_dxp.clears[0]) fprintf(f, " | clears:%s", g_dxp.clears);
        fprintf(f, "\n");
        for (uint32_t i = 0; i < g_dxp.nrows; i++) {
            const DxPassRow *r = &g_dxp.rows[i];
            fprintf(f, "  %4u draws %8u elems %6u inst | %s + %s | vp %.0f,%.0f %.0fx%.0f | blend %u:%u,%u,%u mask %x | depth %u w%u f%u | cull %u sc %u\n", r->draws, r->elems, r->inst,
                    r->vs ? r->vs->file + 5 : "-", r->ps ? r->ps->file + 5 : "-", r->vp.TopLeftX, r->vp.TopLeftY, r->vp.Width, r->vp.Height,
                    r->blend[0], r->blend[1], r->blend[2], r->blend[3], r->blend[4], r->depth[0], r->depth[1], r->depth[2], r->cull, r->scissor);
            if (r->has_cam && (i == 0 || r->has_shadow)) {   // first shader pair of each pass, and every user of the shadow map
                static const char *const nm4[5] = {"view", "projection", "view_projection", "inv_view", "inv_projection"};
                fprintf(f, "        camera position %.3f %.3f %.3f\n", r->cam[0], r->cam[1], r->cam[2]);
                for (int mtx = 0; mtx < 6; mtx++) {
                    const float *q = r->cam + 4 + mtx * 16;
                    fprintf(f, "        %-19s [%11.5f %11.5f %11.5f %11.5f] [%11.5f %11.5f %11.5f %11.5f] [%11.5f %11.5f %11.5f %11.5f] [%11.5f %11.5f %11.5f %11.5f]\n", mtx < 5 ? nm4[mtx] : "inv_view_projection",
                            q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], q[10], q[11], q[12], q[13], q[14], q[15]);
                }
                fprintf(f, "        near/far %.4f %.4f %.4f %.4f | screen %.1f %.1f %.6f %.6f | vpos offset %.3f | viewport dims %.1f %.1f %.6f %.6f\n", r->cam[100], r->cam[101], r->cam[102], r->cam[103],
                        r->cam[108], r->cam[109], r->cam[110], r->cam[111], r->cam[112], r->cam[116], r->cam[117], r->cam[118], r->cam[119]);
            }
            if (r->has_shadow) {
                const float *q = r->shadow;
                fprintf(f, "        shadow buffer size %.1f %.1f %.6f %.6f | fade %.3f %.3f bias %.5f splits %.1f | light dir %.4f %.4f %.4f\n", q[0], q[1], q[2], q[3], q[136], q[137], q[138], q[139], q[140], q[141], q[142]);
                for (int s = 0; s < 4; s++) {
                    const float *m4 = q + 8 + s * 16;
                    fprintf(f, "        split %d matrix      [%11.6f %11.6f %11.6f %11.6f] [%11.6f %11.6f %11.6f %11.6f] [%11.6f %11.6f %11.6f %11.6f] [%11.6f %11.6f %11.6f %11.6f] | distance %.2f | uv bounds %.4f %.4f %.4f %.4f\n", s,
                            m4[0], m4[1], m4[2], m4[3], m4[4], m4[5], m4[6], m4[7], m4[8], m4[9], m4[10], m4[11], m4[12], m4[13], m4[14], m4[15], q[144 + s * 4], q[164 + s * 4], q[165 + s * 4], q[166 + s * 4], q[167 + s * 4]);
                }
            }
            if (r->ps) {
                int any = 0;
                for (uint32_t k = 0; k < r->ps->nbind; k++) {
                    const DxBind *b = &r->ps->bind[k];
                    if (b->kind != DXR_TEX || b->slot >= 16) continue;
                    fprintf(f, "%s%s=%s", any++ ? ", " : "        ", b->name, dx_pass_tex_name(r->tex[b->slot], nm, sizeof nm));
                }
                if (any) fprintf(f, "\n");
                // pictures of render-target / depth textures this shader reads that no pass of this frame has drawn yet
                for (uint32_t k = 0; k < r->ps->nbind; k++) {
                    const DxBind *b = &r->ps->bind[k];
                    uint8_t *t = b->kind == DXR_TEX && b->slot < 16 ? r->tex[b->slot] : NULL;
                    static const char *all_of = (const char *)1;   // ATTILA_DX11_PASS_INPUTS=<part of a pixel shader name>: every texture it reads
                    if (all_of == (const char *)1) all_of = getenv("ATTILA_DX11_PASS_INPUTS");
                    int wanted = all_of && *all_of && strstr(r->ps->file, all_of) != NULL;
                    if (!t || *(uint64_t **)t != g_dx_tex_vtbl || (!wanted && !(DXF(t, DXT_BIND, uint32_t) & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)))) continue;
                    int known = 0;
                    for (uint32_t m = 0; m < g_dxp.nmade; m++) if (g_dxp.made[m].tex == t) known = 1;
                    if (known || g_dxp.nmade >= 512) continue;
                    snprintf(path, sizeof path, "%s\\p%03u_in%u_%s.bmp", g_dxp.dir, g_dxp.pass, b->slot, b->name);
                    dx_dump_tex(t, path, note, sizeof note);
                    fprintf(f, "        input %s as read by pass %03u: %s\n", b->name, g_dxp.pass, note);
                    g_dxp.made[g_dxp.nmade].tex = t; g_dxp.made[g_dxp.nmade].pass = g_dxp.pass; g_dxp.made[g_dxp.nmade++].slot = 90 + b->slot;
                }
            }
        }
        if (g_dxp.draws) {
            for (uint32_t i = 0; i < g_dxc.nrt; i++) {
                snprintf(path, sizeof path, "%s\\p%03u_rt%u.bmp", g_dxp.dir, g_dxp.pass, i);
                dx_dump_tex(g_dxc.rt[i], path, note, sizeof note);
                fprintf(f, "  -> rt%u %s\n", i, note);
                if (g_dxp.nmade < 512) { g_dxp.made[g_dxp.nmade].tex = g_dxc.rt[i]; g_dxp.made[g_dxp.nmade].pass = g_dxp.pass; g_dxp.made[g_dxp.nmade++].slot = i; }
            }
            if (!g_dxc.nrt && g_dxc.depth) {
                snprintf(path, sizeof path, "%s\\p%03u_depth.bmp", g_dxp.dir, g_dxp.pass);
                dx_dump_tex(g_dxc.depth, path, note, sizeof note);
                fprintf(f, "  -> depth %s\n", note);
                if (g_dxp.nmade < 512) { g_dxp.made[g_dxp.nmade].tex = g_dxc.depth; g_dxp.made[g_dxp.nmade].pass = g_dxp.pass; g_dxp.made[g_dxp.nmade++].slot = 9; }
            }
        }
        g_dxp.pass++;
    }
    g_dxp.draws = 0; g_dxp.nrows = 0; g_dxp.clears[0] = 0;
}
static void dx_pass_event(const char *what, uint8_t *dst, uint8_t *src, const char *more) {
    if (g_dxp.state != 1 || !g_dxp.f) return;
    char a[64], b[64];
    fprintf(g_dxp.f, "  [after draw %u of pass %03u] %s: %s (%ux%u/f%x) <- %s (%ux%u/f%x) %s\n", g_dxp.draws, g_dxp.pass, what,
            dx_pass_tex_name(dst, a, sizeof a), dst ? DXF(dst, 0x14, uint32_t) : 0, dst ? DXF(dst, 0x18, uint32_t) : 0, dst ? DXF(dst, 0x10, uint32_t) : 0,
            dx_pass_tex_name(src, b, sizeof b), src ? DXF(src, 0x14, uint32_t) : 0, src ? DXF(src, 0x18, uint32_t) : 0, src ? DXF(src, 0x10, uint32_t) : 0, more ? more : "");
}
static void dx_pass_targets(void) { if (g_dxp.state == 1) dx_pass_flush(); }   // called at the top of dx_ctx_targets, while the old targets are still in g_dxc
static void dx_pass_clear(int colour, const float *rgba, int depth, float z) {
    if (g_dxp.state != 1) return;
    size_t o = strlen(g_dxp.clears);
    if (o + 60 < sizeof g_dxp.clears) {
        if (colour && rgba) snprintf(g_dxp.clears + o, sizeof g_dxp.clears - o, " colour(%.3g,%.3g,%.3g,%.3g)@%u", rgba[0], rgba[1], rgba[2], rgba[3], g_dxp.draws);
        o = strlen(g_dxp.clears);
        if (depth) snprintf(g_dxp.clears + o, sizeof g_dxp.clears - o, " depth(%.3g)@%u", z, g_dxp.draws);
    }
}
static void dx_pass_draw(uint32_t elems, uint32_t inst) {
    if (g_dxp.state != 1) return;
    g_dxp.draws++;
    DxPassRow *r = NULL;
    for (uint32_t i = 0; i < g_dxp.nrows; i++) if (g_dxp.rows[i].vs == g_dxc.sh[0] && g_dxp.rows[i].ps == g_dxc.sh[1]) { r = &g_dxp.rows[i]; break; }
    if (!r) {
        if (g_dxp.nrows >= 64) return;
        r = &g_dxp.rows[g_dxp.nrows++];
        memset(r, 0, sizeof *r);
        r->vs = g_dxc.sh[0]; r->ps = g_dxc.sh[1];
        const uint8_t *b = g_dxc.blend_state, *d = g_dxc.depth_state, *rs = g_dxc.raster_state;
        if (b) { r->blend[0] = DXF(b, 0x18, uint32_t); r->blend[1] = DXF(b, 0x1c, uint32_t); r->blend[2] = DXF(b, 0x20, uint32_t); r->blend[3] = DXF(b, 0x24, uint32_t); r->blend[4] = DXF(b, 0x34, uint8_t); }
        if (d) { r->depth[0] = DXF(d, 0x10, uint32_t); r->depth[1] = DXF(d, 0x14, uint32_t); r->depth[2] = DXF(d, 0x18, uint32_t); }
        if (rs) { r->cull = DXF(rs, 0x14, uint32_t); r->scissor = DXF(rs, 0x2c, uint32_t); }
        r->vp = g_dxc.vp;
        memcpy(r->tex, g_dxc.tex[1], sizeof r->tex);
        for (int st = 0; st < 2; st++) {   // the camera and shadow constants this draw uses
            const DxShader *s = g_dxc.sh[st];
            for (uint32_t k = 0; s && k < s->nbind; k++) {
                const DxBind *bd = &s->bind[k];
                uint8_t *cb = bd->kind == DXR_CB && bd->slot < 14 ? g_dxc.cb[st][bd->slot] : NULL;
                const void *data = cb ? DXF(cb, 0x28, const void *) : NULL;
                uint32_t size = cb ? DXF(cb, 0x38, uint32_t) : 0;
                if (!data) continue;
                if (!strcmp(bd->name, "camera_VS_PS") && !r->has_cam) { memcpy(r->cam, data, size < sizeof r->cam ? size : sizeof r->cam); r->has_cam = 1; }
                if (!strcmp(bd->name, "shadowmap_PS") && !r->has_shadow) { memcpy(r->shadow, data, size < sizeof r->shadow ? size : sizeof r->shadow); r->has_shadow = 1; }
            }
        }
    }
    r->draws++; r->elems += elems; r->inst += inst;
}
// ATTILA_DX11_PASSES=cube: the recording starts at once (mid-frame) the first time the game draws into a cube texture
// (the sky reflection map bake), and runs to the end of that frame.
static int g_dxp_cube = -1;
static void dx_pass_start(double t);
static void dx_pass_cube_target(void) {
    if (g_dxp_cube < 0) { const char *e = getenv("ATTILA_DX11_PASSES"); g_dxp_cube = e && !strcmp(e, "cube"); }
    if (!g_dxp_cube || g_dxp.state != 0) return;
    for (uint32_t i = 0; i < g_dxc.nrt; i++)
        if (g_dxc.rt[i] && *(uint64_t **)g_dxc.rt[i] == g_dx_tex_vtbl && DXF(g_dxc.rt[i], 0x28, uint32_t)) {
            log_line("DirectX 11: first drawing into a cube texture (%ux%u, format 0x%x, %u levels): recording this frame from here",
                     DXF(g_dxc.rt[i], 0x14, uint32_t), DXF(g_dxc.rt[i], 0x18, uint32_t), DXF(g_dxc.rt[i], 0x10, uint32_t), DXF(g_dxc.rt[i], 0x0c, uint32_t));
            dx_pass_start(test_bench_seconds());
            return;
        }
}
static void dx_pass_frame_end(void) {
    if (g_dxp.at == -2) { const char *e = getenv("ATTILA_DX11_PASSES"); g_dxp.at = e && strcmp(e, "cube") ? atof(e) : -1; }
    if (g_dxp.state == 1) {
        dx_pass_flush();
        if (g_dxp.f) fclose(g_dxp.f);
        g_dxp.f = NULL; g_dxp.state = 2;
        log_line("DirectX 11: frame recorded: %u passes in %s", g_dxp.pass, g_dxp.dir);
        return;
    }
    if (g_dxp.at < 0 || g_dxp.state == 2) return;
    double t = test_bench_seconds();
    if (t < g_dxp.at) return;
    dx_pass_start(t);
}
static void dx_pass_start(double t) {
    const char *dir = test_out_dir();
    if (!dir) { g_dxp.state = 2; return; }
    snprintf(g_dxp.dir, sizeof g_dxp.dir, "%s\\passes", dir);
    CreateDirectoryA(g_dxp.dir, NULL);
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\passes.txt", g_dxp.dir);
    g_dxp.f = fopen(path, "w");
    if (!g_dxp.f) { g_dxp.state = 2; return; }
    fprintf(g_dxp.f, "frame at benchmark second %.2f\n", t);
    g_dxp.state = 1; g_dxp.pass = 0; g_dxp.draws = 0; g_dxp.nrows = 0; g_dxp.nmade = 0; g_dxp.clears[0] = 0;
}

// Timing wrappers (ATTILA_DX11_CPU=1 only), put in the tables instead of the plain functions.
SYSV static uint8_t dx_t_targets(uint8_t *ctx, uint32_t n, uint8_t **rtvs, uint32_t nuav, void **uavs, uint8_t *dsv) { double t0 = DX_CPU_T0(); uint8_t r = dx_ctx_targets(ctx, n, rtvs, nuav, uavs, dsv); dx_cpu_add(&g_dxcpu.targets, t0, "target change"); return r; }
SYSV static uint8_t dx_t_clear(void *ctx, const void *rect, uint8_t colour, const float *rgba, uint8_t depth, float z, uint8_t stencil, uint32_t s) { double t0 = DX_CPU_T0(); uint8_t r = dx_ctx_clear(ctx, rect, colour, rgba, depth, z, stencil, s); dx_cpu_add(&g_dxcpu.clear, t0, "clear"); return r; }
SYSV static void dx_t_draw(void *ctx, uint32_t n, uint32_t first) { double t0 = DX_CPU_T0(); dx_ctx_draw(ctx, n, first); dx_cpu_add(&g_dxcpu.draw, t0, "draw"); }
SYSV static void dx_t_draw_inst(void *ctx, uint32_t n, uint32_t inst, uint32_t first, uint32_t first_inst) { double t0 = DX_CPU_T0(); dx_ctx_draw_inst(ctx, n, inst, first, first_inst); dx_cpu_add(&g_dxcpu.draw, t0, "draw"); }
SYSV static void dx_t_draw_idx(void *ctx, uint32_t n, uint32_t first, int32_t base) { double t0 = DX_CPU_T0(); dx_ctx_draw_idx(ctx, n, first, base); dx_cpu_add(&g_dxcpu.draw, t0, "draw"); }
SYSV static void dx_t_draw_idx_inst(void *ctx, uint32_t n, uint32_t inst, uint32_t first, int32_t base, uint32_t first_inst) { double t0 = DX_CPU_T0(); dx_ctx_draw_idx_inst(ctx, n, inst, first, base, first_inst); dx_cpu_add(&g_dxcpu.draw, t0, "draw"); }
SYSV static void dx_t_dispatch(void *ctx, uint32_t x, uint32_t y, uint32_t z) { double t0 = DX_CPU_T0(); dx_ctx_dispatch(ctx, x, y, z); dx_cpu_add(&g_dxcpu.compute, t0, "compute run"); }
static void dx_build_context_table(void) {    uint64_t *v = g_dx.ctx_vtbl;    v[0x000 / 8] = (uint64_t)dx_ctx_topology;      v[0x008 / 8] = (uint64_t)dx_ctx_index_buffer;
    v[0x010 / 8] = (uint64_t)dx_ctx_input_layout;  v[0x018 / 8] = (uint64_t)dx_ctx_vertex_buffers;
    v[0x020 / 8] = (uint64_t)dx_ctx_vs;            v[0x028 / 8] = (uint64_t)dx_ctx_samplers;
    v[0x030 / 8] = (uint64_t)dx_ctx_vs_cb;         v[0x038 / 8] = (uint64_t)dx_ctx_vs_res;
    v[0x040 / 8] = (uint64_t)dx_ctx_gs;            v[0x048 / 8] = (uint64_t)dx_ctx_samplers;
    v[0x050 / 8] = (uint64_t)dx_ctx_gs_cb;         v[0x058 / 8] = (uint64_t)dx_ctx_gs_res;
    v[0x0a0 / 8] = (uint64_t)dx_ctx_ps;            v[0x0a8 / 8] = (uint64_t)dx_ctx_samplers;
    v[0x0b0 / 8] = (uint64_t)dx_ctx_ps_cb;         v[0x0b8 / 8] = (uint64_t)dx_ctx_ps_res;
    v[0x0e0 / 8] = (uint64_t)dx_ctx_clear_res;     v[0x0e8 / 8] = (uint64_t)dx_ctx_raster;
    v[0x0f0 / 8] = (uint64_t)dx_ctx_blend;         v[0x0f8 / 8] = (uint64_t)dx_ctx_depth;
    v[0x100 / 8] = (uint64_t)dx_ctx_targets;       v[0x108 / 8] = (uint64_t)dx_ctx_draw;
    v[0x110 / 8] = (uint64_t)dx_ctx_draw_idx;      v[0x118 / 8] = (uint64_t)dx_ctx_draw_idx_inst;
    v[0x120 / 8] = (uint64_t)dx_ctx_draw_inst;     v[0x130 / 8] = (uint64_t)dx_ctx_viewport;
    v[0x140 / 8] = (uint64_t)dx_ctx_scissor;       v[0x148 / 8] = (uint64_t)dx_ctx_clear;
    v[0x170 / 8] = (uint64_t)dx_ctx_flush;
    v[0x0c0 / 8] = (uint64_t)dx_ctx_cs;            v[0x0c8 / 8] = (uint64_t)dx_ctx_samplers;
    v[0x0d0 / 8] = (uint64_t)dx_ctx_cs_cb;         v[0x0d8 / 8] = (uint64_t)dx_ctx_cs_res;
    v[0x128 / 8] = (uint64_t)dx_ctx_dispatch;
    v[0x150 / 8] = (uint64_t)dx_ctx_clear_uav_uint; v[0x158 / 8] = (uint64_t)dx_ctx_clear_uav_float;
    if (g_dx_cpu_on) {
        v[0x100 / 8] = (uint64_t)dx_t_targets;     v[0x148 / 8] = (uint64_t)dx_t_clear;
        v[0x108 / 8] = (uint64_t)dx_t_draw;        v[0x110 / 8] = (uint64_t)dx_t_draw_idx;
        v[0x118 / 8] = (uint64_t)dx_t_draw_idx_inst; v[0x120 / 8] = (uint64_t)dx_t_draw_inst;
        v[0x128 / 8] = (uint64_t)dx_t_dispatch;
    }
}

// ---- texture map / update / copy. Subresource numbers are DirectX 11's (the game's calc_subresource: mip + slice * levels).
static void dx_level_size(uint8_t *t, uint32_t sub, uint32_t *w, uint32_t *h) {
    uint32_t levels = DXF(t, 0x0c, uint32_t), mip = levels ? sub % levels : 0;
    *w = DXF(t, 0x14, uint32_t) >> mip; *h = DXF(t, 0x18, uint32_t) >> mip;
    if (!*w) *w = 1;
    if (!*h) *h = 1;
}
// ---- Textures kept at twice their size on the card (ui_x2.inc, the sharp interface at 4K): DXT_SCALE = 2. The game keeps
// its own size for them (fields +14h / +18h), so all its rectangles stay as they were; here they are doubled. map hands
// the game a buffer of the game's size, and unmap writes it in at twice the size: the sharp picture when ui_x2.inc has one
// waiting for exactly this upload, else every pixel spread over 2x2 with the weights the card's own bilinear stretch uses
// at 2x (9:3:3:1), which looks the same as before. Copies between two such textures are doubled too. Only for plain
// 4-byte-a-pixel textures with one level (the interface's cache pages and its upload texture).
static const uint8_t *g_x2_repl;              // waiting sharp picture, BGRA, (2 * g_x2_repl_w) x (2 * g_x2_repl_h)
static uint32_t g_x2_repl_w, g_x2_repl_h;
static int g_x2_repl_used;
static uint32_t g_x2_written_w, g_x2_written_h;   // the part the game writes into the upload texture next (0 = all of it)
static const uint8_t *g_x2_big;               // sharp picture for a picture larger than 512 (uploaded in pieces), BGRA 2w x 2h
static uint32_t g_x2_big_w, g_x2_big_h;
static volatile long g_x2_made, g_x2_uploads, g_x2_sharp, g_x2_pieces;

static int dx_x2_plain(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_B8G8R8X8_UNORM ||
           f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_TYPELESS || f == DXGI_FORMAT_B8G8R8A8_TYPELESS;
}
// Each source pixel to 2x2, as a bilinear stretch by 2 samples it: output 2i = 1/4 p[i-1] + 3/4 p[i], 2i+1 = 3/4 p[i] + 1/4 p[i+1]
// (edges repeated), in both directions.
static void x2_expand(const uint8_t *s, uint32_t w, uint32_t h, size_t sp, uint8_t *d, size_t dp) {
    for (uint32_t y2 = 0; y2 < 2 * h; y2++) {
        uint32_t ya = y2 / 2, yb = (y2 & 1) ? (ya + 1 < h ? ya + 1 : ya) : (ya ? ya - 1 : 0);
        const uint8_t *ra = s + (size_t)ya * sp, *rb = s + (size_t)yb * sp;
        uint8_t *o = d + (size_t)y2 * dp;
        for (uint32_t x2 = 0; x2 < 2 * w; x2++) {
            uint32_t xa = x2 / 2, xb = (x2 & 1) ? (xa + 1 < w ? xa + 1 : xa) : (xa ? xa - 1 : 0);
            const uint8_t *p00 = ra + xa * 4, *p01 = ra + xb * 4, *p10 = rb + xa * 4, *p11 = rb + xb * 4;
            for (int c = 0; c < 4; c++) o[x2 * 4 + c] = (uint8_t)((p00[c] * 9 + p01[c] * 3 + p10[c] * 3 + p11[c] + 8) >> 4);
        }
    }
}
// The texture's resource made again at twice the size (its content is dropped: done before anything is written into it).
static int dx_x2_make(uint8_t *t) {
    if (!t || *(uint64_t **)t != g_dx_tex_vtbl) return 0;
    if (DXF(t, DXT_SCALE, uint32_t) == 2) return 1;
    ID3D11Texture2D *r = DXF(t, DXT_RES, ID3D11Texture2D *);
    if (!r || DXF(t, 0x0c, uint32_t) != 1) return 0;
    D3D11_TEXTURE2D_DESC d;
    r->lpVtbl->GetDesc(r, &d);
    if (d.ArraySize != 1 || d.SampleDesc.Count != 1 || d.MipLevels != 1 || (d.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) || !dx_x2_plain(d.Format)) return 0;
    d.Width *= 2; d.Height *= 2;
    ID3D11Texture2D *n = NULL;
    if (FAILED(g_dx.d->lpVtbl->CreateTexture2D(g_dx.d, &d, NULL, &n))) return 0;
    dx_tex_release(t);   // views and the old resource
    DXF(t, DXT_RES, ID3D11Resource *) = (ID3D11Resource *)n;
    DXF(t, DXT_SCALE, uint32_t) = 2;
    dx_bound_forget_textures();
    if (InterlockedIncrement(&g_x2_made) <= 12) log_line("sharp interface: texture %ux%u (format 0x%x, usage %u) kept at %ux%u on the card", DXF(t, 0x14, uint32_t), DXF(t, 0x18, uint32_t), DXF(t, 0x10, uint32_t), DXF(t, 0x2c, uint32_t), d.Width, d.Height);
    return 1;
}
// ---- DirectX 11 lets nothing on the card write into a dynamic texture: no copy (CopyResource / CopySubresourceRegion), no
// UpdateSubresource; only the processor writes it, through Map. The game does it anyway: every font's letter page is a
// dynamic texture that SPRITE_FONT::render_kerned (the copy at 30a1f44) fills by a copy from another dynamic texture (the
// font's SYSTEM_MEM_TEXTURE). AMD's driver and Windows' own software DirectX copy anyway; NVIDIA's driver drops it (report
// 2026-11-17, RTX 5050 laptop: no text anywhere). So a dynamic texture is made again as a plain (default) one the first time
// the game writes it that way, with what it holds; from then on the game's map / unmap of it go through a copy in memory and
// UpdateSubresource, as for every plain texture. Answers 0 when it could not be done (the write then goes ahead as before).
static int dx_tex_writable_by_card(uint8_t *t, const char *how) {
    if (!t || *(uint64_t **)t != g_dx_tex_vtbl || DXF(t, 0x2c, uint32_t) != 2 || (DXF(t, DXT_FLAGS, uint32_t) & DXTF_PLAIN)) return 1;
    static int keep = -1;   // test: ATTILA_DX11_KEEP_DYNAMIC=1 leaves them dynamic (the way before 1.3.6)
    if (keep < 0) { keep = getenv("ATTILA_DX11_KEEP_DYNAMIC") != NULL; if (keep) log_line("DirectX 11 test: dynamic textures written by the card stay dynamic (ATTILA_DX11_KEEP_DYNAMIC)"); }
    if (keep) return 0;
    ID3D11Resource *r = DXF(t, DXT_RES, ID3D11Resource *);
    if (!r || DXF(t, 0x08, uint32_t) == 4 || DXF(t, DXT_MAP_KIND, uint32_t)) return 0;   // 3D, or mapped by the game right now
    D3D11_RESOURCE_DIMENSION rd;
    r->lpVtbl->GetType(r, &rd);
    if (rd != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return 0;
    D3D11_TEXTURE2D_DESC d;
    ((ID3D11Texture2D *)r)->lpVtbl->GetDesc((ID3D11Texture2D *)r, &d);
    if (d.Usage != D3D11_USAGE_DYNAMIC) { DXF(t, DXT_FLAGS, uint32_t) |= DXTF_PLAIN; return 1; }
    d.Usage = D3D11_USAGE_DEFAULT; d.CPUAccessFlags = 0;
    ID3D11Texture2D *n = NULL;
    HRESULT hr = g_dx.d->lpVtbl->CreateTexture2D(g_dx.d, &d, NULL, &n);
    if (FAILED(hr)) {
        static int told;
        if (told++ < 5) log_line("DirectX 11: plain twin of a dynamic texture %ux%u (dxgi %u) refused 0x%08lx", d.Width, d.Height, d.Format, (unsigned long)hr);
        return 0;
    }
    g_dx.c->lpVtbl->CopyResource(g_dx.c, (ID3D11Resource *)n, r);   // what it holds so far (the card may read a dynamic texture)
    dx_tex_release(t);   // our views of it and the old texture (views still bound stay alive in DirectX until replaced)
    DXF(t, DXT_RES, ID3D11Resource *) = (ID3D11Resource *)n;
    DXF(t, DXT_FLAGS, uint32_t) |= DXTF_PLAIN;
    dx_bound_forget_textures();
    static volatile LONG told;
    if (InterlockedIncrement(&told) <= 8)
        log_line("DirectX 11: dynamic texture %ux%u (game format 0x%x) written by %s: made again as a plain texture (DirectX 11 allows that only on plain ones; NVIDIA drops it on dynamic ones)",
                 d.Width, d.Height, DXF(t, 0x10, uint32_t), how);
    return 1;
}
// test: ATTILA_DX11_DROP_DYNAMIC_WRITES=1 drops card writes (copies, updates) into a texture that is dynamic on the card, as
// NVIDIA's driver does, to see that case on another card. Answers 1 = drop this one.
static int dx_test_drop_dynamic_write(uint8_t *t) {
    static int on = -1;
    if (on < 0) on = getenv("ATTILA_DX11_DROP_DYNAMIC_WRITES") != NULL;
    if (!on || !t || DXF(t, 0x2c, uint32_t) != 2 || (DXF(t, DXT_FLAGS, uint32_t) & DXTF_PLAIN)) return 0;
    static int told;
    if (told++ < 3) log_line("DirectX 11 test: a card write into a dynamic texture %ux%u (game format 0x%x) dropped, as NVIDIA does (ATTILA_DX11_DROP_DYNAMIC_WRITES)", DXF(t, 0x14, uint32_t), DXF(t, 0x18, uint32_t), DXF(t, 0x10, uint32_t));
    return 1;
}
// Writes w x h pixels of the game's size (pitch sp) in at twice the size, at (2 * x0, 2 * y0). sharp: a (2w) x (2h) BGRA
// block (pitch sharp_pitch) to write instead, or NULL for the bilinear spread of px.
static void dx_x2_write(uint8_t *t, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, const uint8_t *px, size_t sp, const uint8_t *sharp, size_t sharp_pitch) {
    if (!w || !h) return;
    size_t dp = (size_t)w * 8;
    uint8_t *big = malloc(dp * h * 2);
    if (!big) return;
    D3D11_TEXTURE2D_DESC d;
    DXF(t, DXT_RES, ID3D11Texture2D *)->lpVtbl->GetDesc(DXF(t, DXT_RES, ID3D11Texture2D *), &d);
    int rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    if (sharp) {
        for (uint32_t y = 0; y < 2 * h; y++) {
            const uint8_t *s = sharp + (size_t)y * sharp_pitch;
            uint8_t *o = big + (size_t)y * dp;
            if (!rgba) memcpy(o, s, dp);
            else for (uint32_t x = 0; x < 2 * w; x++) { o[x * 4] = s[x * 4 + 2]; o[x * 4 + 1] = s[x * 4 + 1]; o[x * 4 + 2] = s[x * 4]; o[x * 4 + 3] = s[x * 4 + 3]; }
        }
        g_x2_repl_used = 1;
        InterlockedIncrement(&g_x2_sharp);
    } else x2_expand(px, w, h, sp, big, dp);
    InterlockedIncrement(&g_x2_uploads);
    ID3D11Resource *res = DXF(t, DXT_RES, ID3D11Resource *);
    if (DXF(t, 0x2c, uint32_t) == 2 && !(DXF(t, DXT_FLAGS, uint32_t) & DXTF_PLAIN) && !x0 && !y0) {   // dynamic: map it whole (discard) and write the part
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_dx.c->lpVtbl->Map(g_dx.c, res, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            for (uint32_t y = 0; y < 2 * h; y++) memcpy((uint8_t *)m.pData + (size_t)y * m.RowPitch, big + (size_t)y * dp, dp);
            g_dx.c->lpVtbl->Unmap(g_dx.c, res, 0);
        }
    } else {
        if (dx_tex_writable_by_card(t, "an update (sharp interface)")) res = DXF(t, DXT_RES, ID3D11Resource *);
        D3D11_BOX b = {2 * x0, 2 * y0, 0, 2 * (x0 + w), 2 * (y0 + h), 1};
        g_dx.c->lpVtbl->UpdateSubresource(g_dx.c, res, 0, &b, big, (UINT)dp, 0);
    }
    free(big);
}
static uint8_t dx_x2_map(uint8_t *t, uint32_t sub, void **data, uint32_t *row, uint32_t *depth) {
    uint32_t w = DXF(t, 0x14, uint32_t), h = DXF(t, 0x18, uint32_t);
    if (sub) return 0;
    void *p = calloc(1, (size_t)w * h * 4 + 16);
    if (!p) return 0;
    free(DXF(t, DXT_MAP_PTR, void *));
    DXF(t, DXT_MAP_PTR, void *) = p;
    DXF(t, DXT_MAP_SUB, uint32_t) = 0;
    DXF(t, DXT_MAP_KIND, uint32_t) = 3;
    *data = p;
    if (row) *row = w * 4;
    if (depth) *depth = w * 4 * h;
    return 1;
}
// entry_rsp: the stack pointer at the entry of our unmap (dx_tex_unmap_stub), so [entry_rsp] = the return address. A
// picture larger than 512 comes in pieces, each written at 0,0 of the upload texture by UI_TEXTURE_HANDLER::update_region,
// which unmaps at 30a79b0 (return 30a79b2); its own locals then hold the piece: [rsp+f0h] = x offset in the picture,
// [rsp+110h] = first row, [rsp+e8h] = bytes a row (read in the loop at 30a78d0). (A first version took the frame
// address of a C function: Windows builds do not keep it next to the return address, so it never matched.)
static void dx_x2_unmap(uint8_t *t, const uint8_t *entry_rsp) {
    uint8_t *p = DXF(t, DXT_MAP_PTR, uint8_t *);
    uint32_t w = DXF(t, 0x14, uint32_t), h = DXF(t, 0x18, uint32_t);
    if (p && g_x2_big && entry_rsp && *(const uintptr_t *)entry_rsp == 0x30a79b2) {
        const uint8_t *r = entry_rsp + 8;   // update_region's rsp at the call
        uint32_t xo = *(const uint32_t *)(r + 0xf0), yo = *(const uint32_t *)(r + 0x110), pw = *(const uint32_t *)(r + 0xe8) / 4;
        uint32_t ph = g_x2_big_h > yo ? g_x2_big_h - yo : 0;
        if (ph > h) ph = h;
        { static int told; if (told++ < 6) log_line("sharp interface: piece of a %ux%u picture: x %u, y %u, %ux%u", g_x2_big_w, g_x2_big_h, xo, yo, pw, ph); }
        if (pw && ph && pw <= w && xo + pw <= g_x2_big_w && yo + ph <= g_x2_big_h) {
            size_t pitch = (size_t)g_x2_big_w * 8;
            dx_x2_write(t, 0, 0, pw, ph, p, (size_t)w * 4, g_x2_big + (size_t)(2 * yo) * pitch + (size_t)(2 * xo) * 4, pitch);
            InterlockedIncrement(&g_x2_pieces);
            free(p);
            DXF(t, DXT_MAP_PTR, void *) = NULL;
            return;
        }
        static int told;
        if (told++ < 5) log_line("sharp interface: a piece of a large picture not understood (x %u, y %u, %ux%u of %ux%u): stretched", xo, yo, pw, ph, g_x2_big_w, g_x2_big_h);
    }
    uint32_t ww = g_x2_written_w && g_x2_written_w <= w ? g_x2_written_w : w, wh = g_x2_written_h && g_x2_written_h <= h ? g_x2_written_h : h;
    const uint8_t *sharp = g_x2_repl && g_x2_repl_w == ww && g_x2_repl_h == wh ? g_x2_repl : NULL;
    if (p) dx_x2_write(t, 0, 0, ww, wh, p, (size_t)w * 4, sharp, (size_t)ww * 8);
    free(p);
    DXF(t, DXT_MAP_PTR, void *) = NULL;
}

// map(tex, ctx, sub, flags, void **data, uint *row_pitch, uint *depth_pitch, wait) -> bool
SYSV static uint8_t dx_tex_map(uint8_t *t, void *ctx, uint32_t sub, uint32_t flags, void **data, uint32_t *row, uint32_t *depth, uint32_t wait) {
    (void)ctx;
    dx_thread_note(2);
    if (DXF(t, DXT_SCALE, uint32_t) == 2 && data) return dx_x2_map(t, sub, data, row, depth);
    ID3D11Resource *r = DXF(t, DXT_RES, ID3D11Resource *);
    uint32_t usage = DXF(t, 0x2c, uint32_t);
    if (DXF(t, DXT_FLAGS, uint32_t) & DXTF_PLAIN) usage = 0;   // dynamic for the game, plain on the card (dx_tex_writable_by_card)
    if (!r || !data) return 0;
    if (dx_cube_log(t)) log_line("cube log: map %p sub %u, flags %u, usage %u", (void *)t, sub, flags, usage);
    if (usage == 2 || usage == 3) {
        D3D11_MAPPED_SUBRESOURCE m;
        D3D11_MAP kind = usage == 2 ? D3D11_MAP_WRITE_DISCARD : flags == 1 ? D3D11_MAP_READ : flags == 2 ? D3D11_MAP_WRITE : D3D11_MAP_READ_WRITE;
        // `wait` is DirectX 11's own flag: 0x100000 = do not wait. The game's read-back rings (depth analysis for the
        // shadow splits, 30ee8b0) ask that way and try an older texture when this one is not ready yet.
        double t_map = dx_now_ms();
        HRESULT hr = g_dx.c->lpVtbl->Map(g_dx.c, r, sub, kind, g_dx_test_map_wait ? 0 : wait & D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        g_dxs.map_ms += dx_now_ms() - t_map;
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return 0;
        if (FAILED(hr)) { static int told; if (told++ < 5) log_line("DirectX 11: texture map failed 0x%08lx", (unsigned long)hr); return 0; }
        *data = m.pData;
        if (row) *row = m.RowPitch;
        if (depth) *depth = m.DepthPitch;
        DXF(t, DXT_MAP_KIND, uint32_t) = 1;
        return 1;
    }
    uint32_t w, h, rp, rows;   // default usage: a CPU copy of the level, sent by unmap
    dx_level_size(t, sub, &w, &h);
    dx_pitch(dx_format(DXF(t, 0x10, uint32_t)).res, w, h, &rp, &rows);
    void *p = calloc(1, (size_t)rp * rows + 16);
    if (!p) return 0;
    free(DXF(t, DXT_MAP_PTR, void *));
    DXF(t, DXT_MAP_PTR, void *) = p;
    DXF(t, DXT_MAP_SUB, uint32_t) = sub;
    DXF(t, DXT_MAP_KIND, uint32_t) = 2;
    *data = p;
    if (row) *row = rp;
    if (depth) *depth = rp * rows;
    return 1;
}
// unmap(tex, ctx, sub). The table entry is dx_tex_unmap_stub, which adds the stack pointer at entry (for dx_x2_unmap).
__attribute__((used)) SYSV void dx_tex_unmap4(uint8_t *t, void *ctx, uint32_t sub, const uint8_t *entry_rsp);
__attribute__((naked)) static void dx_tex_unmap_stub(void) {
    __asm__ volatile("mov %rsp, %rcx\n\t"
                     "jmp dx_tex_unmap4\n\t");
}
SYSV static void dx_tex_unmap(uint8_t *t, void *ctx, uint32_t sub) {
    dx_tex_unmap4(t, ctx, sub, NULL);
}
__attribute__((used)) SYSV void dx_tex_unmap4(uint8_t *t, void *ctx, uint32_t sub, const uint8_t *entry_rsp) {
    (void)ctx;
    dx_thread_note(2);
    ID3D11Resource *r = DXF(t, DXT_RES, ID3D11Resource *);
    uint32_t kind = DXF(t, DXT_MAP_KIND, uint32_t);
    DXF(t, DXT_MAP_KIND, uint32_t) = 0;
    if (!r) return;
    if (kind == 3) { dx_x2_unmap(t, entry_rsp); return; }
    if (kind == 1) { g_dx.c->lpVtbl->Unmap(g_dx.c, r, sub); return; }
    if (kind == 2 && DXF(t, DXT_MAP_PTR, void *)) {
        uint32_t w, h, rp, rows;
        dx_level_size(t, sub, &w, &h);
        dx_pitch(dx_format(DXF(t, 0x10, uint32_t)).res, w, h, &rp, &rows);
        g_dx.c->lpVtbl->UpdateSubresource(g_dx.c, r, sub, NULL, DXF(t, DXT_MAP_PTR, void *), rp, rp * rows);
        free(DXF(t, DXT_MAP_PTR, void *));
        DXF(t, DXT_MAP_PTR, void *) = NULL;
    }
}
// update_subresource(tex, ctx, sub, box {left, top, front, right, bottom, back} or NULL, data, row pitch)
SYSV static uint8_t dx_tex_update(uint8_t *t, void *ctx, uint32_t sub, const D3D11_BOX *box, const void *data, uint32_t row) {
    (void)ctx;
    dx_thread_note(2);
    ID3D11Resource *r = DXF(t, DXT_RES, ID3D11Resource *);
    if (!r || !data) return 0;
    if (DXF(t, DXT_SCALE, uint32_t) == 2) {   // the game's rectangle, written in at twice the size
        uint32_t x0 = box ? box->left : 0, y0 = box ? box->top : 0;
        uint32_t bw = box ? box->right - box->left : DXF(t, 0x14, uint32_t), bh = box ? box->bottom - box->top : DXF(t, 0x18, uint32_t);
        if (sub) return 0;
        dx_x2_write(t, x0, y0, bw, bh, (const uint8_t *)data, row ? row : bw * 4, NULL, 0);
        return 1;
    }
    if (!row) { uint32_t w, h, rows; dx_level_size(t, sub, &w, &h); if (box) w = box->right - box->left; dx_pitch(dx_format(DXF(t, 0x10, uint32_t)).res, w, h, &row, &rows); }
    if (dx_cube_log(t)) log_line("cube log: update %p sub %u, box %s %u,%u-%u,%u, row %u, first bytes %08x %08x", (void *)t, sub, box ? "yes" : "no", box ? box->left : 0, box ? box->top : 0, box ? box->right : 0, box ? box->bottom : 0, row, ((const uint32_t *)data)[0], ((const uint32_t *)data)[1]);
    if (dx_tex_writable_by_card(t, "an update")) r = DXF(t, DXT_RES, ID3D11Resource *);
    if (dx_test_drop_dynamic_write(t)) return 1;
    double t_up = dx_now_ms();
    g_dx.c->lpVtbl->UpdateSubresource(g_dx.c, r, sub, box, data, row, 0);
    g_dxs.update_ms += dx_now_ms() - t_up;
    return 1;
}
// DirectX 10.1 and later copy between a block-compressed texture and a plain whole-number texture whose pixel is as big
// as one block (64 bits: BC1, BC4; 128 bits: BC2, BC3, BC5, BC6H, BC7). At shader model 5 the game compresses textures
// on the card that way: a shader writes the blocks into R32G32B32A32_UINT, which is then copied into the BC3 texture.
static int dx_copy_bits(DXGI_FORMAT f, int *blocks) {
    *blocks = 0;
    if (f == DXGI_FORMAT_R16G16B16A16_UINT || f == DXGI_FORMAT_R16G16B16A16_SINT || f == DXGI_FORMAT_R32G32_UINT || f == DXGI_FORMAT_R32G32_SINT) return 64;
    if (f == DXGI_FORMAT_R32G32B32A32_UINT || f == DXGI_FORMAT_R32G32B32A32_SINT) return 128;
    *blocks = 1;
    if ((f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC1_UNORM_SRGB) || (f >= DXGI_FORMAT_BC4_TYPELESS && f <= DXGI_FORMAT_BC4_SNORM)) return 64;
    if ((f >= DXGI_FORMAT_BC2_TYPELESS && f <= DXGI_FORMAT_BC3_UNORM_SRGB) || (f >= DXGI_FORMAT_BC5_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM) || (f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB)) return 128;
    *blocks = 0;
    return 0;
}
static int dx_copy_block_cast(DXGI_FORMAT a, DXGI_FORMAT b) {
    int ba, bb, na = dx_copy_bits(a, &ba), nb = dx_copy_bits(b, &bb);
    return na && na == nb && ba != bb;
}
// Both copies answer bool: fill_texture_array (3442c00) gives up on a whole texture array when a copy answers 0.
SYSV static uint8_t dx_tex_copy_region(uint8_t *dst, void *ctx, uint32_t dsub, uint32_t x, uint32_t y, uint32_t z, uint8_t *src, uint32_t ssub, const D3D11_BOX *box);
// copy_resource: as OpenGL's (33da6d0), level by level through copy_subresource_region with the source's level size.
SYSV static uint8_t dx_tex_copy(uint8_t *dst, void *ctx, uint8_t *src) {
    if (dx_cube_log(dst) || dx_cube_log(src)) log_line("cube log: copy all %p <- %p (format 0x%x <- 0x%x)", (void *)dst, (void *)src, DXF(dst, 0x10, uint32_t), src ? DXF(src, 0x10, uint32_t) : 0);
    if (!(DXF(dst, DXT_RES, void *) && src && *(uint64_t **)src == g_dx_tex_vtbl && DXF(src, DXT_RES, void *))) return 0;
    uint32_t levels = DXF(dst, 0x0c, uint32_t), w = DXF(src, 0x14, uint32_t), h = DXF(src, 0x18, uint32_t);
    uint8_t ok = 1;
    dx_tex_writable_by_card(dst, "a copy");
    if (dx_test_drop_dynamic_write(dst)) return 1;
    if (DXF(dst, 0x08, uint32_t) != 4 && DXF(src, 0x08, uint32_t) != 4) {   // two textures of the same make: everything at once
        D3D11_TEXTURE2D_DESC a, b;
        DXF(dst, DXT_RES, ID3D11Texture2D *)->lpVtbl->GetDesc(DXF(dst, DXT_RES, ID3D11Texture2D *), &a);
        DXF(src, DXT_RES, ID3D11Texture2D *)->lpVtbl->GetDesc(DXF(src, DXT_RES, ID3D11Texture2D *), &b);
        if (a.Format == b.Format && a.Width == b.Width && a.Height == b.Height && a.MipLevels == b.MipLevels && a.ArraySize == b.ArraySize && a.SampleDesc.Count == b.SampleDesc.Count) {
            dx_pass_event("copy all", dst, src, "");
            g_dx.c->lpVtbl->CopyResource(g_dx.c, DXF(dst, DXT_RES, ID3D11Resource *), DXF(src, DXT_RES, ID3D11Resource *));
            return 1;
        }
    }
    for (uint32_t l = 0; l < levels; l++) {
        D3D11_BOX b = {0, 0, 0, w >> l ? w >> l : 1, h >> l ? h >> l : 1, 1};
        ok &= dx_tex_copy_region(dst, ctx, l, 0, 0, 0, src, l, &b);
    }
    return ok;
}
// copy_subresource_region(dst, ctx, dst_sub, x, y, z, src, src_sub, box or NULL)
SYSV static uint8_t dx_tex_copy_region(uint8_t *dst, void *ctx, uint32_t dsub, uint32_t x, uint32_t y, uint32_t z, uint8_t *src, uint32_t ssub, const D3D11_BOX *box) {
    (void)ctx;
    if (dx_cube_log(dst) || dx_cube_log(src)) log_line("cube log: copy %p sub %u at %u,%u <- %p sub %u (format 0x%x %ux%u, %u levels), box %s", (void *)dst, dsub, x, y, (void *)src, ssub, src ? DXF(src, 0x10, uint32_t) : 0, src ? DXF(src, 0x14, uint32_t) : 0, src ? DXF(src, 0x18, uint32_t) : 0, src ? DXF(src, 0x0c, uint32_t) : 0, box ? "yes" : "no");
    if (DXF(dst, 0x3c, uint8_t)) { static int told; if (told++ < 30) log_line("DirectX 11: copy region dst %s fmt 0x%x %ux%u lv %u sl %u sub %u at %u,%u <- src %s fmt 0x%x %ux%u lv %u sub %u box %s", *(uint64_t **)dst == g_dx_tex_vtbl ? "ours" : "NOT OURS", DXF(dst, 0x10, uint32_t), DXF(dst, 0x14, uint32_t), DXF(dst, 0x18, uint32_t), DXF(dst, 0x0c, uint32_t), DXF(dst, 0x1c, uint32_t), dsub, x, y, src && *(uint64_t **)src == g_dx_tex_vtbl ? "ours" : "NOT OURS/NULL", src ? DXF(src, 0x10, uint32_t) : 0, src ? DXF(src, 0x14, uint32_t) : 0, src ? DXF(src, 0x18, uint32_t) : 0, src ? DXF(src, 0x0c, uint32_t) : 0, ssub, box ? "yes" : "no"); }
    if (!(DXF(dst, DXT_RES, void *) && src && *(uint64_t **)src == g_dx_tex_vtbl && DXF(src, DXT_RES, void *))) return 0;
    D3D11_BOX x2box;
    if (DXF(dst, DXT_SCALE, uint32_t) == 2 || DXF(src, DXT_SCALE, uint32_t) == 2) {   // ui_x2.inc: textures at twice the size
        if (DXF(src, DXT_SCALE, uint32_t) == 2 && DXF(dst, DXT_SCALE, uint32_t) != 2) dx_x2_make(dst);   // a fresh interface page
        if (DXF(src, DXT_SCALE, uint32_t) != 2 || DXF(dst, DXT_SCALE, uint32_t) != 2) {
            static int told;
            if (told++ < 5) log_line("sharp interface: copy between a texture at twice the size and a normal one (%ux%u <- %ux%u), left out", DXF(dst, 0x14, uint32_t), DXF(dst, 0x18, uint32_t), DXF(src, 0x14, uint32_t), DXF(src, 0x18, uint32_t));
            return 1;
        }
        if (box) { x2box = *box; x2box.left *= 2; x2box.top *= 2; x2box.right *= 2; x2box.bottom *= 2; box = &x2box; }
        x *= 2; y *= 2;
    }
    // DirectX 11 copies a depth texture only whole: no box, to 0,0,0 (anything else is dropped without a word). The
    // game's OpenGL version copies any rectangle; a box that covers the whole level is the same thing.
    int depth = ((DXF(dst, DXT_BIND, uint32_t) | DXF(src, DXT_BIND, uint32_t)) & D3D11_BIND_DEPTH_STENCIL) != 0;
    if (depth && box) {
        uint32_t w, h;
        dx_level_size(src, ssub, &w, &h);
        if (!x && !y && !z && !box->left && !box->top && box->right >= w && box->bottom >= h) box = NULL;
        else { static int told; if (told++ < 5) log_line("DirectX 11 check: partial copy of a depth texture (%u,%u-%u,%u of %ux%u to %u,%u): not possible in DirectX 11, skipped", box->left, box->top, box->right, box->bottom, w, h, x, y); return 1; }
    }
    if (depth && DXF(dst, DXT_RES, void *) != DXF(src, DXT_RES, void *)) {   // same size and kind needed for a depth copy
        D3D11_TEXTURE2D_DESC a, b;
        DXF(dst, DXT_RES, ID3D11Texture2D *)->lpVtbl->GetDesc(DXF(dst, DXT_RES, ID3D11Texture2D *), &a);
        DXF(src, DXT_RES, ID3D11Texture2D *)->lpVtbl->GetDesc(DXF(src, DXT_RES, ID3D11Texture2D *), &b);
        if (a.Format != b.Format || a.Width != b.Width || a.Height != b.Height) {
            static int told;
            if (told++ < 5) log_line("DirectX 11 check: depth copy between different textures: %ux%u dxgi %u <- %ux%u dxgi %u (game formats 0x%x <- 0x%x)", a.Width, a.Height, a.Format, b.Width, b.Height, b.Format, DXF(dst, 0x10, uint32_t), DXF(src, 0x10, uint32_t));
        }
    }
    if (!depth && DXF(dst, 0x08, uint32_t) != 4 && DXF(src, 0x08, uint32_t) != 4) {   // a copy DirectX 11 would drop: formats of different kinds
        DxFmt fa = dx_format(DXF(dst, 0x10, uint32_t)), fb = dx_format(DXF(src, 0x10, uint32_t));
        uint32_t ra, rb, na, nb;
        dx_pitch(fa.res, 4, 4, &ra, &na); dx_pitch(fb.res, 4, 4, &rb, &nb);
        if (fa.res != fb.res && (ra != rb || na != nb) && !dx_copy_block_cast(fa.res, fb.res)) {
            static int told;
            if (told++ < 8) log_line("DirectX 11 check: copy between textures of different kinds (game formats 0x%x <- 0x%x, %ux%u <- %ux%u): DirectX 11 cannot do it", DXF(dst, 0x10, uint32_t), DXF(src, 0x10, uint32_t), DXF(dst, 0x14, uint32_t), DXF(dst, 0x18, uint32_t), DXF(src, 0x14, uint32_t), DXF(src, 0x18, uint32_t));
        }
    }
    dx_tex_writable_by_card(dst, "a copy");   // fonts' letter pages (dynamic) are filled this way
    if (dx_test_drop_dynamic_write(dst)) return 1;
    dx_pass_event(depth ? "copy (depth)" : "copy", dst, src, box ? "part" : "whole");
    g_dx.c->lpVtbl->CopySubresourceRegion(g_dx.c, DXF(dst, DXT_RES, ID3D11Resource *), dsub, x, y, z, DXF(src, DXT_RES, ID3D11Resource *), ssub, box);
    return 1;
}
// resolve_subresource(dst, ctx, dst_sub, src, src_sub, format): a multisampled picture boiled down to a plain one.
// OpenGL's version is an empty stub (it never offers MSAA).
SYSV static uint8_t dx_tex_resolve(uint8_t *dst, void *ctx, uint32_t dsub, uint8_t *src, uint32_t ssub, uint32_t ttf) {
    (void)ctx;
    if (!(DXF(dst, DXT_RES, void *) && src && *(uint64_t **)src == g_dx_tex_vtbl && DXF(src, DXT_RES, void *))) return 0;
    DxFmt f = dx_format(ttf ? ttf : DXF(dst, 0x10, uint32_t));
    DXGI_FORMAT fmt = f.rtv != DXGI_FORMAT_UNKNOWN ? f.rtv : f.srv;
    dx_tex_writable_by_card(dst, "a copy");
    dx_pass_event("resolve", dst, src, "");
    if (DXF(src, DXT_SAMPLES, uint32_t) > 1) g_dx.c->lpVtbl->ResolveSubresource(g_dx.c, DXF(dst, DXT_RES, ID3D11Resource *), dsub, DXF(src, DXT_RES, ID3D11Resource *), ssub, fmt);
    else g_dx.c->lpVtbl->CopySubresourceRegion(g_dx.c, DXF(dst, DXT_RES, ID3D11Resource *), dsub, 0, 0, 0, DXF(src, DXT_RES, ID3D11Resource *), ssub, NULL);
    return 1;
}
// get_multisample_anti_aliasing_support(format): the highest WS_ANTI_ALIASING the card can draw this format with
// (4 = 8 samples, 3 = 4, 2 = 2; 1 = none of them). The game asks only when it runs at shader model level 3.
SYSV static uint32_t dx_msaa_support(uint8_t *dev, uint32_t ttf) {
    (void)dev;
    DxFmt f = dx_format(ttf);
    DXGI_FORMAT fmt = f.dsv != DXGI_FORMAT_UNKNOWN ? f.dsv : f.rtv;
    uint32_t best = 1;
    for (uint32_t aa = 4; aa >= 2 && best == 1 && fmt != DXGI_FORMAT_UNKNOWN && g_dx.d; aa--) {
        UINT q = 0;
        if (SUCCEEDED(g_dx.d->lpVtbl->CheckMultisampleQualityLevels(g_dx.d, fmt, 1u << (aa - 1), &q)) && q) best = aa;
    }
    { static int told; if (told++ < 6) log_line("DirectX 11: anti-aliasing the card can do for format 0x%x: level %u (4 = 8x)", ttf, best); }
    return best;
}
static uint32_t g_dx_next_array = 1;   // (declared above) slice count for the next dx_create_texture_2d (texture arrays)
SYSV static void *dx_create_texture_array_2d(uint8_t *dev, uint32_t slices, uint32_t w, uint32_t h, uint32_t levels, uint32_t fmt,   // order seen in a run: 1, 256, 512, 10 levels
        uint32_t aa, uint32_t misc, uint32_t pool, uint32_t usage, uint32_t bind, uint32_t cpu, uint8_t b1, uint8_t b2, uint8_t *failed, uint8_t b3) {
    g_dx_next_array = slices ? slices : 1;
    { static int told; if (told++ < 25) log_line("DirectX 11: texture array args %u %u %u %u fmt 0x%x aa %u misc %u pool %u usage %u bind 0x%x cpu 0x%x flags %u %u %u", slices, w, h, levels, fmt, aa, misc, pool, usage, bind, cpu, b1, b2, b3); }
    uint8_t cube = slices == 6 && (b1 || b2 || w == h);   /* square 6-slice arrays are cube maps (sky light 256x256 RGBA32F had no flag set) */   /* which flag means cube here is not known: 6 slices + either flag (seen: sky light cube lost its flag) */
    if (cube) g_dx_next_array = 1;
    void *t = dx_create_texture_2d(dev, w, h, levels, fmt, aa, misc, pool, usage, bind, cpu, 0, cube, failed, b3);
    g_dx_next_array = 1;
    if (t && !cube) DXF((uint8_t *)t, 0x3c, uint8_t) = 1;   // made as an array: always viewed as an array, even with one slice
    return t;
}
// create_texture_3D(dev, data, u1, offset, IMAGE_INFO& (+4 width, +8 height), levels | stack: format, aa, u, pool, usage, bind,
// cpu, b1, depth, bool &failed, b2) (OpenGL: 33d7670). Only level 0 is in the data; one level is made.
SYSV static void *dx_create_texture_3d(uint8_t *dev, const uint8_t *data, uint32_t u1, uint32_t offset, const uint8_t *info, uint32_t levels,
        uint32_t fmt, uint32_t aa, uint32_t u, uint32_t pool, uint32_t usage, uint32_t bind, uint32_t cpu, uint8_t b1, uint32_t depth,
        uint8_t *failed, uint8_t b2) {
    (void)u1; (void)levels; (void)aa; (void)u; (void)pool; (void)bind; (void)b1; (void)b2;
    uint32_t w = DXF(info, 4, uint32_t), h = DXF(info, 8, uint32_t), row, rows;
    DxFmt f = dx_format(fmt);
    if (failed) *failed = 0;
    if (f.res == DXGI_FORMAT_UNKNOWN || !w || !h || !depth) {
        log_line("DirectX 11: 3D texture not made: fmt 0x%x %ux%ux%u", fmt, w, h, depth);
        if (failed) *failed = 1;
        return NULL;
    }
    dx_pitch(f.res, w, h, &row, &rows);
    D3D11_TEXTURE3D_DESC d = {w, h, depth, 1, f.res, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    D3D11_SUBRESOURCE_DATA sd = {data ? data + offset : NULL, row, row * rows};
    ID3D11Texture3D *tex = NULL;
    HRESULT hr = g_dx.d->lpVtbl->CreateTexture3D(g_dx.d, &d, data ? &sd : NULL, &tex);
    if (FAILED(hr)) { log_line("DirectX 11: 3D texture %ux%ux%u fmt 0x%x failed 0x%08lx", w, h, depth, fmt, (unsigned long)hr); if (failed) *failed = 1; return NULL; }
    uint8_t *t = GAME_MALLOC(DXT_SIZE, 0);
    memset(t, 0, DXT_SIZE);
    *(uint64_t **)t = g_dx_tex_vtbl;
    DXF(t, 0x08, uint32_t) = 4; DXF(t, 0x0c, uint32_t) = 1; DXF(t, 0x10, uint32_t) = fmt;
    DXF(t, 0x14, uint32_t) = w; DXF(t, 0x18, uint32_t) = h; DXF(t, 0x1c, uint32_t) = depth; DXF(t, 0x20, uint8_t *) = dev;
    DXF(t, 0x2c, uint32_t) = usage; DXF(t, 0x30, uint32_t) = cpu; DXF(t, 0x34, uint8_t) = 1;
    DXF(t, DXT_RES, ID3D11Resource *) = (ID3D11Resource *)tex;
    DXF(t, DXT_BIND, uint32_t) = D3D11_BIND_SHADER_RESOURCE;
    return t;
}
// ---- queries. One layout for all kinds (OpenGL vtables 3c87040 etc.): begin, end, flush, flush_wait, valid,
// recreated_mid_query, dtor, deleting dtor, create_resource, get_data(T &out, bool wait), pf_on_destroy, pf_on_create,
// getTargetGL. Occlusion = a real D3D11 occlusion query; the timing kinds only feed the game's statistics.
enum { DXQ_OCCLUSION = 0, DXQ_EVENT, DXQ_TIMESTAMP, DXQ_DISJOINT, DXQ_FREQ };
typedef struct { uint64_t *vtbl; uint32_t kind, active; ID3D11Query *q; uint64_t pad[4]; } DxQuery;
static uint64_t g_dx_query_vtbl[13];
static LONG g_dxq_begins, g_dxq_ends, g_dxq_ends_skipped, g_dxq_made;   // test counts (ATTILA_DX11_QUERY_STATS)
SYSV static void dx_q_begin(DxQuery *q) { if (q->q && q->kind == DXQ_OCCLUSION) { g_dx.c->lpVtbl->Begin(g_dx.c, (ID3D11Asynchronous *)q->q); q->active = 1; InterlockedIncrement(&g_dxq_begins); } }
SYSV static void dx_q_end(DxQuery *q) {
    if (q->q && (q->kind != DXQ_OCCLUSION || q->active)) { g_dx.c->lpVtbl->End(g_dx.c, (ID3D11Asynchronous *)q->q); if (q->kind == DXQ_OCCLUSION) InterlockedIncrement(&g_dxq_ends); }
    else if (q->kind == DXQ_OCCLUSION) InterlockedIncrement(&g_dxq_ends_skipped);
    q->active = 0;
}
SYSV static void dx_q_none(DxQuery *q) { (void)q; }
SYSV static uint8_t dx_q_true(DxQuery *q) { (void)q; return 1; }
SYSV static uint8_t dx_q_false(DxQuery *q) { (void)q; return 0; }
SYSV static void dx_q_dtor(DxQuery *q) { DXREL(q->q); }
SYSV static void dx_q_delete(DxQuery *q) { DXREL(q->q); GAME_FREE(q); }
// Test (ATTILA_DX11_QUERY_STATS=1): every 5 s, how many occlusion query answers the game got and how many saw pixels.
// The battle line of sight (WARSCAPE::VISIBILITY_NODE::process_queries) is made of these answers.
static int g_dxq_stats_on = -1;
static LONG g_dxq_got, g_dxq_seen, g_dxq_waits, g_dxq_errs;
static HRESULT g_dxq_last_err;
static DWORD g_dxq_get_thread;
static void dx_q_stats(HRESULT hr, UINT64 n) {
    if (g_dxq_stats_on < 0) g_dxq_stats_on = getenv("ATTILA_DX11_QUERY_STATS") != NULL;
    if (!g_dxq_stats_on) return;
    g_dxq_get_thread = GetCurrentThreadId();
    if (hr == S_OK) { InterlockedIncrement(&g_dxq_got); if (n) InterlockedIncrement(&g_dxq_seen); }
    else if (hr == S_FALSE) InterlockedIncrement(&g_dxq_waits);
    else { InterlockedIncrement(&g_dxq_errs); g_dxq_last_err = hr; }
}
static void dx_q_stats_flush(void) {   // called once a frame from dx_present
    static ULONGLONG last;
    if (g_dxq_stats_on < 0) g_dxq_stats_on = getenv("ATTILA_DX11_QUERY_STATS") != NULL;
    if (!g_dxq_stats_on) return;
    ULONGLONG now = GetTickCount64();
    if (!last) last = now;
    if (now - last >= 5000) {
        last = now;
        log_line("DirectX 11: occlusion queries in the last 5 s: %ld answered, %ld of them saw pixels, %ld asks before the answer was ready, %ld errors (last 0x%08lx); %ld made, %ld begun, %ld ended, %ld ends without a begin; answers read on thread %lu, drawing thread %lu",
                 InterlockedExchange(&g_dxq_got, 0), InterlockedExchange(&g_dxq_seen, 0), InterlockedExchange(&g_dxq_waits, 0), InterlockedExchange(&g_dxq_errs, 0),
                 (unsigned long)g_dxq_last_err, InterlockedExchange(&g_dxq_made, 0), InterlockedExchange(&g_dxq_begins, 0), InterlockedExchange(&g_dxq_ends, 0),
                 InterlockedExchange(&g_dxq_ends_skipped, 0), (unsigned long)g_dxq_get_thread, (unsigned long)GetCurrentThreadId());
    }
}
SYSV static uint8_t dx_q_get(DxQuery *q, void *out, uint8_t wait) {
    if (!out) return 0;
    switch (q->kind) {
    case DXQ_OCCLUSION: {
        UINT64 n = 0;
        if (!q->q) { *(uint32_t *)out = 1000000; return 1; }
        for (;;) {
            HRESULT hr = g_dx.c->lpVtbl->GetData(g_dx.c, (ID3D11Asynchronous *)q->q, &n, sizeof n, 0);
            if (hr == S_OK) { dx_q_stats(hr, n); *(uint32_t *)out = n > 0xffffffffull ? 0xffffffffu : (uint32_t)n; return 1; }
            if (hr != S_FALSE || !wait) { dx_q_stats(hr, 0); return 0; }
            YieldProcessor();
        }
    }
    case DXQ_EVENT: case DXQ_DISJOINT: *(uint8_t *)out = q->kind == DXQ_EVENT; return 1;
    case DXQ_TIMESTAMP: { LARGE_INTEGER t; QueryPerformanceCounter(&t); *(uint64_t *)out = (uint64_t)t.QuadPart; return 1; }
    default: { LARGE_INTEGER f; QueryPerformanceFrequency(&f); *(uint64_t *)out = (uint64_t)f.QuadPart; return 1; }
    }
}
SYSV static void dx_q_platform(DxQuery *q, uint32_t e) { (void)q; (void)e; }
SYSV static uint32_t dx_q_target(DxQuery *q) { (void)q; return 0; }
static DxQuery *dx_new_query(uint32_t kind) {
    DxQuery *q = GAME_MALLOC(sizeof(DxQuery), 0);
    if (!q) return NULL;
    memset(q, 0, sizeof *q);
    q->vtbl = g_dx_query_vtbl; q->kind = kind;
    if (kind == DXQ_OCCLUSION) { D3D11_QUERY_DESC d = {D3D11_QUERY_OCCLUSION, 0}; g_dx.d->lpVtbl->CreateQuery(g_dx.d, &d, &q->q); InterlockedIncrement(&g_dxq_made); }
    return q;
}
SYSV static void *dx_create_occlusion_query(uint8_t *dev) { (void)dev; return dx_new_query(DXQ_OCCLUSION); }
SYSV static void *dx_create_event_query(uint8_t *dev) { (void)dev; return dx_new_query(DXQ_EVENT); }
SYSV static void *dx_create_timestamp_query(uint8_t *dev) { (void)dev; return dx_new_query(DXQ_TIMESTAMP); }
SYSV static void *dx_create_disjoint_query(uint8_t *dev) { (void)dev; return dx_new_query(DXQ_DISJOINT); }
SYSV static void *dx_create_freq_query(uint8_t *dev) { (void)dev; return dx_new_query(DXQ_FREQ); }
static void dx_build_query_table(void) {
    uint64_t *v = g_dx_query_vtbl;
    v[0] = (uint64_t)dx_q_begin; v[1] = (uint64_t)dx_q_end; v[2] = (uint64_t)dx_q_none; v[3] = (uint64_t)dx_q_none;
    v[4] = (uint64_t)dx_q_true; v[5] = (uint64_t)dx_q_false; v[6] = (uint64_t)dx_q_dtor; v[7] = (uint64_t)dx_q_delete;
    v[8] = (uint64_t)dx_q_true; v[9] = (uint64_t)dx_q_get; v[10] = (uint64_t)dx_q_platform; v[11] = (uint64_t)dx_q_none;
    v[12] = (uint64_t)dx_q_target;
    g_dx.dev_vtbl[0x2c8 / 8] = (uint64_t)dx_create_occlusion_query;
    g_dx.dev_vtbl[0x2d0 / 8] = (uint64_t)dx_create_event_query;
    g_dx.dev_vtbl[0x2d8 / 8] = (uint64_t)dx_create_timestamp_query;
    g_dx.dev_vtbl[0x2e0 / 8] = (uint64_t)dx_create_disjoint_query;
    g_dx.dev_vtbl[0x2e8 / 8] = (uint64_t)dx_create_freq_query;
}
SYSV static void dx_tex_mips(uint8_t *t) {   // generate_manual_mip_chain
    uint32_t none = 0;
    ID3D11ShaderResourceView *v = (ID3D11ShaderResourceView *)dx_tex_view(t, 2, &none, 4);
    if (dx_cube_log(t)) log_line("cube log: mip levels made for %p (%u levels, bind 0x%x, view %p)", (void *)t, DXF(t, 0x0c, uint32_t), DXF(t, DXT_BIND, uint32_t), (void *)v);
    if (v && (DXF(t, DXT_BIND, uint32_t) & D3D11_BIND_RENDER_TARGET) && DXF(t, 0x0c, uint32_t) != 1) { g_dx.c->lpVtbl->GenerateMips(g_dx.c, v); return; }
    static volatile LONG told;   // test 2026-11-14 (campaign water specks): which textures ask for levels we do not make
    if (InterlockedIncrement(&told) <= 30)
        log_line("DirectX 11 check: mip levels asked for a texture that is not drawn into: %ux%u, format 0x%x, %u levels, bind 0x%x, view %p",
                 DXF(t, 0x14, uint32_t), DXF(t, 0x18, uint32_t), DXF(t, 0x10, uint32_t), DXF(t, 0x0c, uint32_t), DXF(t, DXT_BIND, uint32_t), (void *)v);
}
SYSV static uint64_t dx_zero(void) { return 0; }
SYSV static uint8_t dx_t_copy(uint8_t *dst, void *ctx, uint8_t *src) { double t0 = DX_CPU_T0(); uint8_t r = dx_tex_copy(dst, ctx, src); dx_cpu_add(&g_dxcpu.copy, t0, "texture copy"); return r; }
SYSV static uint8_t dx_t_copy_region(uint8_t *dst, void *ctx, uint32_t dsub, uint32_t x, uint32_t y, uint32_t z, uint8_t *src, uint32_t ssub, const D3D11_BOX *box) { double t0 = DX_CPU_T0(); uint8_t r = dx_tex_copy_region(dst, ctx, dsub, x, y, z, src, ssub, box); dx_cpu_add(&g_dxcpu.copy, t0, "texture part copy"); return r; }
SYSV static uint8_t dx_t_resolve(uint8_t *dst, void *ctx, uint32_t dsub, uint8_t *src, uint32_t ssub, uint32_t ttf) { double t0 = DX_CPU_T0(); uint8_t r = dx_tex_resolve(dst, ctx, dsub, src, ssub, ttf); dx_cpu_add(&g_dxcpu.copy, t0, "resolve"); return r; }
SYSV static uint8_t dx_t_map(uint8_t *t, void *ctx, uint32_t sub, uint32_t flags, void **data, uint32_t *row, uint32_t *depth, uint32_t wait) { double t0 = DX_CPU_T0(); uint8_t r = dx_tex_map(t, ctx, sub, flags, data, row, depth, wait); dx_cpu_add(&g_dxcpu.map, t0, "texture map"); return r; }
SYSV static uint8_t dx_t_q_get(DxQuery *q, void *out, uint8_t wait) { double t0 = DX_CPU_T0(); uint8_t r = dx_q_get(q, out, wait); dx_cpu_add(&g_dxcpu.query, t0, wait ? "query answer (waited for)" : "query answer"); return r; }
static void dx_build_texture_slots(void) {    g_dx_tex_vtbl[0x30 / 8] = (uint64_t)dx_tex_copy;
    g_dx_tex_vtbl[0x38 / 8] = (uint64_t)dx_tex_copy_region;
    g_dx_tex_vtbl[0x40 / 8] = (uint64_t)dx_tex_resolve;
    g_dx.dev_vtbl[0x0b8 / 8] = (uint64_t)dx_msaa_support;
    g_dx_tex_vtbl[0x48 / 8] = (uint64_t)dx_tex_update;
    g_dx_tex_vtbl[0x50 / 8] = (uint64_t)dx_tex_map;
    g_dx_tex_vtbl[0x58 / 8] = (uint64_t)dx_tex_unmap_stub;
    g_dx_tex_vtbl[0x80 / 8] = (uint64_t)dx_tex_mips;
    if (g_dx_cpu_on) {
        g_dx_tex_vtbl[0x30 / 8] = (uint64_t)dx_t_copy; g_dx_tex_vtbl[0x38 / 8] = (uint64_t)dx_t_copy_region;
        g_dx_tex_vtbl[0x40 / 8] = (uint64_t)dx_t_resolve; g_dx_tex_vtbl[0x50 / 8] = (uint64_t)dx_t_map;
    }
    dx_build_query_table();
    if (g_dx_cpu_on) g_dx_query_vtbl[9] = (uint64_t)dx_t_q_get;
    g_dx.dev_vtbl[0x1c0 / 8] = (uint64_t)dx_create_texture_array_2d;
    g_dx.dev_vtbl[0x1d8 / 8] = (uint64_t)dx_create_texture_3d;
    g_dx.dev_vtbl[0x1e8 / 8] = GL_DEV_VTBL[0x1e8 / 8];   // create_texture_from_file_2D: reads the file, then our 0x1b0 + 0x1e0
    g_dx.dev_vtbl[0x2f8 / 8] = (uint64_t)dx_zero;   // gather_device_info: statistics only
    g_dx.dev_vtbl[0x300 / 8] = (uint64_t)dx_zero;   // get_device_footprint: memory statistics
}

// The game draws into its own back buffer texture (as on OpenGL, where present copies it to the window).
static int dx_create_back_buffer(uint8_t *dev, uint32_t w, uint32_t h) {
    uint8_t failed = 0;
    uint8_t *bb = dx_create_texture_2d(dev, w, h, 1, 0x2b, 0, 0, 0, 0, 0x28, 0, 0, 0, &failed, 0);
    uint8_t *depth = dx_create_texture_2d(dev, w, h, 1, 0x1c, 0, 0, 0, 0, 0, 0, 0, 0, &failed, 0);
    if (!bb || !depth) return 0;
    typedef SYSV void *(*RtvFn)(uint8_t *, uint8_t *, const void *);
    struct { uint32_t format, dimension, a, b, c; } rd = {0x2b, 4, 0, 0, 1};   // as create_back_buffer_resources 33d62c0 passes it
    void *rtv = ((RtvFn)GL_DEV_VTBL[0x258 / 8])(dev, bb, &rd);                 // the game's own view object (no OpenGL in it)
    DXF(dev, 0x1618, uint8_t *) = bb;
    DXF(dev, 0x1620, uint8_t *) = depth;
    DXF(dev, 0x1628, void *) = rtv;
    DXF(dev, 0x1630, void *) = rtv;
    return rtv != NULL;
}

// ---- format support questions (OpenGL answers them from a table it fills from the driver; we ask DirectX 11).
static UINT dx_fmt_support(uint32_t ttf) {
    DxFmt f = dx_format(ttf);
    UINT s = 0, s2 = 0;
    if (f.res == DXGI_FORMAT_UNKNOWN || !g_dx.d) return 0;
    if (f.dsv != DXGI_FORMAT_UNKNOWN) { g_dx.d->lpVtbl->CheckFormatSupport(g_dx.d, f.dsv, &s); g_dx.d->lpVtbl->CheckFormatSupport(g_dx.d, f.srv, &s2); return s | s2; }
    g_dx.d->lpVtbl->CheckFormatSupport(g_dx.d, f.srv, &s);
    return s;
}
SYSV static uint8_t dx_is_depth_fmt(uint8_t *dev, uint32_t t) { (void)dev; return (dx_fmt_support(t) & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL) != 0; }
SYSV static uint8_t dx_is_rt_fmt(uint8_t *dev, uint32_t t) { (void)dev; return (dx_fmt_support(t) & D3D11_FORMAT_SUPPORT_RENDER_TARGET) != 0; }
SYSV static uint8_t dx_is_mipgen_fmt(uint8_t *dev, uint32_t t) { (void)dev; return (dx_fmt_support(t) & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN) != 0; }
SYSV static uint8_t dx_is_filter_fmt(uint8_t *dev, uint32_t t) { (void)dev; return (dx_fmt_support(t) & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) != 0; }
SYSV static uint8_t dx_is_tex_fmt(uint8_t *dev, uint32_t t) { (void)dev; return (dx_fmt_support(t) & D3D11_FORMAT_SUPPORT_TEXTURE2D) != 0; }
SYSV static uint8_t dx_is_vtf_fmt(uint8_t *dev, uint32_t t) { (void)dev; return (dx_fmt_support(t) & D3D11_FORMAT_SUPPORT_SHADER_LOAD) != 0; }
SYSV static uint32_t dx_supported_fmt(uint8_t *dev, uint32_t t) {   // a format the card has; the 24-bit ones become 32-bit
    (void)dev;
    if (t == 0x04 || t == 0x36) return 0x07;
    if (t == 0x05) return 0x09;
    if (t == 0x32) return 0x16;
    return t;
}

// Slots that only read or write the device's own fields stay on the game's code (checked in gl_layer_dis.txt / slots_gl.txt).
static const uint16_t DX_DEV_KEEP[] = {
    0x010, 0x018, 0x020, 0x028, 0x030, 0x038, 0x040, 0x048, 0x050, 0x058, 0x060, 0x068, 0x070, 0x078, 0x080, 0x088, 0x090,
    0x098, 0x0a0, 0x0a8, 0x0b0, 0x0b8, 0x0f8, 0x108, 0x110, 0x118, 0x120, 0x130, 0x138, 0x140, 0x148, 0x150, 0x158, 0x170,
    0x178, 0x188, 0x1f0, 0x1f8, 0x200, 0x248, 0x250, 0x258, 0x260, 0x268, 0x270, 0x278, 0x290, 0x298, 0x2a0, 0x2b8, 0x2c0};
static const uint16_t DX_TEX_KEEP[] = {0x010, 0x018, 0x020, 0x028, 0x040, 0x068, 0x070, 0x078, 0x088, 0x090, 0x098, 0x0a8};
static const uint16_t DX_CTX_KEEP[] = {
    0x060, 0x068, 0x070, 0x078, 0x080, 0x088, 0x090, 0x098, 0x0c0, 0x0c8, 0x0d0, 0x0d8, 0x128, 0x138, 0x150, 0x158, 0x160,
    0x168, 0x178, 0x180, 0x188};

SYSV static void dx_destroy(uint8_t *dev);
SYSV static void dx_device_dtor(uint8_t *dev);
static void dx_build_tables(void) {
    for (int i = 0; i < DX_DEV_SLOTS; i++) g_dx.dev_vtbl[i] = (uint64_t)dx_trap_thunk(0, (uint32_t)i);
    for (int i = 0; i < DX_CTX_SLOTS; i++) g_dx.ctx_vtbl[i] = (uint64_t)dx_trap_thunk(1, (uint32_t)i);
    for (size_t i = 0; i < sizeof DX_DEV_KEEP / sizeof *DX_DEV_KEEP; i++) g_dx.dev_vtbl[DX_DEV_KEEP[i] / 8] = GL_DEV_VTBL[DX_DEV_KEEP[i] / 8];
    for (size_t i = 0; i < sizeof DX_CTX_KEEP / sizeof *DX_CTX_KEEP; i++) g_dx.ctx_vtbl[DX_CTX_KEEP[i] / 8] = GL_CTX_VTBL[DX_CTX_KEEP[i] / 8];
    for (int i = 0; i < DX_TEX_SLOTS; i++) g_dx_tex_vtbl[i] = (uint64_t)dx_trap_thunk(2, (uint32_t)i);
    for (size_t i = 0; i < sizeof DX_TEX_KEEP / sizeof *DX_TEX_KEEP; i++) g_dx_tex_vtbl[DX_TEX_KEEP[i] / 8] = GL_TEX2D_VTBL[DX_TEX_KEEP[i] / 8];
    g_dx_tex_vtbl[0] = (uint64_t)dx_tex_dtor;
    g_dx_tex_vtbl[1] = (uint64_t)dx_tex_delete;
    (void)dx_make_back_buffer_view;
    g_dx.dev_vtbl[0x1b8 / 8] = (uint64_t)dx_create_texture_2d;
    dx_build_buffer_tables();
    dx_build_cb_table();
    dx_build_shader_tables();
    dx_build_context_table();
    dx_build_compute_tables();
    dx_build_texture_slots();
    for (int i = 0x228 / 8; i <= 0x240 / 8; i++) g_dx.dev_vtbl[i] = GL_DEV_VTBL[i];   // state objects: plain DirectX 11 descs, no OpenGL
    g_dx.dev_vtbl[0x218 / 8] = (uint64_t)dx_create_constant_buffer;
    g_dx.dev_vtbl[0x1b0 / 8] = GL_DEV_VTBL[0x1b0 / 8];   // get_texture_info_from_file_in_memory: image header only, no OpenGL
    g_dx.dev_vtbl[0x1e0 / 8] = (uint64_t)dx_create_texture_from_memory_2d;
    g_dx.dev_vtbl[0x208 / 8] = (uint64_t)dx_create_index_buffer;
    g_dx.dev_vtbl[0x210 / 8] = (uint64_t)dx_create_vertex_buffer;
    g_dx.dev_vtbl[0x0f0 / 8] = (uint64_t)dx_create_device;
    g_dx.dev_vtbl[0x0c0 / 8] = (uint64_t)dx_is_depth_fmt;
    g_dx.dev_vtbl[0x0c8 / 8] = (uint64_t)dx_is_rt_fmt;
    g_dx.dev_vtbl[0x0d0 / 8] = (uint64_t)dx_is_mipgen_fmt;
    g_dx.dev_vtbl[0x0d8 / 8] = (uint64_t)dx_is_filter_fmt;
    g_dx.dev_vtbl[0x0e0 / 8] = (uint64_t)dx_is_tex_fmt;
    g_dx.dev_vtbl[0x0e8 / 8] = (uint64_t)dx_is_vtf_fmt;
    g_dx.dev_vtbl[0x1c8 / 8] = (uint64_t)dx_supported_fmt;
    g_dx.dev_vtbl[0x100 / 8] = (uint64_t)dx_destroy_screen; // destroy_device: a new screen mode, the device stays
    g_dx.dev_vtbl[0x128 / 8] = (uint64_t)dx_destroy;        // destroy
    g_dx.dev_vtbl[0x000 / 8] = (uint64_t)dx_device_dtor;
    g_dx.dev_vtbl[0x008 / 8] = (uint64_t)dx_device_dtor;
    g_dx.dev_vtbl[0x280 / 8] = (uint64_t)dx_end_scene;
    g_dx.dev_vtbl[0x288 / 8] = (uint64_t)dx_present;
}

// destroy() / destroy_device(): what OpenGL's does (33d66a0: the back buffer's view, texture and depth texture are
// deleted and their places emptied), then everything DirectX: the state objects made from the game's descriptions, the
// swap chain, the context and the device. The game calls this when it ends, and a new create_device may follow (the
// window is free again for a new swap chain). Seen 2026-11-12 at game exit as two "not done yet" lines.
static void dx_unbind_all(void) {   // nothing of the old screen stays bound; our memos of what is bound start again
    g_dx.c->lpVtbl->ClearState(g_dx.c);
    memset(&g_dxb, 0, sizeof g_dxb);
    memset(&g_dxc, 0, sizeof g_dxc);
    memset(&g_dxset, 0, sizeof g_dxset);
    g_dx_cur_layout = NULL;
}
SYSV static void dx_destroy(uint8_t *dev) {
    if (!g_dx.d) return;
    g_dx.c->lpVtbl->ClearState(g_dx.c);
    static const uint16_t objs[3] = {0x1628, 0x1618, 0x1620};
    for (int i = 0; i < 3; i++) {
        void *o = DXF(dev, objs[i], void *);
        if (o) ((SYSV void (*)(void *))(*(void ***)o)[1])(o);
        DXF(dev, objs[i], void *) = NULL;
    }
    DXF(dev, 0x1630, void *) = NULL;
    for (uint32_t i = 0; i < 8192; i++) if (g_dx_cache[i].obj) ((IUnknown *)g_dx_cache[i].obj)->lpVtbl->Release((IUnknown *)g_dx_cache[i].obj);
    memset(g_dx_cache, 0, sizeof g_dx_cache);
    memset(&g_dxb, 0, sizeof g_dxb);
    memset(&g_dxc, 0, sizeof g_dxc);
    memset(&g_dxset, 0, sizeof g_dxset);
    memset(g_dx_samp_memo, 0, sizeof g_dx_samp_memo);
    g_dx_cur_layout = NULL;
    DXREL(g_dx.bb_rtv);
    DXREL(g_dx_c1);
    DXREL(g_dx.sc);
    g_dx.c->lpVtbl->Flush(g_dx.c);
    DXREL(g_dx.c);
    DXREL(g_dx.d);
    log_line("DirectX 11: device closed");
}
SYSV static void dx_device_dtor(uint8_t *dev) { dx_destroy(dev); }   // the object itself is left: the game is ending

// Replaces create_opengl_render_device: same object, our tables.
SYSV static void *dx_create_render_device(void) {
    uint8_t *dev = GAME_MALLOC(0x16d8, 0);
    if (!dev) return NULL;
    GL_DEVICE_CTOR(dev);
    dx_build_tables();
    *(uint64_t **)dev = g_dx.dev_vtbl;
    *(uint64_t **)(dev + 0x88) = g_dx.ctx_vtbl;
    g_dx.dev = dev;
    log_line("DirectX 11: device object made (test build: missing parts are listed as \"not done yet\")");
    return dev;
}

// Battle line of sight on the processor while drawing with DirectX 11 (2026-11-17, player report: "units hidden until they
// attack", "the AI couldn't see me either"). The game works out who sees whom in a battle (EMPIREBATTLE::VISIBILITY_VIEW)
// either on the card (setting enable_battle_visibility_view_gpu_acceleration, on in the Linux game: WARSCAPE::VISIBILITY_NODE
// draws each unit's view and counts pixels with occlusion queries) or on the processor (the game's own software rasteriser,
// VISIBILITY_VIEW_TBB_TASK -> draw_from_unit, on a helper thread). With our DirectX 11 device the card version never finishes
// its first round (update_round_finished waits for answers that never come), so the battle never gets a
// BCQ_BATTLE_VISIBILITY_PER_UNIT: nobody sees anybody until they touch. Measured in the benchmark (57 s): card version 0
// line-of-sight updates, processor version 23 (OpenGL with the card version 25+); 134 against 143 fps. The updates go to
// every PC as battle commands, so PCs on different graphics routes stay in step. ATTILA_CARD_LINE_OF_SIGHT=1 keeps the card
// version (for work on the DirectX 11 cause; ATTILA_DX11_QUERY_STATS=1 counts the occlusion query answers).
static void dx11_game_settings(void) {
    if (!g_want_dx11 || !g_dx.on) return;
    static const uint8_t los_read[6] = {0x8A, 0x15, 0xE8, 0x95, 0x13, 0x04};   // dc5cfc: mov dl,[enable_battle_visibility_view_gpu_acceleration+62h]
    if (getenv("ATTILA_CARD_LINE_OF_SIGHT")) { log_line("setting: battle line of sight on the graphics card (test switch ATTILA_CARD_LINE_OF_SIGHT)"); return; }
    if (memcmp((void *)0xdc5cfc, los_read, 6)) { log_line("setting skipped, game code differs: battle line of sight on the processor"); return; }
    *(volatile uint8_t *)0x4eff2ea = 0;
    log_line("setting: battle line of sight worked out on the processor (with DirectX 11 the graphics card version never finished a round)");
}

static void patch_dx11(void) {
    if (!g_want_dx11) return;   // set by setup_graphics_route (graphics_choice.inc): the start window's choice, or ATTILA_DX11=1 for tests
    static const uint8_t old[9] = {0x41, 0x56, 0x53, 0x50, 0xbf, 0xd8, 0x16, 0x00, 0x00};   // push r14/rbx/rax; mov edi, 0x16d8
    if (!hook_entry(0x33d4220, old, 9, (void *)dx_create_render_device)) { log_line("patch skipped, game code differs: DirectX 11 device"); return; }
    g_dx.on = 1;
    g_dx_audit_on = getenv("ATTILA_DX11_AUDIT") != NULL;
    {   // Level 3 = the game's full DirectX 11 path (shader model 5), the default since 2026-11-12: MSAA 2x / 4x / 8x in the
        // game's own options, particles sorted by compute shaders, no depth copies. Level 1 = what OpenGL tells the game
        // (shader model 4): settings.txt dx11_mode=basic, or ATTILA_DX11_SM=0..3 for tests.
        extern int settings_dx11_basic(void);
        const char *sm = getenv("ATTILA_DX11_SM");
        g_dx_sm = settings_dx11_basic() ? 1 : 3;
        if (sm && sm[0] >= '0' && sm[0] <= '3' && !sm[1]) g_dx_sm = (uint32_t)(sm[0] - '0');
        log_line(g_dx_sm == 3 ? "DirectX 11: full mode (the game's shader model 5 path)" : "DirectX 11: basic mode, shader model level %u told to the game (dx11_mode=basic / ATTILA_DX11_SM)", g_dx_sm);
    }
    g_dx_test_no_filter = getenv("ATTILA_DX11_NO_FILTER") != NULL;
    g_dx_test_map_wait = getenv("ATTILA_DX11_MAP_WAIT") != NULL;
    g_dx_test_no_plan = getenv("ATTILA_DX11_NO_PLAN") != NULL;
    g_dx_test_no_srvfast = getenv("ATTILA_DX11_NO_SRVFAST") != NULL;
    g_dx_test_no_memo = getenv("ATTILA_DX11_NO_MEMO") != NULL;
    g_dx_cpu_on = getenv("ATTILA_DX11_CPU") != NULL;
    g_dx_old_bind = getenv("ATTILA_DX11_OLD_BIND") != NULL;
    if (g_dx_old_bind) log_line("DirectX 11: binding code of before 2026-11-13 (test)");
    { const char *g = getenv("ATTILA_DX11_GPU"); if (g && *g) { g_dxg.on = 1; g_dxg.list_at = atof(g); log_line("DirectX 11: card timing on (test)"); } }
    { extern int g_dx11_no_gl; g_dx11_no_gl = 1; }
    log_line("patch applied: DirectX 11 device");
    // The game's own "which graphics type am I" answer: WS_ENGINE_IMP::get_gfx_device_type (30ffd80: mov eax,[rdi+0x1c]; ret)
    // gives 2 = OpenGL on Linux, and about 40 places do things the OpenGL way when it does (far terrain tiles placed
    // bottom-up, shadow filtering, texture set-up, shader choices...). With our DirectX 11 device it answers 1, so those
    // places take the DirectX paths the Windows game uses. The engine's own field stays 2 (its constructor needs that to
    // make the device through create_opengl_render_device, which is ours). ATTILA_DX11_TYPE=2 keeps the OpenGL answer.
    const char *ty = getenv("ATTILA_DX11_TYPE");
    int type = ty && *ty ? atoi(ty) : 1;
    if (type != 2) {
        uint8_t *f = (uint8_t *)0x30ffd80;
        static const uint8_t was[4] = {0x8b, 0x47, 0x1c, 0xc3};
        if (memcmp(f, was, 4)) log_line("patch skipped, game code differs: graphics type answer");
        else {
            f[0] = 0xb8; f[1] = (uint8_t)type; f[2] = 0; f[3] = 0; f[4] = 0; f[5] = 0xc3;   // mov eax, type; ret (the 12 bytes after the old ret are padding)
            log_line("patch applied: the game is told its graphics type is %d (DirectX 11) instead of 2 (OpenGL)", type);
        }
    }
}
