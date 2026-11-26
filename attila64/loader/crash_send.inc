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

// The log without the Windows user name: the profile folder (C:\Users\<name>, wherever it is), then any other
// "Users\<name>\" path. The bare name alone is left: a short name such as "Sol" is also part of ordinary words.
static char *crash_anonymise(char *text) {
    char v[MAX_PATH], pat[MAX_PATH + 16];
    DWORD n = GetEnvironmentVariableA("USERPROFILE", v, sizeof v);
    if (n && n < sizeof v) text = crash_replace(text, v, "<profile>");
    n = GetEnvironmentVariableA("USERNAME", v, sizeof v);
    if (n && n < sizeof v) {
        snprintf(pat, sizeof pat, "Users\\%s\\", v);
        text = crash_replace(text, pat, "Users\\<user>\\");
    }
    return text;
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

// Sends the log as a file to the webhook. 1 = Discord took it.
static int crash_post(const char *text, size_t len, const char *line) {
    static const char *boundary = "----attila64crashreport7f3a";
    char json[512];
    snprintf(json, sizeof json, "{\"content\":\"Crash report: 64-bit Attila " APP_VERSION_TEXT "\\n%s\",\"allowed_mentions\":{\"parse\":[]}}", line);
    char head[1024];
    int hn = snprintf(head, sizeof head,
        "--%s\r\nContent-Disposition: form-data; name=\"payload_json\"\r\nContent-Type: application/json\r\n\r\n%s\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"files[0]\"; filename=\"attila64_log.txt\"\r\nContent-Type: text/plain\r\n\r\n",
        boundary, json, boundary);
    char tail[96];
    int tn = snprintf(tail, sizeof tail, "\r\n--%s--\r\n", boundary);
    size_t total = (size_t)hn + len + (size_t)tn;
    char *body = malloc(total);
    if (!body) return 0;
    memcpy(body, head, (size_t)hn);
    memcpy(body + hn, text, len);
    memcpy(body + hn + len, tail, (size_t)tn);

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
                       "your graphics card, Windows version and mod list. Your Windows user name is taken out. Nothing else is sent.\n\n"
                       "(To never be asked: crash_reports=off in settings.txt.)",
                    "64-bit Attila", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    char *text = last_log_text();
    if (!text) return;
    text = crash_anonymise(text);
    char line[160];
    crash_first_line(text, line, sizeof line);
    size_t len = strlen(text);
    if (len > (9u << 20)) len = 9u << 20;   // Discord takes files up to 10 MB
    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    int ok = crash_post(text, len, line);
    SetCursor(old);
    free(text);
    crash_test_note(ok ? "sent" : "not sent", 0, NULL);
    if (!crash_auto()) MessageBoxA(w, ok ? "Crash report sent. Thank you!"
                      : "The crash report could not be sent (no internet?).\nYou can still press \"Copy last log\" and post it on the Discord.",
                "64-bit Attila", ok ? MB_ICONINFORMATION : MB_ICONWARNING);
}
