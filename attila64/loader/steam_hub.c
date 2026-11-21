// steam_hub.c: a local stand-in for Steam's multiplayer parts, for tests only (ATTILA_TEST_MP=<player 1..8>).
// Several copies of 64-bit Attila on one PC play together without real Steam lobbies: the lobbies (ISteamMatchmaking009),
// player-to-player packets (ISteamNetworking005), the host's game server (ISteamGameServer012: log-on, player approval),
// the player's identity (ISteamUser018 GetSteamID, login ticket) and names (ISteamFriends015) come from one shared memory
// block, the hub. Everything else (owning the game and its DLC, achievements, cloud) still goes to the real Steam.
// Which methods the game calls was read from the Linux game (tools\steam_calls.sh, 2026-11-13); messages are written in the
// Linux layout (8-byte fields on 4-byte boundaries). Behaviour follows what Steam documents: a call result is also sent
// to the callbacks of its type; creating a lobby enters it; members hear about joins, leaves, data and chat.
//   ATTILA_TEST_MP=n          this copy is test player n (1 = the one that hosts)
//   ATTILA_TEST_MP_HUB=name   which hub (default "1"); a test run uses a fresh name so nothing is left from the last one
//   ATTILA_TEST_MP_JOIN=s     s seconds after the front end is up, join the first open lobby of the hub the way an accepted
//                             Steam invite does (GameLobbyJoinRequested)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "loader.h"

enum { HP = 9, HL = 8, HM = 16, HD = 96, HMD = 48, HKEYLEN = 64, HVAL = 8192, HMVAL = 2048, HCHAT = 128, HCHATSZ = 4096,
       HEV = 1024, HRING = 16 << 20 };
typedef struct { char key[HKEYLEN]; char val[HVAL]; } HubData;
typedef struct { char key[HKEYLEN]; char val[HMVAL]; } HubMData;
typedef struct { uint32_t id, len; uint64_t from; uint8_t body[HCHATSZ]; } HubChat;
typedef struct { uint64_t id; int ndata; HubMData data[HMD]; } HubMember;
typedef struct {
    uint64_t id, owner, server;
    uint32_t ip; uint16_t port;
    int type, limit, joinable, used, nmembers, ndata;
    HubMember m[HM];
    HubData data[HD];
    uint32_t next_chat;
    HubChat chat[HCHAT];
} HubLobby;
typedef struct { int id, gs, len; uint8_t data[52]; } HubEvent;
typedef struct { uint64_t from, visible; int32_t channel; uint32_t len, consumed, total; } HubPacket;   // visible = when it arrives (sender's time + the test's lag)
typedef struct {
    uint64_t steamid, serverid;
    DWORD pid;
    int alive, server_on;
    char name[64];
    uint32_t ev_head, ev_tail;
    HubEvent ev[HEV];
    uint8_t asked[HP];
    uint64_t r_head, r_tail;
    uint8_t ring[HRING];
} HubPlayer;
typedef struct { uint32_t magic, version, next_lobby, unused; HubPlayer p[HP]; HubLobby l[HL]; } Hub;

static Hub *g_hub;
static HANDLE g_hub_lock;
static int g_me = -1;   // -1 = not looked yet, 0 = off
static SRWLOCK g_local = SRWLOCK_INIT;
// ATTILA_TEST_MP_TRACE=1: every lobby, packet and game server call of the game is logged (up to 4000 lines a copy)
static int g_trace = -1;
static volatile LONG g_trace_lines;
#define HT(...) do { if (g_trace < 0) g_trace = getenv("ATTILA_TEST_MP_TRACE") != NULL; \
                     if (g_trace && InterlockedIncrement(&g_trace_lines) <= 4000) log_line("hub trace: " __VA_ARGS__); } while (0)
static const char *short_text(const char *s) {   // the start of a long text, for the trace
    static char b[8][96];
    static volatile LONG n;
    char *r = b[(unsigned)InterlockedIncrement(&n) % 8];
    snprintf(r, sizeof b[0], "%s", s ? s : "(null)");
    return r;
}

static uint64_t player_id(int i) { return 0x0110000100000000ull | (0x7E000000u + (unsigned)i); }
static uint64_t server_id(int i) { return 0x0140000000000000ull | (0x7E100000u + (unsigned)i); }
static int player_of(uint64_t id) {
    for (int i = 1; i < HP; i++) if (id == player_id(i) || id == server_id(i)) return i;
    return 0;
}

int hub_on(void) {
    if (g_me >= 0) return g_me > 0;
    const char *e = getenv("ATTILA_TEST_MP");
    int n = e ? atoi(e) : 0;
    if (n < 1 || n >= HP) { g_me = 0; return 0; }
    const char *hn = getenv("ATTILA_TEST_MP_HUB");
    char name[128], lock[128];
    snprintf(name, sizeof name, "Local\\attila64_hub_%s", hn && *hn ? hn : "1");
    snprintf(lock, sizeof lock, "Local\\attila64_hub_lock_%s", hn && *hn ? hn : "1");
    g_hub_lock = CreateMutexA(NULL, FALSE, lock);
    HANDLE m = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)sizeof(Hub), name);
    g_hub = m ? MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Hub)) : NULL;
    if (!g_hub || !g_hub_lock) { log_line("test hub: could not open the shared hub (%lu)", GetLastError()); g_me = 0; return 0; }
    WaitForSingleObject(g_hub_lock, INFINITE);
    if (g_hub->magic != 0x42554841) { g_hub->magic = 0x42554841; g_hub->version = 1; }
    HubPlayer *p = &g_hub->p[n];
    // a fresh start of this player: nothing of an earlier copy with the same number is kept (its lobbies are left below)
    p->ev_head = p->ev_tail = 0; p->r_head = p->r_tail = 0;
    memset(p->asked, 0, sizeof p->asked);
    p->steamid = player_id(n); p->serverid = server_id(n); p->pid = GetCurrentProcessId(); p->alive = 1; p->server_on = 0;
    snprintf(p->name, sizeof p->name, "Tester %d", n);
    ReleaseMutex(g_hub_lock);
    g_me = n;
    log_line("test hub: this copy is test player %d (%s) on hub '%s': lobbies, player packets and the game server are local, the rest is real Steam",
             n, p->name, hn && *hn ? hn : "1");
    return 1;
}
static void lock(void) { WaitForSingleObject(g_hub_lock, INFINITE); }
static void unlock(void) { ReleaseMutex(g_hub_lock); }

