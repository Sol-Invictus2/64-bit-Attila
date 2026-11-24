// Real Steam for the Linux game code, through the Windows steam_api64.dll.
// The game holds a proxy object for each Steam interface. Each proxy method swaps in the real object and converts the call.
// Differences handled: Windows puts overloaded methods in reverse order; returned CSteamID values come back through a hidden
// pointer; decimal arguments sit in other registers; Steam's callback objects are called the Windows way.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "loader.h"

void *standin_steam_object(int index);
extern void sysv_to_ms_common(void);

// Same as the common bridge, for methods returning a class: this, hidden result pointer, then up to 4 arguments.
// Windows (MSVC) member functions return every class or struct through a hidden pointer, whatever its size; Linux returns one
// of up to 16 bytes in rax (first 8 bytes) and rdx (the rest). So the result is read back as 16 bytes: 0x30 -> rax, 0x38 -> rdx.
__asm__(".text\n"
        ".globl sysv_to_ms_retclass\n"
        "sysv_to_ms_retclass:\n"
        "push %rbp\n"
        "mov %rsp, %rbp\n"
        "sub $0x40, %rsp\n"
        "movq $0, 0x30(%rsp)\n"
        "movq $0, 0x38(%rsp)\n"
        "mov %rcx, 0x20(%rsp)\n"
        "mov %r8, 0x28(%rsp)\n"
        "mov %rdx, %r9\n"
        "mov %rsi, %r8\n"
        "lea 0x30(%rsp), %rdx\n"
        "mov %rdi, %rcx\n"
        "call *%rax\n"
        "mov 0x30(%rsp), %rax\n"
        "mov 0x38(%rsp), %rdx\n"
        "leave\n"
        "ret\n");
extern void sysv_to_ms_retclass(void);

static HMODULE g_dll;
static void *g_client;
static int32_t g_user, g_pipe;

static FARPROC steam_fn(const char *name) {
    if (!g_dll) {
        g_dll = LoadLibraryA("steam_api64.dll");
        if (!g_dll) fatal("steam_api64.dll not found next to attila64.exe");
    }
    return GetProcAddress(g_dll, name);
}

// float arguments: Windows passes them by position, Linux in the first free decimal register
typedef uint8_t (*set_stat_f)(void *, const char *, float);
typedef uint8_t (*avg_rate)(void *, const char *, float, double);
static set_stat_f r_set_stat_float;
static avg_rate r_avg_rate;
SYSV static uint8_t w_set_stat_float(void *o, const char *n, float v) { return r_set_stat_float(o, n, v); }
SYSV static uint8_t w_avg_rate(void *o, const char *n, float a, double b) { return r_avg_rate(o, n, a, b); }

// Cloud writes stay off until saves are checked: this untested build must not change the user's Steam Cloud files.
SYSV static long long w_cloud_blocked(void *o) {
    static volatile LONG shown;
    if (InterlockedIncrement(&shown) <= 20) log_line("Steam: a cloud write or delete was blocked");
    return 0;
}

