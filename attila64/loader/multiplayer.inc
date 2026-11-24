// Multiplayer: 8-player campaign, players dropping, 8 vs 8 battles, lobby join guard, battle loading screen.
// Part of loader.c (included there, not built by itself; the order of the includes matters).

// ---- Eight player campaign (multiplayer). The game hosts campaigns for 2 players and its lobby (UIDLL::MPCampaignLobby) keeps
// one chosen-faction text per player in a 2-entry array at lobby+0xF0 (CHECKED_ARRAY<CA::String,2>, 16 bytes each), "NULL" when
// empty. The host now asks for 8 players, the lobby finds panel_player1..8 (layout in attila_8player_lobby.pack), and every user
// of that array uses a 16-entry array of ours instead (only one lobby exists at a time; slots go up to 7). The lobby's own two
// strings stay where they are, unused, so its constructor and destructor need no change. Lobby fields: +0x08 root component,
// +0xB8 EMPIRE_MP, +0xC8 campaign info, +0xE0 panel vector (count +0xE4, data +0xE8). Panel index = player slot.
typedef struct { uint32_t len, cap; const char *text; } CaString;
#define MP_SLOTS 8
static CaString *g_mp_factions;   // 16 entries, in the page near the game code so its code can reach it with rip-relative addresses

#define GAME_FN(name, addr, ret, ...) static ret (SYSV *const name)(__VA_ARGS__) = (ret (SYSV *)(__VA_ARGS__))(uintptr_t)(addr)
GAME_FN(ca_string_from_c, 0x512db0, void, CaString *, const char *);
GAME_FN(ca_string_free, 0x512d00, void, CaString *);
GAME_FN(ca_string_set_c, 0x513170, void, CaString *, const char *);
GAME_FN(ca_string_ne_c, 0x51b0f0, uint8_t, const CaString *, const char *);
GAME_FN(component_search, 0x29238f0, void *, void *, const CaString *, int);
GAME_FN(component_vector_push, 0x2975430, void, void *, void **);
GAME_FN(component_set_visible, 0x29168a0, void, void *, int);
GAME_FN(update_faction_dropdown, 0x2ce84c0, void, void *, CaString *, void *, int, int);
static const char *const k_null_text = (const char *)0x3c98d39;   // the game's "NULL"

// Replaces MPCampaignLobby::InitializePlayerSlots: the same steps for 8 panels instead of 2. Panels missing from the layout
// are left out (the game's own version would add an empty entry and crash on it).
static void mp_team_buttons_forget(uint8_t *lobby);   // the team buttons, further down
static SYSV void mp_init_player_slots(uint8_t *lobby) {
    mp_team_buttons_forget(lobby);
    *(uint32_t *)(lobby + 0xe4) = 0;
    void *root = *(void **)(lobby + 0x08);
    for (int i = 1; i <= MP_SLOTS; i++) {
        char name[24];
        snprintf(name, sizeof name, "panel_player%d", i);
        CaString s;
        ca_string_from_c(&s, name);
        void *panel = component_search(root, &s, 1);
        if (panel) component_vector_push(lobby + 0xe0, &panel);
        ca_string_free(&s);
    }
    uint32_t n = *(uint32_t *)(lobby + 0xe4);
    void **panels = *(void ***)(lobby + 0xe8);
    for (uint32_t i = 0; i < n; i++) {
        component_set_visible(panels[i], 0);
        if (i < MP_SLOTS) ca_string_set_c(&g_mp_factions[i], k_null_text);
    }
}

// Replaces the end of MPCampaignLobby::UpdatePlayerFactionFlags, which refreshed the faction dropdowns of panels 0 and 1 by
// hand: now every panel whose player has chosen a faction, with the same arguments the game passed.
static SYSV void mp_update_faction_dropdowns(uint8_t *lobby, uint32_t local_slot) {
    uint8_t *info = *(uint8_t **)(lobby + 0xc8);
    uint32_t n = *(uint32_t *)(lobby + 0xe4);
    void **panels = *(void ***)(lobby + 0xe8);
    if (n > MP_SLOTS) n = MP_SLOTS;
    for (uint32_t i = 0; i < n; i++) {
        if (!ca_string_ne_c(&g_mp_factions[i], k_null_text)) continue;
        if (!panels[i]) continue;
        void *player = *(void **)((uint8_t *)panels[i] + 0x258);
        if (!player) continue;
        // a panel whose slot was never set up (no player info at +0x78; e.g. the host sits in slot 4) would crash in CreateFactionList
        if (!*(void **)((uint8_t *)player + 0x78)) continue;
        int locked = info ? (info[0x60] == 0 && local_slot == i) : 0;
        int mode = info ? ((uint32_t)(*(int32_t *)(info + 0x24) - 5) < 2u) : 0;
        update_faction_dropdown(player, g_mp_factions, *(void **)(lobby + 0xb8), locked, mode);
    }
}

// Replaces UIDLL::MAP_BUILDER::SetRegionEntry (lobby map): it gave the regions of the first 4 chosen factions a colour each
// (MAP_BUILDER: +0x00 palette image, +0x14 faction count, +0x18 faction keys) and left the rest blank. Now 8, the game's own 4
// colours first. ATTILA_MAP_COLOUR_TEST=1 paints player 1's regions in all 8 colours in turn, to check them on screen.
GAME_FN(ca_string_eq, 0x50def0, uint8_t, const CaString *, const CaString *);
GAME_FN(set_palette_entry, 0x30027d0, void, void *, uint32_t, const uint8_t *);
static int g_map_colour_test;
static SYSV void mp_set_region_entry(uint8_t *builder, const CaString *owner, uint32_t entry) {
    static const uint8_t colours[MP_SLOTS][4] = {
        {0x00, 0xFF, 0x00, 0xA8}, {0xFF, 0x00, 0x00, 0xA8}, {0xFF, 0x00, 0xFF, 0xA8}, {0x00, 0xFF, 0xFF, 0xA8},   // the game's
        {0x00, 0x00, 0xFF, 0xA8}, {0xFF, 0xFF, 0x00, 0xA8}, {0xFF, 0x80, 0x00, 0xA8}, {0x00, 0x80, 0xFF, 0xA8},   // added
    };
    uint32_t n = *(uint32_t *)(builder + 0x14);
    const CaString *keys = *(const CaString **)(builder + 0x18);
    if (n > MP_SLOTS) n = MP_SLOTS;
    for (uint32_t k = 0; k < n; k++)
        if (ca_string_eq(owner, &keys[k])) {
            uint32_t c = (g_map_colour_test && k == 0) ? entry % MP_SLOTS : k;
            set_palette_entry(*(void **)builder, entry, colours[c]);
            return;
        }
}

// ---- Team button in the campaign lobby. The game seats every joiner on the team with most room and its campaign lobby has no
// way to change that. The 8-player layout gives each player panel a small button "button_team" (attila_8player\build_lobby_8p.ps1,
// a copy of the Purchase button). Everything happens in the lobby's pulse (MPCampaignLobby::OnUpdatePulse 2cea830):
//  - each panel's button shows "Team N", rewritten when N changes;
//  - the local player's own button is watched: pressed ("down") and then let go on the button = a click, which moves him to the
//    next team that EMPIRE_MP_GAME_LOBBY::can_set_team allows, with the lobby's own set_team: the call the battle lobby uses, so
//    every PC hears of it the usual way. (The lobby's ButtonSelected handler never hears of a button it did not know.)
// Panel index = player slot. Lobby set-up only; a layout without the button (another mod's lobby) changes nothing.
GAME_FN(mp_lobby_of, 0x27d2040, void *, void *);                       // EMPIRE::EMPIRE_MP::lobby()
GAME_FN(lobby_get_team, 0x28524e0, uint32_t, void *, uint32_t);
GAME_FN(lobby_can_set_team, 0x2852280, uint8_t, void *, uint32_t, uint32_t);
GAME_FN(lobby_set_my_team, 0x28523a0, void, void *, uint32_t);
GAME_FN(lobby_slot_filled, 0x281c790, uint8_t, void *, uint32_t);
GAME_FN(state_text_set, 0x29182c0, void, void *, const void *, int);   // TWUI::Component::StateTextSet(UniString, bool)
GAME_FN(uni_from_utf16, 0x51b740, void, void *, const uint16_t *);
GAME_FN(uni_free, 0x51ba50, void, void *);
GAME_FN(component_set_state, 0x2920580, void, void *, const CaString *);
GAME_FN(component_set_interactive, 0x2920950, void, void *, int);
GAME_FN(component_set_disabled, 0x2920e00, void, void *, int);
#define MP_TEAMS 4
static uint8_t *g_team_button[MP_SLOTS];   // each panel's team button, NULL = not looked up yet or not in the layout
static void *g_team_text[MP_SLOTS];        // the text component on it
static int g_team_shown[MP_SLOTS];         // team written on it, -1 = nothing yet
static int g_team_was_down;                // the local player's button was pressed at the last pulse
static int g_team_asked = -1, g_team_asked_pulses;   // team asked for and not yet seen in the lobby (the change travels through the network layer)
static uint8_t *g_team_lobby;              // the lobby screen these belong to
static void (SYSV *g_lobby_pulse_orig)(uint8_t *, uint32_t);

static void *mp_find(void *under, const char *name) {
    CaString s;
    ca_string_from_c(&s, name);
    void *c = under ? component_search(under, &s, 1) : NULL;
    ca_string_free(&s);
    return c;
}

static void mp_team_buttons_forget(uint8_t *lobby) {
    g_team_lobby = lobby;
    g_team_was_down = 0;
    g_team_asked = -1;
    for (int i = 0; i < MP_SLOTS; i++) { g_team_button[i] = NULL; g_team_text[i] = NULL; g_team_shown[i] = -1; }
}

// The name of the state a component is in now (TWUI::Component::CurrentState: state object at +0x140, its name at +0x20).
static const char *mp_state_name(const uint8_t *component) {
    const uint8_t *state = *(const uint8_t *const *)(component + 0x140);
    return state ? ((const CaString *)(state + 0x20))->text : "";
}

static void mp_team_button_setup(uint32_t slot, void *panel) {
    uint8_t *button = mp_find(panel, "button_team");
    g_team_button[slot] = button;
    g_team_text[slot] = mp_find(button, "button_txt");
    if (button) {   // shown and awake: the Purchase button it was copied from sleeps until the game needs it
        CaString active;
        ca_string_from_c(&active, "active");
        component_set_visible(button, 1);
        component_set_disabled(button, 0);
        component_set_interactive(button, 1);
        component_set_state(button, &active);
        ca_string_free(&active);
    }
    if (g_team_text[slot]) component_set_visible(g_team_text[slot], 1);
    log_line("campaign lobby: team button of slot %u %s", slot, g_team_text[slot] ? "found" : "NOT in the layout");
}

// Test only (the local multiplayer test, steam_hub.c / dx11\mp_test.ps1): ATTILA_TEST_MP_FACTION=key1,key2,... takes the first
// of these factions the lobby allows for this player, the way the faction list does (EMPIRE_MP_GAME_LOBBY::set_faction),
// ATTILA_TEST_MP_FACTION_AT front-end seconds in (default at once); ATTILA_TEST_MP_READY_AT=s then sets this player ready
// (set_ready(true): what the Ready / Start Campaign button does). The faction list itself changes shape with the seat, so a
// scripted click cannot pick from it.
GAME_FN(lobby_can_set_faction, 0x2852830, uint8_t, void *, uint32_t, const CaString *);
// what the faction list calls (MPCampaignPlayer::notify(DropdownOptionChanged) 2ce782c): political party and faction together.
// An empty party = the faction's own default party (FACTION::override_setup takes it then). Setting the faction alone left
// the old faction's party behind, and the campaign crashed while being set up (FACTION::override_setup, test 2026-11-13).
GAME_FN(lobby_set_party_and_faction, 0x2853b10, void, void *, uint32_t, const CaString *, const CaString *);
GAME_FN(lobby_set_ready, 0x28514b0, void, void *, int);
static void mp_test_lobby_steps(void *game_lobby, uint32_t me) {
    static int init, fac_done, ready_done, tries;
    static double fac_at, ready_at = -1;
    static const char *keys;
    if (!init) {
        init = 1;
        keys = getenv("ATTILA_TEST_MP_FACTION");
        const char *a = getenv("ATTILA_TEST_MP_FACTION_AT"), *r = getenv("ATTILA_TEST_MP_READY_AT");
        fac_at = a ? atof(a) : 0; ready_at = r ? atof(r) : -1;
    }
    if ((!keys || fac_done) && (ready_at < 0 || ready_done)) return;
    if (me > 15) return;
    extern double test_front_seconds(void);
    double now = test_front_seconds();
    if (keys && !fac_done && now >= fac_at) {
        char buf[512];
        snprintf(buf, sizeof buf, "%s", keys);
        for (char *k = strtok(buf, ","); k && !fac_done; k = strtok(NULL, ",")) {
            // an entry is faction[/party]; without a party, a 1212AD faction mk_fact_X gets its ruling party mk_politics_X_ruler
            // (the list passes a real party too: with an empty one the host's own choice was not taken, test 2026-11-13)
            char party_key[160] = "";
            char *slash = strchr(k, '/');
            if (slash) { *slash = 0; snprintf(party_key, sizeof party_key, "%s", slash + 1); }
            else if (!strncmp(k, "mk_fact_", 8)) snprintf(party_key, sizeof party_key, "mk_politics_%s_ruler", k + 8);
            CaString s;
            ca_string_from_c(&s, k);
            if (lobby_can_set_faction(game_lobby, me, &s)) {
                CaString party;
                ca_string_from_c(&party, party_key);
                lobby_set_party_and_faction(game_lobby, me, &party, &s);
                ca_string_free(&party);
                fac_done = 1;
                log_line("test lobby: slot %u takes faction %s, party %s", me, k, party_key[0] ? party_key : "(none)");
            }
            ca_string_free(&s);
        }
        if (!fac_done && ++tries >= 6) { fac_done = 1; log_line("test lobby: slot %u could take none of %s", me, keys); }
        else if (!fac_done) fac_at = now + 5;   // the lobby may not know everyone yet: again in 5 s
    }
    if (!ready_done && ready_at >= 0 && now >= ready_at && (!keys || fac_done)) { lobby_set_ready(game_lobby, 1); ready_done = 1; log_line("test lobby: slot %u ready", me); }
}