// ---- events for a player (callbacks the next time that copy runs Steam's callbacks)
static void post_locked(int to, int id, int gs, const void *data, int len) {
    if (to < 1 || to >= HP || len > 52) return;
    HubPlayer *p = &g_hub->p[to];
    if (!p->alive) return;
    if (p->ev_tail - p->ev_head >= HEV) { log_line("test hub: event queue of player %d full, message %d dropped", to, id); return; }
    HubEvent *e = &p->ev[p->ev_tail % HEV];
    e->id = id; e->gs = gs; e->len = len;
    memcpy(e->data, data, (size_t)len);
    p->ev_tail++;
}
static void put64(uint8_t *b, int at, uint64_t v) { memcpy(b + at, &v, 8); }
static void put32(uint8_t *b, int at, uint32_t v) { memcpy(b + at, &v, 4); }

static HubLobby *lobby_of(uint64_t id) {
    for (int i = 0; i < HL; i++) if (g_hub->l[i].used && g_hub->l[i].id == id) return &g_hub->l[i];
    return NULL;
}
static int member_index(HubLobby *l, uint64_t who) {
    for (int i = 0; i < l->nmembers; i++) if (l->m[i].id == who) return i;
    return -1;
}
// LobbyChatUpdate_t (506): lobby, who changed, who made the change, what happened (1 entered, 2 left, 4 disconnected)
static void tell_members_locked(HubLobby *l, uint64_t who, uint32_t what, int except) {
    uint8_t b[28] = {0};
    put64(b, 0, l->id); put64(b, 8, who); put64(b, 16, who); put32(b, 24, what);
    for (int i = 0; i < l->nmembers; i++) { int to = player_of(l->m[i].id); if (to != except) post_locked(to, 506, 0, b, 28); }
}
// LobbyDataUpdate_t (505): lobby, member (= the lobby for lobby data), success
static void tell_data_locked(HubLobby *l, uint64_t member) {
    uint8_t b[20] = {0};
    put64(b, 0, l->id); put64(b, 8, member); b[16] = 1;
    for (int i = 0; i < l->nmembers; i++) post_locked(player_of(l->m[i].id), 505, 0, b, 20);
}
static void leave_locked(HubLobby *l, uint64_t who) {
    int k = member_index(l, who);
    if (k < 0) return;
    memmove(&l->m[k], &l->m[k + 1], sizeof(HubMember) * (size_t)(l->nmembers - k - 1));
    l->nmembers--;
    if (!l->nmembers) { log_line("test hub: lobby %llx closed (last member left)", (unsigned long long)l->id); l->used = 0; return; }
    if (l->owner == who) l->owner = l->m[0].id;
    tell_members_locked(l, who, 2, 0);
}

// ---- per-copy waiting results of asynchronous calls (CreateLobby, JoinLobby, RequestLobbyList)
typedef struct { uint64_t handle; int id, len, sent; uint8_t *game; ULONGLONG made; uint8_t data[32]; } Pending;   // sent = given to the callbacks of its type
static Pending g_pend[32];
static uint64_t g_next_call = 1;
static uint64_t new_call(int id, const void *data, int len) {
    AcquireSRWLockExclusive(&g_local);
    uint64_t h = 0x5A5A000000000000ull | ((uint64_t)GetCurrentProcessId() << 20) | (g_next_call++ & 0xFFFFF);
    for (int i = 0; i < 32; i++)
        if (!g_pend[i].handle) {
            g_pend[i].handle = h; g_pend[i].id = id; g_pend[i].len = len; g_pend[i].sent = 0; g_pend[i].game = NULL; g_pend[i].made = GetTickCount64();
            memcpy(g_pend[i].data, data, (size_t)len);
            break;
        }
    ReleaseSRWLockExclusive(&g_local);
    return h;
}
// SteamAPI_RegisterCallResult / Unregister for one of our handles: 1 = it was ours
int hub_call_result(uint8_t *game, uint64_t call, int unregister) {
    if (!hub_on() || (call >> 48) != 0x5A5A) return 0;
    AcquireSRWLockExclusive(&g_local);
    for (int i = 0; i < 32; i++)
        if (g_pend[i].handle == call) {
            if (unregister) g_pend[i].handle = 0; else g_pend[i].game = game;
            break;
        }
    ReleaseSRWLockExclusive(&g_local);
    if (!unregister) game[8] |= 1;
    else game[8] &= (uint8_t)~1;
    return 1;
}

