// Crash reports (included by picker.c). When the mod list window opens and the last game's log has a CRASH line, the player
// is asked once: "Send the crash report?". Yes sends that log to the developer's private Discord channel (a webhook), with
// the Windows user name taken out. Nothing is sent without Yes.
// The webhook link is in crash_webhook.h, which is not published (make_upload_folder.ps1 leaves it out): without that file
// the question is never asked. It holds one line:  #define CRASH_WEBHOOK_PATH "/api/webhooks/<id>/<token>"
// settings.txt crash_reports=off: never ask. ATTILA_CRASH_SEND_TEST=1: ask even in hidden test runs.
#include <winhttp.h>
#include <ctype.h>
#include <wchar.h>
#if __has_include("crash_webhook.h")
#include "crash_webhook.h"
#endif
#ifndef CRASH_WEBHOOK_PATH
#define CRASH_WEBHOOK_PATH ""
#endif
#ifndef CRASH_WEBHOOK_HOST
#define CRASH_WEBHOOK_HOST L"discord.com"   // a test header may point it at an echo service
#endif
// ATTILA_CRASH_SEND_TEST=auto (tests only): no questions, Yes is taken, the answer goes to crash_send_test.txt.
static int crash_auto(void) { const char *t = getenv("ATTILA_CRASH_SEND_TEST"); return t && !strcmp(t, "auto"); }
static void crash_test_note(const char *what, DWORD code, const char *reply) {
    if (!crash_auto()) return;
    FILE *f = fopen("crash_send_test.txt", "ab");
    if (f) { fprintf(f, "%s %lu\n%s\n", what, (unsigned long)code, reply ? reply : ""); fclose(f); }
}

// How many letters of r match what: any case, and \ and / count as the same.
static size_t crash_match(const char *r, const char *what, size_t n) {
    size_t i = 0;
    while (i < n && r[i]) {
        char a = r[i], b = what[i];
        int slash = (a == '\\' || a == '/') && (b == '\\' || b == '/');
        if (!slash && tolower((unsigned char)a) != tolower((unsigned char)b)) break;
        i++;
    }
    return i;
}

// text with every copy of what replaced by rep (text is freed when a new copy is made; the caller frees the result).
static char *crash_replace(char *text, const char *what, const char *rep) {
    size_t n = strlen(what), rn = strlen(rep), hits = 0;
    if (!n) return text;
    for (const char *r = text; *r; r++) if (crash_match(r, what, n) == n) hits++;
    if (!hits) return text;
    char *out = malloc(strlen(text) + hits * rn + 1);
    if (!out) return text;
    char *w = out;
    for (const char *r = text; *r;) {
        if (crash_match(r, what, n) == n) { memcpy(w, rep, rn); w += rn; r += n; }
        else *w++ = *r++;
    }
    *w = 0;
    free(text);
    return out;
}

// Every Steam ID (a 17-digit number starting 7656119, not part of a longer number) becomes "<steam id>". The new text is never
// longer, so it is done in place.
static char *crash_drop_steam_ids(char *text) {
    static const char rep[] = "<steam id>";
    char *w = text;
    for (const char *r = text; *r;) {
        int at_start = r == text || !isdigit((unsigned char)r[-1]);
        size_t d = 0;
        if (at_start && !strncmp(r, "7656119", 7)) while (d < 18 && isdigit((unsigned char)r[d])) d++;
        if (d == 17) { memcpy(w, rep, sizeof rep - 1); w += sizeof rep - 1; r += 17; }
        else *w++ = *r++;
    }
    *w = 0;
    return text;
}

// The log without the Windows user name: the profile folder (C:\Users\<name>, wherever it is), then any other
// "Users\<name>\" path. The bare name alone is left: a short name such as "Sol" is also part of ordinary words.
// Steam IDs go too (since 2026-11-26; the question says nothing else of the player's is sent).
static char *crash_anonymise(char *text) {
    char v[MAX_PATH], pat[MAX_PATH + 16];
    DWORD n = GetEnvironmentVariableA("USERPROFILE", v, sizeof v);
    if (n && n < sizeof v) text = crash_replace(text, v, "<profile>");
    n = GetEnvironmentVariableA("USERNAME", v, sizeof v);
    if (n && n < sizeof v) {
        snprintf(pat, sizeof pat, "Users\\%s\\", v);
        text = crash_replace(text, pat, "Users\\<user>\\");
    }
    return crash_drop_steam_ids(text);
}