// ---- ATTILA_TURN_TIMES=1 (measuring): every faction's campaign turn in the log, with how long it took. Display only.
// EMPIRECAMPAIGN::CAMPAIGN_ENV::notify_env_of_start_of_turn(FACTION const&) 1761080 and notify_env_of_end_of_turn 17614b0;
// FACTION::record_name() ed7910 gives the faction's key (a CA::String).
static void (SYSV *g_tt_start_orig)(void *, const void *);
static void (SYSV *g_tt_end_orig)(void *, const void *);
static ULONGLONG g_tt_started, g_tt_round_started;
static int g_tt_count;
static double g_tt_ai_ms;
// model ticks (CAMPAIGN_MODEL::tick 16de0d0) and army move steps (LOCOMOTION_MANAGER::update_positions 103cf30) in this turn:
// steps per tick = how fast watched moves run (the game allows 5 a tick for watched AI moves, 30 when nobody watches)
// CAMPAIGN_MODEL::tick takes three: rdi (its result, by hidden pointer), rsi = the model, rdx = the TICK (kept in r15). A
// two-argument hook lost rdx as soon as it called anything else and the campaign crashed while loading (2026-11-14, test only).
static uint64_t (SYSV *g_tt_tick_orig)(void *, void *, const void *);
static uint64_t (SYSV *g_tt_steps_orig)(void *, int, int, int);
static volatile LONG g_tt_ticks, g_tt_steps;
static SYSV uint64_t tt_tick(void *ret, void *model, const void *tick) {
    LONG n = InterlockedIncrement(&g_tt_ticks);
    if (!(n % 50)) log_line("turn so far: %ld model ticks, %ld move steps", n, g_tt_steps);   // also when a turn then waits on a player
    return g_tt_tick_orig(ret, model, tick);
}
static SYSV uint64_t tt_steps(void *lm, int a, int b, int c) { InterlockedIncrement(&g_tt_steps); return g_tt_steps_orig(lm, a, b, c); }
static const char *tt_key(const void *faction, char *buf, size_t n) {
    const CaString *s = ((const CaString *(SYSV *)(const void *))(uintptr_t)0xed7910)(faction);
    snprintf(buf, n, "%.*s", s && s->text ? (int)s->len : 1, s && s->text ? s->text : "?");
    return buf;
}
static SYSV void tt_start(void *env, const void *faction) {
    g_tt_started = GetTickCount64();
    if (!g_tt_round_started) g_tt_round_started = g_tt_started;
    InterlockedExchange(&g_tt_ticks, 0);
    InterlockedExchange(&g_tt_steps, 0);
    g_tt_start_orig(env, faction);
}
static SYSV void tt_end(void *env, const void *faction) {
    ULONGLONG now = GetTickCount64();
    char key[96];
    int human = *((const uint8_t *)faction + 0x971) != 0;   // the byte notify_env_of_start_of_turn tests before its human-only steps
    double ms = g_tt_started ? (double)(now - g_tt_started) : 0;
    if (!human) g_tt_ai_ms += ms;
    g_tt_count++;
    log_line("turn %d: %s %.1f s, %ld model ticks, %ld move steps", g_tt_count, tt_key(faction, key, sizeof key), ms / 1000.0, g_tt_ticks, g_tt_steps);
    (void)human;
    if (!(g_tt_count % 20)) log_line("turn times: %d faction turns so far, AI factions %.1f s in all, %.1f s since the first", g_tt_count, g_tt_ai_ms / 1000.0, (double)(now - g_tt_round_started) / 1000.0);
    g_tt_end_orig(env, faction);
}
static void patch_turn_times(void) {
    if (!getenv("ATTILA_TURN_TIMES")) return;
    static const uint8_t start_old[6] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41};   // push rbp; push r15; push r14; push r13 (first byte)
    static const uint8_t end_old[6] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x54};     // push r15; push r14; push r12
    g_tt_start_orig = (void (SYSV *)(void *, const void *))hook_entry(0x1761080, start_old, 5, (void *)tt_start);
    g_tt_end_orig = (void (SYSV *)(void *, const void *))hook_entry(0x17614b0, end_old, 6, (void *)tt_end);
    static const uint8_t prologue[5] = {0x55, 0x41, 0x57, 0x41, 0x56};   // push rbp; push r15; push r14
    g_tt_tick_orig = (uint64_t (SYSV *)(void *, void *, const void *))hook_entry(0x16de0d0, prologue, 5, (void *)tt_tick);
    g_tt_steps_orig = (uint64_t (SYSV *)(void *, int, int, int))hook_entry(0x103cf30, prologue, 5, (void *)tt_steps);
    log_line(g_tt_start_orig && g_tt_end_orig ? "measuring: campaign turn times%s" : "measuring skipped, game code differs: campaign turn times%s",
             g_tt_tick_orig && g_tt_steps_orig ? " (with model ticks and move steps)" : "");
}

// ---- Faster AI army moves on the campaign map while a human can see them (the user, 2026-11-14: "Faster AI moves").
// CAMPAIGN_MODEL::tick runs up to s_campaign_tick_loop_max (30) army move steps a model tick when no human can see a moving
// army, so unseen AI turns are over at once. When a human can see one (the player's "Show AI Moves" choice and line of sight:
// LOCOMOTION_MANAGER::has_moving_pieces_any_humans_might_be_watching) it allows only 5 steps a tick for AI moves (16dec65),
// and 2 (with a path) or 1 when an AI army is dealing with a human (attacking, approaching: has_any_locomotables_interacting_
// with_humans, 16decb7 -> the shared caps at 16deccf/16decdd). Here AI moves get 3 times as many: 15, and 6 or 3 through
// our own copy of that last step (the human player's own moves keep using the game's 2/1 unchanged; the shared code is not
// touched). This is part of the shared campaign simulation: every player in a game must run it the same way.
// OFF since 1.2 Test 9 (user 2026-11-14, after the measurement: "just add the water fix to workshop"): one measured AI turn
// went only from 21.0 to 19.4 s. It was in 1.2 Test 8 (Workshop 04:36-04:46). Test switch ATTILA_AI_MOVE_SPEED=2..6 = on.
static void patch_ai_move_speed(void) {
    const char *e = getenv("ATTILA_AI_MOVE_SPEED");
    int k = e ? atoi(e) : 1;
    if (k < 1) k = 1;
    if (k > 6) k = 6;
    if (k == 1) return;   // the game's own speed
    static const uint8_t cap5_old[8] = {0x83, 0xF8, 0x05, 0xB9, 0x05, 0x00, 0x00, 0x00};   // 16dec65: cmp eax,5; mov ecx,5
    static const uint8_t near_old[11] = {0x49, 0x8B, 0xBC, 0x24, 0x38, 0x2D, 0x00, 0x00, 0x4C, 0x89, 0xE3};   // 16decb7: mov rdi,[r12+2d38]; mov rbx,r12
    static const uint8_t shared_old[5] = {0xE8, 0x69, 0xEB, 0x95, 0xFF};   // 16decc2: call has_any_locomotables_with_path (still the game's)
    if (memcmp((void *)0x16dec65, cap5_old, 8) || memcmp((void *)0x16decb7, near_old, 11) || memcmp((void *)0x16decc2, shared_old, 5)) {
        log_line("patch skipped, game code differs: faster AI army moves");
        return;
    }
    uint8_t *cave = near_game_code(64);
    if (!cave) { log_line("patch skipped, no memory near the game: faster AI army moves"); return; }
    int far_cap = 5 * k > 30 ? 30 : 5 * k, path_cap = 2 * k, still_cap = k;
    uint8_t *p = cave;
    memcpy(p, near_old, 11); p += 11;                                    // mov rdi,[r12+2d38]; mov rbx,r12 (as the game did)
    *p++ = 0xE8; { int32_t r = (int32_t)(0x103d830 - (intptr_t)(p + 4)); memcpy(p, &r, 4); p += 4; }   // call has_any_locomotables_with_path
    *p++ = 0xB9; memcpy(p, &still_cap, 4); p += 4;                       // mov ecx, still_cap
    *p++ = 0xBA; memcpy(p, &path_cap, 4); p += 4;                        // mov edx, path_cap
    *p++ = 0x84; *p++ = 0xC0;                                            // test al,al
    *p++ = 0x0F; *p++ = 0x45; *p++ = 0xCA;                               // cmovne ecx,edx
    *p++ = 0x8B; *p++ = 0x44; *p++ = 0x24; *p++ = 0x20;                  // mov eax,[rsp+20]  (the step limit)
    *p++ = 0x39; *p++ = 0xC8;                                            // cmp eax,ecx
    *p++ = 0x0F; *p++ = 0x47; *p++ = 0xC1;                               // cmova eax,ecx
    *p++ = 0x89; *p++ = 0x44; *p++ = 0x24; *p++ = 0x20;                  // mov [rsp+20],eax
    *p++ = 0xE9; { int32_t r = (int32_t)(0x16decec - (intptr_t)(p + 4)); memcpy(p, &r, 4); p += 4; }  // jmp back (xor r12d,r12d ...)
    uint8_t *g = (uint8_t *)0x16decb7;                                   // the AI path now goes through our copy
    g[0] = 0xE9; { int32_t r = (int32_t)(cave - (g + 5)); memcpy(g + 1, &r, 4); }
    memset(g + 5, 0x90, 6);
    *(uint8_t *)0x16dec67 = (uint8_t)far_cap;                            // cmp eax,far_cap
    memcpy((void *)0x16dec69, &far_cap, 4);                              // mov ecx,far_cap
    log_line("patch applied: AI army moves on the campaign map %dx as fast while a human can see them (%d / %d / %d steps a tick)", k, far_cap, path_cap, still_cap);
}

static SYSV void mp_lobby_pulse(uint8_t *lobby, uint32_t ms) {
    g_lobby_pulse_orig(lobby, ms);
    void *mp = *(void **)(lobby + 0xb8), *game_lobby = mp ? mp_lobby_of(mp) : NULL;
    if (!game_lobby) return;
    if (lobby != g_team_lobby) mp_team_buttons_forget(lobby);
    uint32_t n = *(uint32_t *)(lobby + 0xe4);
    void **panels = *(void ***)(lobby + 0xe8);
    uint32_t me = ((SYSV uint32_t (*)(void *))(*(void ***)game_lobby)[0x28 / 8])(game_lobby);   // get_local_player_slot
    for (uint32_t i = 0; i < n && i < MP_SLOTS; i++) {
        if (!panels[i] || !lobby_slot_filled(game_lobby, i)) { g_team_shown[i] = -1; continue; }
        if (g_team_shown[i] < 0 && !g_team_button[i]) mp_team_button_setup(i, panels[i]);
        if (!g_team_text[i]) { g_team_shown[i] = 99; continue; }   // no button in this layout: looked up once, left alone
        uint32_t team = lobby_get_team(game_lobby, i);
        if (i == me) {
            const char *state = mp_state_name(g_team_button[i]);
            int down = !strcmp(state, "down");
            if (g_team_asked >= 0 && ((int)team == g_team_asked || ++g_team_asked_pulses > 120)) g_team_asked = -1;
            if (g_team_was_down && !down && strcmp(state, "down_off") && g_team_asked < 0)   // let go on the button, and no change under way
                for (uint32_t k = 1; k < MP_TEAMS; k++) {
                    uint32_t want = (team + k) % MP_TEAMS;
                    if (!lobby_can_set_team(game_lobby, me, want)) { log_line("campaign lobby: slot %u may not take team %u", me, want + 1); continue; }
                    lobby_set_my_team(game_lobby, want);
                    g_team_asked = (int)want; g_team_asked_pulses = 0;
                    log_line("campaign lobby: slot %u asks for team %u (was %u)", me, want + 1, team + 1);
                    team = lobby_get_team(game_lobby, i);
                    break;
                }
            g_team_was_down = down || (g_team_was_down && !strcmp(state, "down_off"));
        }
        if ((int)team == g_team_shown[i]) continue;
        uint16_t text[8] = {'T', 'e', 'a', 'm', ' ', (uint16_t)('1' + team), 0};
        uint8_t uni[32];
        uni_from_utf16(uni, text);
        state_text_set(g_team_text[i], uni, 1);
        uni_free(uni);
        g_team_shown[i] = (int)team;
    }
    mp_test_lobby_steps(game_lobby, me);
}

static void patch_campaign_team_button(void) {
    static const uint8_t pulse_old[5] = {0x41, 0x57, 0x41, 0x56, 0x53};   // push r15; push r14; push rbx
    g_lobby_pulse_orig = (void (SYSV *)(uint8_t *, uint32_t))hook_entry(0x2cea830, pulse_old, 5, (void *)mp_lobby_pulse);
    log_line(g_lobby_pulse_orig ? "patch applied: campaign lobby team button" : "patch skipped, game code differs: campaign team button");
}
static void patch_eight_player_campaign(void) {
    if (getenv("ATTILA_TWO_PLAYER_CAMPAIGN")) { log_line("eight player campaign off (ATTILA_TWO_PLAYER_CAMPAIGN)"); return; }
    static const struct { uintptr_t at; uint8_t len; uint8_t old[8]; } sites[] = {
        {0x2cf9597, 5, {0xBE, 0x02, 0x00, 0x00, 0x00}},                   // MPHostGamePopup: MP_GAME_SPECIFICATION max players 2
        {0x2cf959e, 5, {0xB9, 0x02, 0x00, 0x00, 0x00}},                   // same call, 2
        {0x2ceb8f0, 5, {0x55, 0x41, 0x57, 0x41, 0x56}},                   // InitializePlayerSlots start: push rbp, r15, r14
        {0x2ced7d0, 7, {0x4D, 0x8D, 0xB5, 0xF0, 0x00, 0x00, 0x00}},       // UpdatePlayerFactionFlags: lea r14,[r13+F0]
        {0x2ceb009, 8, {0x49, 0x8D, 0xBC, 0x0D, 0xF0, 0x00, 0x00, 0x00}}, // OnLobbyCreated: lea rdi,[r13+rcx+F0]
        {0x2cee1b8, 8, {0x49, 0x8D, 0xB4, 0x24, 0xF0, 0x00, 0x00, 0x00}}, // on_player_joined: lea rsi,[r12+F0]
        {0x2cee410, 8, {0x49, 0x8D, 0xBC, 0x06, 0xF0, 0x00, 0x00, 0x00}}, // faction changed: lea rdi,[r14+rax+F0]
        {0x2cee45d, 7, {0x49, 0x8D, 0xB6, 0xF0, 0x00, 0x00, 0x00}},       // faction changed: lea rsi,[r14+F0]
        {0x2cee490, 8, {0x49, 0x8D, 0xBC, 0x06, 0xF0, 0x00, 0x00, 0x00}}, // faction cleared: lea rdi,[r14+rax+F0]
        {0x2ceec6f, 8, {0x4A, 0x8D, 0xBC, 0x3B, 0xF0, 0x00, 0x00, 0x00}}, // on_player_left: lea rdi,[rbx+r15+F0]
        {0x2ced98b, 7, {0x48, 0x81, 0xC3, 0xF0, 0x00, 0x00, 0x00}},       // UpdateTheatreList: add rbx,F0
        {0x2ce56b2, 3, {0x83, 0xFD, 0x02}},                               // BuildTheatreList: cmp ebp,2 (factions shown on the map)
        {0x2ce45c0, 5, {0x55, 0x41, 0x56, 0x53, 0x48}},                   // MAP_BUILDER::SetRegionEntry start: push rbp, r14, rbx
        {0x2cec6a8, 3, {0x83, 0xFD, 0x02}},                               // UpdatePlayerDetails: cmp ebp,2 (teams whose players are listed)
    };
    g_map_colour_test = getenv("ATTILA_MAP_COLOUR_TEST") != NULL;
    for (size_t i = 0; i < sizeof sites / sizeof sites[0]; i++)
        if (memcmp((void *)sites[i].at, sites[i].old, sites[i].len)) {
            log_line("patch skipped, game code differs at %llx: eight player campaign", (unsigned long long)sites[i].at);
            return;
        }
    uint8_t *mem = near_game_code(16 * 16 + 256);
    if (!mem) { log_line("patch skipped, no memory near the game: eight player campaign"); return; }
    g_mp_factions = (CaString *)mem;
    for (int i = 0; i < 16; i++) { g_mp_factions[i].len = 0; g_mp_factions[i].cap = 0; g_mp_factions[i].text = (const char *)0x4c13a98; }  // as CA::String::String()
    uint8_t *p = mem + 16 * 16;
#define EMIT(...) do { static const uint8_t b_[] = {__VA_ARGS__}; memcpy(p, b_, sizeof b_); p += sizeof b_; } while (0)
#define REL32_TO(target) do { int32_t r_ = (int32_t)((uint8_t *)(target) - (p + 4)); memcpy(p, &r_, 4); p += 4; } while (0)
#define JMP_TO(target) do { *p++ = 0xE9; REL32_TO(target); } while (0)
#define ABS64(v) do { uint64_t v_ = (uint64_t)(v); memcpy(p, &v_, 8); p += 8; } while (0)
    // lea <reg>,[rip+array]; add <reg>,<index reg>; jmp back. For the indexed sites.
    uint8_t *idx_stub[4];
    static const struct { uint8_t lea_reg; uint8_t add[3]; uintptr_t back; } idx[4] = {
        {0x3D, {0x48, 0x01, 0xCF}, 0x2ceb011},   // rdi += rcx
        {0x3D, {0x48, 0x01, 0xC7}, 0x2cee418},   // rdi += rax
        {0x3D, {0x48, 0x01, 0xC7}, 0x2cee498},   // rdi += rax
        {0x3D, {0x4C, 0x01, 0xFF}, 0x2ceec77},   // rdi += r15
    };
    for (int k = 0; k < 4; k++) {
        idx_stub[k] = p;
        *p++ = 0x48; *p++ = 0x8D; *p++ = idx[k].lea_reg; REL32_TO(g_mp_factions);
        memcpy(p, idx[k].add, 3); p += 3;
        JMP_TO(idx[k].back);
    }
    // InitializePlayerSlots: whole function -> mp_init_player_slots (rdi = lobby already)
    uint8_t *init_stub = p;
    EMIT(0x48, 0xB8); ABS64(mp_init_player_slots);    // mov rax,fn
    EMIT(0xFF, 0xE0);                                  // jmp rax
    // UpdatePlayerFactionFlags tail: stack is 16-byte aligned here (6 pushes + sub 0x28)
    uint8_t *flags_stub = p;
    EMIT(0x4C, 0x89, 0xEF);                            // mov rdi,r13
    EMIT(0x44, 0x89, 0xFE);                            // mov esi,r15d
    EMIT(0x48, 0xB8); ABS64(mp_update_faction_dropdowns);
    EMIT(0xFF, 0xD0);                                  // call rax
    JMP_TO(0x2ced8a3);                                 // the function's own epilogue
    uint8_t *region_stub = p;                          // SetRegionEntry: whole function -> mp_set_region_entry
    EMIT(0x48, 0xB8); ABS64(mp_set_region_entry);
    EMIT(0xFF, 0xE0);
#undef EMIT
#undef ABS64
    // write the jumps into the game
    uint8_t *g;
#define JUMP_AT(site, len, stub) do { g = (uint8_t *)(site); int32_t r_ = (int32_t)((stub) - (g + 5)); g[0] = 0xE9; memcpy(g + 1, &r_, 4); memset(g + 5, 0x90, (len) - 5); } while (0)
#define LEA_RIP_AT(site, len, rex_modrm) do { g = (uint8_t *)(site); g[0] = 0x48; g[1] = 0x8D; g[2] = (rex_modrm); int32_t r_ = (int32_t)((uint8_t *)g_mp_factions - (g + 7)); memcpy(g + 3, &r_, 4); memset(g + 7, 0x90, (len) - 7); } while (0)
    *(uint8_t *)0x2cf9598 = MP_SLOTS;                              // mov esi,8
    // MP_GAME_SPECIFICATION(max players, spectators, teams, ...): a campaign is 2 players in 2 teams of one. A player's team is kept in
    // two bits (EMPIRE_MP_GAME_LOBBY::get_team: flags & 3), so there can be 4 teams at most; places per team = players / teams
    // (EMPIRE_MP::get_max_players_for_alliance). 8 players therefore sit as 4 teams of 2 (the user's idea, 2026-11-11). Until
    // then this asked for 8 teams of one, and teams 4-7 cannot be stored.
    { const char *e = getenv("ATTILA_CAMPAIGN_TEAMS"); *(uint8_t *)0x2cf959f = e && atoi(e) >= 1 && atoi(e) <= 8 ? (uint8_t)atoi(e) : 4; }   // mov ecx,4 (teams); the switch is for tests
    {   // the function start gets a 5-byte jump: 55 41 57 41 56 = push rbp; push r15; push r14, never run again
        g = (uint8_t *)0x2ceb8f0; int32_t r_ = (int32_t)(init_stub - (g + 5)); g[0] = 0xE9; memcpy(g + 1, &r_, 4);
    }
    JUMP_AT(0x2ced7d0, 7, flags_stub);
    JUMP_AT(0x2ce45c0, 5, region_stub);
    JUMP_AT(0x2ceb009, 8, idx_stub[0]);
    LEA_RIP_AT(0x2cee1b8, 8, 0x35);                                // lea rsi,[rip+array]
    JUMP_AT(0x2cee410, 8, idx_stub[1]);
    LEA_RIP_AT(0x2cee45d, 7, 0x35);                                // lea rsi,[rip+array]
    JUMP_AT(0x2cee490, 8, idx_stub[2]);
    JUMP_AT(0x2ceec6f, 8, idx_stub[3]);
    LEA_RIP_AT(0x2ced98b, 7, 0x1D);                                // lea rbx,[rip+array]
    *(uint8_t *)0x2ce56b4 = MP_SLOTS;                              // cmp ebp,8
    // UpdatePlayerDetails collected the players of teams 0 and 1 only. A player's team is two bits (0..3) and each joiner gets the
    // team with most room, so a third and fourth player sat on teams 2 and 3 and never reached the lobby's player list
    // (Workshop report 2026-11-11: more than 2 players can be added, but they don't appear in the players tab and can't start).
    *(uint8_t *)0x2cec6aa = 4;                                     // cmp ebp,4
#undef JUMP_AT
#undef LEA_RIP_AT
#undef REL32_TO
#undef JMP_TO
    log_line("patch applied: eight player campaign, lobby array at %p", (void *)g_mp_factions);
    patch_campaign_team_button();
}