// ---- delivering: call results first, then this copy's events, each to the game's own callback objects
extern int steam_listeners(int id, int gs, uint8_t **out, int max);   // shim_steam.c
typedef SYSV void (*RunFn1)(void *, void *);
typedef SYSV void (*RunFn3)(void *, void *, int, uint64_t);
static void deliver(int id, int gs, void *data) {
    uint8_t *objs[32];
    int n = steam_listeners(id, gs, objs, 32);
    HT("message %d%s to %d listeners", id, gs ? " (game server)" : "", n);
    for (int i = 0; i < n; i++) ((RunFn1)(*(void ***)objs[i])[0])(objs[i], data);
}
static double g_join_after = -2;
static int g_join_done;
extern double test_front_seconds(void);   // test_auto.c
void hub_run(int gs) {
    if (!hub_on()) return;
    if (!gs) {
        // Answers of asynchronous calls: to the call result object the game registered for the handle, if any, and once to
        // every callback of the message's type (the game's campaign host waits for LobbyCreated_t that way, 2026-11-13).
        for (;;) {
            Pending p = {0};
            int as_callback = 0;
            AcquireSRWLockExclusive(&g_local);
            for (int i = 0; i < 32; i++) {
                Pending *q = &g_pend[i];
                if (!q->handle) continue;
                if (q->game) { p = *q; q->handle = 0; break; }
                if (!q->sent) { p = *q; q->sent = 1; as_callback = 1; break; }
                if (GetTickCount64() - q->made > 60000) q->handle = 0;   // no call result object came for it
            }
            ReleaseSRWLockExclusive(&g_local);
            if (!p.handle) break;
            if (as_callback) { HT("answer %d for call %llx to the callbacks", p.id, (unsigned long long)p.handle); deliver(p.id, 0, p.data); continue; }
            p.game[8] &= (uint8_t)~1;
            HT("answer %d for call %llx to its call result", p.id, (unsigned long long)p.handle);
            ((RunFn3)(*(void ***)p.game)[1])(p.game, p.data, 0, p.handle);
            if (!p.sent) deliver(p.id, 0, p.data);
        }
        // ATTILA_TEST_MP_JOIN=s: like accepting a Steam invite to the first open lobby, s seconds after the front end is up
        if (g_join_after == -2) { const char *e = getenv("ATTILA_TEST_MP_JOIN"); g_join_after = e ? atof(e) : -1; }
        if (g_join_after >= 0 && !g_join_done && test_front_seconds() >= g_join_after) {
            uint8_t b[16] = {0};
            lock();
            for (int i = 0; i < HL; i++)
                if (g_hub->l[i].used && g_hub->l[i].joinable && member_index(&g_hub->l[i], player_id(g_me)) < 0) {
                    put64(b, 0, g_hub->l[i].id); put64(b, 8, g_hub->l[i].owner);
                    break;
                }
            unlock();
            uint64_t lid; memcpy(&lid, b, 8);
            if (lid) { g_join_done = 1; log_line("test hub: joining lobby %llx as an accepted invite would", (unsigned long long)lid); deliver(333, 0, b); }
        }
    }
    for (int k = 0; k < 256; k++) {
        HubEvent e;
        int got = 0;
        lock();
        HubPlayer *p = &g_hub->p[g_me];
        // events for the other side (game server or player) wait in the queue for that side's run
        for (uint32_t t = p->ev_head; t != p->ev_tail; t++) {
            HubEvent *q = &p->ev[t % HEV];
            if (q->gs == gs && q->id) { e = *q; q->id = 0; got = 1; break; }
        }
        while (p->ev_head != p->ev_tail && !p->ev[p->ev_head % HEV].id) p->ev_head++;
        unlock();
        if (!got) break;
        deliver(e.id, gs, e.data);
    }
}

// ---- ISteamMatchmaking009 (slot numbers in the comments)
typedef struct { char key[HKEYLEN]; char val[256]; int num, cmp, kind; } Filter;   // kind 1 text, 2 number, 3 slots
static Filter g_filters[16];
static int g_nfilters, g_max_results = 50;
static uint64_t g_found[64];
static int g_nfound;
static char g_ret[32][HVAL];
static volatile LONG g_ret_next;
static const char *ret_text(const char *s) {   // the game may hold several answers at once: a ring of copies
    char *b = g_ret[(unsigned)InterlockedIncrement(&g_ret_next) % 32];
    snprintf(b, HVAL, "%s", s ? s : "");
    return b;
}
static const char *data_get(HubData *d, int n, const char *k) {
    for (int i = 0; i < n; i++) if (!strcmp(d[i].key, k)) return d[i].val;
    return "";
}
static int data_set(HubData *d, int *n, int cap, const char *k, const char *v) {   // 1 = changed
    for (int i = 0; i < *n; i++)
        if (!strcmp(d[i].key, k)) {
            if (!strcmp(d[i].val, v)) return 0;
            snprintf(d[i].val, HVAL, "%s", v);
            return 1;
        }
    if (*n >= cap) { log_line("test hub: lobby data full, '%s' not kept", k); return 0; }
    snprintf(d[*n].key, HKEYLEN, "%s", k); snprintf(d[*n].val, HVAL, "%s", v); (*n)++;
    return 1;
}
static int mdata_set(HubMember *m, const char *k, const char *v) {
    for (int i = 0; i < m->ndata; i++)
        if (!strcmp(m->data[i].key, k)) {
            if (!strcmp(m->data[i].val, v)) return 0;
            snprintf(m->data[i].val, HMVAL, "%s", v);
            return 1;
        }
    if (m->ndata >= HMD) { log_line("test hub: member data full, '%s' not kept", k); return 0; }
    if (strlen(v) >= HMVAL) log_line("test hub: member data '%s' cut to %d bytes", k, HMVAL - 1);
    snprintf(m->data[m->ndata].key, HKEYLEN, "%s", k); snprintf(m->data[m->ndata].val, HMVAL, "%s", v); m->ndata++;
    return 1;
}

