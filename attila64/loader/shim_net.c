// Real sockets for the game, on Winsock, with Linux option numbers and error codes on the game's side.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <errno.h>
#include <string.h>
#include "loader.h"

typedef unsigned long long ull;

static void net_start(void) {
    static volatile LONG done;
    if (InterlockedCompareExchange(&done, 1, 0) == 0) {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    }
}

// Linux errno values the game compares against
static void set_errno_from_wsa(void) {
    int e = WSAGetLastError(), v;
    switch (e) {
    case WSAEWOULDBLOCK: v = 11; break;     // EAGAIN
    case WSAEINPROGRESS: v = 115; break;
    case WSAEALREADY: v = 114; break;
    case WSAEISCONN: v = 106; break;
    case WSAENOTCONN: v = 107; break;
    case WSAECONNREFUSED: v = 111; break;
    case WSAECONNRESET: v = 104; break;
    case WSAECONNABORTED: v = 103; break;
    case WSAEADDRINUSE: v = 98; break;
    case WSAEADDRNOTAVAIL: v = 99; break;
    case WSAENETUNREACH: v = 101; break;
    case WSAEHOSTUNREACH: v = 113; break;
    case WSAETIMEDOUT: v = 110; break;
    case WSAEMSGSIZE: v = 90; break;
    case WSAEINVAL: v = 22; break;
    case WSAENOTSOCK: v = 88; break;
    case WSAEINTR: v = 4; break;
    default: v = 5; break;                  // EIO
    }
    errno = v;
    static volatile LONG shown;
    if (v != 11 && v != 115 && InterlockedIncrement(&shown) <= 40) log_line("socket error: Winsock %d", e);
}

// Which numbers are sockets, so close() and fcntl() know what they were given. A socket's blocking state is remembered as well,
// because Linux can ask for "don't wait" on a single call.
#define MAX_SOCKS 256
static struct { int fd; int nonblocking; int connecting; } g_socks[MAX_SOCKS];
static SRWLOCK g_sock_lock = SRWLOCK_INIT;

static int sock_find(int fd) {
    for (int i = 0; i < MAX_SOCKS; i++) if (g_socks[i].fd == fd && fd) return i;
    return -1;
}
int shim_is_socket(int fd) {
    AcquireSRWLockShared(&g_sock_lock);
    int i = sock_find(fd);
    ReleaseSRWLockShared(&g_sock_lock);
    return i >= 0;
}
static void sock_add(int fd) {
    AcquireSRWLockExclusive(&g_sock_lock);
    for (int i = 0; i < MAX_SOCKS; i++) if (!g_socks[i].fd) { g_socks[i].fd = fd; g_socks[i].nonblocking = 0; g_socks[i].connecting = 0; break; }
    ReleaseSRWLockExclusive(&g_sock_lock);
}
int shim_close_socket(int fd) {
    AcquireSRWLockExclusive(&g_sock_lock);
    int i = sock_find(fd);
    if (i >= 0) g_socks[i].fd = 0;
    ReleaseSRWLockExclusive(&g_sock_lock);
    if (closesocket((SOCKET)(ull)fd)) { set_errno_from_wsa(); return -1; }
    return 0;
}
static void set_nonblocking(int fd, int on) {
    u_long v = (u_long)on;
    ioctlsocket((SOCKET)(ull)fd, FIONBIO, &v);
}
static int is_nonblocking(int fd) {
    AcquireSRWLockShared(&g_sock_lock);
    int i = sock_find(fd), r = i >= 0 ? g_socks[i].nonblocking : 0;
    ReleaseSRWLockShared(&g_sock_lock);
    return r;
}