// The log's size and time, so one crash is asked about once.
static int crash_log_stamp(char *out, size_t size) {
    char path[MAX_PATH + 32];
    last_log_path(path, MAX_PATH);
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return 0;
    snprintf(out, size, "%lu %lu %lu", a.nFileSizeLow, a.ftLastWriteTime.dwLowDateTime, a.ftLastWriteTime.dwHighDateTime);
    return 1;
}
static void crash_asked_path(char *out, size_t size) {
    GetModuleFileNameA(NULL, out, (DWORD)size);
    char *s = strrchr(out, '\\');
    snprintf(s ? s + 1 : out, size - (size_t)(s ? s + 1 - out : 0), "crash_report_asked.txt");
}
static int crash_already_asked(const char *stamp) {
    char path[MAX_PATH + 32], old[128] = "";
    crash_asked_path(path, MAX_PATH);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(old, 1, sizeof old - 1, f);
    fclose(f);
    old[n] = 0;
    return !strcmp(old, stamp);
}
static void crash_mark_asked(const char *stamp) {
    char path[MAX_PATH + 32];
    crash_asked_path(path, MAX_PATH);
    FILE *f = fopen(path, "wb");
    if (f) { fputs(stamp, f); fclose(f); }
}

// The first CRASH line, for the message text: at most 150 letters, no quotes or backslashes (it goes inside JSON).
static void crash_first_line(const char *text, char *out, size_t size) {
    const char *c = !strncmp(text, "CRASH ", 6) ? text : strstr(text, "\nCRASH ");
    out[0] = 0;
    if (!c) return;
    if (*c == '\n') c++;
    size_t i = 0;
    for (; c[i] && c[i] != '\r' && c[i] != '\n' && i < size - 1 && i < 150; i++) out[i] = (c[i] == '"' || c[i] == '\\') ? '\'' : c[i];
    out[i] = 0;
}

// The crash dump the game wrote next to the log (loader.c crash_write_dump), read whole (the caller frees it), but only when
// it belongs to this log: written within 2 minutes of the log's last line, and small enough to go with it (Discord takes
// up to 10 MB in all). NULL otherwise.
static char *crash_dump_for_log(size_t *len) {
    char path[MAX_PATH + 32], dump[MAX_PATH + 32];
    last_log_path(path, MAX_PATH);
    snprintf(dump, sizeof dump, "%s", path);
    char *s = strrchr(dump, '\\');
    if (!s) return NULL;
    snprintf(s + 1, sizeof dump - (size_t)(s + 1 - dump), "attila64_crash.dmp");
    WIN32_FILE_ATTRIBUTE_DATA a, b;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a) || !GetFileAttributesExA(dump, GetFileExInfoStandard, &b)) return NULL;
    ULARGE_INTEGER ta = {{a.ftLastWriteTime.dwLowDateTime, a.ftLastWriteTime.dwHighDateTime}}, tb = {{b.ftLastWriteTime.dwLowDateTime, b.ftLastWriteTime.dwHighDateTime}};
    unsigned long long gap = ta.QuadPart > tb.QuadPart ? ta.QuadPart - tb.QuadPart : tb.QuadPart - ta.QuadPart;
    if (gap > 120ull * 10000000ull || b.nFileSizeHigh || b.nFileSizeLow > (8u << 20)) return NULL;
    FILE *f = fopen(dump, "rb");
    if (!f) return NULL;
    char *data = malloc(b.nFileSizeLow ? b.nFileSizeLow : 1);
    *len = data ? fread(data, 1, b.nFileSizeLow, f) : 0;
    fclose(f);
    if (data && !*len) { free(data); data = NULL; }
    return data;
}