SYSV static int mm_zero(void *o) { return 0; }
SYSV static uint64_t mm_request_list(void *o) {   // 4
    uint8_t b[4] = {0};
    AcquireSRWLockExclusive(&g_local);
    g_nfound = 0;
    lock();
    for (int i = 0; i < HL && g_nfound < g_max_results && g_nfound < 64; i++) {
        HubLobby *l = &g_hub->l[i];
        if (!l->used || !l->joinable || l->type == 0 || l->type == 3) continue;
        int ok = 1;
        for (int f = 0; f < g_nfilters && ok; f++) {
            Filter *x = &g_filters[f];
            if (x->kind == 3) { ok = l->limit - l->nmembers >= x->num; continue; }
            const char *v = data_get(l->data, l->ndata, x->key);
            // the lobby's value against the asked one (Steam's comparison numbers: -2 <=, -1 <, 0 =, 1 >, 2 >=, 3 !=); texts
            // compare in letter order: the campaign list asks for 'type' between '00' and '01' that way
            int d = x->kind == 1 ? strcmp(v, x->val) : atoi(v) - x->num;
            ok = x->cmp == -2 ? d <= 0 : x->cmp == -1 ? d < 0 : x->cmp == 1 ? d > 0 : x->cmp == 2 ? d >= 0 : x->cmp == 3 ? d != 0 : d == 0;
        }
        if (ok) g_found[g_nfound++] = l->id;
    }
    unlock();
    log_line("test hub: lobby search with %d filters found %d", g_nfilters, g_nfound);
    put32(b, 0, (uint32_t)g_nfound);
    g_nfilters = 0; g_max_results = 50;
    ReleaseSRWLockExclusive(&g_local);
    return new_call(510, b, 4);
}
static void add_filter(int kind, const char *k, const char *v, int num, int cmp) {
    AcquireSRWLockExclusive(&g_local);
    if (g_nfilters < 16) {
        Filter *f = &g_filters[g_nfilters++];
        snprintf(f->key, HKEYLEN, "%s", k ? k : ""); snprintf(f->val, sizeof f->val, "%s", v ? v : "");
        f->num = num; f->cmp = cmp; f->kind = kind;
    }
    ReleaseSRWLockExclusive(&g_local);
}
SYSV static void mm_filter_text(void *o, const char *k, const char *v, int cmp) { HT("search filter '%s' %d '%s'", short_text(k), cmp, short_text(v)); add_filter(1, k, v, 0, cmp); }   // 5
SYSV static void mm_filter_num(void *o, const char *k, int v, int cmp) { HT("search filter '%s' %d %d", short_text(k), cmp, v); add_filter(2, k, NULL, v, cmp); }       // 6
SYSV static void mm_filter_near(void *o, const char *k, int v) {}                                                 // 7
SYSV static void mm_filter_slots(void *o, int n) { add_filter(3, "", NULL, n, 0); }                               // 8
SYSV static void mm_filter_distance(void *o, int d) {}                                                            // 9
SYSV static void mm_filter_count(void *o, int n) { g_max_results = n > 0 ? n : 50; }                              // 10
SYSV static void mm_filter_compatible(void *o, uint64_t l) {}                                                     // 11
SYSV static uint64_t mm_lobby_by_index(void *o, int i) { return i >= 0 && i < g_nfound ? g_found[i] : 0; }        // 12
SYSV static uint64_t mm_create(void *o, int type, int max) {                                                      // 13
    uint8_t b[12] = {0};
    uint64_t me = player_id(g_me), id = 0;
    lock();   // a player may be in several lobbies at once (the game keeps a hidden party lobby beside the game's own)
    for (int i = 0; i < HL; i++)
        if (!g_hub->l[i].used) {
            HubLobby *l = &g_hub->l[i];
            memset(l, 0, offsetof(HubLobby, m));
            l->ndata = 0; l->next_chat = 1;
            l->id = id = 0x0186000000000000ull | (0x7E200000u + ++g_hub->next_lobby);
            l->owner = me; l->type = type; l->limit = max > 0 ? max : 250; l->joinable = 1; l->used = 1;   // 0 = Steam's own limit
            l->nmembers = 1; l->m[0].id = me; l->m[0].ndata = 0;
            break;
        }
    unlock();
    log_line("test hub: lobby %llx made (type %d, up to %d members)", (unsigned long long)id, type, max);
    put32(b, 0, id ? 1 : 2); put64(b, 4, id);   // LobbyCreated_t: result (1 OK, 2 fail), lobby
    uint64_t h = new_call(513, b, 12);
    if (id) {   // the maker is in the lobby at once: LobbyEnter_t and LobbyDataUpdate_t for it
        uint8_t e[20] = {0};
        put64(e, 0, id); put32(e, 16, 1);
        lock(); post_locked(g_me, 504, 0, e, 20); unlock();
        uint8_t d[20] = {0};
        put64(d, 0, id); put64(d, 8, id); d[16] = 1;
        lock(); post_locked(g_me, 505, 0, d, 20); unlock();
    }
    return h;
}
SYSV static uint64_t mm_join(void *o, uint64_t id) {                                                              // 14
    uint8_t e[20] = {0};
    uint64_t me = player_id(g_me);
    uint32_t answer = 2;   // EChatRoomEnterResponse: 1 success, 2 does not exist, 3 not allowed, 4 full
    lock();
    HubLobby *l = lobby_of(id);
    if (l && member_index(l, me) >= 0) answer = 1;
    else if (l && !l->joinable) answer = 3;
    else if (l && l->nmembers >= l->limit) answer = 4;
    else if (l && l->nmembers < HM) {
        HubMember *m = &l->m[l->nmembers++];
        m->id = me; m->ndata = 0;
        tell_members_locked(l, me, 1, g_me);
        answer = 1;
    }
    unlock();
    log_line("test hub: join lobby %llx answered %u (1 = in)", (unsigned long long)id, answer);
    put64(e, 0, id); put32(e, 16, answer);
    uint64_t h = new_call(504, e, 20);
    if (answer == 1) {
        uint8_t d[20] = {0};
        put64(d, 0, id); put64(d, 8, id); d[16] = 1;
        lock(); post_locked(g_me, 505, 0, d, 20); unlock();
    }
    return h;
}
SYSV static void mm_leave(void *o, uint64_t id) {                                                                 // 15
    lock(); HubLobby *l = lobby_of(id); if (l) leave_locked(l, player_id(g_me)); unlock();
    log_line("test hub: left lobby %llx", (unsigned long long)id);
}
SYSV static uint8_t mm_invite(void *o, uint64_t id, uint64_t who) {                                               // 16
    uint8_t b[24] = {0};
    put64(b, 0, player_id(g_me)); put64(b, 8, id); put64(b, 16, 325610);
    lock(); post_locked(player_of(who), 503, 0, b, 24); unlock();
    return 1;
}
SYSV static int mm_members(void *o, uint64_t id) { lock(); HubLobby *l = lobby_of(id); int n = l ? l->nmembers : 0; unlock(); HT("members of %llx: %d", (unsigned long long)id, n); return n; }   // 17
SYSV static uint64_t mm_member_by_index(void *o, uint64_t id, int i) {                                            // 18
    lock(); HubLobby *l = lobby_of(id); uint64_t r = l && i >= 0 && i < l->nmembers ? l->m[i].id : 0; unlock(); return r;
}
SYSV static const char *mm_get_data(void *o, uint64_t id, const char *k) {                                       // 19
    lock(); HubLobby *l = lobby_of(id); const char *r = ret_text(l && k ? data_get(l->data, l->ndata, k) : ""); unlock(); HT("get data %llx '%s' = '%s'", (unsigned long long)id, short_text(k), short_text(r)); return r;
}
SYSV static uint8_t mm_set_data(void *o, uint64_t id, const char *k, const char *v) {                             // 20
    lock();
    HubLobby *l = lobby_of(id);
    int ok = l && k && v && l->owner == player_id(g_me);
    HT("set data %llx '%s' = '%s' (%s)", (unsigned long long)id, short_text(k), short_text(v), ok ? "kept" : "refused");
    if (ok) {
        if (strlen(v) >= HVAL) log_line("test hub: lobby data '%s' cut to %d bytes", k, HVAL - 1);
        if (data_set(l->data, &l->ndata, HD, k, v)) tell_data_locked(l, l->id);
    }
    unlock();
    return (uint8_t)ok;
}
SYSV static int mm_data_count(void *o, uint64_t id) { lock(); HubLobby *l = lobby_of(id); int n = l ? l->ndata : 0; unlock(); return n; }   // 21
SYSV static uint8_t mm_data_by_index(void *o, uint64_t id, int i, char *k, int kc, char *v, int vc) {            // 22
    lock();
    HubLobby *l = lobby_of(id);
    int ok = l && i >= 0 && i < l->ndata;
    if (ok) { if (k && kc > 0) snprintf(k, (size_t)kc, "%s", l->data[i].key); if (v && vc > 0) snprintf(v, (size_t)vc, "%s", l->data[i].val); }
    unlock();
    return (uint8_t)ok;
}
SYSV static uint8_t mm_delete_data(void *o, uint64_t id, const char *k) {                                         // 23
    lock();
    HubLobby *l = lobby_of(id);
    int ok = 0;
    if (l && k)
        for (int i = 0; i < l->ndata; i++)
            if (!strcmp(l->data[i].key, k)) { memmove(&l->data[i], &l->data[i + 1], sizeof(HubData) * (size_t)(l->ndata - i - 1)); l->ndata--; ok = 1; tell_data_locked(l, l->id); break; }
    unlock();
    return (uint8_t)ok;
}
SYSV static const char *mm_get_member_data(void *o, uint64_t id, uint64_t who, const char *k) {                   // 24
    const char *r = "";
    lock();
    HubLobby *l = lobby_of(id);
    int m = l ? member_index(l, who) : -1;
    if (m >= 0 && k) for (int i = 0; i < l->m[m].ndata; i++) if (!strcmp(l->m[m].data[i].key, k)) { r = l->m[m].data[i].val; break; }
    r = ret_text(r);
    unlock();
    HT("get member data %llx '%s' = '%s'", (unsigned long long)who, short_text(k), short_text(r));
    return r;
}
SYSV static void mm_set_member_data(void *o, uint64_t id, const char *k, const char *v) {                         // 25
    lock();
    HubLobby *l = lobby_of(id);
    int m = l ? member_index(l, player_id(g_me)) : -1;
    HT("set member data '%s' = '%s'%s", short_text(k), short_text(v), m >= 0 ? "" : " (not a member)");
    if (m >= 0 && k && v && mdata_set(&l->m[m], k, v)) tell_data_locked(l, player_id(g_me));
    unlock();
}
SYSV static uint8_t mm_chat_send(void *o, uint64_t id, const void *body, int len) {                               // 26
    int ok = 0;
    lock();
    HubLobby *l = lobby_of(id);
    HT("lobby chat message of %d bytes", len);
    if (l && body && len >= 0 && len <= HCHATSZ && member_index(l, player_id(g_me)) >= 0) {
        HubChat *c = &l->chat[l->next_chat % HCHAT];
        c->id = l->next_chat++; c->len = (uint32_t)len; c->from = player_id(g_me);
        memcpy(c->body, body, (size_t)len);
        uint8_t b[24] = {0};   // LobbyChatMsg_t: lobby, user, entry type (1 = chat message), chat id
        put64(b, 0, id); put64(b, 8, c->from); b[16] = 1; put32(b, 20, c->id);
        for (int i = 0; i < l->nmembers; i++) post_locked(player_of(l->m[i].id), 507, 0, b, 24);
        ok = 1;
    } else if (len > HCHATSZ) log_line("test hub: lobby chat message of %d bytes refused (Steam's limit is 4 KB)", len);
    unlock();
    return (uint8_t)ok;
}
SYSV static int mm_chat_entry(void *o, uint64_t id, int chat, uint64_t *who, void *data, int cap, int *type) {    // 27
    int n = 0;
    lock();
    HubLobby *l = lobby_of(id);
    if (l && chat > 0) {
        HubChat *c = &l->chat[(uint32_t)chat % HCHAT];
        if (c->id == (uint32_t)chat) {
            n = (int)c->len < cap ? (int)c->len : cap;
            if (data && n > 0) memcpy(data, c->body, (size_t)n);
            if (who) *who = c->from;
            if (type) *type = 1;
        }
    }
    unlock();
    return n;
}
SYSV static uint8_t mm_request_data(void *o, uint64_t id) {                                                       // 28
    HT("lobby data asked for %llx", (unsigned long long)id);
    uint8_t d[20] = {0};
    lock();
    int ok = lobby_of(id) != NULL;
    put64(d, 0, id); put64(d, 8, id); d[16] = (uint8_t)ok;
    post_locked(g_me, 505, 0, d, 20);
    unlock();
    return (uint8_t)ok;
}
SYSV static void mm_set_server(void *o, uint64_t id, uint32_t ip, uint16_t port, uint64_t server) {               // 29
    lock();
    HubLobby *l = lobby_of(id);
    if (l) {
        l->ip = ip; l->port = port; l->server = server;
        uint8_t b[24] = {0};   // LobbyGameCreated_t: lobby, game server, ip, port
        put64(b, 0, id); put64(b, 8, server); put32(b, 16, ip); memcpy(b + 20, &port, 2);
        for (int i = 0; i < l->nmembers; i++) post_locked(player_of(l->m[i].id), 509, 0, b, 24);
    }
    unlock();
    log_line("test hub: lobby %llx game server set", (unsigned long long)id);
}
SYSV static uint8_t mm_get_server(void *o, uint64_t id, uint32_t *ip, uint16_t *port, uint64_t *server) {         // 30
    lock();
    HubLobby *l = lobby_of(id);
    int ok = l && (l->server || l->ip);
    if (ok) { if (ip) *ip = l->ip; if (port) *port = l->port; if (server) *server = l->server; }
    unlock();
    return (uint8_t)ok;
}
SYSV static uint8_t mm_set_limit(void *o, uint64_t id, int n) { HT("member limit %d", n); lock(); HubLobby *l = lobby_of(id); if (l) l->limit = n; unlock(); return l != NULL; }      // 31
SYSV static int mm_get_limit(void *o, uint64_t id) { lock(); HubLobby *l = lobby_of(id); int n = l ? l->limit : 0; unlock(); return n; }                    // 32
SYSV static uint8_t mm_set_type(void *o, uint64_t id, int t) { HT("lobby type %d", t); lock(); HubLobby *l = lobby_of(id); if (l) l->type = t; unlock(); return l != NULL; }        // 33
SYSV static uint8_t mm_set_joinable(void *o, uint64_t id, uint8_t j) { HT("joinable %u", j); lock(); HubLobby *l = lobby_of(id); if (l) l->joinable = j; unlock(); return l != NULL; }   // 34
SYSV static uint64_t mm_owner(void *o, uint64_t id) { lock(); HubLobby *l = lobby_of(id); uint64_t r = l ? l->owner : 0; unlock(); HT("owner of %llx: %llx", (unsigned long long)id, (unsigned long long)r); return r; }             // 35
SYSV static uint8_t mm_set_owner(void *o, uint64_t id, uint64_t who) {                                            // 36
    lock();
    HubLobby *l = lobby_of(id);
    int ok = l && l->owner == player_id(g_me) && member_index(l, who) >= 0;
    if (ok) { l->owner = who; tell_data_locked(l, l->id); }
    unlock();
    return (uint8_t)ok;
}
SYSV static uint8_t mm_set_linked(void *o, uint64_t id, uint64_t other) { return 1; }                             // 37