static void set_connecting(int fd, int on) {
    AcquireSRWLockExclusive(&g_sock_lock);
    int i = sock_find(fd);
    if (i >= 0) g_socks[i].connecting = on;
    ReleaseSRWLockExclusive(&g_sock_lock);
}
// Sending or receiving while a connect is still under way: Linux says "try again", Windows says "not connected".
// If the connect has already failed, that failure is what Linux would report.
static int connect_pending(int fd) {
    if (WSAGetLastError() != WSAENOTCONN) return 0;
    AcquireSRWLockShared(&g_sock_lock);
    int i = sock_find(fd), on = i >= 0 ? g_socks[i].connecting : 0;
    ReleaseSRWLockShared(&g_sock_lock);
    if (!on) { WSASetLastError(WSAENOTCONN); return 0; }
    int err = 0, len = sizeof err;
    getsockopt((SOCKET)(ull)fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
    if (err) {
        set_connecting(fd, 0);
        WSASetLastError(err);
        set_errno_from_wsa();
    } else errno = 11;
    return 1;
}

// fcntl: only the non-blocking flag means anything for sockets
SHIM(int, fcntl, (int fd, int cmd, long long arg)) {
    enum { F_GETFL_ = 3, F_SETFL_ = 4, O_NONBLOCK_ = 0x800 };
    if (!shim_is_socket(fd)) return 0;
    if (cmd == F_GETFL_) return is_nonblocking(fd) ? O_NONBLOCK_ : 0;
    if (cmd == F_SETFL_) {
        int on = (arg & O_NONBLOCK_) != 0;
        set_nonblocking(fd, on);
        AcquireSRWLockExclusive(&g_sock_lock);
        int i = sock_find(fd);
        if (i >= 0) g_socks[i].nonblocking = on;
        ReleaseSRWLockExclusive(&g_sock_lock);
    }
    return 0;
}

SHIM(int, socket, (int domain, int type, int proto)) {
    net_start();
    SOCKET s = socket(domain, type & 0xf, proto);
    if (s == INVALID_SOCKET) { set_errno_from_wsa(); return -1; }
    if (s > 0x7fffffff) fatal("socket handle out of range");
    sock_add((int)s);
    log_line("socket: domain %d type %d protocol %d -> %d", domain, type, proto, (int)s);
    if (type & 0x800) { set_nonblocking((int)s, 1); }
    return (int)s;
}
SHIM(int, bind, (int fd, const struct sockaddr *a, unsigned n)) {
    if (bind((SOCKET)(ull)fd, a, (int)n)) { set_errno_from_wsa(); return -1; }
    return 0;
}
SHIM(int, listen, (int fd, int n)) {
    if (listen((SOCKET)(ull)fd, n)) { set_errno_from_wsa(); return -1; }
    return 0;
}
SHIM(int, accept, (int fd, struct sockaddr *a, unsigned *n)) {
    int len = n ? (int)*n : 0;
    SOCKET s = accept((SOCKET)(ull)fd, a, n ? &len : NULL);
    if (s == INVALID_SOCKET) { set_errno_from_wsa(); return -1; }
    if (n) *n = (unsigned)len;
    sock_add((int)s);
    return (int)s;
}
SHIM(int, connect, (int fd, const struct sockaddr *a, unsigned n)) {
    if (connect((SOCKET)(ull)fd, a, (int)n)) {
        int e = WSAGetLastError();
        // a non-blocking connect is reported as "in progress" on Linux
        if (e == WSAEWOULDBLOCK) { set_connecting(fd, 1); errno = 115; return -1; }
        WSASetLastError(e);
        set_errno_from_wsa();
        return -1;
    }
    return 0;
}

// Linux flag bits: MSG_PEEK 2 is shared; MSG_DONTWAIT 0x40 and MSG_NOSIGNAL 0x4000 have no Winsock equal.
enum { LX_MSG_PEEK = 2, LX_MSG_DONTWAIT = 0x40 };
#define ONE_CALL_NONBLOCK_BEGIN(fd, fl) int _nb = ((fl) & LX_MSG_DONTWAIT) && !is_nonblocking(fd); if (_nb) set_nonblocking(fd, 1)
#define ONE_CALL_NONBLOCK_END(fd) if (_nb) set_nonblocking(fd, 0)

SHIM(long long, send, (int fd, const char *d, size_t n, int fl)) {
    ONE_CALL_NONBLOCK_BEGIN(fd, fl);
    int r = send((SOCKET)(ull)fd, d, (int)n, 0);
    if (r < 0) { if (!connect_pending(fd)) set_errno_from_wsa(); }
    else set_connecting(fd, 0);
    ONE_CALL_NONBLOCK_END(fd);
    return r;
}
SHIM(long long, recv, (int fd, char *d, size_t n, int fl)) {
    ONE_CALL_NONBLOCK_BEGIN(fd, fl);
    int r = recv((SOCKET)(ull)fd, d, (int)n, fl & LX_MSG_PEEK);
    if (r < 0) {
        // Windows reports a datagram that did not fit as an error; Linux just cuts it
        if (WSAGetLastError() == WSAEMSGSIZE) r = (int)n;
        else if (!connect_pending(fd)) set_errno_from_wsa();
    }
    else set_connecting(fd, 0);
    ONE_CALL_NONBLOCK_END(fd);
    return r;
}
SHIM(long long, sendto, (int fd, const char *d, size_t n, int fl, const struct sockaddr *a, unsigned al)) {
    ONE_CALL_NONBLOCK_BEGIN(fd, fl);
    int r = sendto((SOCKET)(ull)fd, d, (int)n, 0, a, (int)al);
    if (r < 0) set_errno_from_wsa();
    ONE_CALL_NONBLOCK_END(fd);
    return r;
}
SHIM(long long, recvfrom, (int fd, char *d, size_t n, int fl, struct sockaddr *a, unsigned *al)) {
    int len = al ? (int)*al : 0;
    ONE_CALL_NONBLOCK_BEGIN(fd, fl);
    int r = recvfrom((SOCKET)(ull)fd, d, (int)n, fl & LX_MSG_PEEK, a, al ? &len : NULL);
    if (r < 0) {
        int e = WSAGetLastError();
        // a UDP "port unreachable" from an earlier send is not an error for this receive on Linux
        if (e == WSAECONNRESET) errno = 11;
        else if (e == WSAEMSGSIZE) r = (int)n;
        else set_errno_from_wsa();
    }
    if (al) *al = (unsigned)len;
    ONE_CALL_NONBLOCK_END(fd);
    return r;
}
SHIM(int, setsockopt, (int fd, int level, int name, const void *v, unsigned n)) {
    enum { LX_SOL_SOCKET = 1, LX_REUSEADDR = 2, LX_BROADCAST = 6, LX_SNDBUF = 7, LX_RCVBUF = 8, LX_KEEPALIVE = 9, LX_LINGER = 13 };
    if (level == LX_SOL_SOCKET) {
        level = SOL_SOCKET;
        switch (name) {
        case LX_REUSEADDR: name = SO_REUSEADDR; break;
        case LX_BROADCAST: name = SO_BROADCAST; break;
        case LX_SNDBUF: name = SO_SNDBUF; break;
        case LX_RCVBUF: name = SO_RCVBUF; break;
        case LX_KEEPALIVE: name = SO_KEEPALIVE; break;
        case LX_LINGER: name = SO_LINGER; break;
        default: log_line("setsockopt option %d not mapped", name); return 0;
        }
    }
    if (setsockopt((SOCKET)(ull)fd, level, name, v, (int)n)) { set_errno_from_wsa(); return -1; }
    return 0;
}
SHIM(int, shutdown, (int fd, int how)) {
    if (shutdown((SOCKET)(ull)fd, how)) { set_errno_from_wsa(); return -1; }
    return 0;
}
SHIM(int, gethostname, (char *d, size_t n)) { net_start(); return gethostname(d, (int)n); }
SHIM(unsigned, inet_addr, (const char *s)) { return inet_addr(s); }
SHIM(const char *, inet_ntoa, (unsigned a)) { struct in_addr i; i.s_addr = a; return inet_ntoa(i); }
SHIM(int, inet_pton, (int af, const char *s, void *d)) { net_start(); return inet_pton(af, s, d); }

// struct hostent: the type and length fields are ints on Linux and shorts on Windows
typedef struct { char *name; char **aliases; int addrtype, length; char **addr_list; } HostentL;
SHIM(HostentL *, gethostbyname, (const char *n)) {
    static __thread HostentL out;
    net_start();
    struct hostent *h = gethostbyname(n);
    if (!h) { log_line("gethostbyname failed: %s", n); return NULL; }
    out.name = h->h_name;
    out.aliases = h->h_aliases;
    out.addrtype = h->h_addrtype;
    out.length = h->h_length;
    out.addr_list = h->h_addr_list;
    return &out;
}