// ---- A player dropping no longer ends the game (multiplayer campaign and battles). What the game does when a player leaves:
// EMPIRE_MP::player_left already hands the leaver's armies to the AI with a synced command (in a battle the battle army, on the
// campaign map the faction). Then:
//  - EMPIRE_MP::update_should_end_game_due_to_player_withdrawals: when a side has no human left it ends the battle
//    (issue_battle_network_command_force_battle_end) or flags the campaign over ([mp+0xd3167], which common_update turns into
//    issue_campaign_network_command_force_game_over). Both are switched off.
//  - EMPIRE_MP::handle_player_left marks the disconnect message "game ends" (+0x18) for any other player in a campaign, so the
//    campaign UI opens its end box and the battle UI closes every panel. Now it is a plain notice.
//  - A player dropping during a campaign battle had only the battle army handed over; their campaign faction now goes to the AI too.
// All of it runs the same on every PC (synced commands and rules), and every player needs this loader.
GAME_FN(battle_switch_ai, 0x9a3b80, void, void *, uint32_t, uint32_t);
GAME_FN(campaign_switch_ai, 0x166c870, void, void *, const void *);
GAME_FN(factions_table, 0x26d4bf0, uint8_t *, void *);
static SYSV void mp_left_in_battle(void *battle_queue, uint32_t alliance, uint32_t army, uint8_t *mp, uint8_t *player) {
    battle_switch_ai(battle_queue, alliance, army);
    uint8_t *game = *(uint8_t **)(mp + 0x430);
    void *campaign_queue = *(void **)(mp + 0xd2fe0);
    if (!*(void **)(mp + 0xd2fe8) || !campaign_queue || !game) return;   // not a campaign battle
    uint8_t *table = factions_table(*(void **)(game + 0x20));
    uint32_t idx = *(uint32_t *)(player + 0x70);
    const void *faction = NULL;
    if (table && idx < *(uint32_t *)(table + 0xc)) faction = (*(void ***)(table + 0x10))[idx];
    campaign_switch_ai(campaign_queue, faction ? faction : (const void *)0x50980e8);   // the game's own fallback, as in player_left
}

static void patch_player_drop(void) {
    if (getenv("ATTILA_DROP_ENDS_GAME")) { log_line("player drop keeps the game going: off (ATTILA_DROP_ENDS_GAME)"); return; }
    static const uint8_t flag_old[2] = {0xB0, 0x01};                                         // 27ecc8c mov al,1
    static const uint8_t over_old[7] = {0xC6, 0x83, 0x67, 0x31, 0x0D, 0x00, 0x01};           // 27ed488 mov byte [rbx+d3167],1
    static const uint8_t end_old[5] = {0xE8, 0x57, 0x67, 0x1B, 0xFE};                        // 27ed4a4 call force_battle_end
    static const uint8_t ai_old[5] = {0xE8, 0x1D, 0x50, 0x1B, 0xFE};                         // 27eeb5e call battle switch_ai
    if (memcmp((void *)0x27ecc8c, flag_old, 2) || memcmp((void *)0x27ed488, over_old, 7) ||
        memcmp((void *)0x27ed4a4, end_old, 5) || memcmp((void *)0x27eeb5e, ai_old, 5)) {
        log_line("patch skipped, game code differs: player drop keeps the game going");
        return;
    }
    // "Concede Defeat" in a battle is the game's other way into update_should_end_game_due_to_player_withdrawals (27ed120):
    // EMPIRE_MP::handle_message_MP_NETWORK_MESSAGE_ID_PLAYER_WITHDRAWN calls it at 27ed67e, PLAYER_LEFT (a drop) at 27ec9aa.
    // Up to 1.3.1 the forced battle end was switched off for both, so a conceded campaign battle went on with the AI and ended
    // with the session broken (3-player test 2026-11-15: after the time ran out the host's "End Battle" sent every copy back to
    // the Campaign Games list). Now the forced end is skipped only for a drop; a concede ends the battle as in the game.
    // Switch ATTILA_DROP_OLD_CONCEDE=1 = the 1.2-1.3.1 rule (no forced end for either).
    static const uint8_t wd_old[5] = {0xE8, 0x9D, 0xFA, 0xFF, 0xFF};                        // 27ed67e call 27ed120
    int concede_fix = !getenv("ATTILA_DROP_OLD_CONCEDE") && !memcmp((void *)0x27ed67e, wd_old, 5);
    uint8_t *stub = near_game_code(96);
    if (!stub) { log_line("patch skipped, no memory near the game: player drop keeps the game going"); return; }
    // reached by the game's call (stack as at a normal call): rdi/esi/edx are set; add the EMPIRE_MP (rbx) and the player (rbp)
    uint8_t *p = stub;
    *p++ = 0x48; *p++ = 0x89; *p++ = 0xD9;                   // mov rcx,rbx
    *p++ = 0x49; *p++ = 0x89; *p++ = 0xE8;                   // mov r8,rbp
    *p++ = 0x48; *p++ = 0xB8;                                // mov rax,mp_left_in_battle
    uint64_t fn = (uint64_t)mp_left_in_battle; memcpy(p, &fn, 8); p += 8;
    *p++ = 0xFF; *p++ = 0xE0;                                // jmp rax
    *(uint8_t *)0x27ecc8d = 0x00;                            // mov al,0: the disconnect message no longer says the game ends
    *(uint8_t *)0x27ed48e = 0x00;                            // the campaign is not flagged over
    if (concede_fix) {
        uint8_t *flag = stub + 32;                           // 1 while the game handles a concede (PLAYER_WITHDRAWN)
        *flag = 0;
        uint8_t *end_stub = stub + 40, *wd_stub = stub + 64;
        int32_t r;
        p = end_stub;                                        // at 27ed4a4: the forced battle end only for a concede
        *p++ = 0x80; *p++ = 0x3D; r = (int32_t)(flag - (p + 5)); memcpy(p, &r, 4); p += 4; *p++ = 0x00;   // cmp byte [rip+flag],0
        *p++ = 0x74; *p++ = 0x05;                                                                        // je ret
        *p++ = 0xE9; r = (int32_t)((uint8_t *)0x9a3c00 - (p + 4)); memcpy(p, &r, 4); p += 4;             // jmp force_battle_end
        *p++ = 0xC3;                                                                                     // ret (a drop: no end)
        p = wd_stub;                                         // at 27ed67e: mark the concede around the game's call
        *p++ = 0xC6; *p++ = 0x05; r = (int32_t)(flag - (p + 5)); memcpy(p, &r, 4); p += 4; *p++ = 0x01;   // mov byte [rip+flag],1
        *p++ = 0x48; *p++ = 0x83; *p++ = 0xEC; *p++ = 0x08;                                              // sub rsp,8
        *p++ = 0xE8; r = (int32_t)((uint8_t *)0x27ed120 - (p + 4)); memcpy(p, &r, 4); p += 4;            // call update_should_end...
        *p++ = 0x48; *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;                                              // add rsp,8
        *p++ = 0xC6; *p++ = 0x05; r = (int32_t)(flag - (p + 5)); memcpy(p, &r, 4); p += 4; *p++ = 0x00;   // mov byte [rip+flag],0
        *p++ = 0xC3;                                                                                     // ret
        uint8_t *c = (uint8_t *)0x27ed4a4; r = (int32_t)(end_stub - (c + 5)); memcpy(c + 1, &r, 4);      // call end_stub
        c = (uint8_t *)0x27ed67e; r = (int32_t)(wd_stub - (c + 5)); memcpy(c + 1, &r, 4);                // call wd_stub
    } else memset((void *)0x27ed4a4, 0x90, 5);               // no forced battle end (1.2-1.3.1 rule)
    uint8_t *call = (uint8_t *)0x27eeb5e;
    int32_t rel = (int32_t)(stub - (call + 5));
    memcpy(call + 1, &rel, 4);
    log_line("patch applied: player drop keeps the game going (campaign + battle, AI takes over)%s",
             concede_fix ? "; Concede Defeat still ends the battle" : " (a concede does not end the battle either)");
}

// ---- 8v8 multiplayer battles. The network lobby already has 20 places and the host opens slots as players come (up to 32 with
// spectators); the limits are all in the battle setup screen:
//  - BattleSetupOptions::TeamSizesForMap: players per side from the map's battles row (most say 4) -> 8 on every map.
//  - MPBattleSetup::EvaluateMaxPlayers + MPBattleSetupSlotManager::SetTotalSlotsOpen / EvaluateMaxSpectators: 8 in total, 4 per
//    side, spectators counted in the same 8 -> 16 in total, 8 per side, spectators counted from 16.
//  - MPBattleSetupAlliance (one per side): starts with 4 player boxes (OnCreate), SetMaxPlayers / SetInitialOpenSlots /
//    CheckForAddSlotButton stop at 4, and FindUnusedSlot / FindSlotWithStatus treat the last 4 children as the boxes -> 8.
//  - MPBattleSetupAlliance::SortPlayerSlots lines the boxes up in one row -> two rows of 4 (mp_place_slots). The boxes are made
//    compact (155x64) by attila_8v8_battle.pack so both rows fit in the old height.
//  - BattleSetupPlayer::SortOrderScore = band + lobby place number, bands 10 apart (player 0/10/20, open place 30, "add" box 40,
//    hidden 50). Fine for places 0-7; with 16 players the places run to 19, so open places from 9 on sorted after the "add" box,
//    CheckForAddSlotButton / ChangeSlotAdjacentTo (which expect the used boxes first) turned an open box into a new "add" box and
//    its place stayed in the lobby unseen: a side could keep adding up to 20 places. Bands 40 apart instead (same order, room for
//    places 0-19). The score is only used to sort the boxes (and a debug tooltip): display only.
//  - Five more "8 - open places side 1 - side 2" spectator sums (SetMaxPlayerLimits, SetTeamPlayerCount, ChangeSlotAdjacentTo,
//    OnOpenPlayerSlotAdded, on_open_player_slot_removed) -> 16, like the ones above.
// Every player needs this loader and the pack. ATTILA_NO_8V8=1 turns it off.
GAME_FN(component_width, 0x2916bd0, float, void *);
#define SLOT_ROW_HEIGHT 66.0f
static SYSV void mp_place_slots(uint8_t *alliance, void **items, uint32_t n, float start) {
    int mirrored = *(uint32_t *)(alliance + 0x64) != 0;   // the right-hand side fills from its right edge
    int k = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *c = items[i];
        if (!c || !*(void **)(c + 0x258)) continue;           // the side's title and other children are not player boxes
        float w = component_width(c);
        int col = k % 4, row = k / 4;
        float pos[2] = {mirrored ? start - (float)col * w : start + (float)col * w, (float)row * SLOT_ROW_HEIGHT};
        SYSV void (*move)(void *, const float *) = (SYSV void (*)(void *, const float *))(*(void ***)c)[0xb0 / 8];   // MoveToRelative
        move(c, pos);
        k++;
    }
}

