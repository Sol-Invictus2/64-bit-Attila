// battle_choice.inc - campaign battles in multiplayer games with 3 or more human players: the choice of the players who are not
// in the battle.
//
// How the game does it (read in the code, 2026-11-15; EMPIRECAMPAIGN::PENDING_BATTLE::PRE_BATTLE_VOTING_SYSTEM at
// PENDING_BATTLE+130h): every faction in the battle's ready list gets a vote (FACTION_VOTE, 28h bytes: +0 faction key, +10h vote
// 0 none / 1 fight / 2 auto-resolve / 3 watch / 4 play as the AI / 5 withdraw, +15h "a player's own army", +18h/+1Ch timer).
// When exactly one human faction is in the battle, PENDING_BATTLE::handle_human_playing_as_ai (18b4e10) adds the AI side as one
// more voter for "the other player" (watch, or play the AI army, if the lobby allows). Every human who is not in the battle is
// mapped to that one entry (find_pending_battle_faction_for_player_faction_including_play_as_ai 18aff80) - built for 2 players.
// With 3+ players all of them share it: the first click decides for everyone. Its default when its 60 s run out
// (default_vote 18b1020, and the same order in update 18b0b30) is auto-resolve whenever auto-resolve is allowed, and with the
// lobby's "battle disagreement: auto-resolve" setting (CAMPAIGN_SETUP+40h, on in the testers' game) one auto-resolve vote
// turns the whole battle into an auto-resolve (update: a split vote takes setup+40h). Tester report 2026-11-15 (4 players,
// vanilla grand campaign): "when a player starts a battle, the other players' choice is automatically set not to watch the
// battle but to auto-resolve".
//
// Fix (one rule, in the one function every caller asks: PRE_BATTLE_VOTING_SYSTEM::faction_can_autoresolve 18b1d40, used by the
// timeout default, update() and the pre-battle window's buttons): in a campaign with 3 or more human factions, a player who is
// not in the battle is not offered auto-resolve while they can watch the battle or play the AI army. Their default is then
// "watch" (or "play the AI army"), and only the players who fight decide whether the battle is fought or auto-resolved.
// Campaigns with 2 human players keep the game's own rule. Everything it reads is campaign state (the factions' human flags,
// the votes, the lobby settings), the same on every PC. Test switch ATTILA_BATTLE_CHOICE_OLD=1 = the game's rule.
//
// TEST ONLY (ATTILA_BATTLE_TRACE=1): every voter added, every vote, the final decision and each PC's pre-battle window.

typedef SYSV uint8_t (*BcFactionFn)(void *vs, const void *faction);
static BcFactionFn g_bc_can_autoresolve;
#define BC_CAN_SPECTATE ((BcFactionFn)(uintptr_t)0x18b1a70)
#define BC_CAN_PLAY_AS_AI ((BcFactionFn)(uintptr_t)0x18b17a0)
#define BC_CAN_FIGHT ((BcFactionFn)(uintptr_t)0x18b1b60)
#define BC_IN_VOTE_LIST ((BcFactionFn)(uintptr_t)0x18b0950)
#define BC_OWN_ARMY ((BcFactionFn)(uintptr_t)0x18b27e0)   // faction_is_playing_as_actual_human: the vote entry's +15h
static int g_bc_trace;

// human factions in the campaign (FACTION+970h set), from the world's faction list (WORLD+84h count, +88h FACTION*[])
static int bc_humans(void *vs) {
    uint8_t *pb = vs ? *(uint8_t **)vs : NULL;
    if (!pb) return 0;
    uint8_t *model = ((uint8_t *(SYSV *)(void *))(uintptr_t)0x1108bd0)(pb + 0x20);   // CAMPAIGN_ENV_MODEL_ACCESS::campaign_model() const
    uint8_t *world = model ? *(uint8_t **)(model + 0x2d30) : NULL;
    if (!world) return 0;
    uint32_t n = *(uint32_t *)(world + 0x84);
    uint8_t **f = *(uint8_t ***)(world + 0x88);
    int h = 0;
    for (uint32_t i = 0; f && i < n; i++) if (f[i] && f[i][0x970]) h++;
    return h;
}