// ---- ISteamNetworking005: packets between players
// ATTILA_TEST_MP_LAG=ms: how long a packet takes from one copy to another (default 40, like a normal internet connection;
// 0 = at once). Without it a player's answer could overtake the host's own message about that player (seen 2026-11-13).
static int hub_lag_ms(void) {
    static int ms = -1;
    if (ms < 0) { const char *e = getenv("ATTILA_TEST_MP_LAG"); ms = e ? atoi(e) : 40; if (ms < 0) ms = 0; }
    return ms;
}
static uint8_t g_accept[HP];   // this copy has a session with player n (Steam: accepted, or we sent to them first)
SYSV static uint8_t net_send(void *o, uint64_t to, const void *data, uint32_t len, int type, int channel) {       // 0
    int t = player_of(to);   // a packet to oneself is allowed (Steam delivers it; the game sends to its whole player list)
    if (!t || (!data && len)) {
        static volatile LONG told;
        if (InterlockedIncrement(&told) <= 10) log_line("test hub: packet to %llx refused (not a test player)", (unsigned long long)to);
        return 0;
    }
    g_accept[t] = 1;   // sending to someone also takes their packets (Steam's rule)
    HT("packet to player %d: %u bytes, type %d, channel %d", t, len, type, channel);
    uint32_t total = (uint32_t)((sizeof(HubPacket) + len + 7) & ~7u);
    if (total > HRING / 4) { log_line("test hub: packet of %u bytes too large", len); return 0; }
    for (int tries = 0; tries < 3000; tries++) {
        lock();
        HubPlayer *p = &g_hub->p[t];
        if (!p->alive) { unlock(); return 0; }
        uint64_t used = p->r_tail - p->r_head, at = p->r_tail % HRING;
        uint64_t pad = HRING - at < total ? HRING - at : 0;   // not enough room before the end: skip to the start
        if (used + pad + total <= HRING) {
            if (pad) {
                HubPacket *w = (HubPacket *)(p->ring + at);
                if (pad >= sizeof(HubPacket)) { w->consumed = 1; w->total = (uint32_t)pad; w->len = 0; w->channel = -1; }
                p->r_tail += pad;
                at = 0;
            }
            HubPacket *h = (HubPacket *)(p->ring + at);
            h->from = player_id(g_me); h->channel = channel; h->len = len; h->consumed = 0; h->total = total; h->visible = GetTickCount64() + (uint64_t)hub_lag_ms();
            if (len) memcpy(h + 1, data, len);
            p->r_tail += total;
            if (t != g_me && !p->asked[g_me]) {   // the first packet from us: P2PSessionRequest_t for them
                uint8_t b[8]; put64(b, 0, player_id(g_me));
                post_locked(t, 1202, 0, b, 8);
                p->asked[g_me] = 1;
            }
            unlock();
            return 1;
        }
        unlock();
        Sleep(1);
    }
    log_line("test hub: packet to player %d dropped (their queue stayed full for 3 s)", t);
    return 0;
}
// the first packet waiting for this copy on a channel (consumed ones are skipped; the queue start moves past them). Packets
// still on their way (the test's lag) end the search: later ones were sent later, so they are on their way too.
static HubPacket *next_packet(HubPlayer *p, int channel) {
    ULONGLONG now = GetTickCount64();
    for (uint64_t at = p->r_head; at < p->r_tail;) {
        uint64_t pos = at % HRING;
        if (HRING - pos < sizeof(HubPacket)) { at += HRING - pos; continue; }
        HubPacket *h = (HubPacket *)(p->ring + pos);
        if (!h->total) break;
        if (!h->consumed && h->visible > now) break;
        if (!h->consumed && h->channel == channel) return h;
        at += h->total;
    }
    return NULL;
}
static void skip_consumed(HubPlayer *p) {
    while (p->r_head < p->r_tail) {
        uint64_t pos = p->r_head % HRING;
        if (HRING - pos < sizeof(HubPacket)) { p->r_head += HRING - pos; continue; }
        HubPacket *h = (HubPacket *)(p->ring + pos);
        if (!h->consumed || !h->total) break;
        p->r_head += h->total;
    }
}
SYSV static uint8_t net_available(void *o, uint32_t *len, int channel) {                                          // 1
    lock();
    HubPacket *h = next_packet(&g_hub->p[g_me], channel);
    if (h && len) *len = h->len;
    unlock();
    return h != NULL;
}
SYSV static uint8_t net_read(void *o, void *dest, uint32_t cap, uint32_t *len, uint64_t *from, int channel) {     // 2
    lock();
    HubPlayer *p = &g_hub->p[g_me];
    HubPacket *h = next_packet(p, channel);
    if (h) {
        uint32_t n = h->len < cap ? h->len : cap;
        if (dest && n) memcpy(dest, h + 1, n);
        if (len) *len = h->len;
        if (from) *from = h->from;
        h->consumed = 1;
        skip_consumed(p);
        HT("packet from player %d read: %u bytes, channel %d", player_of(h->from), h->len, channel);
    }
    unlock();
    return h != NULL;
}
SYSV static uint8_t net_accept(void *o, uint64_t who) { HT("session with %llx accepted", (unsigned long long)who); int t = player_of(who); if (t) g_accept[t] = 1; return t != 0; }   // 3
SYSV static uint8_t net_close(void *o, uint64_t who) {                                                            // 4
    int t = player_of(who);
    HT("session with player %d closed", t);
    if (t) { g_accept[t] = 0; lock(); g_hub->p[g_me].asked[t] = 0; unlock(); }
    return t != 0;
}
SYSV static uint8_t net_close_channel(void *o, uint64_t who, int ch) { return player_of(who) != 0; }                // 5
SYSV static uint8_t net_state(void *o, uint64_t who, uint8_t *st) {                                               // 6
    int t = player_of(who);
    if (!t) return 0;
    lock(); int alive = g_hub->p[t].alive; unlock();
    if (st) { memset(st, 0, 20); st[0] = (uint8_t)(alive && g_accept[t]); put32(st, 12, 0x7F000001); }   // P2PSessionState_t: active, connecting, error, relay, queued bytes/packets, ip, port
    return 1;
}
SYSV static uint8_t net_relay(void *o, uint8_t allow) { return 1; }                                               // 7