static void patch_battle_8v8(void) {
    if (getenv("ATTILA_NO_8V8")) { log_line("8v8 battles off (ATTILA_NO_8V8)"); return; }
    static const struct { uintptr_t at; uint8_t len; uint8_t old[8]; uint8_t imm_at; uint8_t value; } caps[] = {
        {0x2cd42fe, 3, {0x83, 0xF9, 0x09}, 2, 0x11},                         // EvaluateMaxPlayers: both sides < 9 -> < 17
        {0x2cd4355, 5, {0xBE, 0x08, 0x00, 0x00, 0x00}, 1, 0x10},             //   spectators 8 - players -> 16 - players
        {0x2cd43ab, 5, {0xBE, 0x08, 0x00, 0x00, 0x00}, 1, 0x10},
        {0x2d3ec0d, 5, {0xBE, 0x08, 0x00, 0x00, 0x00}, 1, 0x10},             // EvaluateMaxSpectators
        {0x2cda4ef, 3, {0x83, 0xF8, 0x07}, 2, 0x0F},                         // SetTotalSlotsOpen: total <= 7 -> <= 15
        {0x2cda4f6, 3, {0x83, 0xFB, 0x04}, 2, 0x08},                         //   side 1 up to 4 -> 8
        {0x2cda4f9, 5, {0xB8, 0x04, 0x00, 0x00, 0x00}, 1, 0x08},
        {0x2cda517, 3, {0x83, 0xFD, 0x04}, 2, 0x08},                         //   side 2 up to 4 -> 8
        {0x2cda52e, 4, {0x41, 0x83, 0xFF, 0x07}, 3, 0x0F},
        {0x2cda555, 5, {0xBE, 0x08, 0x00, 0x00, 0x00}, 1, 0x10},
        {0x2cda55f, 3, {0x83, 0xF8, 0x08}, 2, 0x10},
        {0x2cda5b1, 3, {0x83, 0xF8, 0x08}, 2, 0x10},
        {0x2cda5bf, 5, {0xBB, 0x04, 0x00, 0x00, 0x00}, 1, 0x08},
        {0x2cda5c4, 5, {0xBE, 0x04, 0x00, 0x00, 0x00}, 1, 0x08},
        {0x2d3f809, 8, {0x48, 0xC7, 0x43, 0x60, 0x04, 0x00, 0x00, 0x00}, 4, 0x08},   // MPBattleSetupAlliance::OnCreate: 4 boxes -> 8
        {0x2d3e9f8, 3, {0x83, 0xFE, 0x04}, 2, 0x08},                         // SetMaxPlayers: at most 4 -> 8
        {0x2d3e9fb, 5, {0xBD, 0x04, 0x00, 0x00, 0x00}, 1, 0x08},
        {0x2d3f36c, 5, {0xB8, 0x04, 0x00, 0x00, 0x00}, 1, 0x08},             // FindUnusedSlot: last 4 children -> 8
        {0x2d3f392, 4, {0x48, 0x83, 0xF8, 0xE0}, 3, 0xC0},                   //   stop after 4 (-0x20) -> 8 (-0x40)
        {0x2d3f43d, 5, {0xBA, 0x04, 0x00, 0x00, 0x00}, 1, 0x08},             // FindSlotWithStatus: same
        {0x2d3f487, 5, {0x48, 0x8D, 0x44, 0xC8, 0xE0}, 4, 0xC0},
        {0x2d3f68a, 3, {0x83, 0xF8, 0x04}, 2, 0x08},                         // SetInitialOpenSlots: at most 4 -> 8
        {0x2d3f68d, 6, {0x41, 0xBC, 0x04, 0x00, 0x00, 0x00}, 2, 0x08},
        {0x2d406cd, 3, {0x83, 0xF9, 0x04}, 2, 0x08},                         // CheckForAddSlotButton: at most 4 -> 8
        {0x2d406d0, 5, {0xB8, 0x04, 0x00, 0x00, 0x00}, 1, 0x08},
        {0x2d46383, 5, {0xBD, 0x26, 0x00, 0x00, 0x00}, 1, 0x96},             // SortOrderScore bands: single player box 38 -> 150
        {0x2d4639b, 5, {0xBD, 0x32, 0x00, 0x00, 0x00}, 1, 0xC8},             //   hidden 50 -> 200
        {0x2d463b0, 5, {0xBD, 0x28, 0x00, 0x00, 0x00}, 1, 0xA0},             //   "add" box 40 -> 160
        {0x2d463c3, 5, {0xBD, 0x1E, 0x00, 0x00, 0x00}, 1, 0x78},             //   open place 30 -> 120
        {0x2d463d6, 5, {0xBD, 0x14, 0x00, 0x00, 0x00}, 1, 0x50},             //   player 20 -> 80
        {0x2d46407, 5, {0xBD, 0x0A, 0x00, 0x00, 0x00}, 1, 0x28},             //   player 10 -> 40
        {0x2d46451, 5, {0xBD, 0x0A, 0x00, 0x00, 0x00}, 1, 0x28},
        {0x2cd93a9, 5, {0xBE, 0x08, 0x00, 0x00, 0x00}, 1, 0x10},             // spectators 8 - open places -> 16: SetMaxPlayerLimits
        {0x2d3f045, 5, {0xBE, 0x08, 0x00, 0x00, 0x00}, 1, 0x10},             //   SetTeamPlayerCount
        {0x2d3fe4f, 5, {0xBE, 0x08, 0x00, 0x00, 0x00}, 1, 0x10},             //   ChangeSlotAdjacentTo
        {0x2d40881, 5, {0xBE, 0x08, 0x00, 0x00, 0x00}, 1, 0x10},             //   OnOpenPlayerSlotAdded
        {0x2d40a1e, 5, {0xBE, 0x08, 0x00, 0x00, 0x00}, 1, 0x10},             //   on_open_player_slot_removed
    };
    static const uint8_t sizes_old[9] = {0x48, 0xC1, 0xE1, 0x20, 0x89, 0xD0, 0x48, 0x09, 0xC8};   // TeamSizesForMap end
    static const uint8_t sort_old[9] = {0x41, 0x83, 0x3E, 0x00, 0x74, 0x7D, 0x49, 0x8B, 0x2F};    // SortPlayerSlots placing loop
    for (size_t i = 0; i < sizeof caps / sizeof caps[0]; i++)
        if (memcmp((void *)caps[i].at, caps[i].old, caps[i].len)) {
            log_line("patch skipped, game code differs at %llx: 8v8 battles", (unsigned long long)caps[i].at);
            return;
        }
    if (memcmp((void *)0x2cd9347, sizes_old, 9) || memcmp((void *)0x2d3f275, sort_old, 9)) {
        log_line("patch skipped, game code differs: 8v8 battles");
        return;
    }
    uint8_t *s = near_game_code(128);
    if (!s) { log_line("patch skipped, no memory near the game: 8v8 battles"); return; }
    uint8_t *p = s;
#define PUT(...) do { static const uint8_t b_[] = {__VA_ARGS__}; memcpy(p, b_, sizeof b_); p += sizeof b_; } while (0)
#define JMP_TO(target) do { *p++ = 0xE9; int32_t r_ = (int32_t)((uint8_t *)(target) - (p + 4)); memcpy(p, &r_, 4); p += 4; } while (0)
    // TeamSizesForMap: 8 per side whatever the map says - in multiplayer. Single-player Custom Battle keeps the map's own sizes
    // (4 a side on most maps): its lobby (FRONTEND::SINGLE_PLAYER_GAME_LOBBY) has 8 places in all, shared by both sides, so with
    // 8 a side one side could take every place (user, 2026-11-10). The lobby is FRONT_END_UI::s_ui->+2a0->+78; the
    // single-player one is told by its vtable (3ac81b0 + 10h).
    uint8_t *sizes_stub = p;
    uint8_t *jz1, *jz2, *jz3, *je_keep, *force, *keep;
    PUT(0x4C, 0x8B, 0x1C, 0x25, 0x98, 0x72, 0x16, 0x05);   // mov r11,[5167298h]   (UIDLL::FRONT_END_UI::s_ui)
    PUT(0x4D, 0x85, 0xDB); jz1 = p; PUT(0x74, 0x00);        // test r11,r11; jz force
    PUT(0x4D, 0x8B, 0x9B, 0xA0, 0x02, 0x00, 0x00);          // mov r11,[r11+2a0h]
    PUT(0x4D, 0x85, 0xDB); jz2 = p; PUT(0x74, 0x00);
    PUT(0x4D, 0x8B, 0x5B, 0x78);                            // mov r11,[r11+78h]    (the lobby)
    PUT(0x4D, 0x85, 0xDB); jz3 = p; PUT(0x74, 0x00);
    PUT(0x49, 0x81, 0x3B, 0xC0, 0x81, 0xAC, 0x03);          // cmp qword [r11],3ac81c0h  (single-player lobby?)
    je_keep = p; PUT(0x74, 0x00);                           // je keep (the map's sizes)
    force = p;
    PUT(0xBA, 0x08, 0x00, 0x00, 0x00,  0xB9, 0x08, 0x00, 0x00, 0x00);   // mov edx,8; mov ecx,8
    keep = p;
    PUT(0x48, 0xC1, 0xE1, 0x20, 0x89, 0xD0, 0x48, 0x09, 0xC8);           // the replaced instructions
    JMP_TO(0x2cd9350);
    jz1[1] = (uint8_t)(force - (jz1 + 2)); jz2[1] = (uint8_t)(force - (jz2 + 2)); jz3[1] = (uint8_t)(force - (jz3 + 2));
    je_keep[1] = (uint8_t)(keep - (je_keep + 2));
    // SortPlayerSlots placing loop -> mp_place_slots(alliance=rbx, items=[r15], count=[r14], start=[rsp+18h])
    uint8_t *sort_stub = p;
    PUT(0xF3, 0x0F, 0x10, 0x44, 0x24, 0x18);   // movss xmm0,[rsp+18h]
    PUT(0x48, 0x89, 0xDF);                     // mov rdi,rbx
    PUT(0x49, 0x8B, 0x37);                     // mov rsi,[r15]
    PUT(0x41, 0x8B, 0x16);                     // mov edx,[r14]
    PUT(0x55, 0x48, 0x89, 0xE5, 0x48, 0x83, 0xE4, 0xF0);   // push rbp; mov rbp,rsp; and rsp,-16
    *p++ = 0x48; *p++ = 0xB8; { uint64_t fn = (uint64_t)mp_place_slots; memcpy(p, &fn, 8); p += 8; }   // mov rax,fn
    PUT(0xFF, 0xD0);                           // call rax
    PUT(0x48, 0x89, 0xEC, 0x5D);               // mov rsp,rbp; pop rbp
    JMP_TO(0x2d3f2f8);
#undef PUT
#undef JMP_TO
    for (size_t i = 0; i < sizeof caps / sizeof caps[0]; i++) *(uint8_t *)(caps[i].at + caps[i].imm_at) = caps[i].value;
    uint8_t *g = (uint8_t *)0x2cd9347;
    int32_t r = (int32_t)(sizes_stub - (g + 5)); g[0] = 0xE9; memcpy(g + 1, &r, 4); memset(g + 5, 0x90, 4);
    g = (uint8_t *)0x2d3f275;
    r = (int32_t)(sort_stub - (g + 5)); g[0] = 0xE9; memcpy(g + 1, &r, 4); memset(g + 5, 0x90, 4);
    log_line("patch applied: 8v8 multiplayer battles (8 per side, 16 in total, two rows of boxes)");
}

// Multiplayer battle lobby, joining player (Test 14, 1212AD, 16 players): UIDLL::MPBattleSetup::OnLobbyCreated did not find the
// local player's slot in any player box, so SelectPlayer / BattleSetupPlayer::UpdateArmyManager never gave the army manager its force
// (+1a0h) and MPArmyManager::UpdateEquipmentPanel crashed on it (2cc896f). Guard: no force = nothing to refresh yet. Display only.
// Log: before SetAllianceInformationAndCenterPiece, the local slot, each side's slots from the lobby and the slots the boxes got.
GAME_FN(lobby_alliance_info, 0x2cd3f30, void, void *, const void *);
GAME_FN(game_free, 0x525fd0, void, const void *);
typedef struct { uint32_t cap, count; uint32_t *data; } U32Vector;   // CA_STD::VECTOR<unsigned int>
static int lobby_slots_text(char *out, size_t size, void *lobby, uint32_t team, int spectators) {
    U32Vector v = {0, 0, NULL};
    SYSV void (*slots)(void *, U32Vector *, uint32_t, int) = (SYSV void (*)(void *, U32Vector *, uint32_t, int))(*(void ***)lobby)[0x160 / 8];
    slots(lobby, &v, team, spectators);   // GAME_LOBBY::get_open_and_filled_slots(vector, team, spectators)
    int n = 0;
    for (uint32_t i = 0; i < v.count && v.data && n < (int)size - 8; i++) n += snprintf(out + n, size - n, " %u", v.data[i]);
    if (!v.count) n += snprintf(out + n, size - n, " none");
    if (v.data) game_free(v.data);
    return n;
}
static int box_slots_text(char *out, size_t size, uint8_t *alliance, int *found, uint32_t me) {
    int n = 0;
    uint8_t *comp = alliance ? *(uint8_t **)(alliance + 0x8) : NULL;
    if (!comp) return snprintf(out, size, " (no side)");
    uint32_t count = *(uint32_t *)(comp + 0x114);
    uint8_t **kids = *(uint8_t ***)(comp + 0x118);
    for (uint32_t i = 1; i < count && kids && n < (int)size - 12; i++) {   // child 0 is the side's title
        uint8_t *player = kids[i] ? *(uint8_t **)(kids[i] + 0x258) : NULL;
        if (!player) { n += snprintf(out + n, size - n, " -"); continue; }
        int32_t slot = *(int32_t *)(player + 0xa4);
        if ((uint32_t)slot == me) *found = 1;
        n += snprintf(out + n, size - n, " %d", slot);
    }
    return n;
}
static SYSV void lobby_screen_check(uint8_t *setup, const void *params, void *lobby) {
    if (setup && lobby) {
        char a[200], b[200], s[200], x[200], y[200];
        SYSV uint32_t (*my_slot)(void *) = (SYSV uint32_t (*)(void *))(*(void ***)lobby)[0x28 / 8];   // get_local_player_slot
        uint32_t me = my_slot(lobby);
        lobby_slots_text(a, sizeof a, lobby, 0, 0);
        lobby_slots_text(b, sizeof b, lobby, 1, 0);
        lobby_slots_text(s, sizeof s, lobby, 0, 1);
        uint8_t *manager = *(uint8_t **)(setup + 0x108);
        int found = 0;
        box_slots_text(x, sizeof x, manager ? *(uint8_t **)(manager + 0x50) : NULL, &found, me);
        box_slots_text(y, sizeof y, manager ? *(uint8_t **)(manager + 0x58) : NULL, &found, me);
        uint8_t *army = *(uint8_t **)(setup + 0x118);
        log_line("lobby screen: my slot %u, my box %s, army %s; side 1 slots%s, side 2 slots%s, spectators%s; side 1 boxes%s, side 2 boxes%s",
                 me, found ? "found" : "MISSING", army && *(void **)(army + 0x1a0) ? "set" : "NOT SET", a, b, s, x, y);
    }
    lobby_alliance_info(setup, params);
}