// Lobby search and advertising are logged, to compare what two players send and look for.
typedef void (*filter_str_fn)(void *, const char *, const char *, int);
typedef void (*filter_num_fn)(void *, const char *, int, int);
typedef void (*filter_int_fn)(void *, int);
typedef uint8_t (*set_data_fn)(void *, uint64_t, const char *, const char *);
typedef uint64_t (*lobby_create_fn)(void *, int, int);
typedef uint64_t (*lobby_join_fn)(void *, uint64_t);
static filter_str_fn r_filter_str;
static filter_num_fn r_filter_num;
static filter_int_fn r_filter_distance, r_filter_count;
static set_data_fn r_set_data;
static lobby_create_fn r_create;
static lobby_join_fn r_join;
SYSV static void w_filter_str(void *o, const char *k, const char *v, int cmp) { log_line("lobby search: text '%s' %d '%s'", k, cmp, v); r_filter_str(o, k, v, cmp); }
SYSV static void w_filter_num(void *o, const char *k, int v, int cmp) { log_line("lobby search: number '%s' %d %d", k, cmp, v); r_filter_num(o, k, v, cmp); }
SYSV static void w_filter_distance(void *o, int d) { log_line("lobby search: distance %d", d); r_filter_distance(o, d); }
SYSV static void w_filter_count(void *o, int n) { log_line("lobby search: at most %d results", n); r_filter_count(o, n); }
SYSV static uint8_t w_set_data(void *o, uint64_t id, const char *k, const char *v) { log_line("lobby advert: %llx '%s' = '%s'", (unsigned long long)id, k, v); return r_set_data(o, id, k, v); }
// ATTILA_TEST_NO_LOBBY=1 (set by the unattended test scripts): no lobby is made or joined, so a test click that lands on the
// multiplayer screen cannot walk into other players' games. 0 = Steam's "no call" answer.
static int no_lobby(const char *what) { if (!getenv("ATTILA_TEST_NO_LOBBY") || getenv("ATTILA_TEST_HOST")) return 0;   /* ATTILA_TEST_HOST=1: a test that hosts on purpose (hosting = the game joins the lobby it just made) */ log_line("lobby %s REFUSED: ATTILA_TEST_NO_LOBBY", what); return 1; }
SYSV static uint64_t w_create(void *o, int type, int max) { log_line("lobby create: type %d, max %d members", type, max); return no_lobby("create") ? 0 : r_create(o, type, max); }
SYSV static uint64_t w_join(void *o, uint64_t id) { log_line("lobby join asked: %llx", (unsigned long long)id); return no_lobby("join") ? 0 : r_join(o, id); }

// Login ticket steps, passed on unchanged and logged. Tickets and full IDs stay out of the log: players post these logs in public.
typedef int (*initiate_fn)(void *, void *, int, uint64_t, uint32_t, uint16_t, uint8_t);
typedef uint8_t (*send_connect_fn)(void *, uint32_t, const void *, uint32_t, uint64_t *);
static initiate_fn r_initiate;
static send_connect_fn r_send_connect;
SYSV static int w_initiate(void *o, void *blob, int max, uint64_t server, uint32_t ip, uint16_t port, uint8_t secure) {
    int n = r_initiate(o, blob, max, server, ip, port, secure);
    log_line("[%lu] ticket made: %d bytes, server id type %u universe %u, address %s, port %u, secure %u", GetTickCount(), n,
             (unsigned)((server >> 52) & 0xF), (unsigned)(server >> 56), ip ? "set" : "empty", port, secure);
    return n;
}
SYSV static uint8_t w_send_connect(void *o, uint32_t ip, const void *blob, uint32_t len, uint64_t *out) {
    uint8_t ok = r_send_connect(o, ip, blob, len, out);
    log_line("[%lu] game server given a ticket of %u bytes: answered %u, player ..%04x", GetTickCount(), len, ok, out ? (unsigned)(*out & 0xFFFF) : 0);
    return ok;
}
enum { K_NORMAL, K_RETCLASS, K_SETSTAT_F, K_AVGRATE, K_BLOCK, K_LOBBY_LOG, K_AUTH_LOG };
typedef struct { const char *version; int linux_slot, windows_slot, kind; } SlotRule;