// ---- ISteamGameServer012 for the host (approves the test players' tickets)
static int g_gs_logged;
SYSV static uint8_t gs_true(void *o) { HT("game server call answered yes"); return 1; }
SYSV static void gs_void(void *o) {}
SYSV static void gs_logon(void *o) {                                                                              // 5, 6
    uint8_t b[4] = {0};
    lock();
    g_hub->p[g_me].server_on = 1;
    post_locked(g_me, 101, 1, b, 1);   // SteamServersConnected_t
    post_locked(g_me, 115, 1, b, 1);   // GSPolicyResponse_t: not VAC secure
    unlock();
    g_gs_logged = 1;
    log_line("test hub: game server of player %d logged on", g_me);
}
SYSV static void gs_logoff(void *o) { lock(); g_hub->p[g_me].server_on = 0; unlock(); g_gs_logged = 0; }        // 7
SYSV static uint8_t gs_logged(void *o) { return (uint8_t)g_gs_logged; }                                          // 8
SYSV static uint8_t gs_secure(void *o) { return 0; }                                                              // 9
SYSV static uint64_t gs_id(void *o) { return server_id(g_me); }                                                   // 10
SYSV static uint8_t gs_connect(void *o, uint32_t ip, const void *blob, uint32_t len, uint64_t *who) {             // 24
    uint64_t id = 0;
    if (blob && len >= 16 && !memcmp(blob, "ATHUBTKT", 8)) memcpy(&id, (const uint8_t *)blob + 8, 8);
    int t = player_of(id);
    if (who) *who = id;
    if (t) {
        uint8_t b[16]; put64(b, 0, id); put64(b, 8, id);   // GSClientApprove_t: player, owner
        lock(); post_locked(g_me, 201, 1, b, 16); unlock();
    }
    log_line("test hub: game server given the ticket of player %d: %s", t, t ? "approved" : "not a test ticket");
    return (uint8_t)(t != 0);
}
static volatile LONG g_unauth;
SYSV static uint64_t gs_unauth(void *o) { return 0x0110000100000000ull | (0x7E300000u + (unsigned)InterlockedIncrement(&g_unauth)); }   // 25
SYSV static uint32_t gs_ip(void *o) { return 0x7F000001; }                                                        // 36

