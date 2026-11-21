// SDL2 and OpenGL for the game, from the Windows DLLs.
// A generic bridge handles every function whose arguments are all whole numbers or pointers, up to 6 + 10 of them.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "loader.h"
#include "gl_table.h"
#include "test_auto.h"
#include "fps_overlay.h"
#include "menu_button.h"

// Converts a Linux-style call into a Windows-style one. The target arrives in rax.
__asm__(".text\n"
        ".globl sysv_to_ms_common\n"
        "sysv_to_ms_common:\n"
        "push %rbp\n"
        "mov %rsp, %rbp\n"
        "sub $0x90, %rsp\n"
        // stack arguments: from just above the caller's return address to just above the Windows register slots
        "mov 16(%rbp), %r10\n mov %r10, 0x30(%rsp)\n"
        "mov 24(%rbp), %r10\n mov %r10, 0x38(%rsp)\n"
        "mov 32(%rbp), %r10\n mov %r10, 0x40(%rsp)\n"
        "mov 40(%rbp), %r10\n mov %r10, 0x48(%rsp)\n"
        "mov 48(%rbp), %r10\n mov %r10, 0x50(%rsp)\n"
        "mov 56(%rbp), %r10\n mov %r10, 0x58(%rsp)\n"
        "mov 64(%rbp), %r10\n mov %r10, 0x60(%rsp)\n"
        "mov 72(%rbp), %r10\n mov %r10, 0x68(%rsp)\n"
        "mov 80(%rbp), %r10\n mov %r10, 0x70(%rsp)\n"
        "mov 88(%rbp), %r10\n mov %r10, 0x78(%rsp)\n"
        "mov %r8, 0x20(%rsp)\n"
        "mov %r9, 0x28(%rsp)\n"
        "mov %rcx, %r9\n"
        "mov %rdx, %r8\n"
        "mov %rsi, %rdx\n"
        "mov %rdi, %rcx\n"
        "call *%rax\n"
        "leave\n"
        "ret\n");
extern void sysv_to_ms_common(void);

static uint8_t *g_page;
static size_t g_used;
static SRWLOCK g_page_lock = SRWLOCK_INIT;