static SYSV uint8_t bc_can_autoresolve(void *vs, const void *faction) {
    uint8_t r = g_bc_can_autoresolve(vs, faction);
    if (!r || !faction || bc_humans(vs) < 3) return r;           // 2 players: the game's own rule
    if (!BC_IN_VOTE_LIST(vs, faction) || BC_OWN_ARMY(vs, faction)) return r;   // the players who fight keep auto-resolve
    if (BC_CAN_SPECTATE(vs, faction) || BC_CAN_PLAY_AS_AI(vs, faction)) {
        static volatile LONG told;
        if (g_bc_trace && InterlockedIncrement(&told) <= 6) { char k[64]; log_line("battle trace: %s is not in the battle: auto-resolve not offered (3+ players)", tt_key(faction, k, sizeof k)); }
        return 0;
    }
    return r;
}

// ---- trace (test only)
typedef SYSV void (*BcVotesFn)(void *vs, const void *faction, int vote, uint8_t flag);
typedef SYSV void (*BcReadyFn)(void *pb, const void *faction, uint8_t ready, uint8_t autoresolve, uint8_t c);
typedef SYSV void (*BcPresenceFn)(void *pb, const void *character, uint8_t for_other_players);
typedef SYSV void (*BcVsFn)(void *vs);
typedef SYSV void (*BcPopupFn)(void *popup);
typedef SYSV int (*BcDefaultFn)(void *vs, const void *faction);
static BcVotesFn g_bc_votes;
static BcReadyFn g_bc_ready;
static BcPresenceFn g_bc_presence;
static BcVsFn g_bc_build;
static BcPopupFn g_bc_panel;
static const char *bc_vote_name(int v) {
    static const char *n[] = {"none", "fight", "auto-resolve", "watch", "play the AI army", "withdraw"};
    return v >= 0 && v <= 5 ? n[v] : "?";
}
static void bc_dump(void *vs, const char *when) {
    uint32_t n = *(uint32_t *)((uint8_t *)vs + 0xc);
    uint8_t *e = *(uint8_t **)((uint8_t *)vs + 0x10);
    char line[600];
    int p = snprintf(line, sizeof line, "battle trace: %s, %u voters (humans %d):", when, n, bc_humans(vs));
    for (uint32_t i = 0; e && i < n && i < 8 && p < (int)sizeof line - 100; i++) {
        const CaString *k = (const CaString *)(e + i * 0x28);
        p += snprintf(line + p, sizeof line - p, " [%.*s: %s%s, timer %.1f/%d]", k->text ? (int)k->len : 1, k->text ? k->text : "?",
                      bc_vote_name(*(int *)(e + i * 0x28 + 0x10)), e[i * 0x28 + 0x15] ? "" : " (for the players not in the battle)",
                      *(float *)(e + i * 0x28 + 0x18), *(int *)(e + i * 0x28 + 0x1c));
    }
    log_line("%s", line);
}
static SYSV void bc_votes(void *vs, const void *faction, int vote, uint8_t flag) {
    char k[64];
    log_line("battle trace: vote from %s: %s", faction ? tt_key(faction, k, sizeof k) : "?", bc_vote_name(vote));
    g_bc_votes(vs, faction, vote, flag);
    bc_dump(vs, "after the vote");
}
static SYSV void bc_ready(void *pb, const void *faction, uint8_t ready, uint8_t autoresolve, uint8_t c) {
    char k[64];
    log_line("battle trace: decision for %s: ready %d, %s", faction ? tt_key(faction, k, sizeof k) : "?", ready, autoresolve ? "AUTO-RESOLVE" : "fought on the battle map");
    g_bc_ready(pb, faction, ready, autoresolve, c);
}
static SYSV void bc_presence(void *pb, const void *character, uint8_t for_others) {
    char k[64];
    const void *faction = character ? **(void ***)((const uint8_t *)character + 0x2e0) : NULL;
    uint32_t before = *(uint32_t *)((uint8_t *)pb + 0x9c);
    g_bc_presence(pb, character, for_others);
    if (*(uint32_t *)((uint8_t *)pb + 0x9c) != before)
        log_line("battle trace: voter added: %s%s", faction ? tt_key(faction, k, sizeof k) : "?", for_others ? " (AI side, for the players not in the battle)" : " (a player's own army)");
}
static SYSV void bc_build(void *vs) { g_bc_build(vs); bc_dump(vs, "vote list built"); }
static SYSV void bc_panel(void *popup) {
    g_bc_panel(popup);
    uint8_t *u = (uint8_t *)popup;
    void *pb = *(void **)(u + 0x208);
    const void *faction = *(const void **)(u + 0x210);
    void *shown = *(void **)(u + 0x2b8);
    const char *which = shown == *(void **)(u + 0x2b0) ? "players-not-in-the-battle window" : shown == *(void **)(u + 0x2a8) ? "window B (own army)" :
                        shown == *(void **)(u + 0x2a0) ? "window A (own army)" : "other window";
    char k[64];
    if (!pb || !faction) { log_line("battle trace: pre-battle window: %s (no battle or faction)", which); return; }
    void *vs = (uint8_t *)pb + 0x130;
    log_line("battle trace: pre-battle window here for %s: %s; offered: fight %d, auto-resolve %d, watch %d, play the AI army %d; default %s",
             tt_key(faction, k, sizeof k), which, BC_CAN_FIGHT(vs, faction), ((BcFactionFn)(uintptr_t)0x18b1d40)(vs, faction), BC_CAN_SPECTATE(vs, faction),
             BC_CAN_PLAY_AS_AI(vs, faction), bc_vote_name(((BcDefaultFn)(uintptr_t)0x18b1020)(vs, faction)));
}

