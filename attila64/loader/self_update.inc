// Self-update from our own Workshop item (3834958414), like JGdaCaesar's perffix64.dll keeps itself current from his.
// Steam already downloads every new "Install 64-bit Attila.exe" to players who are subscribed; until 1.3.18 they still had
// to run it by hand. At each start (before the start window) the loader looks at that installer, and when it carries a
// NEWER version (its file version = the APP_VERSION_NUM of the release it holds) it runs it with "self-update <our folder>":
// no questions, only our own files (game file, desktop icon, Steam's Play button, settings and mod list are left alone).
// Files that are in use (this exe) are renamed to <name>.old first; the next start deletes them. Then the new attila64.exe
// is started with the same command line (ATTILA64_UPDATED=<old version>) and this process waits for it and hands on its
// exit code, so Steam sees the game running as before.
// Rules, as his update64.c: newer only (a dev or hand-installed newer build is never replaced by an older Workshop one),
// only a finished download (the file as long as its PE header says), never with attila64\update_off.txt. Any failure:
// this version starts as it is. Only release builds do it (version.h APP_SELF_UPDATE); ATTILA_TEST_SELF_UPDATE=1 turns it on
// in a development build, ATTILA_TEST_UPDATE_FROM=<installer> uses that file instead of the Workshop one (tests),
// ATTILA_NO_SELF_UPDATE=1 turns it off. The outcome is logged once the log is open (g_update_note).

static char g_update_note[600];

#define SU_WORKSHOP_ITEM "3834958414"
#define SU_INSTALLER "Install 64-bit Attila.exe"

// file version of an exe as one number (1.3.18.0 -> 0x0001000300120000); 0 = none
static unsigned long long su_file_version(const char *path) {
    HMODULE v = LoadLibraryA("version.dll");
    if (!v) return 0;
    typedef DWORD (WINAPI *SizeFn)(LPCSTR, LPDWORD);
    typedef BOOL (WINAPI *InfoFn)(LPCSTR, DWORD, DWORD, LPVOID);
    typedef BOOL (WINAPI *QueryFn)(LPCVOID, LPCSTR, LPVOID *, PUINT);
    SizeFn size_fn = (SizeFn)(void *)GetProcAddress(v, "GetFileVersionInfoSizeA");
    InfoFn info_fn = (InfoFn)(void *)GetProcAddress(v, "GetFileVersionInfoA");
    QueryFn query_fn = (QueryFn)(void *)GetProcAddress(v, "VerQueryValueA");
    unsigned long long r = 0;
    DWORD dummy = 0, n = size_fn ? size_fn(path, &dummy) : 0;
    void *buf = n && info_fn && query_fn ? malloc(n) : NULL;
    VS_FIXEDFILEINFO *fi = NULL;
    UINT len = 0;
    if (buf && info_fn(path, 0, n, buf) && query_fn(buf, "\\", (void **)&fi, &len) && fi && len >= sizeof *fi && fi->dwSignature == 0xFEEF04BD)
        r = ((unsigned long long)fi->dwFileVersionMS << 32) | fi->dwFileVersionLS;
    free(buf);
    FreeLibrary(v);
    return r;
}

// the file's identity on its drive (changes when another file is moved in under the same name); 0 = not there
static unsigned long long su_file_id(const char *path) {
    HANDLE f = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    BY_HANDLE_FILE_INFORMATION fi;
    unsigned long long id = GetFileInformationByHandle(f, &fi) ? ((unsigned long long)fi.nFileIndexHigh << 32) | fi.nFileIndexLow : 0;
    CloseHandle(f);
    return id;
}

static void su_version_text(unsigned long long v, char *out, size_t size) {
    snprintf(out, size, "%u.%u.%u", (unsigned)(v >> 48), (unsigned)(v >> 32) & 0xffff, (unsigned)(v >> 16) & 0xffff);
    if (v & 0xffff) snprintf(out + strlen(out), size - strlen(out), ".%u", (unsigned)v & 0xffff);
}

