// frontend_extras.inc - two things for the game's front end (main menu and lobbies). Included in loader.c.
//
// 1. The map picture of the multiplayer campaign lobby. UIDLL::MPCampaignLobby::OnLobbyCreated (2ceaad0) builds the picture's
//    name as "campaign_maps/<map folder>/<campaign>_map_minimap.png" (format at 3b899cf) and puts it on the lobby's map box
//    with TWUI::ComponentUtilities::SetComponentTexture (293aa00), which gives 0 when the picture cannot be loaded; the box then
//    keeps the picture its screen file names. A mod whose map pictures are named another way got a white map with only the
//    factions' colours on it: The Dawnless Days names its picture after its map folder
//    (campaign_maps/rom_third_age_map/rom_third_age_map_minimap.png) and puts it into its own lobby screen file, but
//    64-bit Attila shows its own 8-player lobby screen (attila_8player_lobby.pack, made from the game's screen, which names no
//    map picture). 1212AD and The Dawnless Days both bring a lobby screen of their own; ours stays in front so 8-player
//    campaigns keep working. User report 2026-11-16 ("the campaign doesn't show for dawnless days" = the white map).
//    Fix: when the game's picture cannot be loaded, the lobby takes the map folder's own minimap,
//    "campaign_maps/<folder>/<folder>_minimap.png", then "ui/skins/default/<folder>_minimap.png". The game's own picture is
//    used whenever it exists, so normal Attila and 1212AD look as before. Only the lobby's call (2ceac9e) is redirected; the
//    picture is screen only, nothing of the game's state. Test switch ATTILA_LOBBY_MAP_OLD=1 = the game's way.
//
// 2. Which front-end screen is showing, for the Discord button (menu_button.c). UIDLL::MainMenuCallbackComponent::notify(
//    FrontendScreenChange) (29aa3c0) is told of every screen change; the event points to the new screen's component, whose
//    name is the CA::String at +F8h ("main" = the main menu).

typedef SYSV uint64_t (*FeSetTexFn)(void *component, const void *path, int index, uint8_t flag);
#define FE_SET_COMPONENT_TEXTURE ((FeSetTexFn)(uintptr_t)0x293aa00)
#define FE_STRING_FROM_TEXT ((SYSV void (*)(void *, const char *))(uintptr_t)0x512db0)   // CA::String::String(char const*)
#define FE_STRING_FREE ((SYSV void (*)(void *))(uintptr_t)0x512d00)                       // CA::String::~String()

// CA::String: 4 bytes length, 4 bytes room, then the text pointer
static const char *fe_text(const void *s) { return s ? *(const char *const *)((const uint8_t *)s + 8) : NULL; }

static SYSV uint64_t fe_lobby_map(void *component, const void *path, int index, uint8_t flag) {
    uint64_t r = FE_SET_COMPONENT_TEXTURE(component, path, index, flag);
    const char *p = fe_text(path);
    static int told;
    if (r || !p || !*p) { if (told++ < 4) log_line("lobby map: %.120s %s", p ? p : "?", r ? "shown" : "(no name)"); return r; }
    const char *a = strchr(p, '/'), *b = a ? strchr(a + 1, '/') : NULL;   // campaign_maps/<folder>/<file>
    char folder[96], tries[2][256];
    if (!b || b - a - 1 <= 0 || (size_t)(b - a - 1) >= sizeof folder) { if (told++ < 8) log_line("lobby map: %.120s cannot be loaded", p); return r; }
    memcpy(folder, a + 1, (size_t)(b - a - 1));
    folder[b - a - 1] = 0;
    snprintf(tries[0], sizeof tries[0], "%.*s/%s_minimap.png", (int)(b - p), p, folder);
    snprintf(tries[1], sizeof tries[1], "ui/skins/default/%s_minimap.png", folder);
    for (int i = 0; i < 2 && !r; i++) {
        uint64_t s[2] = {0, 0};
        FE_STRING_FROM_TEXT(s, tries[i]);
        r = FE_SET_COMPONENT_TEXTURE(component, s, index, flag);
        FE_STRING_FREE(s);
        if (told++ < 8) log_line("lobby map: %.120s cannot be loaded; %s %s", p, tries[i], r ? "shown instead" : "not there either");
    }
    return r;
}

static void patch_lobby_map_picture(void) {
    // 2ceac92: lea rsi,[rsp+1E8h] (the built name); xor edx,edx; xor ecx,ecx; call SetComponentTexture
    static const uint8_t old[17] = {0x48, 0x8D, 0xB4, 0x24, 0xE8, 0x01, 0x00, 0x00, 0x31, 0xD2, 0x31, 0xC9, 0xE8, 0x5D, 0xFD, 0xC4, 0xFF};
    uint8_t *at = (uint8_t *)(uintptr_t)0x2ceac92;
    if (getenv("ATTILA_LOBBY_MAP_OLD")) { log_line("test: lobby map picture as in the game (ATTILA_LOBBY_MAP_OLD)"); return; }
    if (memcmp(at, old, sizeof old)) { log_line("patch skipped, game code differs: lobby map picture"); return; }
    uint8_t *stub = near_game_code(16);
    if (!stub) { log_line("patch skipped, no memory near the game: lobby map picture"); return; }
    uint64_t fn = (uint64_t)(uintptr_t)fe_lobby_map;
    stub[0] = 0x48; stub[1] = 0xB8; memcpy(stub + 2, &fn, 8);   // mov rax, fe_lobby_map
    stub[10] = 0xFF; stub[11] = 0xE0;                           // jmp rax (the game's call already pushed its return)
    int32_t rel = (int32_t)(stub - (at + 17));
    memcpy(at + 13, &rel, 4);
    log_line("patch applied: lobby map picture: the map folder's own minimap when the game's name for it does not exist");
}

// ---- 2. the screen in front (menu_button.c reads g_menu_main)
volatile int g_menu_main;
typedef SYSV void (*FeScreenFn)(void *self, void *const *event);
static FeScreenFn g_fe_screen_orig;
static SYSV void fe_screen_change(void *self, void *const *event) {
    const uint8_t *comp = event ? (const uint8_t *)event[0] : NULL;
    const char *name = comp ? fe_text(comp + 0xf8) : NULL;
    g_menu_main = name && !strcmp(name, "main");
    static int told;
    if (told++ < 16) log_line("front end screen: %.40s", name ? name : "?");
    g_fe_screen_orig(self, event);
}
static void patch_menu_screen_watch(void) {
    static const uint8_t old[7] = {0x41, 0x56, 0x53, 0x48, 0x83, 0xEC, 0x38};   // 29aa3c0: push r14; push rbx; sub rsp,38h
    g_fe_screen_orig = (FeScreenFn)hook_entry(0x29aa3c0, old, 7, (void *)fe_screen_change);
    if (g_fe_screen_orig) log_line("patch applied: main menu watch (the Discord button shows on the main menu only)");
    else log_line("patch skipped, game code differs: main menu watch (no Discord button)");
}