static const SlotRule g_rules[] = {
    // SteamUser: GetSteamID
    {"SteamUser018", 2, 2, K_RETCLASS},
    {"SteamUser018", 3, 3, K_AUTH_LOG},
    {"SteamGameServer012", 24, 24, K_AUTH_LOG},
    // SteamFriends: GetFriendByIndex, GetClanByIndex, GetFriendFromSourceByIndex, GetClanOwner, GetClanOfficerByIndex, GetCoplayFriend
    {"SteamFriends015", 4, 4, K_RETCLASS}, {"SteamFriends015", 19, 19, K_RETCLASS}, {"SteamFriends015", 25, 25, K_RETCLASS},
    {"SteamFriends015", 39, 39, K_RETCLASS}, {"SteamFriends015", 41, 41, K_RETCLASS}, {"SteamFriends015", 51, 51, K_RETCLASS},
    {"SteamFriends015", 57, 57, K_RETCLASS},
    // SteamMatchMaking, logged: string filter, number filter, distance, result count, CreateLobby, JoinLobby, SetLobbyData
    {"SteamMatchMaking009", 5, 5, K_LOBBY_LOG}, {"SteamMatchMaking009", 6, 6, K_LOBBY_LOG}, {"SteamMatchMaking009", 9, 9, K_LOBBY_LOG},
    {"SteamMatchMaking009", 10, 10, K_LOBBY_LOG}, {"SteamMatchMaking009", 13, 13, K_LOBBY_LOG}, {"SteamMatchMaking009", 14, 14, K_LOBBY_LOG},
    {"SteamMatchMaking009", 20, 20, K_LOBBY_LOG},
    // SteamMatchMaking: GetLobbyByIndex, GetLobbyMemberByIndex, GetLobbyOwner
    {"SteamMatchMaking009", 12, 12, K_RETCLASS}, {"SteamMatchMaking009", 18, 18, K_RETCLASS}, {"SteamMatchMaking009", 35, 35, K_RETCLASS},
    // SteamController (only used with a game controller connected): GetDigitalActionData (2-byte result) and GetAnalogActionData
    // (13 bytes). Slot order checked against the game's own calls (Init 0, Shutdown 1, RunFrame 2, GetConnectedControllers 3,
    // ShowBindingPanel 4, GetActionSetHandle 5, ActivateActionSet 6, GetDigitalActionHandle 8, GetAnalogActionHandle 11).
    // Without these Steam took the controller handle for the result address and crashed (player log 2026-11-24, 1.3.18).
    {"SteamController003", 9, 9, K_RETCLASS}, {"SteamController003", 12, 12, K_RETCLASS},
    // GameServer: GetSteamID, CreateUnauthenticatedUserConnection
    {"SteamGameServer012", 10, 10, K_RETCLASS}, {"SteamGameServer012", 25, 25, K_RETCLASS},
    // RemoteStorage: FileWrite, FileWriteAsync, FileForget, FileDelete, FileWriteStreamOpen / WriteChunk / Close
    {"STEAMREMOTESTORAGE_INTERFACE_VERSION013", 0, 0, K_BLOCK}, {"STEAMREMOTESTORAGE_INTERFACE_VERSION013", 2, 2, K_BLOCK},
    {"STEAMREMOTESTORAGE_INTERFACE_VERSION013", 5, 5, K_BLOCK}, {"STEAMREMOTESTORAGE_INTERFACE_VERSION013", 6, 6, K_BLOCK},
    {"STEAMREMOTESTORAGE_INTERFACE_VERSION013", 9, 9, K_BLOCK}, {"STEAMREMOTESTORAGE_INTERFACE_VERSION013", 10, 10, K_BLOCK},
    {"STEAMREMOTESTORAGE_INTERFACE_VERSION013", 11, 11, K_BLOCK},
    // UserStats: overloads swap places on Windows; decimal SetStat and the rate stat need their own conversion
    {"STEAMUSERSTATS_INTERFACE_VERSION011", 1, 2, K_NORMAL}, {"STEAMUSERSTATS_INTERFACE_VERSION011", 2, 1, K_NORMAL},
    {"STEAMUSERSTATS_INTERFACE_VERSION011", 3, 4, K_NORMAL}, {"STEAMUSERSTATS_INTERFACE_VERSION011", 4, 3, K_SETSTAT_F},
    {"STEAMUSERSTATS_INTERFACE_VERSION011", 5, 5, K_AVGRATE},
    {"STEAMUSERSTATS_INTERFACE_VERSION011", 17, 18, K_NORMAL}, {"STEAMUSERSTATS_INTERFACE_VERSION011", 18, 17, K_NORMAL},
    {"STEAMUSERSTATS_INTERFACE_VERSION011", 39, 40, K_NORMAL}, {"STEAMUSERSTATS_INTERFACE_VERSION011", 40, 39, K_NORMAL},
    {"STEAMUSERSTATS_INTERFACE_VERSION011", 41, 42, K_NORMAL}, {"STEAMUSERSTATS_INTERFACE_VERSION011", 42, 41, K_NORMAL},
    {NULL, 0, 0, 0}};

enum { PROXY_SLOTS = 128, STUB_SIZE = 40 };

