// Temporary stand-ins: network and web calls fail politely, and Steam interfaces Steam no longer offers answer zero while
// logging what was asked. Real sockets and curl replace these later.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <errno.h>
#include <string.h>
#include "loader.h"

SHIM(int, poll, (void)) { errno = 38; return -1; }

// The web calls are logged so a test shows whether the multiplayer service depends on them.
#define ZERO(sym) SHIM(long long, sym, (void)) { \
    static int seen; \
    if (!seen++) log_line("web stand-in: " #sym " called"); \
    return 0; \
}
ZERO(curl_easy_cleanup) ZERO(curl_easy_getinfo) ZERO(curl_easy_init) ZERO(curl_easy_perform) ZERO(curl_easy_reset)
ZERO(curl_easy_setopt) ZERO(curl_formadd) ZERO(curl_formfree) ZERO(curl_global_init) ZERO(curl_multi_add_handle)
ZERO(curl_multi_cleanup) ZERO(curl_multi_info_read) ZERO(curl_multi_init) ZERO(curl_multi_perform) ZERO(curl_multi_remove_handle)
ZERO(curl_slist_append) ZERO(curl_slist_free_all)

static const char *const g_names[12] = {"SteamApps", "SteamController", "SteamFriends", "SteamGameServer", "SteamHTMLSurface", "SteamMatchmaking",
                                        "SteamMatchmakingServers", "SteamNetworking", "SteamRemoteStorage", "SteamUser", "SteamUserStats", "SteamUtils"};
static uint8_t g_seen[12][256];

SYSV static long long standin_slot_called(unsigned long long id) {
    unsigned i = (unsigned)(id >> 16), s = (unsigned)(id & 0xffff);
    if (!g_seen[i][s]) {
        g_seen[i][s] = 1;
        log_line("Steam stand-in: %s method %u asked", g_names[i], s);
    }
    return 0;
}

// An object whose every method logs its slot number once and returns 0.
void *standin_steam_object(int index) {
    enum { SLOTS = 256, STUB = 24 };
    static void *made[12];
    if (made[index]) return made[index];
    uint8_t *code = VirtualAlloc(NULL, SLOTS * STUB, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    void **table = calloc(SLOTS, sizeof(void *));
    void **obj = calloc(4, sizeof(void *));
    for (int s = 0; s < SLOTS; s++) {
        uint8_t *p = code + s * STUB;
        unsigned long long id = ((unsigned long long)index << 16) | (unsigned)s;
        p[0] = 0x48; p[1] = 0xBF;               // mov rdi, id
        memcpy(p + 2, &id, 8);
        p[10] = 0x48; p[11] = 0xB8;             // mov rax, handler
        void *h = (void *)standin_slot_called;
        memcpy(p + 12, &h, 8);
        p[20] = 0xFF; p[21] = 0xE0;             // jmp rax
        table[s] = p;
    }
    obj[0] = table;
    made[index] = obj;
    return obj;
}