void *make_bridge(void *target) {
    AcquireSRWLockExclusive(&g_page_lock);
    if (!g_page || g_used + 24 > (1u << 20)) {
        g_page = VirtualAlloc(NULL, 1u << 20, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        g_used = 0;
    }
    uint8_t *p = g_page + g_used;
    g_used += 24;
    ReleaseSRWLockExclusive(&g_page_lock);
    p[0] = 0x48; p[1] = 0xB8;                   // mov rax, target
    memcpy(p + 2, &target, 8);
    p[10] = 0x49; p[11] = 0xBB;                 // mov r11, common
    void *c = (void *)sysv_to_ms_common;
    memcpy(p + 12, &c, 8);
    p[20] = 0x41; p[21] = 0xFF; p[22] = 0xE3;   // jmp r11
    return p;
}

// The short bridge, for functions whose arguments all travel in registers (at most 6, no floating point: g_gl_short_names).
// make_bridge copies ten stack slots on every call in case there are more arguments; the game makes millions of OpenGL calls
// a second (one or two arguments each, mostly), so each of those gets its own small piece of code instead:
//   sub rsp,38h; mov [rsp+20h],r8; mov [rsp+28h],r9; mov r9,rcx; mov r8,rdx; mov rdx,rsi; mov rcx,rdi;
//   mov rax,target; call rax; add rsp,38h; ret
// ATTILA_OLD_GL_BRIDGE=1 goes back to make_bridge for all (to compare).
static void *make_bridge6(void *target) {
    static const uint8_t head[] = {0x48, 0x83, 0xEC, 0x38, 0x4C, 0x89, 0x44, 0x24, 0x20, 0x4C, 0x89, 0x4C, 0x24, 0x28,
                                   0x49, 0x89, 0xC9, 0x49, 0x89, 0xD0, 0x48, 0x89, 0xF2, 0x48, 0x89, 0xF9, 0x48, 0xB8};
    static const uint8_t tail[] = {0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x38, 0xC3};
    AcquireSRWLockExclusive(&g_page_lock);
    if (!g_page || g_used + 48 > (1u << 20)) {
        g_page = VirtualAlloc(NULL, 1u << 20, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        g_used = 0;
    }
    uint8_t *p = g_page + g_used;
    g_used += 48;
    ReleaseSRWLockExclusive(&g_page_lock);
    memcpy(p, head, sizeof head);
    memcpy(p + sizeof head, &target, 8);
    memcpy(p + sizeof head + 8, tail, sizeof tail);
    return p;
}

static int gl_short_name(const char *name) {
    static int old = -1;
    if (old < 0) old = getenv("ATTILA_OLD_GL_BRIDGE") != NULL;
    if (old) return 0;
    for (const char *const *n = g_gl_short_names; *n; n++) if (!strcmp(*n, name)) return 1;
    return 0;
}

// With measuring on, each graphics call goes through this bridge instead, which adds up calls and time per function.
// The target arrives in rax, the function's counters in r10.
typedef struct GlTime { uint64_t calls, ticks; const char *name; struct GlTime *next; } GlTime;
static GlTime *g_gl_times;
__asm__(".text\n"
        "sysv_to_ms_timed:\n"
        "push %rbp\n"
        "mov %rsp, %rbp\n"
        "sub $0xb0, %rsp\n"
        "mov 16(%rbp), %r11\n mov %r11, 0x30(%rsp)\n"
        "mov 24(%rbp), %r11\n mov %r11, 0x38(%rsp)\n"
        "mov 32(%rbp), %r11\n mov %r11, 0x40(%rsp)\n"
        "mov 40(%rbp), %r11\n mov %r11, 0x48(%rsp)\n"
        "mov 48(%rbp), %r11\n mov %r11, 0x50(%rsp)\n"
        "mov 56(%rbp), %r11\n mov %r11, 0x58(%rsp)\n"
        "mov 64(%rbp), %r11\n mov %r11, 0x60(%rsp)\n"
        "mov 72(%rbp), %r11\n mov %r11, 0x68(%rsp)\n"
        "mov 80(%rbp), %r11\n mov %r11, 0x70(%rsp)\n"
        "mov 88(%rbp), %r11\n mov %r11, 0x78(%rsp)\n"
        "mov %r10, -8(%rbp)\n"
        "mov %rax, -16(%rbp)\n"
        "mov %rdx, %r11\n"
        "rdtsc\n"
        "shl $32, %rdx\n"
        "or %rdx, %rax\n"
        "mov %rax, -24(%rbp)\n"
        "mov %r11, %rdx\n"
        "mov -16(%rbp), %rax\n"
        "mov %r8, 0x20(%rsp)\n"
        "mov %r9, 0x28(%rsp)\n"
        "mov %rcx, %r9\n"
        "mov %rdx, %r8\n"
        "mov %rsi, %rdx\n"
        "mov %rdi, %rcx\n"
        "call *%rax\n"
        "mov %rax, -32(%rbp)\n"
        "rdtsc\n"
        "shl $32, %rdx\n"
        "or %rdx, %rax\n"
        "sub -24(%rbp), %rax\n"
        "mov -8(%rbp), %r10\n"
        "add %rax, 8(%r10)\n"
        "incq (%r10)\n"
        "mov -32(%rbp), %rax\n"
        "leave\n"
        "ret\n");
extern void sysv_to_ms_timed(void);
static void *make_timed_bridge(void *target, const char *name) {
    GlTime *rec = calloc(1, sizeof *rec);
    rec->name = _strdup(name);
    AcquireSRWLockExclusive(&g_page_lock);
    if (!g_page || g_used + 40 > (1u << 20)) {
        g_page = VirtualAlloc(NULL, 1u << 20, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        g_used = 0;
    }
    uint8_t *p = g_page + g_used;
    g_used += 40;
    rec->next = g_gl_times;
    g_gl_times = rec;
    ReleaseSRWLockExclusive(&g_page_lock);
    void *c = (void *)sysv_to_ms_timed;
    p[0] = 0x48; p[1] = 0xB8; memcpy(p + 2, &target, 8);     // mov rax, target
    p[10] = 0x49; p[11] = 0xBA; memcpy(p + 12, &rec, 8);     // mov r10, counters
    p[20] = 0x49; p[21] = 0xBB; memcpy(p + 22, &c, 8);       // mov r11, bridge
    p[30] = 0x41; p[31] = 0xFF; p[32] = 0xE3;                // jmp r11
    return p;
}

// SDL2: nothing in the functions the game uses takes floating point. Two take callbacks, handled below.
static HMODULE sdl_dll(void) {
    static HMODULE h;
    if (!h) {
        h = LoadLibraryA("SDL2.dll");
        if (!h) fatal("SDL2.dll not found next to attila64.exe");
        // The game frees SDL objects with plain free(), as Linux shares one memory pool. Make that true here as well.
        int (__cdecl *set_mem)(void *, void *, void *, void *) = (void *)GetProcAddress(h, "SDL_SetMemoryFunctions");
        if (set_mem) set_mem((void *)malloc, (void *)calloc, (void *)realloc, (void *)free);
    }
    return h;
}

typedef struct { uint32_t (*unused)(void); } TimerUnused;
typedef SYSV uint32_t (*timer_cb)(uint32_t, void *);
typedef struct { timer_cb fn; void *param; } TimerCtx;
static uint32_t __cdecl timer_tramp(uint32_t ms, void *p) { TimerCtx *c = p; return c->fn(ms, c->param); }
SHIM(int, SDL_AddTimer, (uint32_t ms, timer_cb fn, void *param)) {
    int (__cdecl *real)(uint32_t, void *, void *) = (void *)GetProcAddress(sdl_dll(), "SDL_AddTimer");
    TimerCtx *c = malloc(sizeof *c);
    c->fn = fn;
    c->param = param;
    return real(ms, (void *)timer_tramp, c);
}

typedef SYSV void (*audio_cb)(void *, uint8_t *, int);
typedef struct { int freq; uint16_t format; uint8_t channels, silence; uint16_t samples, padding; uint32_t size; void *callback; void *userdata; } AudioSpec;
typedef struct { audio_cb fn; void *user; } AudioCtx;
static volatile long g_audio_calls;
static volatile int g_audio_peak;
static uint16_t g_audio_format;
static void __cdecl audio_tramp(void *u, uint8_t *s, int n) {
    AudioCtx *c = u;
    c->fn(c->user, s, n);
    if (test_mute()) { memset(s, 0, (size_t)n); return; }   // unattended test runs make no sound
    if (!shim_debug()) return;
    // how loud was this block? shows in the log whether the game really produces sound
    int peak = 0;
    if ((g_audio_format & 0xff) == 16 && !(g_audio_format & 0x100))
        for (int i = 0; i + 1 < n; i += 2) {
            int v = *(int16_t *)(s + i);
            if (v < 0) v = -v;
            if (v > peak) peak = v;
        }
    else if ((g_audio_format & 0xff) == 32 && (g_audio_format & 0x100))
        for (int i = 0; i + 3 < n; i += 4) {
            float f = *(float *)(s + i);
            int v = (int)((f < 0 ? -f : f) * 32767.0f);
            if (v > peak) peak = v;
        }
    if (peak > g_audio_peak) g_audio_peak = peak;
    long k = ++g_audio_calls;
    if (k == 1 || k % 2000 == 0) {
        // CAkSinkSDL2::AudioCallback writes silence unless its flag at +0x58 is set and the global behind GOT slot 0x4befe38 is filled
        uint8_t ready = *((uint8_t *)c->user + 0x58);
        void *global = **(void ***)0x4befe38;
        log_line("sound block %ld: %d bytes, loudest sample so far %d of 32767, ready flag %u, engine pointer %p, channels %u", k, n, g_audio_peak, ready, global, *((uint8_t *)c->user + 0xc));
        g_audio_peak = 0;
    }
}
SHIM(uint32_t, SDL_OpenAudioDevice, (const char *dev, int cap, const AudioSpec *want, AudioSpec *have, int allowed)) {
    uint32_t (__cdecl *real)(const char *, int, const AudioSpec *, AudioSpec *, int) = (void *)GetProcAddress(sdl_dll(), "SDL_OpenAudioDevice");
    AudioSpec w = *want;
    AudioCtx *c = malloc(sizeof *c);
    c->fn = (audio_cb)want->callback;
    c->user = want->userdata;
    w.callback = (void *)audio_tramp;
    w.userdata = c;
    g_audio_format = want->format;
    uint32_t r = real(dev, cap, &w, have, allowed);
    const char *(__cdecl *get_err)(void) = (void *)GetProcAddress(sdl_dll(), "SDL_GetError");
    const char *(__cdecl *get_drv)(void) = (void *)GetProcAddress(sdl_dll(), "SDL_GetCurrentAudioDriver");
    log_line("sound device: asked %d Hz, format %x, %d channels, %d samples, allowed changes %x -> device %u, driver %s, error '%s'",
             want->freq, want->format, want->channels, want->samples, allowed, r, get_drv() ? get_drv() : "none", r ? "" : get_err());
    if (have) {
        log_line("sound device: got %d Hz, format %x, %d channels, %d samples", have->freq, have->format, have->channels, have->samples);
        g_audio_format = have->format;
        have->callback = want->callback;
        have->userdata = want->userdata;
    }
    return r;
}
SHIM(void, SDL_PauseAudioDevice, (uint32_t dev, int pause)) {
    void (__cdecl *real)(uint32_t, int) = (void *)GetProcAddress(sdl_dll(), "SDL_PauseAudioDevice");
    log_line("sound device %u: %s", dev, pause ? "paused" : "started");
    real(dev, pause);
}
SHIM(void, SDL_CloseAudioDevice, (uint32_t dev)) {
    void (__cdecl *real)(uint32_t) = (void *)GetProcAddress(sdl_dll(), "SDL_CloseAudioDevice");
    log_line("sound device %u: closed by the game", dev);
    real(dev);
}

// OpenGL
static HMODULE g_gl;
typedef PROC (WINAPI *wgl_get_fn)(LPCSTR);
static wgl_get_fn g_wgl_get;

static void *gl_real(const char *name) {
    if (!g_gl) {
        g_gl = LoadLibraryA("opengl32.dll");
        g_wgl_get = (wgl_get_fn)GetProcAddress(g_gl, "wglGetProcAddress");
    }
    void *p = (void *)g_wgl_get(name);
    // wglGetProcAddress can answer with small error values
    if ((uintptr_t)p <= 3 || (intptr_t)p == -1) p = NULL;
    if (!p) p = (void *)GetProcAddress(g_gl, name);
    return p;
}

// Buffer opening: the game asks for read + invalidate in one call on its screen-sized buffer. Dropping invalidate keeps the
// same buffer, and it is also the combination the OpenGL rules forbid.
#define GL_MAP_READ_BIT_ 1u
#define GL_MAP_INVALIDATE_BUFFER_BIT_ 8u
// Each write map with "invalidate" and each large buffer re-creation makes the driver hand out fresh memory while the old is still queued for the card. That is what
// lets the game carry on without waiting, but if the game runs ahead of the card (sprite pages while a battle loads: 6 MB each, thousands of them) the driver''s memory
// grows until the PC runs out. So a marker goes into the graphics queue every so often, and the game only waits when too much is still unfinished behind those markers.
#define ORPHAN_MARK_EVERY (64ull << 20)
#define ORPHAN_RING 256u
static uint64_t g_orphan_limit = 2048ull << 20;
static void *(__stdcall *r_fence_sync)(unsigned, unsigned);
static unsigned (__stdcall *r_client_wait)(void *, unsigned, uint64_t);
static void (__stdcall *r_delete_sync)(void *);
static struct { void *sync; uint64_t bytes; } g_orphan[ORPHAN_RING];
static unsigned g_orphan_head, g_orphan_tail;
static uint64_t g_orphan_pending, g_orphan_since, g_orphan_waits, g_orphan_wait_ticks, g_orphan_peak;
static void *gl_real(const char *name);
static void orphan_note(uint64_t bytes) {
    g_orphan_since += bytes;
    if (g_orphan_since < ORPHAN_MARK_EVERY) return;
    if (!r_fence_sync) {
        r_fence_sync = gl_real("glFenceSync");
        r_client_wait = gl_real("glClientWaitSync");
        r_delete_sync = gl_real("glDeleteSync");
        const char *e = getenv("ATTILA_ORPHAN_LIMIT_MB");
        if (e) g_orphan_limit = (uint64_t)atoi(e) << 20;
        if (!r_fence_sync || !r_client_wait || !r_delete_sync) { r_fence_sync = NULL; return; }
    }
    void *s = r_fence_sync(0x9117 /* GPU commands complete */, 0);
    if (!s) return;
    g_orphan[g_orphan_tail % ORPHAN_RING].sync = s;
    g_orphan[g_orphan_tail % ORPHAN_RING].bytes = g_orphan_since;
    g_orphan_tail++;
    g_orphan_pending += g_orphan_since;
    g_orphan_since = 0;
    if (g_orphan_pending > g_orphan_peak) g_orphan_peak = g_orphan_pending;
    // markers the card has passed: that memory is free again
    while (g_orphan_head != g_orphan_tail) {
        unsigned r = r_client_wait(g_orphan[g_orphan_head % ORPHAN_RING].sync, 0, 0);
        if (r == 0x911B /* not yet */) break;
        r_delete_sync(g_orphan[g_orphan_head % ORPHAN_RING].sync);
        g_orphan_pending -= g_orphan[g_orphan_head % ORPHAN_RING].bytes;
        g_orphan_head++;
    }
    // too much behind: wait for the oldest markers, no more than needed
    while (g_orphan_head != g_orphan_tail && (g_orphan_pending > g_orphan_limit || g_orphan_tail - g_orphan_head >= ORPHAN_RING - 1)) {
        uint64_t t0 = __builtin_ia32_rdtsc();
        r_client_wait(g_orphan[g_orphan_head % ORPHAN_RING].sync, 1 /* flush */, 10000000000ull);
        g_orphan_wait_ticks += __builtin_ia32_rdtsc() - t0;
        g_orphan_waits++;
        r_delete_sync(g_orphan[g_orphan_head % ORPHAN_RING].sync);
        g_orphan_pending -= g_orphan[g_orphan_head % ORPHAN_RING].bytes;
        g_orphan_head++;
    }
}
// Buffer sizes, kept as the game binds and sizes buffers: invalidating throws away the whole buffer, not just the part being mapped. No driver calls needed.
#define BUFSIZE_SLOTS 8192u
static struct { uint32_t id; uint32_t pad; uint64_t size; } g_bufsize[BUFSIZE_SLOTS];
static uint32_t g_bound_array, g_bound_pack, g_bound_unpack, g_bound_uniform, g_bound_other;
static uint32_t *bound_slot(unsigned target) {
    switch (target) {
    case 0x8892: return &g_bound_array;
    case 0x88EB: return &g_bound_pack;
    case 0x88EC: return &g_bound_unpack;
    case 0x8A11: return &g_bound_uniform;
    default: return &g_bound_other;
    }
}
static void bufsize_set(uint32_t id, uint64_t size) {
    if (!id) return;
    uint32_t i = (id * 2654435761u) & (BUFSIZE_SLOTS - 1);
    for (int probe = 0; probe < 32; probe++, i = (i + 1) & (BUFSIZE_SLOTS - 1))
        if (!g_bufsize[i].id || g_bufsize[i].id == id) { g_bufsize[i].id = id; g_bufsize[i].size = size; return; }
    g_bufsize[(id * 2654435761u) & (BUFSIZE_SLOTS - 1)].id = id;
    g_bufsize[(id * 2654435761u) & (BUFSIZE_SLOTS - 1)].size = size;
}
static uint64_t bufsize_get(uint32_t id) {
    uint32_t i = (id * 2654435761u) & (BUFSIZE_SLOTS - 1);
    for (int probe = 0; probe < 32; probe++, i = (i + 1) & (BUFSIZE_SLOTS - 1)) {
        if (g_bufsize[i].id == id) return g_bufsize[i].size;
        if (!g_bufsize[i].id) break;
    }
    return 0;
}
static void *(__stdcall *r_map_range)(unsigned, long long, long long, unsigned);
static unsigned (__stdcall *r_get_error)(void);
// The caller, OPENGL_TEXTURE_2D::map, keeps its texture object in rbx; note it before any compiled code changes that register.
__attribute__((used)) static uint32_t *g_map_caller_rbx;
static int g_map_timing;
static struct { unsigned target, access; uint64_t calls, ticks, bytes; } g_map_stat[16];
SYSV __attribute__((used)) void *w_map_range(unsigned target, long long off, long long len, unsigned access);
__asm__(".text\n"
        "w_map_entry:\n"
        "mov %rbx, g_map_caller_rbx(%rip)\n"
        "jmp w_map_range\n");
extern void w_map_entry(void);
SYSV __attribute__((used)) void *w_map_range(unsigned target, long long off, long long len, unsigned access) {
    uint32_t *tex = g_map_caller_rbx;
    // Reading and invalidating together is against the OpenGL rules, so invalidate goes on those. ATTILA_DROP_INVALIDATE drops it on every map, as older builds did.
    static int drop_all = -1;
    if (drop_all < 0) drop_all = getenv("ATTILA_DROP_INVALIDATE") != NULL;
    unsigned a = (drop_all || (access & GL_MAP_READ_BIT_)) ? access & ~GL_MAP_INVALIDATE_BUFFER_BIT_ : access;
    if (a & GL_MAP_INVALIDATE_BUFFER_BIT_) {
        uint64_t whole = bufsize_get(*bound_slot(target));
        orphan_note(whole > (uint64_t)len ? whole : (uint64_t)len);
    }
    uint64_t t0_ = g_map_timing ? __builtin_ia32_rdtsc() : 0;
    void *p = r_map_range(target, off, len, a);
    if (g_map_timing) {
        uint64_t d_ = __builtin_ia32_rdtsc() - t0_;
        for (int i_ = 0; i_ < 16; i_++) {
            if (!g_map_stat[i_].calls || (g_map_stat[i_].target == target && g_map_stat[i_].access == access)) {
                g_map_stat[i_].target = target; g_map_stat[i_].access = access; g_map_stat[i_].calls++; g_map_stat[i_].ticks += d_; g_map_stat[i_].bytes += (uint64_t)len;
                break;
            }
        }
    }
    if (!p) {
        static int shown;
        if (shown++ < 12) {
            log_line("glMapBufferRange refused: target %x offset %lld length %lld access %x -> %x, error %x", target, off, len, access, a, r_get_error());
            log_line("  texture object %p: +0c %u, format %u, width %u, height %u, +28 %u", tex, tex[3], tex[4], tex[5], tex[6], tex[10]);
        }
    }
    return p;
}

// Graphics call counter, only with measuring on (ATTILA_PROFILE or measure_on.txt next to the exe); otherwise none of this runs. Each call is passed on
// untouched. Only functions with 6 or fewer whole-number or pointer arguments can go here. "Same" means the call would have set what is already set, so a filter could skip it. Counts are approximate.
typedef struct { const char *name; void **real; void *wrap; uint64_t calls, same, ticks; } GlCount;
static uint64_t g_gl_last[8192];
static unsigned g_gl_unit;
#define U64S uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f
typedef uint64_t (*gl_fn6)(U64S);
#define GLC_TIMED(n) uint64_t t0_ = __builtin_ia32_rdtsc(); uint64_t r_ = ((gl_fn6)r_##n)(a, b, c, d, e, f); c_##n.ticks += __builtin_ia32_rdtsc() - t0_; return r_;
#define GLC_ANY(n) static void *r_##n; static GlCount c_##n = {#n, &r_##n, 0, 0, 0, 0}; \
    SYSV static uint64_t w_##n(U64S) { c_##n.calls++; GLC_TIMED(n) }
#define GLC_KV(n, group, key, val) static void *r_##n; static GlCount c_##n = {#n, &r_##n, 0, 0, 0, 0}; \
    SYSV static uint64_t w_##n(U64S) { \
        uint64_t k_ = ((uint64_t)(group) * 0x9E3779B97F4A7C15ull ^ (uint64_t)(key) * 0xC2B2AE3D27D4EB4Full) >> 51, v_ = (uint64_t)(val) + 1; \
        c_##n.calls++; if (g_gl_last[k_] == v_) c_##n.same++; g_gl_last[k_] = v_; \
        GLC_TIMED(n) }
static void *r_glActiveTexture; static GlCount c_glActiveTexture = {"glActiveTexture", &r_glActiveTexture, 0, 0, 0, 0};
SYSV static uint64_t w_glActiveTexture(U64S) {
    c_glActiveTexture.calls++; if (g_gl_unit == (unsigned)a) c_glActiveTexture.same++; g_gl_unit = (unsigned)a;
    GLC_TIMED(glActiveTexture)
}
GLC_KV(glBindVertexArray, 3, 0, a)
GLC_KV(glUseProgram, 4, 0, a)
GLC_KV(glBindBuffer, 5, a, b)
GLC_KV(glBindFramebuffer, 6, a, b)
GLC_KV(glBindSampler, 7, a, b)
GLC_KV(glBindBufferBase, 8, (a << 8) ^ b, c)
GLC_KV(glBindBufferRange, 8, (a << 8) ^ b, c ^ (d << 20) ^ (e << 40))
GLC_KV(glEnable, 9, a, 1)
GLC_KV(glDisable, 9, a, 0)
GLC_KV(glDepthMask, 10, 0, a)
GLC_KV(glDepthFunc, 11, 0, a)
GLC_KV(glBlendFunc, 12, 0, (a << 20) ^ b)
GLC_KV(glBlendFuncSeparate, 12, 1, (a << 36) ^ (b << 24) ^ (c << 12) ^ d)
GLC_KV(glColorMask, 13, 0, (a << 3) ^ (b << 2) ^ (c << 1) ^ d)
GLC_KV(glCullFace, 14, 0, a)
GLC_KV(glFrontFace, 15, 0, a)
GLC_KV(glPolygonMode, 16, a, b)
static void *r_glBindTexture; static GlCount c_glBindTexture = {"glBindTexture", &r_glBindTexture, 0, 0, 0, 0};
SYSV static uint64_t w_glBindTexture(U64S) {
    uint64_t k_ = ((uint64_t)2 * 0x9E3779B97F4A7C15ull ^ (((uint64_t)g_gl_unit << 20) ^ a) * 0xC2B2AE3D27D4EB4Full) >> 51, v_ = b + 1;
    c_glBindTexture.calls++; if (g_gl_last[k_] == v_) c_glBindTexture.same++; g_gl_last[k_] = v_;
    GLC_TIMED(glBindTexture)
}
GLC_ANY(glDrawElements) GLC_ANY(glDrawArrays) GLC_ANY(glDrawElementsInstanced) GLC_ANY(glDrawElementsBaseVertex) GLC_ANY(glDrawElementsInstancedBaseVertex)
GLC_ANY(glDrawArraysInstanced) GLC_ANY(glGetError) GLC_ANY(glGetIntegerv) GLC_ANY(glBufferSubData) GLC_ANY(glBufferData)
GLC_ANY(glUnmapBuffer)  GLC_ANY(glVertexAttribPointer) GLC_ANY(glEnableVertexAttribArray) GLC_ANY(glDisableVertexAttribArray) GLC_ANY(glVertexAttribDivisor)
GLC_ANY(glUniform1i) GLC_ANY(glUniform4fv) GLC_ANY(glUniformMatrix4fv) GLC_ANY(glUniformBlockBinding) GLC_ANY(glGetUniformLocation)
#define GLC(n) {&c_##n, (void *)w_##n}
static const struct { GlCount *c; void *wrap; } g_gl_counts[] = {
    GLC(glActiveTexture), GLC(glBindTexture), GLC(glBindVertexArray), GLC(glUseProgram), GLC(glBindBuffer), GLC(glBindFramebuffer), GLC(glBindSampler),
    GLC(glBindBufferBase), GLC(glBindBufferRange), GLC(glEnable), GLC(glDisable), GLC(glDepthMask), GLC(glDepthFunc), GLC(glBlendFunc), GLC(glBlendFuncSeparate),
    GLC(glColorMask), GLC(glCullFace), GLC(glFrontFace), GLC(glPolygonMode), GLC(glDrawElements), GLC(glDrawArrays), GLC(glDrawElementsInstanced),
    GLC(glDrawElementsBaseVertex), GLC(glDrawElementsInstancedBaseVertex), GLC(glDrawArraysInstanced), GLC(glGetError),
    GLC(glGetIntegerv), GLC(glBufferSubData), GLC(glBufferData), GLC(glUnmapBuffer), GLC(glVertexAttribPointer), GLC(glEnableVertexAttribArray),
    GLC(glDisableVertexAttribArray), GLC(glVertexAttribDivisor), GLC(glUniform1i), GLC(glUniform4fv), GLC(glUniformMatrix4fv), GLC(glUniformBlockBinding),
    GLC(glGetUniformLocation)};
static int gl_counting(void) {
    static int v = -1;
    if (v < 0) {
        char p[MAX_PATH + 20];
        DWORD n = GetModuleFileNameA(NULL, p, MAX_PATH);
        while (n && p[n - 1] != '\\') n--;
        strcpy(p + n, "measure_on.txt");
        // ATTILA_NO_GL_TIMES=1 with the profiler: graphics calls keep their normal bridges, so the profile shows the game as players run it
        v = (getenv("ATTILA_PROFILE") && !getenv("ATTILA_NO_GL_TIMES")) || GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
    }
    return v;
}
static void *gl_count_wrap(const char *name, void *real) {
    if (!gl_counting()) return NULL;
    for (size_t i = 0; i < sizeof g_gl_counts / sizeof g_gl_counts[0]; i++)
        if (!strcmp(g_gl_counts[i].c->name, name)) { *g_gl_counts[i].c->real = real; return g_gl_counts[i].wrap; }
    return NULL;
}
typedef struct { const char *name; uint64_t calls, same, ticks; } GlRow;
static GlTime g_t_teximage = {0, 0, "glTexImage2D", NULL};
static uint64_t g_t_zero_textures;
static int gl_row_cmp(const void *a, const void *b) {
    uint64_t x = ((const GlRow *)a)->ticks, y = ((const GlRow *)b)->ticks;
    return x < y ? 1 : x > y ? -1 : 0;
}
static void gl_count_report(void) {
    if (!gl_counting()) return;
    extern void shim_mem_report(double ticks_per_ms);
    static uint64_t prev_tsc;
    static LARGE_INTEGER prev_qpc;
    LARGE_INTEGER q, f;
    uint64_t tsc = __builtin_ia32_rdtsc();
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    double window_ms = prev_tsc ? (double)(q.QuadPart - prev_qpc.QuadPart) * 1000.0 / (double)f.QuadPart : 0;
    double per_ms = prev_tsc && window_ms > 0 ? (double)(tsc - prev_tsc) / window_ms : 4.0e6;
    prev_tsc = tsc;
    prev_qpc = q;
    static GlRow rows[1024];
    int n = 0;
    for (size_t i = 0; i < sizeof g_gl_counts / sizeof g_gl_counts[0] && n < 1024; i++) {
        GlCount *c = g_gl_counts[i].c;
        if (c->calls) { rows[n].name = c->name; rows[n].calls = c->calls; rows[n].same = c->same; rows[n].ticks = c->ticks; n++; }
        c->calls = c->same = c->ticks = 0;
    }
    AcquireSRWLockShared(&g_page_lock);
    for (GlTime *t_ = g_gl_times; t_ && n < 1024; t_ = t_->next) {
        if (t_->calls) { rows[n].name = t_->name; rows[n].calls = t_->calls; rows[n].same = 0; rows[n].ticks = t_->ticks; n++; }
        t_->calls = t_->ticks = 0;
    }
    ReleaseSRWLockShared(&g_page_lock);
    if (g_t_teximage.calls) { rows[n].name = g_t_teximage.name; rows[n].calls = g_t_teximage.calls; rows[n].same = 0; rows[n].ticks = g_t_teximage.ticks; n++; g_t_teximage.calls = g_t_teximage.ticks = 0; }
    qsort(rows, (size_t)n, sizeof rows[0], gl_row_cmp);
    double all = 0;
    for (int i = 0; i < n; i++) all += (double)rows[i].ticks / per_ms;
    log_line("gl time: window %.0f ms, all graphics calls measured %.0f ms", window_ms, all);
    for (int i = 0; i < n && i < 34; i++)
        log_line("gl %s: %llu calls, %llu same, %.1f ms", rows[i].name, (unsigned long long)rows[i].calls, (unsigned long long)rows[i].same, (double)rows[i].ticks / per_ms);
    for (int i = 0; i < 16 && g_map_stat[i].calls; i++) {
        log_line("gl map target %x access %x: %llu calls, %.1f ms, %.0f MB", g_map_stat[i].target, g_map_stat[i].access, (unsigned long long)g_map_stat[i].calls, (double)g_map_stat[i].ticks / per_ms, (double)g_map_stat[i].bytes / 1048576.0);
        g_map_stat[i].calls = g_map_stat[i].ticks = g_map_stat[i].bytes = 0;
    }
    {
        MEMORYSTATUSEX ms;
        ms.dwLength = sizeof ms;
        GlobalMemoryStatusEx(&ms);
        log_line("memory: PC commit in use %.1f GB of %.1f GB, RAM free %.1f GB", (double)(ms.ullTotalPageFile - ms.ullAvailPageFile) / 1073741824.0, (double)ms.ullTotalPageFile / 1073741824.0, (double)ms.ullAvailPhys / 1073741824.0);
    }
    log_line("textures started blank instead of uploading zeros: %llu", (unsigned long long)g_t_zero_textures);
    g_t_zero_textures = 0;
    log_line("throwaway buffers: %.0f MB behind now, peak %.0f MB, %llu waits %.1f ms", (double)g_orphan_pending / 1048576.0, (double)g_orphan_peak / 1048576.0, (unsigned long long)g_orphan_waits, (double)g_orphan_wait_ticks / per_ms);
    g_orphan_waits = g_orphan_wait_ticks = 0;
    g_orphan_peak = g_orphan_pending;
    shim_mem_report(per_ms);
}
static GlTime g_t_bindbuffer = {0, 0, "glBindBuffer", NULL};
static void (__stdcall *r_bind_buffer)(unsigned, unsigned);
SYSV static void w_bind_buffer(unsigned target, unsigned id) {
    *bound_slot(target) = id;
    if (g_map_timing) {
        uint64_t t0 = __builtin_ia32_rdtsc();
        r_bind_buffer(target, id);
        g_t_bindbuffer.ticks += __builtin_ia32_rdtsc() - t0;
        g_t_bindbuffer.calls++;
    } else r_bind_buffer(target, id);
}
static GlTime g_t_bufferdata = {0, 0, "glBufferData", NULL};
static void (__stdcall *r_buffer_data)(unsigned, long long, const void *, unsigned);
SYSV static void w_buffer_data(unsigned target, long long size, const void *data, unsigned usage) {
    bufsize_set(*bound_slot(target), (uint64_t)size);
    if (size >= (1 << 20)) orphan_note((uint64_t)size);
    if (g_map_timing) {
        uint64_t t0 = __builtin_ia32_rdtsc();
        r_buffer_data(target, size, data, usage);
        g_t_bufferdata.ticks += __builtin_ia32_rdtsc() - t0;
        g_t_bufferdata.calls++;
    } else r_buffer_data(target, size, data, usage);
}

// The game asks the driver for a shader setting's location by name, over and over; a location only changes when the program is linked again or deleted.
// So each answer is remembered per program, per name. ATTILA_CHECK_UNIFORMS also asks the driver every time and logs any difference.
#define UNI_SLOTS (1u << 16)
#define UNI_GENS 4096u
typedef struct { unsigned program, gen; int location; char *name; } UniSlot;
static UniSlot g_uni[UNI_SLOTS];
static unsigned g_uni_gen[UNI_GENS];
static SRWLOCK g_uni_lock = SRWLOCK_INIT;
static int (*r_uniform_location)(unsigned, const char *);
static void (*r_link_program)(unsigned);
static void (*r_delete_program)(unsigned);
SYSV static int w_uniform_location(unsigned program, const char *name) {
    static int check = -1;
    static volatile long mismatches;
    if (check < 0) check = getenv("ATTILA_CHECK_UNIFORMS") != NULL;
    c_glGetUniformLocation.calls++;
    uint32_t h = 2166136261u ^ program;
    for (const char *s = name; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    AcquireSRWLockExclusive(&g_uni_lock);
    unsigned gen = g_uni_gen[program % UNI_GENS];
    uint32_t i = h & (UNI_SLOTS - 1);
    int free_slot = -1;
    for (int probe = 0; probe < 16; probe++, i = (i + 1) & (UNI_SLOTS - 1)) {
        UniSlot *u = &g_uni[i];
        if (!u->name) { free_slot = (int)i; break; }
        if (u->program == program && u->gen == gen && !strcmp(u->name, name)) {
            int loc = u->location;
            ReleaseSRWLockExclusive(&g_uni_lock);
            c_glGetUniformLocation.same++;
            if (check) {
                int real = r_uniform_location(program, name);
                if (real != loc && InterlockedIncrement(&mismatches) <= 20) log_line("uniform cache WRONG: program %u '%s' remembered %d, real %d", program, name, loc, real);
            }
            return loc;
        }
    }
    if (free_slot < 0) free_slot = (int)(h & (UNI_SLOTS - 1));   // full neighbourhood: this entry gets replaced
    ReleaseSRWLockExclusive(&g_uni_lock);
    int loc = r_uniform_location(program, name);
    char *copy = _strdup(name);
    AcquireSRWLockExclusive(&g_uni_lock);
    UniSlot *u = &g_uni[free_slot];
    if (g_uni_gen[program % UNI_GENS] == gen) {
        free(u->name);
        u->program = program; u->gen = gen; u->location = loc; u->name = copy;
        copy = NULL;
    }
    ReleaseSRWLockExclusive(&g_uni_lock);
    free(copy);
    return loc;
}
static void uniform_forget(unsigned program) {
    AcquireSRWLockExclusive(&g_uni_lock);
    g_uni_gen[program % UNI_GENS]++;
    ReleaseSRWLockExclusive(&g_uni_lock);
}
SYSV static void w_link_program(unsigned program) { r_link_program(program); uniform_forget(program); }
SYSV static void w_delete_program(unsigned program) { uniform_forget(program); r_delete_program(program); }

// Texture sharpness as the graphics card gets it. Each level is logged the first time it is sent, so a log shows whether the
// game's gfx_texture_filtering setting really arrives. Measured 2026-11-11 in the game's own benchmark: the game sets it per
// texture with glTexParameterf (below), setting 2 -> 4x, 4 -> 16x, 0 -> never sent; only in battle, not in the menu. The
// per-sampler call here (game code at 33dbb8e) was never used in those runs.
static void (__stdcall *r_sampler_f)(unsigned sampler, unsigned pname, float value);
SYSV static void w_sampler_f(unsigned sampler, unsigned pname, float value) {
    static volatile long seen;   // one bit per whole level 0..31
    if (pname == 0x84FE) {
        int level = value >= 0.0f && value < 32.0f ? (int)value : 31;
        if (!(_InterlockedOr(&seen, 1L << level) & (1L << level)))
            log_line("texture sharpness sent to the graphics card: %gx (first sampler with this level: %u)", (double)value, sampler);
    }
    r_sampler_f(sampler, pname, value);
}

// The same watch on glTexParameterf(target, GL_TEXTURE_MAX_ANISOTROPY, level), the per-texture way to set it.
static void (__stdcall *r_tex_f)(unsigned target, unsigned pname, float value);
SYSV static void w_tex_f(unsigned target, unsigned pname, float value) {
    static volatile long seen;
    if (pname == 0x84FE) {
        int level = value >= 0.0f && value < 32.0f ? (int)value : 31;
        if (!(_InterlockedOr(&seen, 1L << level) & (1L << level)))
            log_line("texture sharpness sent to the graphics card per texture: %gx", (double)value);
    }
    r_tex_f(target, pname, value);
}

// Draw calls so far, counted only with ATTILA_PASS_STATS=1 (pass_stats.inc splits them by frame step). The draw functions get
// a wrapper that adds one and passes the call on; they take whole numbers and pointers only, at most 7 of them.
volatile unsigned long long g_gl_draw_total;
typedef uint64_t (*gl_fn7)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
#define GL_DRAW_COUNTED(n) static void *rd_##n; \
    SYSV static uint64_t wd_##n(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g) { g_gl_draw_total++; return ((gl_fn7)rd_##n)(a, b, c, d, e, f, g); }
GL_DRAW_COUNTED(glDrawElements) GL_DRAW_COUNTED(glDrawArrays) GL_DRAW_COUNTED(glDrawElementsInstanced) GL_DRAW_COUNTED(glDrawElementsBaseVertex)
GL_DRAW_COUNTED(glDrawArraysInstanced) GL_DRAW_COUNTED(glDrawRangeElements)
// The main draw function also sorts its calls by size, for the view pass_stats.inc says is running (g_view_kind: 1 shadow
// map, 2 main picture, 0 other): index count under 300 / 3,000 / 30,000 / more, and 1 / 2-9 / 10+ copies per call.
volatile int g_view_kind;
volatile unsigned long long g_draw_hist[3][4][3], g_draw_indices[3], g_draw_instances[3];
static void draw_caller_note(int copies, int kind);   // draw_callers.inc, at the end of this file
static void *rd_glDrawElementsInstancedBaseVertex;
SYSV static uint64_t wd_glDrawElementsInstancedBaseVertex(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g) {
    g_gl_draw_total++;
    int k = g_view_kind, n = (int)b, copies = (int)e;
    g_draw_hist[k][n < 300 ? 0 : n < 3000 ? 1 : n < 30000 ? 2 : 3][copies <= 1 ? 0 : copies < 10 ? 1 : 2]++;
    g_draw_indices[k] += (uint64_t)n * (uint64_t)(copies > 0 ? copies : 1);
    g_draw_instances[k] += (uint64_t)(copies > 0 ? copies : 1);
    draw_caller_note(copies, k);
    return ((gl_fn7)rd_glDrawElementsInstancedBaseVertex)(a, b, c, d, e, f, g);
}
GL_DRAW_COUNTED(glDrawRangeElementsBaseVertex)
#define GL_DRAW_ROW(n) {#n, &rd_##n, (void *)wd_##n}
static void *gl_draw_count_wrap(const char *name, void *real) {
    static const struct { const char *name; void **real; void *wrap; } rows[] = {
        GL_DRAW_ROW(glDrawElements), GL_DRAW_ROW(glDrawArrays), GL_DRAW_ROW(glDrawElementsInstanced), GL_DRAW_ROW(glDrawElementsBaseVertex),
        GL_DRAW_ROW(glDrawElementsInstancedBaseVertex), GL_DRAW_ROW(glDrawArraysInstanced), GL_DRAW_ROW(glDrawRangeElements),
        GL_DRAW_ROW(glDrawRangeElementsBaseVertex)};
    static int on = -1;
    if (on < 0) on = getenv("ATTILA_PASS_STATS") != NULL;
    if (!on) return NULL;
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) if (!strcmp(rows[i].name, name)) { *rows[i].real = real; return rows[i].wrap; }
    return NULL;
}
static void *render_scale_wrap(const char *name, void *real);   // render_scale.inc, at the end of this file
void upscale_report(void);
static void *tex_trace_wrap(const char *name, void *real);   // tex_trace.inc, at the end of this file
void tex_trace_report(long frames);
static void *lazy_unit_wrap(const char *name, void *found);   // lazy_unit.inc, at the end of this file
void lazy_unit_report(long frames);
void lazy_unit_flush(void);
typedef struct { const char *name; void *bridge; } GlCache;
#define GL_CACHE (1u << 13)
static GlCache g_gl_cache[GL_CACHE];
static SRWLOCK g_gl_lock = SRWLOCK_INIT;

static void *gl_resolve(const char *name) {
    uint32_t h = 2166136261u;
    for (const char *s = name; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    uint32_t i = h & (GL_CACHE - 1);
    AcquireSRWLockExclusive(&g_gl_lock);
    void *found = NULL;
    while (g_gl_cache[i].name) {
        if (!strcmp(g_gl_cache[i].name, name)) { found = g_gl_cache[i].bridge; break; }
        i = (i + 1) & (GL_CACHE - 1);
    }
    if (!found) {
        void *real = gl_real(name);
        if (real) {
            for (const GlFloatBridge *b = g_gl_float_bridges; b->name; b++)
                if (!strcmp(b->name, name)) { *b->real = real; found = b->wrapper; break; }
            if (!strcmp(name, "glSamplerParameterf")) { r_sampler_f = real; found = (void *)w_sampler_f; }
            if (!strcmp(name, "glTexParameterf")) { r_tex_f = real; found = (void *)w_tex_f; }
            if (!strcmp(name, "glMapBufferRange")) {
                r_map_range = real;
                r_get_error = gl_real("glGetError");
                g_map_timing = gl_counting();
                found = (void *)w_map_entry;
            }
            if (!found && !strcmp(name, "glBindBuffer")) {
                r_bind_buffer = real;
                found = (void *)w_bind_buffer;
                g_map_timing = gl_counting();
                AcquireSRWLockExclusive(&g_page_lock);
                g_t_bindbuffer.next = g_gl_times;
                g_gl_times = &g_t_bindbuffer;
                ReleaseSRWLockExclusive(&g_page_lock);
            }
            if (!found && !strcmp(name, "glBufferData")) {
                r_buffer_data = real;
                found = (void *)w_buffer_data;
                g_map_timing = gl_counting();
                AcquireSRWLockExclusive(&g_page_lock);
                g_t_bufferdata.next = g_gl_times;
                g_gl_times = &g_t_bufferdata;
                ReleaseSRWLockExclusive(&g_page_lock);
            }
            if (!found && !strcmp(name, "glGetUniformLocation")) { r_uniform_location = real; found = (void *)w_uniform_location; }
            if (!found && !strcmp(name, "glLinkProgram")) { r_link_program = real; found = (void *)w_link_program; }
            if (!found && !strcmp(name, "glDeleteProgram")) { r_delete_program = real; found = (void *)w_delete_program; }
            if (!found) found = render_scale_wrap(name, real);
            if (!found) found = tex_trace_wrap(name, real);
            if (!found) found = gl_draw_count_wrap(name, real);
            if (!found) found = gl_count_wrap(name, real);
            if (!found) found = gl_counting() ? make_timed_bridge(real, name) : gl_short_name(name) ? make_bridge6(real) : make_bridge(real);
            found = lazy_unit_wrap(name, found);
            g_gl_cache[i].name = _strdup(name);
            g_gl_cache[i].bridge = found;
        }
    }
    ReleaseSRWLockExclusive(&g_gl_lock);
    return found;
}

// GLX is the Linux side of OpenGL. Windows has none; the game's setup only needs this one to answer with something.
SHIM(void *, glXGetCurrentDisplay, (void)) { return (void *)1; }

SHIM(void *, glXGetProcAddressARB, (const char *name)) {
    if (!strncmp(name, "glX", 3)) {
        const ShimEntry *e = shim_find(name);
        return e ? e->ptr : NULL;
    }
    return strncmp(name, "gl", 2) ? NULL : gl_resolve(name);
}
SHIM(int, glXQueryVersion, (void *d, int *ma, int *mi)) { *ma = 1; *mi = 4; return 1; }
SHIM(const char *, glXGetClientString, (void *d, int which)) { return which == 2 ? "1.4" : ""; }

// Texture creation is logged for the first few, to see the sizes the game really asks for.
SHIM(void, glTexImage2D, (unsigned t, int lvl, int ifmt, int w, int h, int b, unsigned fmt, unsigned type, const void *d)) {
    static void (__stdcall *r)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
    static int shown;
    if (!r) r = gl_real("glTexImage2D");
    if (shim_debug() && shown++ < 24) log_line("glTexImage2D target %x level %d internal %x size %dx%d format %x type %x data %p", t, lvl, ifmt, w, h, fmt, type, d);
    // The game makes render textures by filling a block with zeros byte by byte and uploading it. For big ones, a new texture without data avoids copying megabytes of nothing to the card.
    static int keep_zero = -1;
    if (keep_zero < 0) keep_zero = getenv("ATTILA_KEEP_ZERO_UPLOADS") != NULL;
    if (d && !keep_zero && (uint64_t)w * (uint64_t)h >= 256 * 256 && fmt == 0x1908 /* RGBA */ && type == 0x1401 /* bytes */) {
        const uint64_t *q = d;
        size_t words = (size_t)((uint64_t)w * (uint64_t)h / 2);
        size_t i = 0;
        while (i < words && !q[i]) i++;
        if (i == words) { d = NULL; g_t_zero_textures++; }
    }
    uint64_t t0 = __builtin_ia32_rdtsc();
    r(t, lvl, ifmt, w, h, b, fmt, type, d);
    g_t_teximage.ticks += __builtin_ia32_rdtsc() - t0;
    {   // DirectX 12 route: the first eight texture creations and what OpenGL says about each. Two testers' starts crashed in the
        // game's first texture upload (texture never set up); their next logs show whether creating it was refused, and why.
        static int route = -1, told;
        if (route < 0) { const char *g = getenv("GALLIUM_DRIVER"); route = g && !_stricmp(g, "d3d12"); }
        if (route && told < 8) {
            static unsigned (__stdcall *get_error)(void);
            if (!get_error) get_error = gl_real("glGetError");
            told++;
            log_line("first textures: target %x level %d internal format %x size %dx%d format %x type %x -> error %x", t, lvl, ifmt, w, h, fmt, type, get_error ? get_error() : 0xFFFF);
        }
    }
    g_t_teximage.calls++;
}

// The moment the game gets its OpenGL context. Logged at once: whether it was made, and by which driver. Two testers'
// DirectX 12 starts (GTX 1660 Super, a 12 GB card; 2026-11-11/12) crashed before the first frame in the game's first texture
// upload (texture object still at format 63 = never set up), and the "graphics driver" line of the first frame was never
// reached, so their logs could not say whether there was a working context at all. Now the log says so before anything is drawn.
// ---- Unattended test runs (test_auto.c): with ATTILA_TEST_HIDDEN=1 the game window is created hidden and stays hidden, so
// a test never takes the screen or the keyboard from the user. The game is told the window is shown and focused.
static uint32_t g_hidden_fullscreen;
static void *g_test_window;
SHIM(void *, SDL_CreateWindow, (const char *title, int x, int y, int w, int h, uint32_t flags)) {
    static void *(__cdecl *r)(const char *, int, int, int, int, uint32_t);
    if (!r) r = (void *)GetProcAddress(sdl_dll(), "SDL_CreateWindow");
    if (test_hidden()) {
        g_hidden_fullscreen = flags & 0x1001u;
        log_line("test run: game window %dx%d kept hidden (asked flags 0x%x)", w, h, flags);
        flags = (flags & ~(0x1001u | 0x4u | 0x40u | 0x80u | 0x100u)) | 0x8u;
        // ATTILA_TEST_WINDOW_SIZE=WxH: the window gets that size whatever the game asks, as desktop full screen does (flags
        // 0x1001: the window takes the screen's size while the game keeps drawing at its own resolution)
        const char *ws = getenv("ATTILA_TEST_WINDOW_SIZE");
        int tw, th;
        if (ws && sscanf(ws, "%dx%d", &tw, &th) == 2 && tw > 0 && th > 0) {
            log_line("test run: window made %dx%d instead of %dx%d (ATTILA_TEST_WINDOW_SIZE)", tw, th, w, h);
            w = tw; h = th;
        }
    }
    g_test_window = r(title, x, y, w, h, flags);   // kept for scripted test input (its window number goes into the events)
    return g_test_window;
}
// The game window's size as SDL reports it (0 x 0 before there is one); for the mouse trace.
void test_window_size(int *w, int *h) {
    static void (__cdecl *gs)(void *, int *, int *);
    *w = *h = 0;
    if (!g_test_window) return;
    if (!gs) gs = (void *)GetProcAddress(sdl_dll(), "SDL_GetWindowSize");
    if (gs) gs(g_test_window, w, h);
}
SHIM(void, SDL_ShowWindow, (void *w)) {
    static void (__cdecl *r)(void *);
    if (test_hidden()) return;
    if (!r) r = (void *)GetProcAddress(sdl_dll(), "SDL_ShowWindow");
    r(w);
}
SHIM(int, SDL_SetWindowFullscreen, (void *w, uint32_t flags)) {
    static int (__cdecl *r)(void *, uint32_t);
    if (test_hidden()) { g_hidden_fullscreen = flags & 0x1001u; return 0; }
    if (!r) r = (void *)GetProcAddress(sdl_dll(), "SDL_SetWindowFullscreen");
    return r(w, flags);
}
SHIM(uint32_t, SDL_GetWindowFlags, (void *w)) {
    static uint32_t (__cdecl *r)(void *);
    if (!r) r = (void *)GetProcAddress(sdl_dll(), "SDL_GetWindowFlags");
    uint32_t f = r(w);
    if (test_hidden()) f = (f & ~(0x8u | 0x40u)) | 0x4u | 0x200u | 0x400u | g_hidden_fullscreen;
    return f;
}
// ---- test: scripted input for unattended runs. ATTILA_TEST_INPUT="140:click 2464 80;142:wheel 6;150:key w 2.5"
// Times are test seconds (benchmark or battle seconds). click X Y = left click at that window pixel; wheel N = N steps of
// the mouse wheel (+ = away from the user = zoom in); key L S = hold letter key L for S seconds. The events go straight
// into what the game reads (its event pump, SDL_GetMouseState, SDL_GetKeyboardState): the real mouse and keyboard are
// never touched, so this works in a hidden window while the user works at the PC.
// A step ending in @P repeats every P seconds until the program ends ("300:key return 0.1@8"): for long unattended
// runs such as a campaign played for many turns. Repeats are logged twice only.
typedef struct { double at, every; int kind, a, b, done, fired; } TestInput;   // kind: 1 move, 2 left down, 3 left up, 4 wheel step, 5 key down, 6 key up, 7 middle down, 8 middle up, 9/10 left down/up of a double click
static TestInput g_tin[512];
static int g_ntin = -1, g_tin_x = -1, g_tin_y, g_tin_buttons, g_tin_keys_on;
static uint8_t g_tin_keys[512], g_tin_keystate[512];
static double g_tin_every;   // the repeat of the step being read (0 = once)
static void test_input_add(double at, int kind, int a, int b) {
    if (g_ntin < 512) { TestInput *t = &g_tin[g_ntin++]; memset(t, 0, sizeof *t); t->at = at; t->every = g_tin_every; t->kind = kind; t->a = a; t->b = b; }
}
static void test_input_parse_from(const char *e, double base) {
    while (e && *e) {
        double at = base + atof(e);
        const char *c = strchr(e, ':');
        if (!c) break;
        c++;
        {   // "@P" before the end of this step = repeat every P seconds
            const char *end = strpbrk(c, ";|"), *rep = strchr(c, '@');
            g_tin_every = rep && (!end || rep < end) ? atof(rep + 1) : 0;
            if (g_tin_every < 0.5) g_tin_every = 0;
        }
        int x = 0, y = 0, n = 0; double secs = 0; char key = 0;
        if (sscanf(c, "click %d %d", &x, &y) == 2) { test_input_add(at, 1, x, y); test_input_add(at + 0.15, 2, x, y); test_input_add(at + 0.30, 3, x, y); }
        else if (sscanf(c, "move %d %d", &x, &y) == 2) test_input_add(at, 1, x, y);
        else if (sscanf(c, "mdown %d %d", &x, &y) == 2) test_input_add(at, 7, x, y);   // middle mouse button (camera drag)
        else if (sscanf(c, "mup %d %d", &x, &y) == 2) test_input_add(at, 8, x, y);
        else if (sscanf(c, "ldown %d %d", &x, &y) == 2) { test_input_add(at, 1, x, y); test_input_add(at + 0.1, 2, x, y); }   // left button held (drags)
        else if (sscanf(c, "lup %d %d", &x, &y) == 2) { test_input_add(at, 1, x, y); test_input_add(at + 0.1, 3, x, y); }
        else if (sscanf(c, "dclick %d %d", &x, &y) == 2) {   // double click: the second press carries clicks = 2 (SDL), as the game's double-click event wants
            test_input_add(at, 1, x, y); test_input_add(at + 0.05, 2, x, y); test_input_add(at + 0.10, 3, x, y);
            test_input_add(at + 0.15, 9, x, y); test_input_add(at + 0.20, 10, x, y);
        }
        else if (sscanf(c, "wheel %d", &n) == 1) { for (int i = 0; i < (n < 0 ? -n : n) && i < 30; i++) test_input_add(at + i * 0.06, 4, n < 0 ? -1 : 1, 0); }
        else if (sscanf(c, "key %c %lf", &key, &secs) == 2 && key >= 'a' && key <= 'z' && c[5] == ' ') { test_input_add(at, 5, 4 + (key - 'a'), key); test_input_add(at + secs, 6, 4 + (key - 'a'), key); g_tin_keys_on = 1; }
        else {   // named keys: key return 0.1 (also enter, escape, space, tab): SDL scancode and key code
            char name[16] = "";
            static const struct { const char *n; int scan, sym; } named[] = {{"return", 40, 13}, {"enter", 40, 13}, {"escape", 41, 27}, {"space", 44, 32}, {"tab", 43, 9}};
            if (sscanf(c, "key %15s %lf", name, &secs) == 2)
                for (size_t i = 0; i < sizeof named / sizeof named[0]; i++)
                    if (!strcmp(name, named[i].n)) { test_input_add(at, 5, named[i].scan, named[i].sym); test_input_add(at + secs, 6, named[i].scan, named[i].sym); g_tin_keys_on = 1; break; }
        }
        e = strpbrk(e, ";|");   // steps are separated by ; or | (autobench.ps1 -Extra already splits at ;)
        if (e) e++;
    }
}
static void test_input_parse(void) {
    g_ntin = 0;
    test_input_parse_from(getenv("ATTILA_TEST_INPUT"), 0);
    if (g_ntin) log_line("test input: %d scripted steps", g_ntin);
}
// ATTILA_TEST_CMD_FILE=path (tests only): every half second that file is read and deleted. Each line is one step as in
// ATTILA_TEST_INPUT without the time ("click 640 360", "key return 0.1"), run at once, or "shot" = a picture now. For
// steering a hidden copy during a long run, e.g. answering a dilemma window after looking at a picture.
void test_shot_now(void);
static void test_cmd_poll(double now) {
    static int init;
    static const char *path;
    static ULONGLONG next;
    if (!init) { init = 1; path = getenv("ATTILA_TEST_CMD_FILE"); }
    if (!path || !*path) return;
    ULONGLONG t = GetTickCount64();
    if (t < next) return;
    next = t + 500;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    DeleteFileA(path);
    buf[n] = 0;
    int k = 0;   // forget steps that ran once already, so there is room
    for (int i = 0; i < g_ntin; i++) if (!g_tin[i].done) g_tin[k++] = g_tin[i];
    g_ntin = k;
    for (char *line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        if (!strncmp(line, "shot", 4)) { test_shot_now(); log_line("test command: picture"); continue; }
        char step[160];
        snprintf(step, sizeof step, "0:%.150s", line);
        g_tin_every = 0;
        test_input_parse_from(step, now + 0.1);
        log_line("test command: %.150s", line);
    }
}
// The scripted test mouse for mouse_cursor.inc (the game reading and moving the cursor for camera drags): in a hidden test
// the real cursor is never read or moved; a move becomes a scripted mouse move at once. The game thread calls these while
// the event thread reads the steps, so both take g_tin_lock.
static volatile LONG g_tin_lock;
static void tin_lock(void) { while (InterlockedExchange(&g_tin_lock, 1)) Sleep(0); }
static void tin_unlock(void) { InterlockedExchange(&g_tin_lock, 0); }
int test_mouse_get(int *x, int *y) {
    if (!test_hidden()) return 0;
    *x = g_tin_x < 0 ? 0 : g_tin_x;
    *y = g_tin_x < 0 ? 0 : g_tin_y;
    return 1;
}
int test_mouse_warp(int x, int y) {
    if (!test_hidden()) return 0;
    tin_lock();
    if (g_ntin < 0) test_input_parse();
    double now = test_bench_seconds();
    double every = g_tin_every;
    g_tin_every = 0;
    test_input_add(now > 0 ? now : 0, 1, x, y);
    g_tin_every = every;
    tin_unlock();
    static int told;
    if (told++ < 40) log_line("test mouse: the game put the cursor at %d,%d", x, y);
    return 1;
}
static int test_input_next_locked(uint8_t *ev);
static int test_input_next(uint8_t *ev) {
    tin_lock();
    int r = test_input_next_locked(ev);
    tin_unlock();
    return r;
}
static int test_input_next_locked(uint8_t *ev) {
    if (g_ntin < 0) test_input_parse();
    double now = test_bench_seconds();
    if (now >= 0) test_cmd_poll(now);
    if (!g_ntin) return 0;
    if (now < 0) return 0;
    static uint32_t (__cdecl *window_id)(void *);
    if (!window_id) window_id = (void *)GetProcAddress(sdl_dll(), "SDL_GetWindowID");
    uint32_t wid = g_test_window && window_id ? window_id(g_test_window) : 1;
    for (int i = 0; i < g_ntin; i++) {
        TestInput *t = &g_tin[i];
        if (t->done || t->at > now) continue;
        if (t->every > 0) { t->at += t->every; while (t->at <= now) t->at += t->every; }   // a repeating step comes back after its period (missed ones are skipped, the phase stays)
        else t->done = 1;
        int quiet = t->fired++ >= 2 && t->every > 0;
        memset(ev, 0, 56);
        uint32_t *u = (uint32_t *)ev;
        int32_t *s = (int32_t *)ev;
        u[1] = GetTickCount(); u[2] = wid;
        switch (t->kind) {
        case 1: u[0] = 0x400; u[4] = (uint32_t)g_tin_buttons; s[7] = g_tin_x < 0 ? 0 : t->a - g_tin_x; s[8] = g_tin_x < 0 ? 0 : t->b - g_tin_y; s[5] = g_tin_x = t->a; s[6] = g_tin_y = t->b; break;
        case 2: u[0] = 0x401; ev[16] = 1; ev[17] = 1; ev[18] = 1; s[5] = g_tin_x = t->a; s[6] = g_tin_y = t->b; g_tin_buttons |= 1; break;
        case 3: u[0] = 0x402; ev[16] = 1; ev[17] = 0; ev[18] = 1; s[5] = g_tin_x = t->a; s[6] = g_tin_y = t->b; g_tin_buttons &= ~1; break;
        case 4: u[0] = 0x403; s[4] = 0; s[5] = t->a; break;                                   // which = 0, x = 0, y = steps
        case 7: u[0] = 0x401; ev[16] = 2; ev[17] = 1; ev[18] = 1; s[5] = g_tin_x = t->a; s[6] = g_tin_y = t->b; g_tin_buttons |= 2; break;    // middle button
        case 8: u[0] = 0x402; ev[16] = 2; ev[17] = 0; ev[18] = 1; s[5] = g_tin_x = t->a; s[6] = g_tin_y = t->b; g_tin_buttons &= ~2; break;
        case 9: u[0] = 0x401; ev[16] = 1; ev[17] = 1; ev[18] = 2; s[5] = g_tin_x = t->a; s[6] = g_tin_y = t->b; g_tin_buttons |= 1; break;     // second press of a double click
        case 10: u[0] = 0x402; ev[16] = 1; ev[17] = 0; ev[18] = 2; s[5] = g_tin_x = t->a; s[6] = g_tin_y = t->b; g_tin_buttons &= ~1; break;
        case 5: u[0] = 0x300; ev[12] = 1; s[4] = t->a; s[5] = t->b; g_tin_keys[t->a & 511] = 1; break;   // state pressed; scancode, key code
        default: u[0] = 0x301; ev[12] = 0; s[4] = t->a; s[5] = t->b; g_tin_keys[t->a & 511] = 0; break;
        }
        if (!quiet) log_line("test input: second %.1f: %s %d %d", now, t->kind == 1 ? "mouse to" : t->kind == 2 ? "left button down at" : t->kind == 3 ? "left button up at" : t->kind == 4 ? "wheel step" : t->kind == 5 ? "key down" : t->kind == 7 ? "middle button down at" : t->kind == 8 ? "middle button up at" : t->kind == 9 ? "left double-click press at" : t->kind == 10 ? "left double-click release at" : "key up", t->a, t->b);
        return 1;
    }
    return 0;
}
SHIM(uint32_t, SDL_GetMouseState, (int *x, int *y)) {
    static uint32_t (__cdecl *r)(int *, int *);
    if (!r) r = (void *)GetProcAddress(sdl_dll(), "SDL_GetMouseState");
    if (g_tin_x >= 0) { if (x) *x = g_tin_x; if (y) *y = g_tin_y; return (uint32_t)g_tin_buttons; }   // a scripted test has taken the mouse over
    return r(x, y);
}
SHIM(const uint8_t *, SDL_GetKeyboardState, (int *numkeys)) {
    static const uint8_t *(__cdecl *r)(int *);
    if (!r) r = (void *)GetProcAddress(sdl_dll(), "SDL_GetKeyboardState");
    int n = 0;
    const uint8_t *real = r(&n);
    if (numkeys) *numkeys = n;
    if (!g_tin_keys_on || !real) return real;
    if (n > 512) n = 512;
    for (int i = 0; i < n; i++) g_tin_keystate[i] = real[i] | g_tin_keys[i];
    return g_tin_keystate;
}
SHIM(int, SDL_PollEvent, (void *event)) {
    static int (__cdecl *r)(void *);
    if (!r) r = (void *)GetProcAddress(sdl_dll(), "SDL_PollEvent");
    test_auto_tick();   // the game's event pump, on its main thread: where an unattended run starts the benchmark
    if (!event) return r(event);
    for (;;) {   // a left click on the Discord button (menu_button.c) is ours: the game gets the next event instead
        int got = test_input_next(event) ? 1 : r(event);
        if (!got || !menu_button_event(event)) return got;
    }
}
// A picture of the game's own back buffer on the OpenGL route (the frame buffer object present() has just read from,
// device +0x1644; rows come bottom-up). Whatever pack state the game had is put back.
static void *gl_real(const char *name);
static int g_gl_shot_window;   // 1 = the picture comes from the window's own frame buffer (what is about to be shown)
static void gl_test_shot(int second) {
    uint8_t *dev = *(uint8_t **)0x5252e20;   // WARSCAPE::WS_PLATFORM::m_pf_device
    if (!dev) return;
    int w = *(int *)(dev + 0x1610), h = *(int *)(dev + 0x1614);
    void (__stdcall *read_pixels)(int, int, int, int, unsigned, unsigned, void *) = gl_real("glReadPixels");
    void (__stdcall *get_int)(unsigned, int *) = gl_real("glGetIntegerv");
    void (__stdcall *pixel_store)(unsigned, int) = gl_real("glPixelStorei");
    void (__stdcall *bind_buffer)(unsigned, unsigned) = gl_real("glBindBuffer");
    void (__stdcall *bind_fb)(unsigned, unsigned) = gl_real("glBindFramebuffer");
    void (__stdcall *read_buffer)(unsigned) = gl_real("glReadBuffer");
    if (!read_pixels || !get_int || !pixel_store || !bind_buffer || !bind_fb || !read_buffer || w <= 0 || h <= 0) return;
    int pack_buffer = 0, align = 4, row_length = 0, read_fb = 0;
    get_int(0x88ED, &pack_buffer); get_int(0x0D05, &align); get_int(0x0D02, &row_length); get_int(0x8CAA, &read_fb);
    if (pack_buffer) bind_buffer(0x88EB, 0);
    bind_fb(0x8CA8, g_gl_shot_window ? 0 : *(unsigned *)(dev + 0x1644));
    read_buffer(g_gl_shot_window ? 0x0405 /* GL_BACK */ : 0x8CE0);
    pixel_store(0x0D05, 1); pixel_store(0x0D02, 0);
    uint8_t *buf = malloc((size_t)w * (size_t)h * 4);
    if (buf) {
        read_pixels(0, 0, w, h, 0x80E1 /* BGRA */, 0x1401, buf);
        test_shot_save(second, buf, w, h, w * 4, "bgra", 1);
        free(buf);
    }
    pixel_store(0x0D05, align); pixel_store(0x0D02, row_length);
    bind_fb(0x8CA8, (unsigned)read_fb);
    if (pack_buffer) bind_buffer(0x88EB, (unsigned)pack_buffer);
}

// The frame-rate number on the OpenGL routes (fps_overlay.c), drawn into the window's own picture right before it is
// shown: a black box and white dots, each a clear inside a scissor box, so no shader, texture or buffer of the game's is
// touched. The five pieces of OpenGL state used are read first and put back exactly (the game's layer remembers what it set).
static void gl_fps_overlay(void) {
    uint8_t *dev = *(uint8_t **)0x5252e20;   // WARSCAPE::WS_PLATFORM::m_pf_device
    if (!dev) return;
    int W = *(int *)(dev + 0x1610), H = *(int *)(dev + 0x1614);
    static void (__stdcall *get_int)(unsigned, int *), (__stdcall *get_float)(unsigned, float *), (__stdcall *enable)(unsigned),
                (__stdcall *disable)(unsigned), (__stdcall *scissor)(int, int, int, int), (__stdcall *clear_color)(float, float, float, float),
                (__stdcall *clear)(unsigned), (__stdcall *bind_fb)(unsigned, unsigned), (__stdcall *get_bool_i)(unsigned, unsigned, unsigned char *),
                (__stdcall *mask_i)(unsigned, unsigned char, unsigned char, unsigned char, unsigned char);
    static unsigned char (__stdcall *is_enabled)(unsigned);
    static int looked;
    if (!looked) {
        looked = 1;
        get_int = gl_real("glGetIntegerv"); get_float = gl_real("glGetFloatv"); enable = gl_real("glEnable"); disable = gl_real("glDisable");
        scissor = gl_real("glScissor"); clear_color = gl_real("glClearColor"); clear = gl_real("glClear"); bind_fb = gl_real("glBindFramebuffer");
        get_bool_i = gl_real("glGetBooleani_v"); mask_i = gl_real("glColorMaski"); is_enabled = gl_real("glIsEnabled");
    }
    if (!get_int || !get_float || !enable || !disable || !scissor || !clear_color || !clear || !bind_fb || !is_enabled || W <= 0 || H <= 0) return;
    const char *text = fps_overlay_tick();
    int chars = (int)strlen(text), x, y, dot;
    fps_overlay_place(W, H, chars, &x, &y, &dot);
    int bw = (6 * chars + 1) * dot, bh = 9 * dot;
    int draw_fb = 0, box[4] = {0, 0, W, H};
    float cc[4] = {0, 0, 0, 0};
    unsigned char m[4] = {1, 1, 1, 1};
    get_int(0x8CA6, &draw_fb); get_int(0x0C10, box); get_float(0x0C22, cc);
    unsigned char scissor_was_on = is_enabled(0x0C11);
    if (get_bool_i && mask_i) get_bool_i(0x0C23, 0, m);
    if (draw_fb) bind_fb(0x8CA9, 0);
    if (!scissor_was_on) enable(0x0C11);
    if (get_bool_i && mask_i) mask_i(0, 1, 1, 1, 1);
    clear_color(0, 0, 0, 1);
    scissor(x, H - y - bh, bw, bh);   // OpenGL counts rows from the bottom
    clear(0x4000);
    clear_color(1, 1, 1, 1);
    for (int ci = 0; ci < chars; ci++) {
        const uint8_t *g = fps_overlay_glyph(text[ci]);
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++) {
                if (!(g[row] & (0x10 >> col))) continue;
                int end = col;
                while (end + 1 < 5 && (g[row] & (0x10 >> (end + 1)))) end++;   // a run of dots in one clear
                scissor(x + (1 + ci * 6 + col) * dot, H - (y + (row + 2) * dot), (end - col + 1) * dot, dot);
                clear(0x4000);
                col = end;
            }
    }
    clear_color(cc[0], cc[1], cc[2], cc[3]);
    scissor(box[0], box[1], box[2], box[3]);
    if (get_bool_i && mask_i) mask_i(0, m[0], m[1], m[2], m[3]);
    if (!scissor_was_on) disable(0x0C11);
    if (draw_fb) bind_fb(0x8CA9, (unsigned)draw_fb);
}

// The Discord button on the OpenGL routes (menu_button.c): its picture sits in a texture of our own with a frame buffer object
// around it, and a blit copies it into the window's picture right before it is shown. The OpenGL state touched (read and draw
// frame buffers, scissor test, colour mask 0, and during an upload the texture of the active unit and the unpack settings) is
// read first and put back exactly.
static void gl_menu_button(void) {
    uint8_t *dev = *(uint8_t **)0x5252e20;   // WARSCAPE::WS_PLATFORM::m_pf_device
    if (!dev) return;
    int W = *(int *)(dev + 0x1610), H = *(int *)(dev + 0x1614), x, y, w, h;
    const uint8_t *px = menu_button_pixels(W, H, 0, &x, &y, &w, &h);
    if (!px) return;
    static void (__stdcall *get_int)(unsigned, int *), (__stdcall *enable)(unsigned), (__stdcall *disable)(unsigned),
                (__stdcall *gen_tex)(int, unsigned *), (__stdcall *bind_tex)(unsigned, unsigned), (__stdcall *tex_param)(unsigned, unsigned, int),
                (__stdcall *tex_image)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *),
                (__stdcall *pixel_store)(unsigned, int), (__stdcall *bind_buffer)(unsigned, unsigned),
                (__stdcall *gen_fb)(int, unsigned *), (__stdcall *bind_fb)(unsigned, unsigned), (__stdcall *fb_tex)(unsigned, unsigned, unsigned, unsigned, int),
                (__stdcall *blit)(int, int, int, int, int, int, int, int, unsigned, unsigned),
                (__stdcall *get_bool_i)(unsigned, unsigned, unsigned char *), (__stdcall *mask_i)(unsigned, unsigned char, unsigned char, unsigned char, unsigned char);
    static unsigned char (__stdcall *is_enabled)(unsigned);
    static int looked, broken;
    static unsigned tex, fbo;
    static const uint8_t *in_tex;
    static int tw, th;
    if (!looked) {
        looked = 1;
        get_int = gl_real("glGetIntegerv"); enable = gl_real("glEnable"); disable = gl_real("glDisable"); is_enabled = gl_real("glIsEnabled");
        gen_tex = gl_real("glGenTextures"); bind_tex = gl_real("glBindTexture"); tex_param = gl_real("glTexParameteri"); tex_image = gl_real("glTexImage2D");
        pixel_store = gl_real("glPixelStorei"); bind_buffer = gl_real("glBindBuffer"); gen_fb = gl_real("glGenFramebuffers"); bind_fb = gl_real("glBindFramebuffer");
        fb_tex = gl_real("glFramebufferTexture2D"); blit = gl_real("glBlitFramebuffer"); get_bool_i = gl_real("glGetBooleani_v"); mask_i = gl_real("glColorMaski");
        broken = !get_int || !enable || !disable || !is_enabled || !gen_tex || !bind_tex || !tex_param || !tex_image || !pixel_store || !bind_buffer ||
                 !gen_fb || !bind_fb || !fb_tex || !blit;
        if (broken) log_line("Discord button: OpenGL functions missing, not drawn");
    }
    if (broken || W <= 0 || H <= 0) return;
    int read_fb = 0, draw_fb = 0;
    get_int(0x8CAA, &read_fb); get_int(0x8CA6, &draw_fb);           // GL_READ / GL_DRAW_FRAMEBUFFER_BINDING
    if (in_tex != px || tw != w || th != h) {   // a new picture (first time, another size, or the mouse came onto / left the button)
        int tex2d = 0, unpack_buf = 0, align = 4, row_len = 0;
        get_int(0x8069, &tex2d); get_int(0x88EF, &unpack_buf); get_int(0x0CF5, &align); get_int(0x0CF2, &row_len);
        if (!tex) gen_tex(1, &tex);
        if (unpack_buf) bind_buffer(0x88EC, 0);                      // GL_PIXEL_UNPACK_BUFFER
        pixel_store(0x0CF5, 4); pixel_store(0x0CF2, 0);
        bind_tex(0x0DE1, tex);
        tex_param(0x0DE1, 0x2801, 0x2600); tex_param(0x0DE1, 0x2800, 0x2600);   // nearest, no mip levels
        tex_param(0x0DE1, 0x813D, 0);                                // GL_TEXTURE_MAX_LEVEL 0
        tex_image(0x0DE1, 0, 0x8058, w, h, 0, 0x80E1, 0x1401, px);   // RGBA8 from B,G,R,A bytes
        bind_tex(0x0DE1, (unsigned)tex2d);
        pixel_store(0x0CF5, align); pixel_store(0x0CF2, row_len);
        if (unpack_buf) bind_buffer(0x88EC, (unsigned)unpack_buf);
        if (!fbo) gen_fb(1, &fbo);
        bind_fb(0x8CA8, fbo);
        fb_tex(0x8CA8, 0x8CE0, 0x0DE1, tex, 0);                      // GL_COLOR_ATTACHMENT0
        in_tex = px; tw = w; th = h;
    }
    unsigned char m[4] = {1, 1, 1, 1};
    unsigned char scissor_was_on = is_enabled(0x0C11);
    if (get_bool_i && mask_i) { get_bool_i(0x0C23, 0, m); mask_i(0, 1, 1, 1, 1); }
    if (scissor_was_on) disable(0x0C11);
    bind_fb(0x8CA8, fbo);
    bind_fb(0x8CA9, 0);
    // texture rows are the picture's rows from the top, the window counts from the bottom: copied upside down
    blit(0, 0, w, h, x, H - y, x + w, H - y - h, 0x4000, 0x2600);
    bind_fb(0x8CA8, (unsigned)read_fb);
    bind_fb(0x8CA9, (unsigned)draw_fb);
    if (scissor_was_on) enable(0x0C11);
    if (get_bool_i && mask_i) mask_i(0, m[0], m[1], m[2], m[3]);
}

// DirectX 11 route (loader\dx11_device.inc, ATTILA_DX11=1): no OpenGL context at all. AMD's OpenGL driver puts its own
// DXGI swap chain on the window, and DirectX 11 is then refused one (0x80070005). The game gets a stand-in context, the other
// SDL OpenGL calls do nothing for it, and the DirectX 11 present calls SDL_GL_SwapWindow only for the frame count / frame cap.
int g_dx11_no_gl;
static char g_dx11_fake_context;
SHIM(int, SDL_GL_MakeCurrent, (void *window, void *context)) {
    static int (__cdecl *r)(void *, void *);
    if (g_dx11_no_gl) return 0;
    if (!r) r = (void *)GetProcAddress(sdl_dll(), "SDL_GL_MakeCurrent");
    return r(window, context);
}
SHIM(void, SDL_GL_DeleteContext, (void *context)) {
    static void (__cdecl *r)(void *);
    if (g_dx11_no_gl || context == &g_dx11_fake_context) return;
    if (!r) r = (void *)GetProcAddress(sdl_dll(), "SDL_GL_DeleteContext");
    r(context);
}
SHIM(int, SDL_GL_SetSwapInterval, (int interval)) {
    static int (__cdecl *r)(int);
    if (g_dx11_no_gl) return 0;
    if (!r) r = (void *)GetProcAddress(sdl_dll(), "SDL_GL_SetSwapInterval");
    return r(interval);
}
SHIM(void *, SDL_GL_CreateContext, (void *window)) {
    if (g_dx11_no_gl) { log_line("OpenGL context: none (DirectX 11 route)"); return &g_dx11_fake_context; }
    static void *(__cdecl *r)(void *);
    static const char *(__cdecl *sdl_error)(void);
    if (!r) { r = (void *)GetProcAddress(sdl_dll(), "SDL_GL_CreateContext"); sdl_error = (void *)GetProcAddress(sdl_dll(), "SDL_GetError"); }
    void *context = r(window);
    if (!context) { log_line("OpenGL context: NOT made. SDL says: %s", sdl_error ? sdl_error() : "?"); return context; }
    const char *(__stdcall *get_string)(unsigned) = gl_real("glGetString");
    unsigned (__stdcall *get_error)(void) = gl_real("glGetError");
    const char *vendor = get_string ? get_string(0x1F00) : NULL, *renderer = get_string ? get_string(0x1F01) : NULL, *version = get_string ? get_string(0x1F02) : NULL;
    log_line("OpenGL context made: %s | %s | OpenGL %s; error state %x", vendor ? vendor : "?", renderer ? renderer : "?", version ? version : "?", get_error ? get_error() : 0xFFFF);
    return context;
}
volatile unsigned g_frame_no;   // frames shown so far (loader.c spreads some per-frame checks over frames with it)

// Frames per second, once every 5 seconds, into the log.
SHIM(void, SDL_GL_SwapWindow, (void *w)) {
    static void (__cdecl *r)(void *);
    static long frames;
    static ULONGLONG start;
    if (!r) r = (void *)GetProcAddress(sdl_dll(), "SDL_GL_SwapWindow");
    {   // Test only, ATTILA_TEST_BURN_US=n: keeps the main thread busy n microseconds every frame. If fps falls by that much the
        // main thread sets the pace; if it does not, something else does and the main thread has slack.
        static long long burn = -1, freq;
        if (burn < 0) { const char *e = getenv("ATTILA_TEST_BURN_US"); LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; burn = e ? atoll(e) * freq / 1000000 : 0; }
        if (burn) { LARGE_INTEGER t0, t; QueryPerformanceCounter(&t0); do QueryPerformanceCounter(&t); while (t.QuadPart - t0.QuadPart < burn); }
    }
    if (!g_dx11_no_gl) {
        int shot = test_shot_due();   // unattended test runs: a picture of this frame before it is shown
        static int shot_final = -1;   // test: ATTILA_TEST_SHOT_FINAL=1 takes it from the window's own picture, after the frame-rate number
        if (shot_final < 0) shot_final = getenv("ATTILA_TEST_SHOT_FINAL") != NULL;
        if (shot >= 0 && !shot_final) gl_test_shot(shot);
        if (fps_overlay_on()) gl_fps_overlay();
        if (menu_button_visible()) gl_menu_button();
        if (shot >= 0 && shot_final) { g_gl_shot_window = 1; gl_test_shot(shot); g_gl_shot_window = 0; }
        r(w);
    }
    test_frame_done();
    g_frame_no++;
    if (g_frame_no == 600) { extern void dx12_start_worked(void); dx12_start_worked(); }   // drawing for a while: a DirectX 12 start got through
    // Frame cap: menus otherwise run at thousands of frames per second, working the card for nothing. ATTILA_FPS_CAP=0 turns it off.
    static LARGE_INTEGER freq, last;
    static long long cap_ticks = -1;
    static HANDLE timer;
    if (cap_ticks < 0) {
        const char *e = getenv("ATTILA_FPS_CAP");
        int cap = e ? atoi(e) : 240;
        QueryPerformanceFrequency(&freq);
        cap_ticks = cap > 0 ? freq.QuadPart / cap : 0;
        timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* high resolution */, TIMER_ALL_ACCESS);
        QueryPerformanceCounter(&last);
    }
    if (cap_ticks) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        long long left = cap_ticks - (t.QuadPart - last.QuadPart);
        if (left > 0 && timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(left * 10000000 / freq.QuadPart);
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(timer, 100);
        }
        QueryPerformanceCounter(&last);
    }
    ULONGLONG now = GetTickCount64();
    if (!start) start = now;
    frames++;
    // Long waits between frames, such as loading: logged when normal speed returns, so loading can be timed.
    {
        static ULONGLONG bucket_start, slow_start;
        static long bucket_frames;
        static int slow, said;
        if (!said) {
            said = 1;
            log_line("settings in use: enable_vertex_attrib_binding %u, gl_uniform_buffer_mode %d", *(uint8_t *)0x526961a, *(int *)0x5269714);
            // who is drawing: the card's own OpenGL driver or Mesa on DirectX 12, and which OpenGL version it offers
            const char *(__stdcall *get_string)(unsigned) = gl_real("glGetString");
            if (get_string && !g_dx11_no_gl) {   // on the DirectX 11 route there is no OpenGL to ask; its own line names the card
                const char *vendor = get_string(0x1F00), *renderer = get_string(0x1F01), *version = get_string(0x1F02);
                log_line("graphics driver: %s | %s | OpenGL %s", vendor ? vendor : "?", renderer ? renderer : "?", version ? version : "?");
            }
        }
        if (!bucket_start) bucket_start = now;
        bucket_frames++;
        if (now - bucket_start >= 1000) {
            static int steady_run, armed;
            if (bucket_frames * 1000 >= 25 * (long)(now - bucket_start)) {
                if (armed && ++steady_run == 8) { log_line("steady for 8 s"); armed = 0; }
            } else { steady_run = 0; armed = 1; }
            int is_slow = bucket_frames * 1000 < 10 * (long)(now - bucket_start);
            if (is_slow && !slow) { slow = 1; slow_start = bucket_start; }
            else if (!is_slow && slow) {
                slow = 0;
                log_line("slow period ended: %.1f s", (double)(bucket_start - slow_start) / 1000.0);
                gl_count_report();
            }
            bucket_start = now;
            bucket_frames = 0;
        }
    }
    static ULONGLONG fps_every;   // ATTILA_FPS_INTERVAL=ms (tests: finer fps lines), default 30 s
    if (!fps_every) { const char *e = getenv("ATTILA_FPS_INTERVAL"); fps_every = e && atoi(e) >= 500 ? (ULONGLONG)atoi(e) : 30000; }
    if (now - start >= fps_every) {
        extern void *shim_settings_object(int);
        shim_settings_object(1);
        log_line("fps %.1f", frames * 1000.0 / (double)(now - start));
        { extern void pass_stats_report(long); pass_stats_report(frames); }
        { extern void shadow_rate_report(long); shadow_rate_report(frames); }
        upscale_report();
        tex_trace_report(frames);
        lazy_unit_report(frames);
        { extern void batch_report(long); batch_report(frames); } { extern void task_spin_report(long); task_spin_report(frames); }
        { extern void task_report(void); task_report(); } { extern void mutex_stats_report(long); mutex_stats_report(frames); } { extern void vfx_flip(void); vfx_flip(); } { extern void fast_sort_report(long); fast_sort_report(frames); } { extern void task_waits_report(long); task_waits_report(frames); } { extern void prep_parallel_report(long); prep_parallel_report(frames); } { extern void tracker_grain_report(void); tracker_grain_report(); } { extern void ui_x2_report(void); ui_x2_report(); } { extern void log_flip_report(void); log_flip_report(); }
        gl_count_report();
        extern void vfx_count_report(void);
        vfx_count_report();
        extern void vfx_gate_report(void);
        vfx_gate_report();
        frames = 0;
        start = now;
    }
}

// Called by the loader for imports no fixed shim supplies.
void *shim_dynamic(const char *name) {
    if (!strncmp(name, "SDL_", 4)) {
        void *p = (void *)GetProcAddress(sdl_dll(), name);
        return p ? make_bridge(p) : NULL;
    }
    if (name[0] == 'g' && name[1] == 'l' && name[2] >= 'A' && name[2] <= 'Z') return gl_resolve(name);
    return NULL;
}

#include "render_scale.inc"
#include "tex_trace.inc"
#include "lazy_unit.inc"
#include "draw_callers.inc"
#include "task_spin.inc"
