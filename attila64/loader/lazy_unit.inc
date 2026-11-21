// Texture unit selection only when something uses it (TEST, ATTILA_LAZY_UNIT=1). Part of shim_gfx.c (included at its end).
//
// For every draw the game walks all 13 texture slots of the shader: glActiveTexture(slot), then glBindTexture only if that
// slot's texture changed. Measured 2026-11-11 (test copy, big battle, every call timed): 30.4 million glActiveTexture calls
// in 5 s (about 73,000 a frame) against 2.2 million glBindTexture. So more than nine in ten selections are followed by
// nothing that depends on the selected unit, yet each one crosses into the driver.
//
// Here glActiveTexture only notes the unit. The real call is made just before the next OpenGL call whose effect depends on
// the selected unit, and only if the unit differs from the one OpenGL really has:
//   glBindTexture, glTexParameter*, glTexImage*, glTexSubImage*, glTexStorage*, glTexBuffer*, glCompressedTex*, glCopyTex*,
//   glGenerateMipmap, glGetTex*, glGetCompressedTex*, and the glGet<number> queries (which can ask for the unit itself).
// Those are found by name when the game first asks for a function; the two hot ones have their own small wrappers, the rest
// go through a generated stub that calls lazy_unit_flush with all argument registers kept. Calls that name their unit
// (glBindSampler, glUniform1i, glBindImageTexture) do not depend on the selection and are left alone.
// The state is kept per thread, as OpenGL keeps it per context. Code of ours that calls OpenGL directly and touches a texture
// binding calls lazy_unit_flush first.
typedef void (SYSV *LazyUnitFn)(unsigned);
typedef void (SYSV *LazyBindFn)(unsigned, unsigned);
static int g_lazy_unit_on = -1;
static LazyUnitFn g_lazy_active_inner;
static LazyBindFn g_lazy_bind_inner;
static __thread unsigned t_unit_want, t_unit_real;   // 0 = nothing asked yet / not known
static volatile unsigned long long g_lazy_asked, g_lazy_sent;

void lazy_unit_flush(void) {
    if (t_unit_want != t_unit_real && t_unit_want && g_lazy_active_inner) {
        g_lazy_active_inner(t_unit_want);
        t_unit_real = t_unit_want;
        g_lazy_sent++;
    }
}
SYSV static void w_lazy_active(unsigned unit) { t_unit_want = unit; g_lazy_asked++; }
SYSV static void w_lazy_bind(unsigned target, unsigned texture) { lazy_unit_flush(); g_lazy_bind_inner(target, texture); }

// A stub the game calls instead of `target`: saves the argument registers (six whole-number, eight floating-point), calls
// lazy_unit_flush, puts them back and jumps to `target`. Entered with the stack 8 off a 16-byte line, as every function is.
static void *lazy_flush_stub(void *target) {
    static uint8_t *page, *next;
    static const uint8_t save[] = {
        0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51,                 // push rdi, rsi, rdx, rcx, r8, r9
        0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00,                       // sub rsp,0xA8 (32 bytes for the callee + 8 x 16 + 8)
        0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30,   // movdqu [rsp+20h],xmm0 ; [rsp+30h],xmm1
        0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50,   // xmm2, xmm3
        0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x60, 0xF3, 0x0F, 0x7F, 0x6C, 0x24, 0x70,   // xmm4, xmm5
        0xF3, 0x0F, 0x7F, 0xB4, 0x24, 0x80, 0x00, 0x00, 0x00,                     // xmm6 -> [rsp+80h]
        0xF3, 0x0F, 0x7F, 0xBC, 0x24, 0x90, 0x00, 0x00, 0x00,                     // xmm7 -> [rsp+90h]
        0x48, 0xB8};                                                    // mov rax, lazy_unit_flush
    static const uint8_t restore[] = {
        0xFF, 0xD0,                                                     // call rax
        0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30,
        0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50,
        0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x60, 0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x70,
        0xF3, 0x0F, 0x6F, 0xB4, 0x24, 0x80, 0x00, 0x00, 0x00,
        0xF3, 0x0F, 0x6F, 0xBC, 0x24, 0x90, 0x00, 0x00, 0x00,
        0x48, 0x81, 0xC4, 0xA8, 0x00, 0x00, 0x00,                       // add rsp,0xA8
        0x41, 0x59, 0x41, 0x58, 0x59, 0x5A, 0x5E, 0x5F,                 // pop r9, r8, rcx, rdx, rsi, rdi
        0x48, 0xB8};                                                    // mov rax, target
    static const uint8_t go[] = {0xFF, 0xE0};                           // jmp rax
    size_t size = sizeof save + 8 + sizeof restore + 8 + sizeof go;
    if (!page || next + size > page + 65536) {
        page = VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!page) return target;
        next = page;
    }
    uint8_t *p = next, *start = next;
    uint64_t flush = (uint64_t)(uintptr_t)lazy_unit_flush, to = (uint64_t)(uintptr_t)target;
    memcpy(p, save, sizeof save); p += sizeof save;
    memcpy(p, &flush, 8); p += 8;
    memcpy(p, restore, sizeof restore); p += sizeof restore;
    memcpy(p, &to, 8); p += 8;
    memcpy(p, go, sizeof go); p += sizeof go;
    next = p;
    return start;
}

static int lazy_name_uses_unit(const char *n) {
    static const char *const starts[] = {"glTex", "glCompressedTex", "glCopyTex", "glGetTex", "glGetCompressedTex", "glGenerateMipmap",
                                         "glGetInteger", "glGetBoolean", "glGetFloat", "glGetDouble", "glClientActiveTexture", NULL};
    for (int i = 0; starts[i]; i++) if (!strncmp(n, starts[i], strlen(starts[i]))) return 1;
    return 0;
}

// For gl_resolve, last of all: `found` is what the game would be given for `name` (a bridge or one of our wrappers).
static void *lazy_unit_wrap(const char *name, void *found) {
    if (g_lazy_unit_on < 0) {
        g_lazy_unit_on = getenv("ATTILA_LAZY_UNIT") != NULL;
        if (g_lazy_unit_on) log_line("texture unit selection: only when used (test, ATTILA_LAZY_UNIT)");
    }
    if (!g_lazy_unit_on || !found) return found;
    if (!strcmp(name, "glActiveTexture") || !strcmp(name, "glActiveTextureARB")) { g_lazy_active_inner = (LazyUnitFn)found; return (void *)w_lazy_active; }
    if (!strcmp(name, "glBindTexture")) { g_lazy_bind_inner = (LazyBindFn)found; return (void *)w_lazy_bind; }
    if (lazy_name_uses_unit(name)) return lazy_flush_stub(found);
    return found;
}

void lazy_unit_report(long frames) {
    if (g_lazy_unit_on != 1 || frames <= 0 || !g_lazy_asked) return;
    log_line("texture unit selections per frame: %.0f asked by the game, %.0f sent to OpenGL", (double)g_lazy_asked / frames, (double)g_lazy_sent / frames);
    g_lazy_asked = g_lazy_sent = 0;
}