// ---- Steam texts with a 4-byte letter (2026-11-14). CA::utf8_decode (524880; its only caller is MP_LIB::UTF8ToUniString
// 28211d0, which makes room for strlen(text) letters) knows letters of 1, 2 and 3 bytes only. A 4-byte letter (an emoji
// in a Steam name, for example) puts it out of step, a continuation byte then read as the start of a letter can take the
// text's end marker with it, and it decodes and writes on past the end of its room. Windows' heap stops the program
// (c0000374, heap corruption): both joining copies of a 3-player test crashed so, while the lobby's friends list was
// built. Used for every MP_LIB text from Steam: friends' and players' names, lobby texts, chat. This decoder gives the
// same letters for every correct 1-3 byte letter (the game's own sums), one '?' for a 4-byte letter and one '?' for each
// byte that does not start a whole letter, and never reads past the end marker. Every player must decode the same way.
static SYSV uint16_t *mp_utf8_decode(uint16_t *out, const char *text) {
    const uint8_t *s = (const uint8_t *)text;
    while (*s) {
        uint8_t b = *s;
        if (b < 0x80) { *out++ = b; s++; continue; }
        if (b >= 0xC0 && b < 0xE0 && (s[1] & 0xC0) == 0x80) {
            *out++ = (uint16_t)(((b & 0x1F) << 6) | (s[1] & 0x3F)); s += 2; continue;
        }
        if (b >= 0xE0 && b < 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
            *out++ = (uint16_t)(((b & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F)); s += 3; continue;
        }
        if (b >= 0xF0 && b < 0xF8 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
            *out++ = '?'; s += 4; continue;
        }
        *out++ = '?'; s++;
    }
    *out = 0;
    return out;   // as the game's: the place of the end marker
}
static void patch_mp_text_decode(void) {   // test switch ATTILA_NO_MP_TEXT_FIX=1: the game's own decoder (to show the crash)
    if (getenv("ATTILA_NO_MP_TEXT_FIX")) { log_line("test: Steam texts are decoded the game's own way (ATTILA_NO_MP_TEXT_FIX)"); return; }
    static const uint8_t old[116] = {   // the whole of CA::utf8_decode
        0xeb, 0x18, 0x66, 0x66, 0x66, 0x66, 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x66, 0x89, 0x07, 0x48, 0xff, 0xc6, 0x48, 0x83, 0xc7, 0x02, 0x8a, 0x0e, 0x84, 0xc9, 0x74, 0x4b,
        0x0f, 0xb6, 0xc1, 0xa8, 0x80, 0x74, 0xe9, 0xf6, 0xc1, 0x20, 0x75, 0x1b, 0x0f, 0xb6, 0x4e, 0x01,
        0xc1, 0xe0, 0x06, 0x8d, 0x84, 0x08, 0x80, 0xcf, 0x00, 0x00, 0x66, 0x89, 0x07, 0x48, 0x83, 0xc6,
        0x02, 0x48, 0x83, 0xc7, 0x02, 0xeb, 0xd3, 0x0f, 0xb6, 0x4e, 0x02, 0x0f, 0xb6, 0x56, 0x01, 0xc1,
        0xe2, 0x06, 0xc1, 0xe0, 0x0c, 0x09, 0xc8, 0x8d, 0x84, 0x10, 0x80, 0xdf, 0x00, 0x00, 0x66, 0x89,
        0x07, 0x48, 0x83, 0xc6, 0x03, 0x48, 0x83, 0xc7, 0x02, 0xeb, 0xaf, 0x66, 0xc7, 0x07, 0x00, 0x00,
        0x48, 0x89, 0xf8, 0xc3};
    if (memcmp((const void *)0x524880, old, sizeof old)) { log_line("patch skipped, game code differs: Steam texts with 4-byte letters"); return; }
    if (!hook_entry(0x524880, old, 5, (void *)mp_utf8_decode)) { log_line("patch skipped, no memory near the game: Steam texts with 4-byte letters"); return; }
    log_line("patch applied: Steam names and texts with 4-byte letters (emoji) are read safely (CA::utf8_decode)");
}

// ---- Campaign messages (the event feed) for every human player.
// Attila's campaign is built for 2 players: EVENT_FEED::update_human_factions (1059da0) lists the human factions and cuts the
// list to the first 2 of the turn order. A human not on the list gets no campaign messages, and EVENT_FEED::add_event records
// an event only when it concerns a listed human - so a dilemma for a 3rd human is never recorded: its window never opens, the
// pending dilemma stops the turn timer and End Turn (Чебупель: "the third player's time stops at 4:59"; reproduced in the
// 3-player test 2026-11-14). Now the list keeps every human faction, in the game's own sort by faction number: built from the
// factions' human flags and the turn order, so it is the same on every PC.
// The feed keeps per listed human the place of their last end of turn (feed+130h, room for 2): EVENT_FEED::end_of_turn writes
// it, EVENT_FEED::summary (the events list) reads it and refused humans 3+. Humans 3 and up keep theirs in g_feed_mark, written
// and read at the same moments. The save holds the game's 2; after a load or a change of players the marks of humans 3+
// follow the first human's mark until their own next end of turn (on a change the game sets its own 2 to that same place).
// Read marks (2 bits per message) exist for the first 2 humans only, so for humans 3+ messages stay marked unread.
// Test switch ATTILA_FEED_TWO_HUMANS=1 = the game's limit (shows the freeze).
#define FEED_MARKS 16
#define FEED_MARK_UNSET 0xFFFFFFFFu
static uint32_t g_feed_mark[FEED_MARKS];
static void feed_marks_reset(void) { for (int i = 0; i < FEED_MARKS; i++) g_feed_mark[i] = FEED_MARK_UNSET; }
static void feed_log_humans(const uint8_t *feed, const char *why) {
    static int told;
    if (told >= 20) return;
    told++;
    uint32_t n = *(const uint32_t *)(feed + 0x15c);
    const uint8_t *const *d = *(const uint8_t *const *const *)(feed + 0x160);
    char keys[300];
    size_t k = 0;
    keys[0] = 0;
    for (uint32_t i = 0; i < n && d && i < FEED_MARKS && k + 40 < sizeof keys; i++) {
        const uint8_t *rec = d[i] ? *(const uint8_t *const *)(d[i] + 0x8f8) : NULL;   // FACTION +8f8h: its record, key text at +8
        const char *key = rec ? *(const char *const *)(rec + 8) : NULL;
        k += (size_t)snprintf(keys + k, sizeof keys - k, "%s%.32s", i ? ", " : "", key ? key : "?");
    }
    log_line("campaign messages: %u human factions listed (%s)%s", n, keys, why);
}
static SYSV void (*g_feed_update_orig)(uint8_t *feed);
static SYSV void feed_update_humans(uint8_t *feed) {
    uint32_t n0 = *(const uint32_t *)(feed + 0x15c);
    const void *const *d0 = *(const void *const *const *)(feed + 0x160);
    const void *before[FEED_MARKS];
    uint32_t nb = n0 < FEED_MARKS ? n0 : FEED_MARKS;
    for (uint32_t i = 0; i < nb; i++) before[i] = d0 ? d0[i] : NULL;
    g_feed_update_orig(feed);
    uint32_t n1 = *(const uint32_t *)(feed + 0x15c);
    const void *const *d1 = *(const void *const *const *)(feed + 0x160);
    int changed = n1 != n0;
    for (uint32_t i = 0; !changed && i < nb; i++) changed = !d1 || d1[i] != before[i];
    if (!changed) return;
    feed_marks_reset();
    feed_log_humans(feed, "");
}
static SYSV void *(*g_feed_read_orig)(void *ret, void *access, void *section);
static SYSV void *feed_read_humans(void *ret, void *access, void *section) {   // only the feed's load constructor calls it
    feed_marks_reset();
    return g_feed_read_orig(ret, access, section);
}
static void feed_put(uint8_t **p, const void *b, size_t n) { memcpy(*p, b, n); *p += n; }
static void feed_put_jmp(uint8_t **p, uintptr_t to) {
    int32_t r = (int32_t)((intptr_t)to - (intptr_t)(*p + 5));
    (*p)[0] = 0xE9; memcpy(*p + 1, &r, 4); *p += 5;
}
static void feed_put_marks(uint8_t **p) {   // mov rcx,g_feed_mark
    uint64_t a = (uint64_t)(uintptr_t)g_feed_mark;
    (*p)[0] = 0x48; (*p)[1] = 0xB9; memcpy(*p + 2, &a, 8); *p += 10;
}
static void feed_site_jump(uint8_t *at, const uint8_t *to, int len) {   // jmp to the new code, the rest of the old instruction = nop
    int32_t r = (int32_t)(to - (at + 5));
    at[0] = 0xE9; memcpy(at + 1, &r, 4);
    memset(at + 5, 0x90, (size_t)(len - 5));
}
static void patch_event_feed_humans(void) {
    if (getenv("ATTILA_FEED_TWO_HUMANS")) { log_line("test: campaign messages for the first 2 human players only, the game's limit (ATTILA_FEED_TWO_HUMANS)"); return; }
    uint8_t *cap = (uint8_t *)0x1059e2f, *eot = (uint8_t *)0x105a9f0, *chk = (uint8_t *)0x105d4d3;
    uint8_t *rd1 = (uint8_t *)0x105d55d, *rd2 = (uint8_t *)0x105d5e0;
    static const uint8_t cap_old[6] = {0x48, 0x83, 0xF9, 0x02, 0x76, 0x71};                       // cmp rcx,2; jbe (list kept)
    static const uint8_t eot_old[8] = {0x43, 0x89, 0x84, 0xAF, 0x30, 0x01, 0x00, 0x00};           // mov [r15+r13*4+130h],eax
    static const uint8_t chk_old[10] = {0x48, 0x83, 0xF8, 0x0F, 0x0F, 0x87, 0x6A, 0x04, 0x00, 0x00}; // cmp rax,0Fh; ja (humans 1-2 only)
    static const uint8_t rd1_old[8] = {0x45, 0x8B, 0xBC, 0x94, 0x30, 0x01, 0x00, 0x00};           // mov r15d,[r12+rdx*4+130h]
    static const uint8_t rd2_old[8] = {0x43, 0x8B, 0x84, 0xAC, 0x30, 0x01, 0x00, 0x00};           // mov eax,[r12+r13*4+130h]
    static const uint8_t upd_old[5] = {0x55, 0x41, 0x57, 0x41, 0x56};                             // update_human_factions start
    static const uint8_t rdh_old[5] = {0x41, 0x57, 0x41, 0x56, 0x53};                             // read_human_factions start
    if (memcmp(cap, cap_old, sizeof cap_old) || memcmp(eot, eot_old, sizeof eot_old) || memcmp(chk, chk_old, sizeof chk_old) ||
        memcmp(rd1, rd1_old, sizeof rd1_old) || memcmp(rd2, rd2_old, sizeof rd2_old) ||
        memcmp((const void *)0x1059da0, upd_old, sizeof upd_old) || memcmp((const void *)0x1059b00, rdh_old, sizeof rdh_old)) {
        log_line("patch skipped, game code differs: campaign messages for every human player"); return;
    }
    uint8_t *s = near_game_code(192);
    if (!s) { log_line("patch skipped, no memory near the game: campaign messages for every human player"); return; }
    feed_marks_reset();
    g_feed_read_orig = (SYSV void *(*)(void *, void *, void *))hook_entry(0x1059b00, rdh_old, 5, (void *)feed_read_humans);
    if (!g_feed_read_orig) { log_line("patch skipped, no memory near the game: campaign messages for every human player"); return; }
    g_feed_update_orig = (SYSV void (*)(uint8_t *))hook_entry(0x1059da0, upd_old, 5, (void *)feed_update_humans);
    if (!g_feed_update_orig) { log_line("patch skipped, no memory near the game: campaign messages for every human player"); return; }
    uint8_t *p = s;
    // EVENT_FEED::end_of_turn: r13 = the human's place on the list, eax = where this end of turn is in the feed.
    uint8_t *a = p;
    static const uint8_t a1[] = {0x49, 0x83, 0xFD, 0x02, 0x73, 0x0D};                 // cmp r13,2; jae -> humans 3+
    feed_put(&p, a1, sizeof a1);
    feed_put(&p, eot_old, sizeof eot_old);                                             // humans 1-2: the game's own
    feed_put_jmp(&p, 0x105a9f8);
    static const uint8_t a2[] = {0x49, 0x83, 0xFD, FEED_MARKS, 0x73, 0x10, 0x51};     // cmp r13,16; jae -> back; push rcx
    feed_put(&p, a2, sizeof a2);
    feed_put_marks(&p);
    static const uint8_t a3[] = {0x42, 0x89, 0x04, 0xA9, 0x59};                       // mov [rcx+r13*4],eax; pop rcx
    feed_put(&p, a3, sizeof a3);
    feed_put_jmp(&p, 0x105a9f8);
    // EVENT_FEED::summary, first read: rdx = place on the list, into r15d. Humans 3+: own mark, else the first human's.
    uint8_t *b = p;
    static const uint8_t b1[] = {0x48, 0x83, 0xFA, 0x02, 0x73, 0x0D};                 // cmp rdx,2; jae -> humans 3+
    feed_put(&p, b1, sizeof b1);
    feed_put(&p, rd1_old, sizeof rd1_old);
    feed_put_jmp(&p, 0x105d565);
    static const uint8_t b2[] = {0x45, 0x8B, 0xBC, 0x24, 0x30, 0x01, 0x00, 0x00,      // mov r15d,[r12+130h]
                                 0x48, 0x83, 0xFA, FEED_MARKS, 0x73, 0x17, 0x51};      // cmp rdx,16; jae -> back; push rcx
    feed_put(&p, b2, sizeof b2);
    feed_put_marks(&p);
    static const uint8_t b3[] = {0x8B, 0x0C, 0x91,                                    // mov ecx,[rcx+rdx*4]
                                 0x83, 0xF9, 0xFF, 0x74, 0x03,                         // cmp ecx,-1; je -> keep
                                 0x41, 0x89, 0xCF, 0x59};                              // mov r15d,ecx; pop rcx
    feed_put(&p, b3, sizeof b3);
    feed_put_jmp(&p, 0x105d565);
    // EVENT_FEED::summary, second read: r13 = place on the list, into eax (rcx is in use by the game here: kept).
    uint8_t *c = p;
    static const uint8_t c1[] = {0x49, 0x83, 0xFD, 0x02, 0x73, 0x0D};                 // cmp r13,2; jae -> humans 3+
    feed_put(&p, c1, sizeof c1);
    feed_put(&p, rd2_old, sizeof rd2_old);
    feed_put_jmp(&p, 0x105d5e8);
    static const uint8_t c2[] = {0x41, 0x8B, 0x84, 0x24, 0x30, 0x01, 0x00, 0x00,      // mov eax,[r12+130h]
                                 0x49, 0x83, 0xFD, FEED_MARKS, 0x73, 0x17, 0x51};      // cmp r13,16; jae -> back; push rcx
    feed_put(&p, c2, sizeof c2);
    feed_put_marks(&p);
    static const uint8_t c3[] = {0x42, 0x8B, 0x0C, 0xA9,                              // mov ecx,[rcx+r13*4]
                                 0x83, 0xF9, 0xFF, 0x74, 0x02,                         // cmp ecx,-1; je -> keep
                                 0x89, 0xC8, 0x59};                                    // mov eax,ecx; pop rcx
    feed_put(&p, c3, sizeof c3);
    feed_put_jmp(&p, 0x105d5e8);
    feed_site_jump(eot, a, 8);
    feed_site_jump(rd1, b, 8);
    feed_site_jump(rd2, c, 8);
    chk[3] = FEED_MARKS * 8 - 1;   // summary: humans 1-16 (was 1-2)
    cap[4] = 0xEB;                 // update_human_factions: never cut the list
    log_line("patch applied: campaign messages and dilemmas for every human player (event feed list not cut at 2, %d bytes)", (int)(p - s));
}

// ---- Campaign script commands in "direct" mode on every PC.
// Attila gives the campaign scripts a command queue (Lua global CommandQueue, read by GAME() into SCRIPTING_INTERFACE+8),
// but in multiplayer only on the PC of the first human faction (CAMPAIGN_ENV: local faction != first_human_faction -> no
// queue). Commands sent through the queue are issued by that one PC and reach every PC through the lockstep. Some script
// commands take a switch for this (CA's script library calls it command_queue): off = change the game at once, as in
// single player - the scripts run on every PC at the same moment, so every PC makes the same change. kill_character,
// create_force, create_agent, hide_character and seek_exchange test for the queue BEFORE that switch and do nothing at all
// without one, so in direct mode they only work on the first human's PC and the PCs drift apart.
// Seen in the 3-player test 2026-11-14 (1212AD): the Pope change at turn 10 (cm:kill_character(old pope, false, false))
// killed the old Pope on France's PC only -> "Desync Detected" when France ended the turn. 1212AD's plague and pretender
// scripts kill the same way; its invasions, rebels and buffer states create forces in direct mode.
// Now: with no queue, direct mode goes on as it does on the first human's PC; queued mode still does nothing there (the
// first human's PC sends it to everyone, the game's own way). unhide_character is left as it is: its direct path hands
// the queue pointer to the campaign model call (game bug), without a queue that would crash.
// Test switch ATTILA_SCRIPT_DIRECT_FIRST_PC=1 = the game's rule (shows the desync).
static void patch_script_direct_mode(void) {
    if (getenv("ATTILA_SCRIPT_DIRECT_FIRST_PC")) { log_line("test: script commands in direct mode only on the first human's PC, the game's rule (ATTILA_SCRIPT_DIRECT_FIRST_PC)"); return; }
    // each site: queue test + "je end" (0F 84 rel32) + the switch test that picks direct mode (74 xx = jump there when off)
    static const uint8_t kill_old[24] = {0x49, 0x8B, 0x7F, 0x08, 0x48, 0x85, 0xFF, 0x0F, 0x84, 0xDC, 0x00, 0x00,   // kill_character
                                         0x00, 0x80, 0x7C, 0x24, 0x1E, 0x00, 0x8A, 0x44, 0x24, 0x1F, 0x74, 0x24};
    static const uint8_t force_old[11] = {0x49, 0x83, 0x7F, 0x08, 0x00, 0x0F, 0x84, 0xB8, 0x00, 0x00, 0x00};      // create_force
    static const uint8_t force_sw[7] = {0x80, 0x7C, 0x24, 0x47, 0x00, 0x74, 0x48};                                  // its switch, later
    static const uint8_t agent_old[21] = {0x49, 0x8B, 0x7C, 0x24, 0x08, 0x48, 0x85, 0xFF, 0x0F, 0x84, 0x9F, 0x00,   // create_agent
                                          0x00, 0x00, 0x80, 0x7C, 0x24, 0x3F, 0x00, 0x74, 0x31};
    static const uint8_t hide_old[20] = {0x49, 0x8B, 0x7F, 0x08, 0x48, 0x85, 0xFF, 0x0F, 0x84, 0x8E, 0x01, 0x00,   // hide_character
                                         0x00, 0x80, 0x7C, 0x24, 0x1F, 0x00, 0x74, 0x22};
    static const uint8_t seek_old[20] = {0x49, 0x8B, 0x7E, 0x08, 0x48, 0x85, 0xFF, 0x0F, 0x84, 0x90, 0x00, 0x00,   // seek_exchange
                                         0x00, 0x80, 0x7C, 0x24, 0x3E, 0x00, 0x74, 0x2E};
    struct { uintptr_t at; const uint8_t *old; size_t n; int je; uint8_t sw; } site[5] = {
        {0x17445bd, kill_old, sizeof kill_old, 7, 0x1E},
        {0x17420d5, force_old, sizeof force_old, 5, 0x47},
        {0x1743f5f, agent_old, sizeof agent_old, 8, 0x3F},
        {0x17423b5, hide_old, sizeof hide_old, 7, 0x1F},
        {0x1741be3, seek_old, sizeof seek_old, 7, 0x3E},
    };
    int same = !memcmp((const void *)0x174210c, force_sw, sizeof force_sw);
    for (int i = 0; i < 5 && same; i++) same = !memcmp((const void *)site[i].at, site[i].old, site[i].n);
    if (!same) { log_line("patch skipped, game code differs: script commands in direct mode on every PC"); return; }
    uint8_t *s = near_game_code(5 * 16);
    if (!s) { log_line("patch skipped, no memory near the game: script commands in direct mode on every PC"); return; }
    uint8_t *p = s;
    for (int i = 0; i < 5; i++) {
        uint8_t *je = (uint8_t *)site[i].at + site[i].je;
        int32_t rel;
        memcpy(&rel, je + 2, 4);
        uintptr_t next = (uintptr_t)(je + 6), end = next + (intptr_t)rel;
        uint8_t *stub = p;
        const uint8_t sw[5] = {0x80, 0x7C, 0x24, site[i].sw, 0x00};                    // cmp byte [rsp+switch],0
        feed_put(&p, sw, sizeof sw);
        int32_t r = (int32_t)((intptr_t)end - (intptr_t)(p + 6));                       // jne end: queued mode, nothing here
        p[0] = 0x0F; p[1] = 0x85; memcpy(p + 2, &r, 4); p += 6;
        feed_put_jmp(&p, next);                                                         // direct mode: the game's own path
        r = (int32_t)(stub - (je + 6));
        memcpy(je + 2, &r, 4);                                                          // no queue -> the stub (was: -> end)
    }
    log_line("patch applied: script commands in direct mode on every PC (kill_character, create_force, create_agent, hide_character, seek_exchange)");
}
static void patch_lobby_join_guard(void) {
    uint8_t *at = (uint8_t *)0x2cc8968, *call = (uint8_t *)0x2cd2a27;
    static const uint8_t old[10] = {0x49, 0x8B, 0xB7, 0xA0, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x06};   // mov rsi,[r15+1a0h]; mov rax,[rsi]
    int32_t want = (int32_t)(0x2cd3f30 - (0x2cd2a27 + 5));
    if (memcmp(at, old, sizeof old) || call[0] != 0xE8 || memcmp(call + 1, &want, 4)) {
        log_line("patch skipped, game code differs: lobby join guard"); return;
    }
    uint8_t *s = near_game_code(48);
    if (!s) { log_line("patch skipped, no memory near the game: lobby join guard"); return; }
    uint8_t *p = s;
    static const uint8_t body[] = {0x49, 0x8B, 0xB7, 0xA0, 0x01, 0x00, 0x00,   // mov rsi,[r15+1a0h]   (the army's force)
                                   0x48, 0x85, 0xF6,                           // test rsi,rsi
                                   0x75, 0x05};                                // jnz -> has a force
    memcpy(p, body, sizeof body); p += sizeof body;
    int32_t r = (int32_t)(0x2cc89ca - (uintptr_t)(p + 5));                     // no force: leave, nothing to refresh
    *p = 0xE9; memcpy(p + 1, &r, 4); p += 5;
    *p++ = 0x48; *p++ = 0x8B; *p++ = 0x06;                                     // mov rax,[rsi]
    r = (int32_t)(0x2cc8972 - (uintptr_t)(p + 5));
    *p = 0xE9; memcpy(p + 1, &r, 4); p += 5;
    uint8_t *w = p;                                                            // the lobby check: rdx = the lobby (r15 in OnLobbyCreated)
    *p++ = 0x4C; *p++ = 0x89; *p++ = 0xFA;                                     // mov rdx,r15
    *p++ = 0x48; *p++ = 0xB8; { uint64_t fn = (uint64_t)lobby_screen_check; memcpy(p, &fn, 8); p += 8; }   // mov rax,fn
    *p++ = 0xFF; *p++ = 0xE0;                                                  // jmp rax
    r = (int32_t)(s - (at + 5));
    at[0] = 0xE9; memcpy(at + 1, &r, 4); memset(at + 5, 0x90, 5);
    r = (int32_t)(w - (call + 5));
    memcpy(call + 1, &r, 4);
    log_line("patch applied: lobby join guard (no crash when the joining player's box is missing) + lobby screen log");
}

// Battle loading screen with more than 4 armies a side: the layout has icons "mon0".."mon3" per side (main army + 3 allies). After the
// game has filled those (UIDLL::BattleLoadingScreenPanel::SetupAllianceFactionFlags), mon4.. are made as copies of mon1 with the
// game's own TWUI::ComponentTemplateLibrary::CreateFromCopy, get the faction's mon_64.png like the game's own, and the side becomes
// two rows of 4: mon0-3 move up half a row, mon4-7 sit under them. Up to 4 armies a side nothing changes. Display only: UI code.
GAME_FN(lsp_flags_setup, 0x2b5b490, void, void *, void *, const uint8_t *, uint32_t);
GAME_FN(ui_set_texture, 0x293aa00, void, void *, const CaString *, int, int);
GAME_FN(ui_create_from_copy, 0x2939610, uint8_t *, void *, void *, const CaString *, void *, void *, void *, int, int, const void *,
        int, void *);
GAME_FN(ui_set_visible, 0x29168a0, void, void *, int);
GAME_FN(lsui_create_from, 0x2b58f10, uint8_t *, void *, void *, void *, const CaString *);   // LoadingScreenUI::CreateFromComponent
GAME_FN(ui_height, 0x2916bf0, float, void *);
static uint8_t *ui_find(uint8_t *root, const char *name) {
    CaString s;
    ca_string_from_c(&s, name);
    uint8_t *c = component_search(root, &s, 1);
    ca_string_free(&s);
    return c;
}
static uint8_t *ui_parent_of(uint8_t *root, uint8_t *child, int depth) {   // children: count at +114h, array at +118h
    uint32_t n = *(uint32_t *)(root + 0x114);
    uint8_t **kids = *(uint8_t ***)(root + 0x118);
    if (!kids) return NULL;
    for (uint32_t i = 0; i < n; i++) if (kids[i] == child) return root;
    if (depth > 0)
        for (uint32_t i = 0; i < n; i++) { uint8_t *p = kids[i] ? ui_parent_of(kids[i], child, depth - 1) : NULL; if (p) return p; }
    return NULL;
}
static void ui_move(uint8_t *c, float x, float y) {   // MoveToRelative, vtable +b0h; the position relative to the parent is at +120h
    float pos[2] = {x, y};
    SYSV void (*move)(void *, const float *) = (SYSV void (*)(void *, const float *))(*(void ***)c)[0xb0 / 8];
    move(c, pos);
}
// an icon of the loading screen's header: placed through its dock offset when docked, else in its parent; made size x size
static void mon_place(uint8_t *c, float x, float y) {
    if (*(int32_t *)(c + 0x1a0)) {
        float off[2] = {x, y};
        ((SYSV void (*)(void *, const float *))(uintptr_t)0x29235b0)(c, off);   // TWUI::Component::SetDockOffset (re-docks)
    } else ui_move(c, x, y);
}
static void mon_size(uint8_t *c, float size) {
    if (ui_height(c) <= size + 0.5f) return;
    uint8_t was_w = c[0x190], was_h = c[0x191];
    c[0x190] = c[0x191] = 1;
    ((SYSV void (*)(void *, float, float, int, int))(uintptr_t)0x2916c40)(c, size, size, 1, 1);   // TWUI::Component::Resize
    c[0x190] = was_w; c[0x191] = was_h;
}
static SYSV void loading_screen_mons(void *self, uint8_t *panel, const uint8_t *alliance, uint32_t main_army) {
    lsp_flags_setup(self, panel, alliance, main_army);
    if (!panel || !alliance) return;
    uint32_t n = *(const uint32_t *)(alliance + 4);              // armies on this side, array at +8, 2c8h bytes each
    const uint8_t *armies = *(const uint8_t *const *)(alliance + 8);
    if (n <= 4 || !armies) return;
    uint8_t *mon[8];
    char name[8];
    for (int k = 0; k < 8; k++) { snprintf(name, sizeof name, "mon%d", k); mon[k] = ui_find(panel, name); }
    if (!mon[0] || !mon[1] || !mon[2] || !mon[3] || mon[4]) return;   // another layout, or done already
    // The icons are docked (dock point +1a0h, offset +1a4h / +1a8h): the engine places them from that offset every time it
    // docks, so a plain move did not last and the copies (mon1's offset) all landed on mon1 (user's 2026-11-22 picture: 4 a
    // side only). Rows now go through the dock offset; icons 44 high instead of 50 so two rows fit in the header frame.
    float size = ui_height(mon[1]) > 44.0f ? 44.0f : ui_height(mon[1]), half = (size + 2.0f) * 0.5f, rel[4][2];
    for (int k = 0; k < 4; k++) {
        int docked = *(int32_t *)(mon[k] + 0x1a0) != 0;
        rel[k][0] = *(float *)(mon[k] + (docked ? 0x1a4 : 0x120)); rel[k][1] = *(float *)(mon[k] + (docked ? 0x1a8 : 0x124));
    }
    for (int k = 0; k < 4; k++) { mon_size(mon[k], size); mon_place(mon[k], rel[k][0], rel[k][1] - half); }
    void *lib = *(void **)0x5163480;                             // TWUI::ComponentTemplateLibrary::m_LibraryInstance
    int made = 0, slot = 1;                                      // the game's numbering: mon1.. = the other armies in order
    if (!lib && !*(void **)0x5172fc8) log_line("loading screen: no template library");
    log_line("loading screen: panel %p, mon0 %p, mon1 %p, parent found %p, parent link %p, children %u",
             (void *)panel, (void *)mon[0], (void *)mon[1], (void *)ui_parent_of(panel, mon[0], 3), *(void **)(mon[0] + 0x108),
             *(uint32_t *)(panel + 0x114));
    for (uint32_t i = 0; i < n && slot < 8 && (lib || *(void **)0x5172fc8); i++) {
        if (i == main_army) continue;
        int k = slot++;
        if (k < 4) continue;
        uint8_t *parent = ui_parent_of(panel, mon[k - 4], 3);
        if (!parent) parent = *(uint8_t **)(mon[k - 4] + 0x108);   // the component's own parent link
        if (!parent) { log_line("loading screen: no parent for mon%d", k - 4); break; }
        CaString nm;
        snprintf(name, sizeof name, "mon%d", k);
        ca_string_from_c(&nm, name);
        void *lsui = *(void **)0x5172fc8;   // UIDLL::LoadingScreenUI::s_loading_screen_ui: makes copies the way the screen's own code does
        uint8_t *c = lsui ? lsui_create_from(lsui, mon[1], parent, &nm) : ui_create_from_copy(lib, mon[1], &nm, NULL, NULL, parent, 0, 0, NULL, 1, NULL);
        ca_string_free(&nm);
        if (!c) break;
        mon_size(c, size);
        mon_place(c, rel[k - 4][0], rel[k - 4][1] + half);
        ui_set_visible(c, 1);
        const CaString *fs = (const CaString *)(armies + (size_t)i * 0x2c8 + 0x40);   // the faction's flag folder, as the game uses it
        if (fs->text && fs->len && fs->len < 400) {
            char path[512];
            snprintf(path, sizeof path, "%.*s\\mon_64.png", (int)fs->len, fs->text);
            CaString ps;
            ca_string_from_c(&ps, path);
            ui_set_texture(c, &ps, 0, 0);
            ca_string_free(&ps);
        }
        mon[k] = c;
        made++;
    }
    log_line("loading screen: %u armies on a side, %d more icons in a second row", n, made);
}
static void patch_loading_screen_eight(void) {
    static const uintptr_t calls[2] = {0x2b5ab31, 0x2b5ab80};   // BattleLoadingScreenPanel::OnCreate: player side, enemy side
    for (int i = 0; i < 2; i++) {
        uint8_t *c = (uint8_t *)calls[i];
        int32_t want = (int32_t)(0x2b5b490 - (calls[i] + 5));
        if (c[0] != 0xE8 || memcmp(c + 1, &want, 4)) { log_line("patch skipped, game code differs: loading screen 8 a side"); return; }
    }
    uint8_t *s = near_game_code(16);
    if (!s) { log_line("patch skipped, no memory near the game: loading screen 8 a side"); return; }
    s[0] = 0x48; s[1] = 0xB8; { uint64_t fn = (uint64_t)loading_screen_mons; memcpy(s + 2, &fn, 8); }   // mov rax,fn
    s[10] = 0xFF; s[11] = 0xE0;                                                                          // jmp rax
    for (int i = 0; i < 2; i++) {
        uint8_t *c = (uint8_t *)calls[i];
        int32_t r = (int32_t)(s - (c + 5));
        memcpy(c + 1, &r, 4);
    }
    log_line("patch applied: battle loading screen shows up to 8 faction icons a side (two rows)");
}

// ---- 8v8 battle screens (the 64-bit Rome 2 engine's stage 25 idea, for Attila). Display only: UI code, run once when each
// screen is made, after the game's own OnCreate (its vtable entry points to ours).
//  - Battle loading screen (UIDLL::BattleLoadingScreenPanel): in multiplayer the game hides the side boxes and shows the armies
//    as faction icons "mon0".."mon3" a side; mon4..7 (patch_loading_screen_eight) are now made with the screen's own
//    LoadingScreenUI::CreateFromComponent (the template library it used before was empty: no icons 5-8 were ever made).
//  - Results screen after a multiplayer battle (UIDLL::PostBattleLoadingScreenPanel): see results_screen_create.
// TWUI::Component fields used (the engine's own, read from the Linux build): +f8h name, +108h parent, +114h / +118h children,
// +120h / +124h position in the parent, +128h / +12ch position on screen, +140h state (+40h width, +44h height), +190h / +191h
// "may change width / height", +1a0h dock point (0 = none), +1a4h / +1a8h dock offset. ATTILA_NO_8V8_SCREENS=1 = the results
// screen as the game makes it; ATTILA_UI_DUMP=1 writes both screens' component trees to the log; ATTILA_TEST_SHOT_SCREENS=1
// takes a test picture as each screen appears.
GAME_FN(ui_resize_fn, 0x2916c40, void, void *, float, float, int, int);   // TWUI::Component::Resize(w, h, scale children, ...)
GAME_FN(ui_set_dock_offset, 0x29235b0, void, void *, const float *);       // TWUI::Component::SetDockOffset
GAME_FN(ui_is_visible, 0x2914b40, uint8_t, void *, int);                   // TWUI::Component::IsVisible
#define UI_KIDS(c) (*(uint32_t *)((uint8_t *)(c) + 0x114))
#define UI_KID(c, i) ((*(uint8_t ***)((uint8_t *)(c) + 0x118))[i])
#define UI_X(c) (*(float *)((uint8_t *)(c) + 0x128))
#define UI_Y(c) (*(float *)((uint8_t *)(c) + 0x12c))
static float ui_w(uint8_t *c) { uint8_t *s = *(uint8_t **)(c + 0x140); return s ? *(float *)(s + 0x40) : 0.0f; }
static float ui_h(uint8_t *c) { uint8_t *s = *(uint8_t **)(c + 0x140); return s ? *(float *)(s + 0x44) : 0.0f; }
static const char *ui_name(uint8_t *c, char *buf, size_t n) {
    const CaString *s = (const CaString *)(c + 0xf8);
    snprintf(buf, n, "%.*s", s->text && s->len < 80 ? (int)s->len : 0, s->text ? s->text : "");
    return buf;
}
static void ui_dump(uint8_t *c, int depth, int maxdepth) {
    if (!c || depth > maxdepth) return;
    char nm[96];
    log_line("ui %*s%s x %.0f y %.0f w %.0f h %.0f dock %d (%.0f, %.0f) %s kids %u", depth * 2, "", ui_name(c, nm, sizeof nm), UI_X(c),
             UI_Y(c), ui_w(c), ui_h(c), *(int32_t *)(c + 0x1a0), *(float *)(c + 0x1a4), *(float *)(c + 0x1a8),
             ui_is_visible(c, 0) ? "shown" : "hidden", UI_KIDS(c));
    uint32_t n = UI_KIDS(c);
    if (n > 40) return;
    for (uint32_t i = 0; i < n; i++) ui_dump(UI_KID(c, i), depth + 1, maxdepth);
}
// test command "uidump <name>" (ATTILA_TEST_CMD_FILE): the tree of the first interface part with that name, from the root
void test_ui_dump(const char *name) {
    uint8_t *root = *(uint8_t **)0x51632d8;   // TWUI::Component::m_Root
    uint8_t *c = root ? ui_find(root, name) : NULL;
    log_line("test command: uidump %s: %s", name, c ? "found" : "not found");
    if (c) ui_dump(c, 0, 12);
}
static int ui_holds(uint8_t *outer, uint8_t *c) { for (int i = 0; c && i < 64; i++, c = *(uint8_t **)(c + 0x108)) if (c == outer) return 1; return 0; }
// moves a component down by dy: a docked one through its dock offset (the engine places it from that), others in their parent
static void ui_shift(uint8_t *c, float dy) {
    if (*(int32_t *)(c + 0x1a0)) {
        float off[2] = {*(float *)(c + 0x1a4), *(float *)(c + 0x1a8) + dy};
        ui_set_dock_offset(c, off);
    } else ui_move(c, *(float *)(c + 0x120), *(float *)(c + 0x124) + dy);
}
// makes a component taller, its top edge where it was (a centre- or bottom-docked one would grow both ways / upwards)
static void ui_taller(uint8_t *c, float dh) {
    float y0 = UI_Y(c);
    uint8_t was_w = c[0x190], was_h = c[0x191];
    c[0x190] = c[0x191] = 1;
    ui_resize_fn(c, ui_w(c), ui_h(c) + dh, 0, 0);
    c[0x190] = was_w; c[0x191] = was_h;
    if (UI_Y(c) != y0) ui_shift(c, y0 - UI_Y(c));
}
static uint8_t *g_room_keep[4];
static int g_room_nkeep, g_room_moves;
static int room_kept(uint8_t *k) { for (int i = 0; i < g_room_nkeep; i++) if (k == g_room_keep[i]) return 1; return 0; }
static int room_holds_kept(uint8_t *k) { for (int i = 0; i < g_room_nkeep; i++) if (ui_holds(k, g_room_keep[i])) return 1; return 0; }
// Under c: what starts at or below the line moves down by add; what reaches across it (or holds a kept list) grows by add and
// is looked into. before[] = c's children's y before c itself grew (children docked to its bottom moved with it already).
static void room_walk(uint8_t *c, float line, float add, int depth, const float *before) {
    uint32_t n = UI_KIDS(c);
    if (!n || n > 200 || depth > 12) return;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *k = UI_KID(c, i);
        if (!k) continue;
        float y = UI_Y(k), h = ui_h(k);
        if (before) {
            float want = before[i] < line - 2.0f ? before[i] : before[i] + add;
            if (y != want) { ui_shift(k, want - y); y = want; }
            if (before[i] >= line - 2.0f) { g_room_moves++; continue; }
        }
        if (room_kept(k)) continue;                    // the lists themselves: the caller makes them taller
        int holds = room_holds_kept(k), spans = y < line - 2.0f && y + h > line + 2.0f;
        if (y >= line - 2.0f && !holds) { ui_shift(k, add); g_room_moves++; continue; }
        if (!spans && !holds) continue;                // wholly above the line
        float snap[200];
        uint32_t kn = UI_KIDS(k);
        int ns = kn <= 200 ? (int)kn : 0;
        for (int j = 0; j < ns; j++) snap[j] = UI_KID(k, j) ? UI_Y(UI_KID(k, j)) : 0.0f;
        if (spans) { ui_taller(k, add); g_room_moves++; }
        room_walk(k, line, add, depth + 1, spans && ns ? snap : NULL);
    }
}
static int g_ui_dump = -1, g_screens_off, g_screens_shot = -1;
void test_shot_now(void);
static void screens_dump(uint8_t *root, const char *what) {
    if (g_ui_dump < 0) g_ui_dump = getenv("ATTILA_UI_DUMP") != NULL;
    if (g_screens_shot < 0) g_screens_shot = getenv("ATTILA_TEST_SHOT_SCREENS") != NULL;
    if (g_screens_shot && strstr(what, "after")) test_shot_now();   // test picture of the screen as soon as it is drawn
    if (!g_ui_dump || !root) return;
    log_line("ui tree of the %s:", what);
    ui_dump(root, 0, 9);
}
// Battle loading screen: in multiplayer battles the game hides the side boxes and shows each side's armies only as faction
// icons at the top (the two rows of patch_loading_screen_eight); this hook only logs the tree and takes the test picture.
static SYSV void (*g_ls_create_orig)(void *, const void *);
static SYSV void loading_screen_create(void *self, const void *init) {
    g_ls_create_orig(self, init);
    screens_dump(*(uint8_t **)((uint8_t *)self + 8), "battle loading screen (after)");
}
// Results screen (measured, vanilla, 8 v 8, layout units 1440 x 900): "middle_frame" (y 20..683) holds the two sides one above
// the other; each side's "team_clip_box" is 184 high = 4 rows of 46 ("team_entry" + "army_entry" rows placed 46 apart), so
// rows 5-8 of a side were cut off or drawn over the other side. Under it: battle time, then "bottom_parent" with the battle tip
// (y 682..850) and "button_continue" (y 851). Now each clip box grows to fit its rows and everything under it moves down; if
// 8 + 8 rows at full height would not end above the Continue button, the rows get shorter (scaled, at least 26) so they do,
// and the battle tip (not needed on this screen) is hidden where the grown panel covers it. Up to 4 a side: no change.
static int clip_rows(uint8_t *clip, uint8_t **rows, int max) {
    int n = 0;
    uint32_t k = UI_KIDS(clip);
    for (uint32_t i = 0; i < k && k <= 64 && n < max; i++) { uint8_t *r = UI_KID(clip, i); if (r && ui_is_visible(r, 0)) rows[n++] = r; }
    return n;
}
static SYSV void (*g_rs_create_orig)(void *, const void *);
static SYSV void results_screen_create(void *self, const void *init) {
    g_rs_create_orig(self, init);
    uint8_t *root = *(uint8_t **)((uint8_t *)self + 8), *side[2] = {*(uint8_t **)((uint8_t *)self + 0xb8), *(uint8_t **)((uint8_t *)self + 0xc0)};
    screens_dump(root, "results screen (before)");
    if (!root || g_screens_off || !*(void **)((uint8_t *)self + 0xa0)) { screens_dump(root, "results screen (after)"); return; }
    uint8_t *clip[2] = {NULL, NULL}, *rows[2][32];
    int n[2] = {0, 0}, ntot = 0;
    float rh = 0.0f, clip_h = 0.0f;
    for (int s = 0; s < 2; s++) {
        if (!side[s] || !(clip[s] = ui_find(side[s], "team_clip_box"))) continue;
        n[s] = clip_rows(clip[s], rows[s], 32);
        if (!n[s]) { clip[s] = NULL; continue; }
        ntot += n[s]; clip_h += ui_h(clip[s]);
        if (ui_h(rows[s][0]) > rh) rh = ui_h(rows[s][0]);
    }
    float grow46 = 0.0f;
    for (int s = 0; s < 2; s++) if (clip[s] && n[s] * rh + 4.0f > ui_h(clip[s])) grow46 += n[s] * rh + 4.0f - ui_h(clip[s]);
    if (!ntot || rh < 10.0f || grow46 <= 1.0f) { screens_dump(root, "results screen (after)"); return; }
    // the panel that holds both sides (child of the screen-sized "docker") and the lowest line it may reach
    uint8_t *mf = NULL, *docker = NULL;
    for (uint8_t *c = clip[0] ? clip[0] : clip[1]; c; c = *(uint8_t **)(c + 0x108)) {
        uint8_t *p = *(uint8_t **)(c + 0x108);
        if (p && *(uint8_t **)(p + 0x108) == root) { mf = c; docker = p; break; }
    }
    if (!mf) { log_line("results screen: layout not as expected, nothing changed"); return; }
    uint8_t *cont = ui_find(root, "button_continue");
    float limit = (cont ? UI_Y(cont) : UI_Y(docker) + ui_h(docker)) - 6.0f, mf_bottom = UI_Y(mf) + ui_h(mf);
    float r = rh;
    if (mf_bottom + grow46 > limit) {
        int sides = (clip[0] != NULL) + (clip[1] != NULL);
        r = (float)(int)((limit - mf_bottom + clip_h - 4.0f * sides) / ntot);
        if (r > rh) r = rh;
        if (r < 26.0f) r = 26.0f;
    }
    if (r < rh - 0.5f)   // shorter rows: each row and what is in it scaled down, placed r apart
        for (int s = 0; s < 2; s++)
            for (int i = 0; clip[s] && i < n[s]; i++) {
                uint8_t *w = rows[s][i], was_w = w[0x190], was_h = w[0x191];
                w[0x190] = w[0x191] = 1;
                ui_resize_fn(w, ui_w(w), r, 1, 1);
                w[0x190] = was_w; w[0x191] = was_h;
                ui_move(w, *(float *)(w + 0x120), (float)i * r);
            }
    // make room: the lower side first, each clip box with its own line
    g_room_moves = 0;
    int lo = clip[0] && clip[1] ? (UI_Y(clip[0]) > UI_Y(clip[1]) ? 0 : 1) : (clip[0] ? 0 : 1), order[2] = {lo, 1 - lo};
    float total = 0.0f;
    for (int j = 0; j < 2; j++) {
        int s = order[j];
        if (!clip[s]) continue;
        float need = n[s] * r + 4.0f - ui_h(clip[s]);
        if (need <= 1.0f) continue;
        g_room_keep[0] = clip[s]; g_room_nkeep = 1;
        room_walk(docker, UI_Y(clip[s]) + ui_h(clip[s]) - 2.0f, need, 0, NULL);
        ui_taller(clip[s], need);
        total += need;
    }
    // the bottom part (battle tip, loading bar, Continue) stays where the game put it: the walk moved it down with the rest,
    // off the bottom of the screen (user's 2026-11-22 test: loading bar and Continue not seen)
    uint8_t *bottom = ui_find(root, "bottom_parent");
    if (bottom && total > 0.5f) ui_shift(bottom, -total);
    // the battle tip under the panel: hidden where the grown panel now covers it
    float new_bottom = UI_Y(mf) + ui_h(mf);
    uint8_t *tip_parts[2] = {ui_find(root, "panel_parchment"), ui_find(root, "quote")};
    int hid = 0;
    for (int i = 0; i < 2; i++) if (tip_parts[i] && UI_Y(tip_parts[i]) < new_bottom) { ui_set_visible(tip_parts[i], 0); hid++; }
    log_line("results screen: %d + %d army rows, rows %.0f high (game: %.0f), panel %.0f taller, %d parts moved or stretched%s",
             n[0], n[1], r, rh, total, g_room_moves, hid ? ", battle tip hidden" : "");
    screens_dump(root, "results screen (after)");
}
static int vtable_hook(uintptr_t vt, uintptr_t want, void *fn, void **orig, const char *what) {
    uintptr_t *v = (uintptr_t *)vt;
    int at = -1;
    for (int i = 0; i < 80; i++) if (v[i] == want) { if (at >= 0) { at = -2; break; } at = i; }
    if (at < 0) { log_line("patch skipped, game code differs: %s", what); return 0; }
    DWORD was;
    if (!VirtualProtect(&v[at], 8, PAGE_READWRITE, &was)) { log_line("patch skipped, vtable not writable: %s", what); return 0; }
    *orig = (void *)v[at];
    v[at] = (uintptr_t)fn;
    VirtualProtect(&v[at], 8, was, &was);
    return 1;
}
// Unit stats panel (UIDLL::UnitInformationPanelCallback, "Unit Info" in battle set-up and in battle): InitStatistics fills the
// list "dynamic_stats" (+308h) row by row and makes it as high as all rows in ONE column (rows x row height). 1212AD's layout
// (1-1212scripts.pack ui\common ui\unit_information) places stat1..stat8 in two columns 214 apart while each row is 294 wide,
// so the second column lay over the first (user's 2026-11-22 report; measured hidden: list 293 x 240 for 10 rows, rows at
// x +0 / +214). After InitStatistics: if two shown rows overlap, the shown rows go one under the other at the first row's x,
// in their order - one column, as in vanilla Attila. Layouts whose rows do not overlap are left alone. Display only.
static SYSV void (*const g_unit_stats_init)(void *) = (SYSV void (*)(void *))(uintptr_t)0x2a2b6f0;
// Two columns (user's 2026-11-24 wish): melee stats on the left (Melee Attack / Damage / Defence, Charge Bonus, Attack Against
// Cavalry), the rest on the right. A row (stat_name 150 + dy_value 40 + bar_frame 90 = 294) is too wide for half the panel, so
// each stat becomes a cell of two lines: name with its value at the right end (the value is right-aligned), the bar under them
// stretched to the cell's width. The columns use 1.3 x the list's width, centred on it (the parchment around the list has the
// room). The game re-sets bar fill widths (hover / compare) in its own 90-wide scale: we note every width we wrote, and a width
// that differs from ours is the game's and is scaled again. Not two columns (one column, as before) when: a label cannot be
// read, all stats fall in one group, or a column would need cells under 30 high in the list's height. Display only.
typedef struct { void *c; float v; uint32_t used; } UsMark;
static UsMark g_us_mark[1024];
static int g_us_nmark;
static uint32_t g_us_clock;
static float us_abs(float a) { return a < 0 ? -a : a; }
static int us_mark_get(void *c, float *v) {
    for (int i = 0; i < g_us_nmark; i++)
        if (g_us_mark[i].c == c) { g_us_mark[i].used = ++g_us_clock; if (v) *v = g_us_mark[i].v; return 1; }
    return 0;
}
static void us_mark_set(void *c, float v) {
    int at = -1;
    for (int i = 0; i < g_us_nmark && at < 0; i++) if (g_us_mark[i].c == c) at = i;
    if (at < 0 && g_us_nmark < 1024) at = g_us_nmark++;
    if (at < 0) { at = 0; for (int i = 1; i < 1024; i++) if (g_us_mark[i].used < g_us_mark[at].used) at = i; }   // full: the longest unused (old panels)
    g_us_mark[at].c = c; g_us_mark[at].v = v; g_us_mark[at].used = ++g_us_clock;
}
static void us_size(uint8_t *c, float w, float h) {
    if (us_abs(ui_w(c) - w) < 0.5f && us_abs(ui_h(c) - h) < 0.5f) return;
    float x = *(float *)(c + 0x120), y = *(float *)(c + 0x124);
    uint8_t was_w = c[0x190], was_h = c[0x191];
    c[0x190] = c[0x191] = 1;
    ui_resize_fn(c, w, h, 0, 0);
    c[0x190] = was_w; c[0x191] = was_h;
    if (*(float *)(c + 0x120) != x || *(float *)(c + 0x124) != y) ui_move(c, x, y);
}
static void us_place(uint8_t *c, float x, float y) {
    if (us_abs(*(float *)(c + 0x120) - x) > 0.25f || us_abs(*(float *)(c + 0x124) - y) > 0.25f) ui_move(c, x, y);
}
static int us_label_melee(uint8_t *name) {   // 1 melee, 0 other, -1 no text. TWUI::Component::GetStateText = state +50h (UniString)
    uint8_t *st = *(uint8_t **)(name + 0x140);
    if (!st) return -1;
    uint32_t len = *(uint32_t *)(st + 0x50);
    const uint16_t *t = *(const uint16_t *const *)(st + 0x58);
    if (!t || !len || len > 200) return -1;
    char a[201];
    for (uint32_t i = 0; i < len; i++) a[i] = t[i] < 128 ? (char)(t[i] >= 'A' && t[i] <= 'Z' ? t[i] + 32 : t[i]) : '?';
    a[len] = 0;
    return strstr(a, "melee") || strstr(a, "charge") || strstr(a, "cavalry") ? 1 : 0;
}
// a fill / marker inside the bar: the game's value (any value that is not the one we wrote). The game works it out from the
// bar's width at that moment: a value written while the bar was still the game's own width (bar0) is scaled up by k; one written
// after we widened the bar is already in our width (user's 2026-11-24 picture: scaling those again made every bar full).
static void us_scale_w(uint8_t *c, float k, float most, int widened) {   // most: the bar's width (a value over the top end is cut)
    float mine, w = ui_w(c);
    if (us_mark_get(c, &mine) && us_abs(mine - w) < 0.25f) return;
    if (widened) k = 1.0f;
    us_size(c, w * k < most ? w * k : most, ui_h(c));
    us_mark_set(c, ui_w(c));
    static int told;
    if (widened && told++ < 5) log_line("unit stats: the game set a bar fill to %.0f in the widened bar (kept, not scaled)", w);
}
static void us_scale_x(uint8_t *c, float k, int widened) {
    float mine, x = *(float *)(c + 0x120);
    if (us_mark_get(c, &mine) && us_abs(mine - x) < 0.25f) return;
    if (!widened) us_place(c, x * k, *(float *)(c + 0x124));
    us_mark_set(c, *(float *)(c + 0x120));
}
// the room under the list: the top of the nearest shown part below the list's top (in the list's parents, not inside the list),
// e.g. the ability icons; 0 = nothing found
static float us_room_below(uint8_t *list) {
    float top = UI_Y(list), lx = UI_X(list) - ui_w(list) * 0.2f, rx = UI_X(list) + ui_w(list) * 1.2f, best = 0;
    uint8_t *p = list;
    for (int up = 0; up < 3; up++) {
        uint8_t *par = *(uint8_t **)(p + 0x108);
        if (!par) break;
        for (uint32_t i = 0; i < UI_KIDS(par) && i < 200; i++) {
            uint8_t *s = UI_KID(par, i);
            if (!s || s == p || ui_holds(s, list) || !ui_is_visible(s, 0)) continue;
            float sy = UI_Y(s), sh = ui_h(s);
            if (sy <= top + 8.0f || sh < 4.0f || UI_X(s) > rx || UI_X(s) + ui_w(s) < lx) continue;
            if (best == 0 || sy - top < best) best = sy - top;
        }
        p = par;
    }
    return best;
}
static int unit_stats_two_columns(uint8_t *list, uint8_t **rows, int n) {
    if (getenv("ATTILA_STATS_ONE_COLUMN")) return 0;
    static float bar0;   // the game's bar width (90 in 1212AD's layout), read once from a bar we have not touched
    uint8_t *nm[64], *val[64], *bar[64];
    int side[64], cnt[2] = {0, 0};
    for (int i = 0; i < n; i++) {
        nm[i] = ui_find(rows[i], "stat_name"); val[i] = ui_find(rows[i], "dy_value"); bar[i] = ui_find(rows[i], "bar_frame");
        if (!nm[i] || !val[i] || !bar[i]) return 0;
        side[i] = us_label_melee(nm[i]);
        if (side[i] < 0) return 0;
        cnt[side[i] ? 0 : 1]++;
        if (bar0 < 1.0f && !us_mark_get(bar[i], NULL) && ui_w(bar[i]) > 10.0f) bar0 = ui_w(bar[i]);
    }
    if (!cnt[0] || !cnt[1] || bar0 < 1.0f) return 0;
    // balance (user's 2026-11-24 picture: 4 left, 8 right, the last one under the ability icons): while the right column is
    // longer by 2 or more, its last stat goes to the bottom of the left column (melee stats stay at the top on the left)
    int moved = 0;
    while (cnt[1] - cnt[0] >= 2) {
        int last = -1;
        for (int i = 0; i < n; i++) if (side[i] == 0) last = i;
        if (last < 0) break;
        side[last] = 2;   // 2 = other stat shown on the left, after the melee ones
        cnt[1]--; cnt[0]++; moved++;
    }
    float lw = ui_w(list), lh = ui_h(list), gap = 14.0f, room = us_room_below(list);
    if (room > 30.0f && room - 4.0f < lh) lh = room - 4.0f;   // never down into the parts under the list
    float row_h = ui_h(val[0]) > 10.0f ? ui_h(val[0]) : 24.0f, bar_h = ui_h(bar[0]) > 2.0f ? ui_h(bar[0]) : 10.0f;
    float cell = row_h + bar_h + 4.0f, most = (float)(cnt[0] > cnt[1] ? cnt[0] : cnt[1]);
    if (most * cell > lh + 0.5f) cell = lh / most;
    if (cell < 28.0f || lw < 100.0f) return 0;
    float col = (lw * 1.3f - gap) * 0.5f, left = -lw * 0.15f, bar_y = row_h - 2.0f, k = col / bar0;
    if (cell < row_h + bar_h) bar_y = cell - bar_h - 1.0f;
    float y[2] = {0, 0};
    for (int pass = 0; pass < 3; pass++)   // left: melee (1), then moved others (2); right: others (0)
        for (int i = 0; i < n; i++) {
            if (side[i] != (pass == 0 ? 1 : pass == 1 ? 2 : 0)) continue;
            int c = pass == 2 ? 1 : 0;
            int widened = us_mark_get(bar[i], NULL) && us_abs(ui_w(bar[i]) - col) < 0.5f;   // the bar was already ours
            us_size(rows[i], col, cell);
            us_place(rows[i], left + c * (col + gap), y[c]);
            y[c] += cell;
            us_mark_set(rows[i], col);
            us_size(nm[i], col, row_h);              us_place(nm[i], 0, 0);
            us_size(val[i], ui_w(val[i]), row_h);    us_place(val[i], col - ui_w(val[i]), 0);
            us_size(bar[i], col, bar_h);             us_place(bar[i], 0, bar_y);
            us_mark_set(bar[i], col);
            for (uint32_t j = 0; j < UI_KIDS(bar[i]) && j < 16; j++) {
                uint8_t *b = UI_KID(bar[i], j);
                char kn[96];
                ui_name(b, kn, sizeof kn);
                if (!strcmp(kn, "frame") || !strcmp(kn, "bar_border")) us_size(b, col, ui_h(b));
                else if (!strcmp(kn, "icon_compare")) us_scale_x(b, k, widened);
                else us_scale_w(b, k, col, widened);     // bar_base, bar_mod: the fill
            }
        }
    static int told;
    if (told++ < 3) log_line("unit stats: two columns, %d left (%d of them not melee), %d right (cells %.0f x %.0f, list %.0f x %.0f, room below %.0f)",
                             cnt[0], moved, cnt[1], col, cell, lw, ui_h(list), room);
    return 1;
}
static void unit_stats_one_column(void *self) {
    uint8_t *list = *(uint8_t **)((uint8_t *)self + 0x308), *rows[64];
    if (!list) return;
    int n = 0, overlap = 0;
    uint32_t k = UI_KIDS(list);
    for (uint32_t i = 0; i < k && k <= 64; i++) { uint8_t *r = UI_KID(list, i); if (r && ui_is_visible(r, 0)) rows[n++] = r; }
    for (int i = 0; i < n && !overlap; i++)
        for (int j = i + 1; j < n; j++) {
            uint8_t *a = rows[i], *b = rows[j];
            if (UI_X(a) < UI_X(b) + ui_w(b) - 1.0f && UI_X(b) < UI_X(a) + ui_w(a) - 1.0f && UI_Y(a) < UI_Y(b) + ui_h(b) - 1.0f &&
                UI_Y(b) < UI_Y(a) + ui_h(a) - 1.0f) { overlap = 1; break; }
        }
    if (!overlap && !us_mark_get(rows[0], NULL)) return;   // our own two columns: run again so bars the game re-set get our width
    if (unit_stats_two_columns(list, rows, n)) return;
    if (!overlap) return;
    float x0 = *(float *)(rows[0] + 0x120), y0 = *(float *)(rows[0] + 0x124), y = y0;
    for (int i = 0; i < n; i++) { ui_move(rows[i], x0, y); y += ui_h(rows[i]); }
    static int told;
    if (told++ < 3) log_line("unit stats: %d rows overlapped (two columns narrower than a row): put in one column", n);
}
static SYSV void unit_stats_init(void *self) {
    g_unit_stats_init(self);
    unit_stats_one_column(self);
}
// Hovering a unit card re-places the rows by another way (user's 2026-11-22 report: two columns again on hover), so the
// check also runs after the panel's OnUpdatePulse (vtable 3b0c2b0); it only moves rows that overlap.
static SYSV void (*g_unit_stats_pulse_orig)(void *, uint32_t);
static SYSV void unit_stats_pulse(void *self, uint32_t t) {
    g_unit_stats_pulse_orig(self, t);
    unit_stats_one_column(self);
}
static void patch_unit_stats_column(void) {
    static const uintptr_t calls[2] = {0x2a2a4b4, 0x2a2d5c7};   // UnitInformationPanelCallback::InitialisePanel, ::RefreshStats
    for (int i = 0; i < 2; i++) {
        uint8_t *c = (uint8_t *)calls[i];
        int32_t want = (int32_t)(0x2a2b6f0 - (calls[i] + 5));
        if (c[0] != 0xE8 || memcmp(c + 1, &want, 4)) { log_line("patch skipped, game code differs: unit stats in one column"); return; }
    }
    uint8_t *s = near_game_code(16);
    if (!s) { log_line("patch skipped, no memory near the game: unit stats in one column"); return; }
    s[0] = 0x48; s[1] = 0xB8; { uint64_t fn = (uint64_t)unit_stats_init; memcpy(s + 2, &fn, 8); }   // mov rax,fn
    s[10] = 0xFF; s[11] = 0xE0;                                                                       // jmp rax
    for (int i = 0; i < 2; i++) { uint8_t *c = (uint8_t *)calls[i]; int32_t r = (int32_t)(s - (c + 5)); memcpy(c + 1, &r, 4); }
    vtable_hook(0x3b0c2b0, 0x2a2dcd0, (void *)unit_stats_pulse, (void **)&g_unit_stats_pulse_orig, "unit stats in one column (pulse)");
    log_line("patch applied: unit stats panel rows that overlap go in one column");
}
static void patch_battle_screens_room(void) {
    if (getenv("ATTILA_NO_8V8")) return;
    g_screens_off = getenv("ATTILA_NO_8V8_SCREENS") != NULL;   // the screens' own look (with ATTILA_UI_DUMP their trees still go to the log)
    if (g_screens_off) log_line("8v8 battle screens: off (ATTILA_NO_8V8_SCREENS)");
    int a = vtable_hook(0x3b36f60, 0x2b5a740, (void *)loading_screen_create, (void **)&g_ls_create_orig, "battle loading screen room for 8 a side");
    int b = vtable_hook(0x3b37460, 0x2b5c6e0, (void *)results_screen_create, (void **)&g_rs_create_orig, "results screen room for 8 a side");
    if (a || b) log_line("patch applied: battle loading screen%s%s make room for 8 armies a side", a && b ? " and results screen" : "", a && b ? "" : " (one of them only)");
}