// a small "Updating..." note in the middle of the screen while the installer runs (a few seconds)
static HWND su_note_window(const char *text) {
    int w = 420, h = 70;
    HWND wnd = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, "STATIC", text, WS_POPUP | WS_BORDER | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                               (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h, NULL, NULL, NULL, NULL);
    if (wnd) {
        SendMessageA(wnd, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        UpdateWindow(wnd);
    }
    return wnd;
}

// waits for a process while keeping the note window painted; 0 = timed out
static int su_wait(HANDLE proc, DWORD ms) {
    DWORD start = GetTickCount();
    for (;;) {
        DWORD left = GetTickCount() - start >= ms ? 0 : ms - (GetTickCount() - start);
        DWORD r = MsgWaitForMultipleObjects(1, &proc, FALSE, left, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) return 1;
        if (r != WAIT_OBJECT_0 + 1) return 0;
        MSG m;
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
    }
}

// Returns -1 to go on starting this version, otherwise the exit code to end this process with (the updated game ran).
static int self_update(const char *own_dir) {
    static const unsigned short mine_parts[] = {APP_VERSION_NUM};
    unsigned long long mine = ((unsigned long long)mine_parts[0] << 48) | ((unsigned long long)mine_parts[1] << 32) |
                              ((unsigned long long)mine_parts[2] << 16) | mine_parts[3];
    char p[MAX_PATH * 2 + 64], ws[MAX_PATH * 2 + 64], a[32], b[32];
    // what an earlier update left (files that were in use then)
    static const char *const old[] = {"attila64.exe.old", "SDL2.dll.old", "steam_api64.dll.old"};
    for (size_t i = 0; i < sizeof old / sizeof old[0]; i++) {
        snprintf(p, sizeof p, "%s\\%s", own_dir, old[i]);
        DeleteFileA(p);
    }
    char was[32];
    if (GetEnvironmentVariableA("ATTILA64_UPDATED", was, sizeof was)) {   // the new version, started by the old one
        SetEnvironmentVariableA("ATTILA64_UPDATED", NULL);
        snprintf(g_update_note, sizeof g_update_note, "self-update: updated from %s by the Workshop item " SU_WORKSHOP_ITEM, was);
        return -1;
    }
#ifndef APP_SELF_UPDATE
    if (!getenv("ATTILA_TEST_SELF_UPDATE")) { snprintf(g_update_note, sizeof g_update_note, "self-update: off (development build)"); return -1; }
#endif
    if (getenv("ATTILA_NO_SELF_UPDATE")) { snprintf(g_update_note, sizeof g_update_note, "self-update: off (ATTILA_NO_SELF_UPDATE)"); return -1; }
    snprintf(p, sizeof p, "%s\\update_off.txt", own_dir);
    if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) {
        snprintf(g_update_note, sizeof g_update_note, "self-update: update_off.txt found, staying on " APP_VERSION_TEXT);
        return -1;
    }
    const char *from = getenv("ATTILA_TEST_UPDATE_FROM");
    if (from) snprintf(ws, sizeof ws, "%s", from);
    else snprintf(ws, sizeof ws, "%s\\..\\..\\..\\workshop\\content\\325610\\" SU_WORKSHOP_ITEM "\\" SU_INSTALLER, own_dir);
    if (GetFileAttributesA(ws) == INVALID_FILE_ATTRIBUTES) {
        snprintf(g_update_note, sizeof g_update_note, "self-update: no Workshop installer (not subscribed to 64-bit Attila on the Workshop?)");
        return -1;
    }
    if (!pe_complete(ws)) {
        snprintf(g_update_note, sizeof g_update_note, "self-update: Steam is still downloading the Workshop installer, checked again at the next start");
        return -1;
    }
    unsigned long long theirs = su_file_version(ws);
    su_version_text(mine, a, sizeof a);
    su_version_text(theirs, b, sizeof b);
    if (!theirs) { snprintf(g_update_note, sizeof g_update_note, "self-update: the Workshop installer has no version number, not used"); return -1; }
    if (theirs <= mine) {
        snprintf(g_update_note, sizeof g_update_note, theirs == mine ? "self-update: up to date with the Workshop item (%s)"
                                                                       : "self-update: this version (%s) is newer than the Workshop item's (%s), kept",
                 a, b);
        return -1;
    }

    // run the newer installer on our folder; its messages go to self_update_log.txt
    char self[MAX_PATH * 2 + 32];
    snprintf(self, sizeof self, "%s\\attila64.exe", own_dir);
    unsigned long long self_id = su_file_id(self);
    char cmd[MAX_PATH * 5], text[128];
    snprintf(cmd, sizeof cmd, "\"%s\" self-update \"%s\"", ws, own_dir);
    snprintf(p, sizeof p, "%s\\self_update_log.txt", own_dir);
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE out = CreateFileA(p, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (out != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = out;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    snprintf(text, sizeof text, "Updating 64-bit Attila from %s to %s...", a, b);
    HWND note = su_note_window(text);
    DWORD code = 99;
    int ran = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, own_dir, &si, &pi);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (ran) {
        if (!su_wait(pi.hProcess, 120000)) TerminateProcess(pi.hProcess, 98);
        WaitForSingleObject(pi.hProcess, 5000);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    if (note) DestroyWindow(note);
    if (!ran || code != 0) {
        snprintf(g_update_note, sizeof g_update_note, "self-update: updating to %s FAILED (%s %lu, see self_update_log.txt), starting %s as it is",
                 b, ran ? "installer exit code" : "could not start the installer, error", ran ? code : GetLastError(), a);
        return -1;
    }

    // start the new version with the same command line, and wait for it. (Asking Windows for the version of attila64.exe
    // here gives the running image's, not the new file's, so the check is that the file on disk is another one now.)
    if (su_file_id(self) == self_id) {
        snprintf(g_update_note, sizeof g_update_note, "self-update: the installer ran but attila64.exe is not %s, starting %s as it is", b, a);
        return -1;
    }
    const char *args = GetCommandLineA();   // skip our own name (quoted or not)
    if (*args == '"') { args = strchr(args + 1, '"'); args = args ? args + 1 : ""; }
    else while (*args && *args != ' ' && *args != '\t') args++;
    char *line = (char *)malloc(strlen(self) + strlen(args) + 4);
    sprintf(line, "\"%s\"%s", self, args);
    SetEnvironmentVariableA("ATTILA64_UPDATED", a);
    STARTUPINFOA si2 = {sizeof si2};
    PROCESS_INFORMATION pi2;
    if (!CreateProcessA(self, line, NULL, NULL, FALSE, 0, NULL, NULL, &si2, &pi2)) {
        // the files are already new: this old process can not go on with them (packs, picker), so say so plainly
        MessageBoxA(NULL, "64-bit Attila was updated. Please start it again.", "64-bit Attila", MB_ICONINFORMATION);
        return 0;
    }
    free(line);
    CloseHandle(pi2.hThread);
    WaitForSingleObject(pi2.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi2.hProcess, &exit_code);
    CloseHandle(pi2.hProcess);
    return (int)exit_code;
}
