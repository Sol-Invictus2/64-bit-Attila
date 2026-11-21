// script_trace.inc - TEST ONLY (switch ATTILA_SCRIPT_TRACE=1, off by default): logs campaign Lua script errors and the
// script commands that change characters (kill_character, spawn_character_into_family_tree, set_character_immortality,
// force_add_trait), so the logs of the PCs of a multiplayer test can be compared line by line. Read-only: the hooks only
// look at the Lua stack (no lua_tolstring on numbers, which would change the value) and then run the game's own code.
// Every hooked function is checked byte by byte first (hook_entry); a function that differs is left alone.

static SYSV int (*g_st_gettop)(void *L) = (SYSV int (*)(void *))0x355fa60;          // lua_gettop
static SYSV int (*g_st_type)(void *L, int i) = (SYSV int (*)(void *, int))0x355fef0;  // lua_type
static SYSV const char *(*g_st_tolstring)(void *L, int i, size_t *n) = (SYSV const char *(*)(void *, int, size_t *))0x3560b20;
static SYSV double (*g_st_tonumber)(void *L, int i) = (SYSV double (*)(void *, int))0x3560860;
static SYSV int (*g_st_toboolean)(void *L, int i) = (SYSV int (*)(void *, int))0x3560a40;

typedef SYSV int (*ScriptCmdFn)(void *self, void *L);
typedef SYSV int (*LuaErrFn)(void *L);
static ScriptCmdFn g_st_kill, g_st_spawn, g_st_immortal, g_st_trait;
static LuaErrFn g_st_error;
static volatile long g_st_lines;

static int st_room(void) { return InterlockedIncrement(&g_st_lines) <= 4000; }

static void st_args(void *L, char *out, size_t cap) {   // "'character_cqi:629', false, false"
    size_t n = 0;
    out[0] = 0;
    int top = g_st_gettop(L);
    for (int i = 1; i <= top && i <= 14; i++) {
        char one[100];
        int t = g_st_type(L, i);
        if (t == 4) { const char *s = g_st_tolstring(L, i, NULL); snprintf(one, sizeof one, "'%.80s'", s ? s : ""); }
        else if (t == 3) snprintf(one, sizeof one, "%g", g_st_tonumber(L, i));
        else if (t == 1) snprintf(one, sizeof one, "%s", g_st_toboolean(L, i) ? "true" : "false");
        else if (t == 0) snprintf(one, sizeof one, "nil");
        else snprintf(one, sizeof one, "<type %d>", t);
        int w = snprintf(out + n, cap - n, "%s%s", i > 1 ? ", " : "", one);
        if (w < 0 || (size_t)w >= cap - n) break;
        n += (size_t)w;
    }
}

static SYSV int st_kill(void *self, void *L) {
    if (st_room()) {
        char a[480];
        st_args(L, a, sizeof a);
        log_line("script: kill_character(%s), command queue %s", a, *(void **)((uint8_t *)self + 8) ? "set" : "MISSING");
    }
    return g_st_kill(self, L);
}
static SYSV int st_spawn(void *self, void *L) {
    if (st_room()) { char a[480]; st_args(L, a, sizeof a); log_line("script: spawn_character_into_family_tree(%s)", a); }
    return g_st_spawn(self, L);
}
static SYSV int st_immortal(void *self, void *L) {
    if (st_room()) { char a[480]; st_args(L, a, sizeof a); log_line("script: set_character_immortality(%s)", a); }
    return g_st_immortal(self, L);
}
static SYSV int st_trait(void *self, void *L) {
    if (st_room()) {
        char a[480];
        st_args(L, a, sizeof a);
        log_line("script: force_add_trait(%s), command queue %s", a, *(void **)((uint8_t *)self + 8) ? "set" : "MISSING");
    }
    return g_st_trait(self, L);
}
static SYSV int st_error(void *L) {   // the game's Lua error handler: the message is on top of the stack
    if (st_room()) {
        if (g_st_type(L, -1) == 4) { const char *s = g_st_tolstring(L, -1, NULL); log_line("script error: %.600s", s ? s : "?"); }
        else log_line("script error (message of Lua type %d)", g_st_type(L, -1));
    }
    return g_st_error(L);
}

static void patch_script_trace(void) {
    if (!getenv("ATTILA_SCRIPT_TRACE")) return;
    static const uint8_t err_old[5] = {0x55, 0x41, 0x57, 0x41, 0x56};                  // error_handler 2de8960
    static const uint8_t kill_old[5] = {0x41, 0x57, 0x41, 0x56, 0x53};                 // kill_character 1744550
    static const uint8_t spawn_old[6] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55};          // spawn_..._family_tree 1746280
    static const uint8_t imm_old[7] = {0x41, 0x56, 0x53, 0x48, 0x83, 0xEC, 0x18};      // set_character_immortality 17461e0
    static const uint8_t trait_old[5] = {0x41, 0x57, 0x41, 0x56, 0x53};                // force_add_trait 17409d0
    g_st_error = (LuaErrFn)hook_entry(0x2de8960, err_old, 5, (void *)st_error);
    g_st_kill = (ScriptCmdFn)hook_entry(0x1744550, kill_old, 5, (void *)st_kill);
    g_st_spawn = (ScriptCmdFn)hook_entry(0x1746280, spawn_old, 6, (void *)st_spawn);
    g_st_immortal = (ScriptCmdFn)hook_entry(0x17461e0, imm_old, 7, (void *)st_immortal);
    g_st_trait = (ScriptCmdFn)hook_entry(0x17409d0, trait_old, 5, (void *)st_trait);
    log_line("test: script trace (ATTILA_SCRIPT_TRACE): errors %s, kill %s, spawn %s, immortality %s, traits %s",
             g_st_error ? "on" : "NOT hooked", g_st_kill ? "on" : "NOT hooked", g_st_spawn ? "on" : "NOT hooked",
             g_st_immortal ? "on" : "NOT hooked", g_st_trait ? "on" : "NOT hooked");
}