static void *make_proxy(const char *version, void *real) {
    void **real_table = *(void ***)real;
    uint8_t *code = VirtualAlloc(NULL, PROXY_SLOTS * STUB_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    void **table = calloc(PROXY_SLOTS, sizeof(void *));
    void **obj = calloc(2, sizeof(void *));
    MEMORY_BASIC_INFORMATION mbi;
    for (int s = 0; s < PROXY_SLOTS; s++) {
        int win = s, kind = K_NORMAL;
        for (const SlotRule *r = g_rules; r->version; r++)
            if (r->linux_slot == s && !strcmp(r->version, version)) { win = r->windows_slot; kind = r->kind; }
        // tables have no end marker: stop where a slot no longer points at mapped code
        void *fn = real_table[win];
        if (!VirtualQuery(fn, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) continue;
        void *target = fn, *via = (void *)sysv_to_ms_common;
        if (kind == K_RETCLASS) via = (void *)sysv_to_ms_retclass;
        else if (kind == K_SETSTAT_F) { r_set_stat_float = fn; via = (void *)w_set_stat_float; }
        else if (kind == K_AVGRATE) { r_avg_rate = fn; via = (void *)w_avg_rate; }
        else if (kind == K_BLOCK) via = (void *)w_cloud_blocked;
        else if (kind == K_AUTH_LOG) {
            if (s == 3) { r_initiate = fn; via = (void *)w_initiate; } else { r_send_connect = fn; via = (void *)w_send_connect; }
        }
        else if (kind == K_LOBBY_LOG) {
            switch (s) {
            case 5: r_filter_str = fn; via = (void *)w_filter_str; break;
            case 6: r_filter_num = fn; via = (void *)w_filter_num; break;
            case 9: r_filter_distance = fn; via = (void *)w_filter_distance; break;
            case 10: r_filter_count = fn; via = (void *)w_filter_count; break;
            case 13: r_create = fn; via = (void *)w_create; break;
            case 14: r_join = fn; via = (void *)w_join; break;
            case 20: r_set_data = fn; via = (void *)w_set_data; break;
            }
        }
        uint8_t *p = code + s * STUB_SIZE;
        p[0] = 0x48; p[1] = 0xBF;                   // mov rdi, real object
        memcpy(p + 2, &real, 8);
        p[10] = 0x48; p[11] = 0xB8;                 // mov rax, target
        memcpy(p + 12, &target, 8);
        p[20] = 0x49; p[21] = 0xBB;                 // mov r11, bridge
        memcpy(p + 22, &via, 8);
        p[30] = 0x41; p[31] = 0xFF; p[32] = 0xE3;   // jmp r11
        table[s] = p;
    }
    obj[0] = table;
    return obj;
}

static int connect_steam(void);

static void *get_interface(const char *version, int standin_index) {
    if (connect_steam()) {
        // ISteamClient slot 12: GetISteamGenericInterface(user, pipe, version)
        void *(*generic)(void *, int32_t, int32_t, const char *) = (*(void ***)g_client)[12];
        void *real = generic(g_client, g_user, g_pipe, version);
        if (real) {
            log_line("Steam: %s connected", version);
            return make_proxy(version, real);
        }
        log_line("Steam: %s not available, using a stand-in", version);
    }
    return standin_steam_object(standin_index);
}

// The local multiplayer test (steam_hub.c, ATTILA_TEST_MP=n) answers lobbies, player packets and the player's identity itself.
int hub_on(void);
void *hub_interface(const char *version, void *real);
void *hub_game_server(void);
void hub_game_server_stop(void);
void hub_run(int gs);
int hub_call_result(uint8_t *game, uint64_t call, int unregister);
#define STEAM_IFACE(sym, version, idx) \
    SHIM(void *, sym, (void)) { \
        static void *volatile o; \
        if (!o) o = hub_interface(version, get_interface(version, idx)); \
        return o; \
    }
STEAM_IFACE(SteamApps, "STEAMAPPS_INTERFACE_VERSION007", 0)
STEAM_IFACE(SteamController, "SteamController003", 1)
STEAM_IFACE(SteamFriends, "SteamFriends015", 2)
STEAM_IFACE(SteamHTMLSurface, "STEAMHTMLSURFACE_INTERFACE_VERSION_003", 4)
STEAM_IFACE(SteamMatchmaking, "SteamMatchMaking009", 5)
STEAM_IFACE(SteamMatchmakingServers, "SteamMatchMakingServers002", 6)
STEAM_IFACE(SteamNetworking, "SteamNetworking005", 7)
STEAM_IFACE(SteamRemoteStorage, "STEAMREMOTESTORAGE_INTERFACE_VERSION013", 8)
STEAM_IFACE(SteamUser, "SteamUser018", 9)
STEAM_IFACE(SteamUserStats, "STEAMUSERSTATS_INTERFACE_VERSION011", 10)
STEAM_IFACE(SteamUtils, "SteamUtils007", 11)

// The host side of a multiplayer game runs a Steam game server.
static void *volatile g_gameserver;
SHIM(int, SteamGameServer_Init, (uint32_t ip, uint16_t steam_port, uint16_t game_port, uint16_t query_port, int mode, const char *version)) {
    if (hub_on()) {   // local multiplayer test: no real game server (it would be listed on Steam's server list)
        g_gameserver = hub_game_server();
        log_line("Steam: game server for the local test (ports %u/%u/%u, mode %d, version '%s')", steam_port, game_port, query_port, mode, version ? version : "");
        return 1;
    }
    if (!connect_steam()) return 0;
    uint8_t (*init)(uint32_t, uint16_t, uint16_t, uint16_t, int, const char *) = (void *)steam_fn("SteamInternal_GameServer_Init");
    if (!init) { log_line("Steam: this steam_api64.dll has no game server start"); return 0; }
    uint8_t ok = init(ip, steam_port, game_port, query_port, mode, version);
    log_line("Steam: game server ip %x ports %u/%u/%u mode %d version '%s' answered %u", ip, steam_port, game_port, query_port, mode, version ? version : "", ok);
    if (!ok) return 0;
    int32_t (*get_user)(void) = (void *)steam_fn("SteamGameServer_GetHSteamUser");
    int32_t (*get_pipe)(void) = (void *)steam_fn("SteamGameServer_GetHSteamPipe");
    void *(*generic)(void *, int32_t, int32_t, const char *) = (*(void ***)g_client)[12];
    void *real = generic(g_client, get_user(), get_pipe(), "SteamGameServer012");
    log_line("Steam: game server interface %p", real);
    if (!real) return 0;
    g_gameserver = make_proxy("SteamGameServer012", real);
    // Which game Steam thinks each side belongs to: the player and the game server must match.
    {
        int32_t (*user_get)(void) = (void *)steam_fn("SteamAPI_GetHSteamUser");
        int32_t (*user_pipe)(void) = (void *)steam_fn("SteamAPI_GetHSteamPipe");
        void *u_client = generic(g_client, user_get(), user_pipe(), "SteamUtils009");
        void *u_server = generic(g_client, get_user(), get_pipe(), "SteamUtils009");
        uint32_t (*app_of)(void *);
        unsigned a = 0, b = 0;
        if (u_client) { app_of = (*(void ***)u_client)[9]; a = app_of(u_client); }
        if (u_server) { app_of = (*(void ***)u_server)[9]; b = app_of(u_server); }
        log_line("Steam: game id: player side %u, game server side %u", a, b);
    }
    return 1;
}
SHIM(void *, SteamGameServer, (void)) { return g_gameserver ? g_gameserver : standin_steam_object(3); }
SHIM(void, SteamGameServer_RunCallbacks, (void)) {
    if (hub_on()) { if (g_gameserver) hub_run(1); return; }
    if (g_dll && g_gameserver) ((void (*)(void))steam_fn("SteamGameServer_RunCallbacks"))();
}
SHIM(void, SteamGameServer_Shutdown, (void)) {
    if (hub_on()) hub_game_server_stop();
    else if (g_dll && g_gameserver) ((void (*)(void))steam_fn("SteamGameServer_Shutdown"))();
    g_gameserver = NULL;
    log_line("Steam: game server stopped");
}

// Connects once, the first time the game asks for Steam in any way. Without Steam (not running, not signed in, or the game
// started as administrator while Steam is not) every Steam object was a stand-in answering 0 / NULL, and the game crashed
// later in its menu script (tester log, 2026-11-09). Now the player is asked to start Steam: Retry tries again, Cancel
// closes the game. ATTILA_TEST_NO_STEAM=1 makes the first try fail (test).
static int connect_steam(void) {
    static int state;   // 0 not tried, 1 connected, -1 failed
    if (state) return state > 0;
    state = -1;
    SetEnvironmentVariableA("SteamAppId", "325610");
    SetEnvironmentVariableA("SteamGameId", "325610");
    uint8_t (*init)(void) = (void *)steam_fn("SteamAPI_Init");
    if (!init) fatal("steam_api64.dll next to attila64.exe has no SteamAPI_Init");
    int pretend_fail = getenv("ATTILA_TEST_NO_STEAM") != NULL;
    for (;;) {
        uint8_t ok = pretend_fail ? 0 : init();
        pretend_fail = 0;
        log_line("Steam: SteamAPI_Init answered %u", ok);
        if (ok) break;
        log_line("Steam: not connected (is Steam running and signed in?) - asking the player");
        int r = MessageBoxA(NULL,
                            "64-bit Attila cannot reach Steam.\n\n"
                            "Please start Steam and sign in, then press Retry.\n"
                            "(If you started the game as administrator, start it the normal way instead.)\n\n"
                            "Cancel closes the game.",
                            "64-bit Attila", MB_RETRYCANCEL | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
        if (r != IDRETRY) { log_go_sync(); log_line("Steam: the player closed the game (no Steam)"); ExitProcess(0); }
        log_line("Steam: trying again");
    }
    int32_t (*get_user)(void) = (void *)steam_fn("SteamAPI_GetHSteamUser");
    int32_t (*get_pipe)(void) = (void *)steam_fn("SteamAPI_GetHSteamPipe");
    void *(*create)(const char *) = (void *)steam_fn("SteamInternal_CreateInterface");
    g_user = get_user();
    g_pipe = get_pipe();
    g_client = create("SteamClient017");
    log_line("Steam: user %d, pipe %d, client %p", g_user, g_pipe, g_client);
    if (g_client) state = 1;
    return g_client != NULL;
}
SHIM(int, SteamAPI_Init, (void)) { return connect_steam(); }
// Called by loader.c before the game makes its window and DirectX 11 swap chain. SteamAPI_Init is what loads Steam's
// overlay (gameoverlayrenderer64.dll) into the process, and the overlay only hooks swap chains made AFTER it is loaded
// (it wraps CreateDXGIFactory / CreateSwapChainForHwnd). The game itself asks for Steam only after its swap chain is made,
// so the overlay stayed dark (Shift+Tab did nothing; found 2026-11-22 with Steam\logs\gameoverlay_renderer.txt and a probe
// program). Same connect as the game's own; the game's later SteamAPI_Init gets the stored answer.
void steam_connect_early(void) {
    if (getenv("ATTILA_STEAM_LATE")) { log_line("Steam: connect left to the game (ATTILA_STEAM_LATE, test)"); return; }
    log_line("Steam: connecting before the game's graphics start (for the Steam overlay)");
    connect_steam();
}
// Never let Steam restart "the real game": that would launch the 32-bit Windows Attila instead of this one.
SHIM(int, SteamAPI_RestartAppIfNecessary, (unsigned app)) { return 0; }
SHIM(void, SteamAPI_Shutdown, (void)) { if (g_dll) ((void (*)(void))steam_fn("SteamAPI_Shutdown"))(); }
SHIM(void, SteamAPI_RunCallbacks, (void)) {
    if (g_dll) ((void (*)(void))steam_fn("SteamAPI_RunCallbacks"))();
    hub_run(0);
}

// Callback objects. Steam calls a Windows-style object; that object calls the game's Linux-style one.
// Layout on both sides: table pointer, flags byte at +8, callback id at +12.
typedef struct CallbackProxy {
    void *const *vtable;
    uint8_t flags;
    int id;
    uint8_t *game;
    struct CallbackProxy *next;
    int kind;   // 1 = a callback (every message of its type), 2 = a call result (one answer)
} CallbackProxy;

typedef SYSV void (*game_run1)(void *, void *);
typedef SYSV void (*game_run3)(void *, void *, int, uint64_t);
typedef SYSV int (*game_size)(void *);

// Steam messages: on Linux they pack 8-byte fields on 4-byte boundaries, on Windows on 8-byte ones. Same fields, other positions
// and sizes for some messages. Each move is {windows offset, linux offset, bytes}.
typedef struct { int win, lin, len; } FieldMove;
typedef struct { int id, win_size, lin_size; FieldMove moves[8]; } MessageLayout;
static const MessageLayout g_layouts[] = {
    {202, 144, 140, {{0, 0, 140}}},                                 // GSClientDeny
    {203, 16, 12, {{0, 0, 12}}},                                    // GSClientKick
    {304, 16, 12, {{0, 0, 12}}},                                    // PersonaStateChange
    {334, 24, 20, {{0, 0, 20}}},                                    // AvatarImageLoaded
    {336, 16, 12, {{0, 0, 12}}},                                    // FriendRichPresenceUpdate
    {504, 24, 20, {{0, 0, 20}}},                                    // LobbyEnter
    {505, 24, 20, {{0, 0, 17}}},                                    // LobbyDataUpdate
    {506, 32, 28, {{0, 0, 28}}},                                    // LobbyChatUpdate
    {512, 24, 20, {{0, 0, 17}}},                                    // LobbyKicked
    {513, 16, 12, {{0, 0, 4}, {8, 4, 8}}},                          // LobbyCreated
    {1101, 24, 20, {{0, 0, 12}, {16, 12, 8}}},                      // UserStatsReceived
    {1103, 152, 148, {{0, 0, 148}}},                                // UserAchievementStored
    {1104, 16, 12, {{0, 0, 9}}},                                    // LeaderboardFindResult
    {1105, 24, 20, {{0, 0, 20}}},                                   // LeaderboardScoresDownloaded
    {1106, 32, 28, {{0, 0, 1}, {8, 4, 8}, {16, 12, 1}, {20, 16, 12}}}, // LeaderboardScoreUploaded
    {1112, 16, 12, {{0, 0, 12}}},                                   // GlobalStatsReceived
    {1203, 16, 12, {{0, 0, 9}}},                                    // P2PSessionConnectFail
    {1307, 272, 272, {{0, 0, 4}, {8, 4, 8}, {16, 12, 260}}},        // RemoteStorageFileShareResult
    {0, 0, 0, {{0, 0, 0}}}};

static const MessageLayout *layout_for(int id) {
    for (const MessageLayout *l = g_layouts; l->id; l++) if (l->id == id) return l;
    return NULL;
}
// Returns the message as the game expects it: rewritten into buf when the layouts differ.
static void *message_for_game(int id, void *param, uint8_t *buf, size_t cap) {
    static uint8_t seen[8192];
    if (id > 0 && id < 8192 && !seen[id]) {
        seen[id] = 1;
        log_line("Steam message %d first seen%s", id, layout_for(id) ? " (layout converted)" : "");
    }
    if (id == 510 && param) log_line("lobby search answered: %u lobbies found", *(uint32_t *)param);
    // Game server's answers about a connecting player. Only the end of the Steam ID: players post these logs in public.
    if (param && (id == 201 || id == 202 || id == 203)) {
        static const char *const reasons[] = {"none", "invalid version", "generic", "not logged on", "no license", "cheater", "logged in elsewhere",
                                              "unknown text", "incompatible anticheat", "memory corruption", "incompatible software", "Steam connection lost",
                                              "Steam connection error", "Steam response timed out", "Steam validation stalled", "owner left guest user"};
        uint64_t who = *(uint64_t *)param;
        if (id == 201) log_line("game server: player ..%04x approved", (unsigned)(who & 0xFFFF));
        else {
            int r = *(int *)((uint8_t *)param + 8);
            log_line("game server: player ..%04x %s, reason %d (%s)%s%s", (unsigned)(who & 0xFFFF), id == 202 ? "denied" : "kicked", r,
                     r >= 0 && r < 16 ? reasons[r] : "?", id == 202 ? " text: " : "", id == 202 ? (char *)param + 12 : "");
        }
    }
    if (id == 1203 && param) {
        static const char *const p2p[] = {"none", "target not running the game", "no rights to the game", "target not logged in", "timeout"};
        int e = ((uint8_t *)param)[8];
        log_line("[%lu] P2P connection to player ..%04x FAILED: %d (%s)", GetTickCount(), (unsigned)(*(uint64_t *)param & 0xFFFF), e, e < 5 ? p2p[e] : "?");
    }
    if (id == 1202 && param) log_line("[%lu] P2P: player ..%04x asks to connect", GetTickCount(), (unsigned)(*(uint64_t *)param & 0xFFFF));
    if (id == 115 && param) log_line("game server: policy says VAC secure = %u", *(uint8_t *)param);
    if (id == 101 || id == 102 || id == 103) log_line("[%lu] game server: Steam message %d (101 logged on, 102 failed, 103 lost)", GetTickCount(), id);
    const MessageLayout *l = layout_for(id);
    if (!l || (size_t)l->lin_size > cap || !param) return param;
    memset(buf, 0, (size_t)l->lin_size);
    for (int i = 0; i < 8 && l->moves[i].len; i++) memcpy(buf + l->moves[i].lin, (uint8_t *)param + l->moves[i].win, (size_t)l->moves[i].len);
    return buf;
}

// On Windows the overloaded Run methods sit in reverse order: the three-argument one comes first.
static void proxy_run3(CallbackProxy *p, void *param, uint8_t failed, uint64_t call) {
    uint8_t buf[512];
    void **t = *(void ***)p->game;
    ((game_run3)t[1])(p->game, message_for_game(p->id, param, buf, sizeof buf), failed, call);
}
static void proxy_run1(CallbackProxy *p, void *param) {
    uint8_t buf[512];
    void **t = *(void ***)p->game;
    ((game_run1)t[0])(p->game, message_for_game(p->id, param, buf, sizeof buf));
}
// Steam compares this with the size it holds, which is the Windows one.
static int proxy_size(CallbackProxy *p) {
    const MessageLayout *l = layout_for(p->id);
    if (l) return l->win_size;
    void **t = *(void ***)p->game;
    return ((game_size)t[2])(p->game);
}
static void *const g_proxy_vtable[] = {(void *)proxy_run3, (void *)proxy_run1, (void *)proxy_size};

static CallbackProxy *g_proxies;
static SRWLOCK g_proxy_lock = SRWLOCK_INIT;

static CallbackProxy *proxy_for(uint8_t *game, int create) {
    AcquireSRWLockExclusive(&g_proxy_lock);
    CallbackProxy *p = g_proxies;
    while (p && p->game != game) p = p->next;
    if (!p && create) {
        p = calloc(1, sizeof *p);
        p->vtable = g_proxy_vtable;
        p->game = game;
        p->next = g_proxies;
        g_proxies = p;
    }
    ReleaseSRWLockExclusive(&g_proxy_lock);
    return p;
}

// The game's registered callback objects for a message (gs = the game server's ones), for the local test's messages.
int steam_listeners(int id, int gs, uint8_t **out, int max) {
    int n = 0;
    AcquireSRWLockShared(&g_proxy_lock);
    for (CallbackProxy *p = g_proxies; p && n < max; p = p->next)
        if (p->kind == 1 && p->id == id && (p->flags & 1) && ((p->flags & 2) != 0) == (gs != 0)) out[n++] = p->game;
    ReleaseSRWLockShared(&g_proxy_lock);
    return n;
}
SHIM(void, SteamAPI_RegisterCallback, (uint8_t *game, int id)) {
    CallbackProxy *p = proxy_for(game, 1);
    void (*real)(void *, int) = (void *)steam_fn("SteamAPI_RegisterCallback");
    p->flags = game[8];
    p->id = id;
    p->kind = 1;
    real(p, id);
    game[8] = p->flags;
    *(int *)(game + 12) = id;
}
SHIM(void, SteamAPI_UnregisterCallback, (uint8_t *game)) {
    CallbackProxy *p = proxy_for(game, 0);
    if (!p) return;
    void (*real)(void *) = (void *)steam_fn("SteamAPI_UnregisterCallback");
    real(p);
    game[8] = p->flags;
}
SHIM(void, SteamAPI_RegisterCallResult, (uint8_t *game, uint64_t call)) {
    if (hub_call_result(game, call, 0)) return;   // an answer of the local test (steam_hub.c)
    CallbackProxy *p = proxy_for(game, 1);
    p->kind = 2;
    void (*real)(void *, uint64_t) = (void *)steam_fn("SteamAPI_RegisterCallResult");
    p->flags = game[8];
    p->id = *(int *)(game + 12);
    real(p, call);
    game[8] = p->flags;
}
SHIM(void, SteamAPI_UnregisterCallResult, (uint8_t *game, uint64_t call)) {
    if (hub_call_result(game, call, 1)) return;
    CallbackProxy *p = proxy_for(game, 0);
    if (!p) return;
    void (*real)(void *, uint64_t) = (void *)steam_fn("SteamAPI_UnregisterCallResult");
    real(p, call);
    game[8] = p->flags;
}
