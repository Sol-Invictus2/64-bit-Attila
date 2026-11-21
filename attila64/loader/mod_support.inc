// mod_support.inc - things big mods made for normal (32-bit Windows) Attila need from this engine. First user: The Dawnless
// Days (Beta 1.1.0, six tdd_pack*.pack). Part of loader.c (included there, not built by itself).

// ---- Animation bones without a translation or rotation track. UTILITYLIB::ANIMATION_KEY_FRAME<VFS_FILE_STREAM> (306b460)
// looks up each bone's place in the frame's translations (stride 12) and rotations (stride 8) and, on two paths (306b960 and
// 306bf60, a root bone whose root motion is taken out), reads and writes there without checking for -1 = "this bone has no
// track". 32-bit Attila computes index*12 in 32 bits: -1 wraps to 12 bytes before the array, junk that is thrown away. This
// engine computes it in 64 bits: 0xffffffff*12 lands 48 GB away and the game crashes while a battle loads (The Dawnless Days'
// animations, 2026-11-15: c0000005 at 306cf14). Here a -1 track points at a scratch area instead (read, changed, thrown away,
// as 32-bit does with its junk); every other bone takes the game's own code unchanged. Battle and campaign models only load
// what they show, nothing in the shared game differs. ATTILA_NO_ANIM_TRACK_FIX=1 = the game's code.
static uint8_t *g_anim_scratch;   // 128 bytes in the same block as the stubs (rip-relative reach), 16-aligned

static void anim_emit_lea_scratch(uint8_t **p, uint8_t rex, uint8_t modrm) {   // lea reg,[rip+scratch middle]
    uint8_t *c = *p;
    c[0] = rex; c[1] = 0x8D; c[2] = modrm;
    int32_t rel = (int32_t)((intptr_t)(g_anim_scratch + 64) - (intptr_t)(c + 7));
    memcpy(c + 3, &rel, 4);
    *p = c + 7;
}
static int anim_emit_jmp(uint8_t **p, uintptr_t to) {
    uint8_t *c = *p;
    int64_t rel = (int64_t)to - (int64_t)(uintptr_t)(c + 5);
    if (rel != (int32_t)rel) return 0;
    c[0] = 0xE9;
    int32_t r32 = (int32_t)rel;
    memcpy(c + 1, &r32, 4);
    *p = c + 5;
    return 1;
}
#define AE(...) do { const uint8_t b_[] = {__VA_ARGS__}; memcpy(p, b_, sizeof b_); p += sizeof b_; } while (0)