// Battle loading screen: each side shows its armies' faction icons in "mon0".."mon3" only, the game's own 4 a side. With more
// armies UIDLL::BattleLoadingScreenPanel::SetupAllianceFactionFlags also asks for "mon4" and on, gets no component back and
// SetComponentTexture crashes on it (seen in Test 13, 8 attackers). Skip an icon that has no place. Display only: UI code.
static void patch_loading_screen_icons(void) {
    uint8_t *at = (uint8_t *)0x2b5b6f6;
    static const uint8_t old[20] = {0x44, 0x8B, 0x7C, 0x24, 0x14,  0x31, 0xD2,  0x31, 0xC9,  0x48, 0x89, 0xC7,  0x48, 0x89, 0xEE,
                                    0xE8, 0xF6, 0xF2, 0xDD, 0xFF};   // mov r15d,[rsp+14h]; xor edx,edx; xor ecx,ecx;
                                                                     // mov rdi,rax; mov rsi,rbp; call SetComponentTexture
    if (memcmp(at, old, sizeof old)) { log_line("patch skipped, game code differs: loading screen icons"); return; }
    uint8_t *s = near_game_code(40);
    if (!s) { log_line("patch skipped, no memory near the game: loading screen icons"); return; }
    static const uint8_t body[] = {
        0x44, 0x8B, 0x7C, 0x24, 0x14,   // mov r15d,[rsp+14h]   (the replaced instruction)
        0x48, 0x85, 0xC0,               // test rax,rax         (the icon component found?)
        0x74, 0x0F,                     // jz -> past the call
        0x31, 0xD2, 0x31, 0xC9,         // xor edx,edx; xor ecx,ecx
        0x48, 0x89, 0xC7,               // mov rdi,rax
        0x48, 0x89, 0xEE};              // mov rsi,rbp
    uint8_t *p = s;
    memcpy(p, body, sizeof body); p += sizeof body;
    int32_t r = (int32_t)((uint8_t *)0x293aa00 - (p + 5));   // call TWUI::ComponentUtilities::SetComponentTexture
    *p = 0xE8; memcpy(p + 1, &r, 4); p += 5;
    r = (int32_t)((at + 20) - (p + 5));                      // jmp back after the original call
    *p = 0xE9; memcpy(p + 1, &r, 4);
    r = (int32_t)(s - (at + 5));
    at[0] = 0xE9; memcpy(at + 1, &r, 4);
    log_line("patch applied: battle loading screen skips faction icons it has no place for");
    patch_loading_screen_eight();
    patch_battle_screens_room();
    patch_unit_stats_column();
}