// ---- ISteamUser018 / ISteamFriends015: only the identity parts; the rest goes to Steam
static void **g_user_real, **g_friends_real;
SYSV static uint8_t user_logged(void *o) { return 1; }                                                            // 1
SYSV static uint64_t user_id(void *o) { return player_id(g_me); }                                                 // 2
SYSV static int user_ticket(void *o, void *blob, int max, uint64_t server, uint32_t ip, uint16_t port, uint8_t secure) {   // 3
    if (!blob || max < 16) return 0;
    uint64_t me = player_id(g_me);
    memcpy(blob, "ATHUBTKT", 8); memcpy((uint8_t *)blob + 8, &me, 8);
    log_line("test hub: login ticket made for the game server %llx", (unsigned long long)server);
    return 16;
}
SYSV static void user_end_game(void *o, uint32_t ip, uint16_t port) {}                                            // 4
typedef SYSV uint64_t (*U64Fn)(void *, ...);
SYSV static const char *fr_name(void *o) { return g_hub->p[g_me].name; }                                          // 0
SYSV static int fr_relationship(void *o, uint64_t id) {                                                           // 5
    if (player_of(id)) return 3;
    return (int)((SYSV int (*)(void *, uint64_t))g_friends_real[5])(o, id);
}
SYSV static int fr_state(void *o, uint64_t id) {                                                                  // 6
    if (player_of(id)) return 1;
    return ((SYSV int (*)(void *, uint64_t))g_friends_real[6])(o, id);
}
// What the lobby's friends list gets from the account's real friends, never names or ids: how many, and per name its
// length and its special bytes (2026-11-14: both joining copies of a 3-player test crashed building that list).
static void fr_note_name(const char *s) {
    static volatile LONG told;
    if (!s || InterlockedIncrement(&told) > 80) return;
    int n = 0, hi = 0, four = 0, bad = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; n++, p++) {
        if (*p < 0x80) continue;
        hi++;
        int more = *p >= 0xF0 && *p < 0xF8 ? 3 : *p >= 0xE0 && *p < 0xF0 ? 2 : *p >= 0xC2 && *p < 0xE0 ? 1 : -1;
        if (more == 3) four++;
        if (more < 0) { bad++; continue; }
        for (int k = 1; k <= more; k++) if ((p[k] & 0xC0) != 0x80) { bad++; more = k - 1; break; }
        n += more; hi += more; p += more;
    }
    log_line("test hub: a real friend's name: %d bytes, %d of them not plain letters, %d 4-byte letters, %d bytes not valid UTF-8", n, hi, four, bad);
}
SYSV static const char *fr_friend_name(void *o, uint64_t id) {                                                    // 7
    int t = player_of(id);
    if (t) return g_hub->p[t].name;
    const char *r = ((SYSV const char *(*)(void *, uint64_t))g_friends_real[7])(o, id);
    fr_note_name(r);
    return r;
}
SYSV static int fr_count(void *o, int flags) {                                                                    // 3
    int n = ((SYSV int (*)(void *, int))g_friends_real[3])(o, flags);
    static volatile LONG told;
    if (InterlockedIncrement(&told) <= 20) log_line("test hub: the account's real friends asked for (flags 0x%x): %d", flags, n);
    return n;
}
SYSV static uint8_t fr_has_friend(void *o, uint64_t id, int flags) {                                              // 17
    if (player_of(id)) return 1;
    return ((SYSV uint8_t (*)(void *, uint64_t, int))g_friends_real[17])(o, id, flags);
}
SYSV static uint8_t fr_in_source(void *o, uint64_t who, uint64_t source) {                                        // 26
    lock(); HubLobby *l = lobby_of(source); int in = l && member_index(l, who) >= 0; unlock();
    if (l) return (uint8_t)in;
    return ((SYSV uint8_t (*)(void *, uint64_t, uint64_t))g_friends_real[26])(o, who, source);
}
SYSV static int fr_avatar(void *o, uint64_t id) {                                                                 // 34, 35, 36
    return 0;
}
// things that would reach the real Steam account's friends: never from a test (played-with list, rich presence, invites)
SYSV static void fr_quiet(void *o) {}                                                                             // 32, 44
SYSV static uint8_t fr_quiet_true(void *o) { return 1; }                                                          // 43, 49