static void patch_animation_missing_tracks(void) {
    if (getenv("ATTILA_NO_ANIM_TRACK_FIX")) { log_line("test: animation tracks taken as the game does (ATTILA_NO_ANIM_TRACK_FIX)"); return; }
    static const uint8_t a_old[37] = {0x42, 0x8B, 0x0C, 0xB1, 0x4C, 0x8B, 0x6B, 0x58, 0x4C, 0x8D, 0x24, 0x49, 0x4B, 0x8D, 0x6C, 0xA5, 0x00,
                                      0x48, 0x8B, 0x4A, 0x18, 0x46, 0x8B, 0x3C, 0xB1, 0x48, 0x8B, 0x4B, 0x78, 0x48, 0x89, 0x8C, 0x24, 0x88,
                                      0x00, 0x00, 0x00};
    static const uint8_t b_old[28] = {0x42, 0x8B, 0x04, 0xB0, 0x4C, 0x8B, 0x7B, 0x58, 0x4C, 0x8D, 0x24, 0x40, 0x4B, 0x8D, 0x2C, 0xA7,
                                      0x48, 0x8B, 0x41, 0x18, 0x42, 0x8B, 0x14, 0xB0, 0x48, 0x8B, 0x73, 0x78};
    uint8_t *a = (uint8_t *)0x306b960, *b = (uint8_t *)0x306bf60;
    if (memcmp(a, a_old, sizeof a_old) || memcmp(b, b_old, sizeof b_old)) { log_line("patch skipped, game code differs: animation bones without a track"); return; }
    uint8_t *code = near_game_code(192 + 16 + 128);   // stubs (166 bytes), then the scratch area
    if (!code) { log_line("patch skipped, no memory near the game: animation bones without a track"); return; }
    g_anim_scratch = (uint8_t *)(((uintptr_t)code + 192 + 15) & ~(uintptr_t)15);
    memset(g_anim_scratch, 0, 128);
    uint8_t *p = code, *sa = p;
    // site A: translation index in ecx (base r13 -> rbp, r12), rotation index in r15d (base rcx -> [rsp+0x88])
    AE(0x9C);                                    // pushfq (the je after this keeps the flags of an earlier compare)
    AE(0x42, 0x8B, 0x0C, 0xB1);                  // mov ecx,[rcx+r14*4]
    AE(0x4C, 0x8B, 0x6B, 0x58);                  // mov r13,[rbx+0x58]
    AE(0x83, 0xF9, 0xFF);                        // cmp ecx,-1
    AE(0x75, 0x00); uint8_t *ja1 = p - 1;        // jne A1
    anim_emit_lea_scratch(&p, 0x48, 0x2D);       // lea rbp,[rip+scratch]
    AE(0x49, 0x89, 0xEC);                        // mov r12,rbp
    AE(0x4D, 0x29, 0xEC);                        // sub r12,r13
    AE(0x49, 0xC1, 0xFC, 0x02);                  // sar r12,2      (r13 + r12*4 = scratch, within 3 bytes)
    AE(0xEB, 0x00); uint8_t *ja2 = p - 1;        // jmp A2
    *ja1 = (uint8_t)(p - (ja1 + 1));
    AE(0x4C, 0x8D, 0x24, 0x49);                  // A1: lea r12,[rcx+rcx*2]
    AE(0x4B, 0x8D, 0x6C, 0xA5, 0x00);            // lea rbp,[r13+r12*4+0]
    *ja2 = (uint8_t)(p - (ja2 + 1));
    AE(0x48, 0x8B, 0x4A, 0x18);                  // A2: mov rcx,[rdx+0x18]
    AE(0x46, 0x8B, 0x3C, 0xB1);                  // mov r15d,[rcx+r14*4]
    AE(0x48, 0x8B, 0x4B, 0x78);                  // mov rcx,[rbx+0x78]
    AE(0x41, 0x83, 0xFF, 0xFF);                  // cmp r15d,-1
    AE(0x75, 0x00); uint8_t *ja3 = p - 1;        // jne A3
    anim_emit_lea_scratch(&p, 0x4C, 0x3D);       // lea r15,[rip+scratch]
    AE(0x49, 0x29, 0xCF);                        // sub r15,rcx
    AE(0x49, 0xC1, 0xFF, 0x03);                  // sar r15,3      (rcx + r15*8 = scratch, within 7 bytes)
    *ja3 = (uint8_t)(p - (ja3 + 1));
    AE(0x9D);                                    // A3: popfq
    AE(0x48, 0x89, 0x8C, 0x24, 0x88, 0x00, 0x00, 0x00);   // mov [rsp+0x88],rcx
    if (!anim_emit_jmp(&p, (uintptr_t)a + sizeof a_old)) { log_line("patch skipped, jump too far: animation bones without a track"); return; }
    uint8_t *sb = p;
    // site B: translation index in eax (base r15 -> rbp, r12), rotation index in edx (base rsi)
    AE(0x9C);                                    // pushfq (the je after this uses the flags of cmp [rsp+0x70],0)
    AE(0x42, 0x8B, 0x04, 0xB0);                  // mov eax,[rax+r14*4]
    AE(0x4C, 0x8B, 0x7B, 0x58);                  // mov r15,[rbx+0x58]
    AE(0x83, 0xF8, 0xFF);                        // cmp eax,-1
    AE(0x75, 0x00); uint8_t *jb1 = p - 1;
    anim_emit_lea_scratch(&p, 0x48, 0x2D);       // lea rbp,[rip+scratch]
    AE(0x49, 0x89, 0xEC);                        // mov r12,rbp
    AE(0x4D, 0x29, 0xFC);                        // sub r12,r15
    AE(0x49, 0xC1, 0xFC, 0x02);                  // sar r12,2
    AE(0xEB, 0x00); uint8_t *jb2 = p - 1;
    *jb1 = (uint8_t)(p - (jb1 + 1));
    AE(0x4C, 0x8D, 0x24, 0x40);                  // B1: lea r12,[rax+rax*2]
    AE(0x4B, 0x8D, 0x2C, 0xA7);                  // lea rbp,[r15+r12*4]
    *jb2 = (uint8_t)(p - (jb2 + 1));
    AE(0x48, 0x8B, 0x41, 0x18);                  // B2: mov rax,[rcx+0x18]
    AE(0x42, 0x8B, 0x14, 0xB0);                  // mov edx,[rax+r14*4]
    AE(0x48, 0x8B, 0x73, 0x78);                  // mov rsi,[rbx+0x78]
    AE(0x83, 0xFA, 0xFF);                        // cmp edx,-1
    AE(0x75, 0x00); uint8_t *jb3 = p - 1;
    anim_emit_lea_scratch(&p, 0x48, 0x15);       // lea rdx,[rip+scratch]
    AE(0x48, 0x29, 0xF2);                        // sub rdx,rsi
    AE(0x48, 0xC1, 0xFA, 0x03);                  // sar rdx,3
    *jb3 = (uint8_t)(p - (jb3 + 1));
    AE(0x9D);                                    // B3: popfq
    if (!anim_emit_jmp(&p, (uintptr_t)b + sizeof b_old)) { log_line("patch skipped, jump too far: animation bones without a track"); return; }
    if (p - code > 192) { log_line("patch skipped, stub too long: animation bones without a track"); return; }
    // the two sites: jmp to the stubs, the rest of the old bytes int3 (nothing in the game jumps into them)
    uint8_t ja[37], jb[28];
    memset(ja, 0xCC, sizeof ja);
    memset(jb, 0xCC, sizeof jb);
    int32_t r1 = (int32_t)((intptr_t)sa - (intptr_t)(a + 5)), r2 = (int32_t)((intptr_t)sb - (intptr_t)(b + 5));
    ja[0] = 0xE9; memcpy(ja + 1, &r1, 4);
    jb[0] = 0xE9; memcpy(jb + 1, &r2, 4);
    FlushInstructionCache(GetCurrentProcess(), code, (SIZE_T)(p - code));
    memcpy(a, ja, sizeof ja);
    memcpy(b, jb, sizeof jb);
    FlushInstructionCache(GetCurrentProcess(), a, sizeof ja);
    FlushInstructionCache(GetCurrentProcess(), b, sizeof jb);
    log_line("patch applied: animation bones without a translation or rotation track are skipped, not read 48 GB away (32-bit reads junk there)");
}
#undef AE