static void patch_battle_choice(void) {
    static const uint8_t ar_old[7] = {0x41, 0x56, 0x53, 0x50, 0x49, 0x89, 0xF6};   // faction_can_autoresolve 18b1d40: push r14; push rbx; push rax; mov r14,rsi
    g_bc_trace = getenv("ATTILA_BATTLE_TRACE") != NULL;
    if (getenv("ATTILA_BATTLE_CHOICE_OLD")) log_line("test: battle choice of the players not in the battle as in the game (ATTILA_BATTLE_CHOICE_OLD)");
    else {
        g_bc_can_autoresolve = (BcFactionFn)hook_entry(0x18b1d40, ar_old, 7, (void *)bc_can_autoresolve);
        if (g_bc_can_autoresolve) log_line("patch applied: campaign battles with 3+ players: the players not in the battle watch by default and cannot auto-resolve it");
        else log_line("patch skipped, game code differs: campaign battle choice of the players not in the battle");
    }
    if (!g_bc_trace) return;
    static const uint8_t push5[5] = {0x55, 0x41, 0x57, 0x41, 0x56};   // push rbp; push r15; push r14 (all five functions)
    g_bc_votes = (BcVotesFn)hook_entry(0x18b00a0, push5, 5, (void *)bc_votes);              // PRE_BATTLE_VOTING_SYSTEM::faction_votes
    g_bc_ready = (BcReadyFn)hook_entry(0x18b16a0, push5, 5, (void *)bc_ready);              // PENDING_BATTLE::set_ready_to_start
    g_bc_presence = (BcPresenceFn)hook_entry(0x18bf150, push5, 5, (void *)bc_presence);     // PENDING_BATTLE::handle_faction_presence
    g_bc_build = (BcVsFn)hook_entry(0x18afc00, push5, 5, (void *)bc_build);                 // PRE_BATTLE_VOTING_SYSTEM::build_vote_list
    g_bc_panel = (BcPopupFn)hook_entry(0x2b1c370, push5, 5, (void *)bc_panel);              // UIDLL::PopupPreBattle::SetButtonPanel
    log_line("test: battle trace (ATTILA_BATTLE_TRACE): votes %s, decisions %s, voters %s, vote list %s, pre-battle window %s",
             g_bc_votes ? "on" : "NOT hooked", g_bc_ready ? "on" : "NOT hooked", g_bc_presence ? "on" : "NOT hooked",
             g_bc_build ? "on" : "NOT hooked", g_bc_panel ? "on" : "NOT hooked");
}