// ---- making the objects: every slot logs once when the game asks for one we did not write
static void *make_object(const char *tag, void *const *mine, int n, void **base) {
    enum { SLOTS = 128 };
    void **table = calloc(SLOTS, sizeof(void *));
    void **obj = calloc(2, sizeof(void *));
    for (int s = 0; s < SLOTS; s++) table[s] = base ? base[s] : NULL;
    if (!base) {   // a full stand-in: slots without a method of ours log once and answer 0 (standin_steam_object does the same)
        extern void *standin_steam_object(int index);
        void **st = *(void ***)standin_steam_object(!strcmp(tag, "SteamNetworking005") ? 7 : !strcmp(tag, "SteamMatchMaking009") ? 5 : 3);
        for (int s = 0; s < SLOTS; s++) table[s] = st[s];
    }
    for (int s = 0; s < n && s < SLOTS; s++) if (mine[s]) table[s] = mine[s];
    obj[0] = table;
    log_line("test hub: %s answered locally", tag);
    return obj;
}
void *hub_interface(const char *version, void *real) {
    if (!hub_on()) return real;
    if (!strcmp(version, "SteamMatchMaking009")) {
        static void *const mm[38] = {
            (void *)mm_zero, (void *)mm_zero, (void *)mm_zero, (void *)mm_zero, (void *)mm_request_list, (void *)mm_filter_text,
            (void *)mm_filter_num, (void *)mm_filter_near, (void *)mm_filter_slots, (void *)mm_filter_distance, (void *)mm_filter_count,
            (void *)mm_filter_compatible, (void *)mm_lobby_by_index, (void *)mm_create, (void *)mm_join, (void *)mm_leave, (void *)mm_invite,
            (void *)mm_members, (void *)mm_member_by_index, (void *)mm_get_data, (void *)mm_set_data, (void *)mm_data_count,
            (void *)mm_data_by_index, (void *)mm_delete_data, (void *)mm_get_member_data, (void *)mm_set_member_data, (void *)mm_chat_send,
            (void *)mm_chat_entry, (void *)mm_request_data, (void *)mm_set_server, (void *)mm_get_server, (void *)mm_set_limit,
            (void *)mm_get_limit, (void *)mm_set_type, (void *)mm_set_joinable, (void *)mm_owner, (void *)mm_set_owner, (void *)mm_set_linked};
        return make_object(version, mm, 38, NULL);
    }
    if (!strcmp(version, "SteamNetworking005")) {
        static void *const net[8] = {(void *)net_send, (void *)net_available, (void *)net_read, (void *)net_accept, (void *)net_close,
                                     (void *)net_close_channel, (void *)net_state, (void *)net_relay};
        return make_object(version, net, 8, NULL);
    }
    if (!strcmp(version, "SteamUser018") && real) {
        g_user_real = *(void ***)real;
        static void *u[20];
        u[1] = (void *)user_logged; u[2] = (void *)user_id; u[3] = (void *)user_ticket; u[4] = (void *)user_end_game;
        u[19] = (void *)user_end_game;   // AdvertiseGame: would show the test's game server to the account's friends
        return make_object(version, u, 20, g_user_real);
    }
    if (!strcmp(version, "SteamFriends015") && real) {
        g_friends_real = *(void ***)real;
        static void *f[50];
        f[0] = (void *)fr_name; f[3] = (void *)fr_count; f[5] = (void *)fr_relationship; f[6] = (void *)fr_state; f[7] = (void *)fr_friend_name;
        f[17] = (void *)fr_has_friend; f[26] = (void *)fr_in_source; f[34] = f[35] = f[36] = (void *)fr_avatar;
        f[32] = f[44] = (void *)fr_quiet; f[43] = f[49] = (void *)fr_quiet_true;
        return make_object(version, f, 50, g_friends_real);
    }
    return real;
}
void *hub_game_server(void) {
    static void *o;
    if (!o) {
        static void *gs[44];
        for (int i = 0; i < 44; i++) gs[i] = (void *)gs_void;
        gs[0] = (void *)gs_true; gs[5] = gs[6] = (void *)gs_logon; gs[7] = (void *)gs_logoff; gs[8] = (void *)gs_logged;
        gs[9] = (void *)gs_secure; gs[10] = (void *)gs_id; gs[11] = (void *)mm_zero; gs[24] = (void *)gs_connect; gs[25] = (void *)gs_unauth;
        gs[27] = (void *)gs_true; gs[28] = gs[29] = gs[32] = gs[33] = gs[35] = gs[37] = gs[38] = gs[42] = gs[43] = (void *)mm_zero;
        gs[36] = (void *)gs_ip;
        o = make_object("SteamGameServer012", gs, 44, NULL);
    }
    return o;
}
void hub_game_server_stop(void) {
    if (!hub_on()) return;
    lock(); g_hub->p[g_me].server_on = 0; unlock();
    g_gs_logged = 0;
}