// ---- Commands a mod script runs (Lua os.execute -> system). Normal Attila runs them through cmd.exe; this engine runs no
// programs. What mods use them for is done here instead:
//   - settlement building-slot tools, "<name>_<major>_<minor>_slots....exe [address]" (The Dawnless Days' TDD_12_9_slots_A.exe,
//     written by its campaign script lua_scripts/extend_slots.lua): they find "Total War: ATTILA", open 32-bit Empire.Retail.dll
//     in memory and change the immediate of `mov dword [ecx+0x48],6; ret` (major settlement, 32-bit pattern 01 c7 41 48 06 00 00
//     00) and `mov dword [ecx+0x48],4; mov dword [ecx+0x40],0` (minor), then point the encyclopedia at the address given. Those
//     are EMPIRECAMPAIGN::CampaignSettlementCallback::SetAsCapital (2a785c0: [rdi+0x74] = 6) and ::Construct (2a7b700: [rdi+0x74] =
//     4) here, and the address text "http://Atenc.totalwar.com/#" (39fb485). Interface only: how many building slots a settlement's
//     panel shows, and the encyclopedia page; the slots themselves come from the mod's own tables, the same on every PC.
//   - "mkdir <folder>" (also inside "IF NOT EXIST x ( mkdir x )"): the folder is made, in the game folder like cmd would.
//   - "start <http(s) address>": the address opens in the browser (a mod's Discord or help button).
// Anything else is not run and is logged (the first words only).
static int mod_slots_tool(const char *cmd) {
    char prog[MAX_PATH];
    const char *c = cmd;
    while (*c == ' ' || *c == '"') c++;
    size_t n = 0;
    while (*c && *c != ' ' && *c != '"' && n + 1 < sizeof prog) prog[n++] = *c++;
    prog[n] = 0;
    const char *base = prog;
    for (const char *s = prog; *s; s++) if (*s == '\\' || *s == '/') base = s + 1;
    int major = 0, minor = 0, found = 0;
    for (const char *s = base; *s && !found; s++) {
        if (*s < '0' || *s > '9' || (s > base && s[-1] != '_')) continue;
        int m1 = 0, m2 = 0, k = 0;
        if (sscanf(s, "%d_%d_%n", &m1, &m2, &k) == 2 && k > 0 && !_strnicmp(s + k, "slots", 5)) { major = m1; minor = m2; found = 1; }
    }
    if (!found) return 0;
    while (*c == ' ' || *c == '"') c++;
    char url[256];
    n = 0;
    while (*c && *c != ' ' && *c != '"' && n + 1 < sizeof url) url[n++] = *c++;
    url[n] = 0;
    if (major < 1 || major > 40 || minor < 1 || minor > 40) { log_line("mod slot tool %s: slot numbers %d / %d not taken (1 to 40)", base, major, minor); return 1; }
    uint8_t *maj = (uint8_t *)0x2a785c4, *min = (uint8_t *)0x2a7b704;
    static const uint8_t op[3] = {0xC7, 0x47, 0x74};   // mov dword [rdi+0x74], imm32
    int32_t cur_maj, cur_min;
    memcpy(&cur_maj, maj + 3, 4);
    memcpy(&cur_min, min + 3, 4);
    if (memcmp(maj, op, 3) || memcmp(min, op, 3) || cur_maj < 1 || cur_maj > 40 || cur_min < 1 || cur_min > 40 || maj[7] != 0xC3) {
        log_line("mod slot tool %s: game code differs, building slots left as they are", base);
        return 1;
    }
    int32_t v1 = major, v2 = minor;
    memcpy(maj + 3, &v1, 4);
    memcpy(min + 3, &v2, 4);
    FlushInstructionCache(GetCurrentProcess(), maj, 8);
    FlushInstructionCache(GetCurrentProcess(), min, 8);
    char note[96] = "no encyclopedia address given";
    static const char vanilla[] = "http://Atenc.totalwar.com/#";
    char *text = (char *)0x39fb485;
    if (url[0]) {
        static int url_done;
        if (!_strnicmp(url, "http://", 7) || !_strnicmp(url, "https://", 8) || !_strnicmp(url, "file://", 7)) {
            if (strlen(url) > sizeof vanilla - 1) snprintf(note, sizeof note, "encyclopedia address too long (%u letters), not changed", (unsigned)strlen(url));
            else if (!url_done && memcmp(text, vanilla, sizeof vanilla)) snprintf(note, sizeof note, "encyclopedia address: game text differs, not changed");
            else { memset(text, 0, sizeof vanilla); memcpy(text, url, strlen(url)); url_done = 1; snprintf(note, sizeof note, "encyclopedia address set (%u letters)", (unsigned)strlen(url)); }
        } else snprintf(note, sizeof note, "encyclopedia address not a web or file address, not changed");
    }
    log_line("mod slot tool %s: settlement building slots %d (major) / %d (minor), as the tool does on 32-bit Attila; %s", base, major, minor, note);
    return 1;
}
int mod_command(const char *cmd);
// Test only: ATTILA_TEST_MOD_COMMAND="<command>" runs that command as a mod script would, once the game code is patched, and
// logs what the four changed places hold afterwards.
static void test_mod_command(void) {
    const char *c = getenv("ATTILA_TEST_MOD_COMMAND");
    if (!c || !*c) return;
    int r = mod_command(c);
    int32_t maj, min;
    memcpy(&maj, (void *)0x2a785c7, 4);
    memcpy(&min, (void *)0x2a7b707, 4);
    log_line("test: mod command (ATTILA_TEST_MOD_COMMAND) -> %d; slots now %d (major) / %d (minor); encyclopedia address now %.40s", r,
             maj, min, (const char *)0x39fb485);
}
int mod_command(const char *cmd) {   // shim_stdio.c system(): 0 = done, -1 = not run
    if (!cmd || !*cmd) return -1;
    if (mod_slots_tool(cmd)) return 0;
    const char *m = NULL;
    for (const char *s = cmd; *s; s++) if (!_strnicmp(s, "mkdir ", 6) && (s == cmd || s[-1] == ' ' || s[-1] == '(')) { m = s + 6; break; }
    if (m) {
        while (*m == ' ' || *m == '"') m++;
        char dir[MAX_PATH];
        size_t n = 0;
        while (*m && *m != '"' && *m != ')' && *m != ' ' && *m != '&' && *m != '|' && n + 1 < sizeof dir) dir[n++] = *m++;
        dir[n] = 0;
        if (n && !strstr(dir, "..")) {
            CreateDirectoryA(dir, NULL);
            static int told;
            if (told++ < 4) log_line("mod script command: folder %s made (mkdir)", dir);
            return 0;
        }
    }
    if (!_strnicmp(cmd, "start ", 6)) {
        const char *u = cmd + 6;
        while (*u == ' ' || *u == '"') u++;
        if (!_strnicmp(u, "http://", 7) || !_strnicmp(u, "https://", 8)) {
            char url[1024];
            size_t n = 0;
            while (*u && *u != '"' && *u != ' ' && n + 1 < sizeof url) url[n++] = *u++;
            url[n] = 0;
            HMODULE sh = LoadLibraryA("shell32.dll");
            HINSTANCE (WINAPI *open_fn)(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT) =
                sh ? (HINSTANCE (WINAPI *)(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT))GetProcAddress(sh, "ShellExecuteA") : NULL;
            if (open_fn) { open_fn(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL); log_line("mod script command: web page opened in the browser"); return 0; }
        }
    }
    static int told;
    if (told++ < 20) {
        char first[48];
        size_t n = 0;
        for (const char *s = cmd; *s && *s != ' ' && n + 1 < sizeof first; s++) first[n++] = *s;
        first[n] = 0;
        log_line("mod script command not run (this engine runs no programs): %s ...", first);
    }
    return -1;
}