// Sends the log (and the crash dump, when dump is not NULL) as files to the webhook. 1 = Discord took it.
static int crash_post(const char *text, size_t len, const char *line, const char *dump, size_t dump_len) {
    static const char *boundary = "----attila64crashreport7f3a";
    char json[512];
    snprintf(json, sizeof json, "{\"content\":\"Crash report: 64-bit Attila " APP_VERSION_TEXT "\\n%s%s\",\"allowed_mentions\":{\"parse\":[]}}",
             line, dump ? "\\n(with crash dump)" : "");
    char head[1024];
    int hn = snprintf(head, sizeof head,
        "--%s\r\nContent-Disposition: form-data; name=\"payload_json\"\r\nContent-Type: application/json\r\n\r\n%s\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"files[0]\"; filename=\"attila64_log.txt\"\r\nContent-Type: text/plain\r\n\r\n",
        boundary, json, boundary);
    char mid[256];
    int mn = dump ? snprintf(mid, sizeof mid, "\r\n--%s\r\nContent-Disposition: form-data; name=\"files[1]\"; filename=\"attila64_crash.dmp\"\r\n"
                                              "Content-Type: application/octet-stream\r\n\r\n", boundary) : 0;
    char tail[96];
    int tn = snprintf(tail, sizeof tail, "\r\n--%s--\r\n", boundary);
    size_t total = (size_t)hn + len + (size_t)mn + (dump ? dump_len : 0) + (size_t)tn;
    char *body = malloc(total);
    if (!body) return 0;
    char *w = body;
    memcpy(w, head, (size_t)hn); w += hn;
    memcpy(w, text, len); w += len;
    if (dump) { memcpy(w, mid, (size_t)mn); w += mn; memcpy(w, dump, dump_len); w += dump_len; }
    memcpy(w, tail, (size_t)tn);

    wchar_t path[256];
    MultiByteToWideChar(CP_UTF8, 0, CRASH_WEBHOOK_PATH, -1, path, 256);
    wchar_t ctype[128];
    swprintf(ctype, 128, L"Content-Type: multipart/form-data; boundary=%hs", boundary);
    int ok = 0;
    HINTERNET s = WinHttpOpen(L"64-bit Attila crash report", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, CRASH_WEBHOOK_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0) : NULL;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"POST", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    if (r) {
        WinHttpSetTimeouts(r, 10000, 10000, 30000, 30000);
        if (WinHttpSendRequest(r, ctype, (DWORD)-1L, body, (DWORD)total, (DWORD)total, 0) && WinHttpReceiveResponse(r, NULL)) {
            DWORD code = 0, sz = sizeof code;
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
            ok = code >= 200 && code < 300;
            static char reply[4096];
            DWORD got = 0, total_got = 0;
            while (total_got < sizeof reply - 1 && WinHttpReadData(r, reply + total_got, sizeof reply - 1 - total_got, &got) && got) total_got += got;
            reply[total_got] = 0;
            crash_test_note("http status", code, reply);
        }
        WinHttpCloseHandle(r);
    }
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    free(body);
    return ok;
}

// Called once when the mod list window is made.
static void crash_send_offer(HWND w) {
    if (!CRASH_WEBHOOK_PATH[0]) return;
    if (getenv("ATTILA_TEST_HIDDEN") && !getenv("ATTILA_CRASH_SEND_TEST")) return;
    char v[32];
    if (settings_get("crash_reports", v, sizeof v) && !_stricmp(v, "off")) return;
    if (!last_game_crashed()) return;
    char stamp[128];
    if (!crash_log_stamp(stamp, sizeof stamp) || crash_already_asked(stamp)) return;
    crash_mark_asked(stamp);   // asked once, whatever the answer
    if (!crash_auto() && MessageBoxA(w, "64-bit Attila crashed last time.\n\n"
                       "Send the crash report to the developer? It is the game's log: what the game did before the crash, "
                       "your graphics card, Windows version and mod list. Your Windows user name is taken out. Nothing else is sent without asking.\n\n"
                       "(To never be asked: crash_reports=off in settings.txt.)",
                    "64-bit Attila", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    char *text = last_log_text();
    if (!text) return;
    text = crash_anonymise(text);
    char line[160];
    crash_first_line(text, line, sizeof line);
    size_t len = strlen(text);
    if (len > (9u << 20)) len = 9u << 20;   // Discord takes files up to 10 MB
    // The crash dump goes only with a second Yes: it is a copy of part of the game's memory, so unlike the log it can not be
    // cleaned of names first. Not offered when log and dump together would pass 9.5 MB.
    size_t dump_len = 0;
    char *dump = crash_dump_for_log(&dump_len);
    if (dump && len + dump_len > (19u << 19)) { free(dump); dump = NULL; }
    if (dump && !crash_auto()) {
        char q[600];
        snprintf(q, sizeof q, "Also send the crash dump (%lu KB)?\n\n"
                 "It is a snapshot of part of the game's memory at the moment of the crash, and the best help for finding the cause. "
                 "It can contain your Steam name and the names of folders on this PC, and it is only seen by the developer.\n\n"
                 "No = only the log is sent.", (unsigned long)(dump_len / 1024 + 1));
        if (MessageBoxA(w, q, "64-bit Attila", MB_YESNO | MB_ICONQUESTION) != IDYES) { free(dump); dump = NULL; }
    }
    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    int ok = crash_post(text, len, line, dump, dump_len);
    SetCursor(old);
    free(dump);
    free(text);
    crash_test_note(ok ? "sent" : "not sent", 0, NULL);
    if (!crash_auto()) MessageBoxA(w, ok ? "Crash report sent. Thank you!"
                      : "The crash report could not be sent (no internet?).\nYou can still press \"Copy last log\" and post it on the Discord.",
                "64-bit Attila", ok ? MB_ICONINFORMATION : MB_ICONWARNING);
}
