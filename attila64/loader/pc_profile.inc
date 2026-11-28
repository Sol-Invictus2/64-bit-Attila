// The player's PC in the log ("PC:" lines), so every log and crash report says what the game ran on. Part of loader.c
// (included there). Written by a background thread at the start (pc_profile_start), so the game does not wait for it;
// programs that put themselves inside the game (overlays, recorders, ReShade) are listed 45 s after the start and again
// at 5 minutes when there are new ones. Nothing personal: no computer name, user name, serial numbers or monitor names;
// folders only from "steamapps" on. ATTILA_NO_PC_PROFILE=1: none of it.
#include <cpuid.h>
#include <winioctl.h>

static void pc_line(const char *fmt, ...) {
    char b[1024];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    log_line("PC: %s", b);
}

static void pc_windows(void) {
    typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOW *);
    RtlGetVersionFn get = (RtlGetVersionFn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW v;
    memset(&v, 0, sizeof v);
    v.dwOSVersionInfoSize = sizeof v;
    if (get) get(&v);
    char shown[64] = "", product[128] = "";
    DWORD ubr = 0, n;
    HKEY k;
    if (!RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &k)) {
        n = sizeof shown; RegQueryValueExA(k, "DisplayVersion", NULL, NULL, (BYTE *)shown, &n);
        n = sizeof product; RegQueryValueExA(k, "ProductName", NULL, NULL, (BYTE *)product, &n);
        n = sizeof ubr; RegQueryValueExA(k, "UBR", NULL, NULL, (BYTE *)&ubr, &n);
        RegCloseKey(k);
    }
    // ProductName still says "Windows 10" on Windows 11; build 22000 and up is 11
    const char *name = v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000 ? "Windows 11" : product[0] ? product : "Windows";
    if (!strncmp(product, "Windows 10", 10) && v.dwBuildNumber >= 22000) {
        static char fixed[128];
        snprintf(fixed, sizeof fixed, "Windows 11%s", product + 10);
        name = fixed;
    }
    // Wine / Proton say so through ntdll
    const char *wine = GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version") ? " (Wine / Proton)" : "";
    pc_line("Windows: %s %s, build %lu.%lu%s", name, shown, (unsigned long)v.dwBuildNumber, (unsigned long)ubr, wine);
    // hardware-accelerated GPU scheduling (frame generation needs it) and Game Mode
    DWORD hags = 0, gm = 1, sz = sizeof hags;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", "HwSchMode", RRF_RT_REG_DWORD, NULL, &hags, &sz)) hags = 9;
    sz = sizeof gm;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Microsoft\\GameBar", "AutoGameModeEnabled", RRF_RT_REG_DWORD, NULL, &gm, &sz)) gm = 1;   // default on
    // power plan
    const char *plan = "unknown";
    HMODULE pp = LoadLibraryA("powrprof.dll");
    if (pp) {
        typedef DWORD (WINAPI *ActiveFn)(HKEY, GUID **);
        ActiveFn act = (ActiveFn)(void *)GetProcAddress(pp, "PowerGetActiveScheme");
        GUID *g = NULL;
        if (act && !act(NULL, &g) && g) {
            unsigned long d = g->Data1;
            plan = d == 0x381b4222 ? "Balanced" : d == 0x8c5e7fda ? "High performance" : d == 0xa1841308 ? "Power saver"
                 : d == 0xe9a42b02 ? "Ultimate performance" : "a custom plan";
            LocalFree(g);
        }
        FreeLibrary(pp);
    }
    pc_line("Windows settings: hardware GPU scheduling %s, Game Mode %s, power plan %s",
            hags == 2 ? "on" : hags == 1 ? "off" : "not set (Windows' default)", gm ? "on" : "off", plan);
}

static void pc_cpu(void) {
    unsigned a, b, c, d;
    char brand[49] = "";
    if (__get_cpuid(0x80000000, &a, &b, &c, &d) && a >= 0x80000004) {
        unsigned *w = (unsigned *)brand;
        for (unsigned i = 0; i < 3; i++) __get_cpuid(0x80000002 + i, &w[i * 4], &w[i * 4 + 1], &w[i * 4 + 2], &w[i * 4 + 3]);
    }
    char *t = brand;
    while (*t == ' ') t++;
    for (size_t k = strlen(t); k && t[k - 1] == ' '; k--) t[k - 1] = 0;
    // features the OS also allows (AVX needs XGETBV saying the registers are saved)
    int sse42 = 0, avx = 0, avx2 = 0, avx512 = 0;
    if (__get_cpuid(1, &a, &b, &c, &d)) {
        sse42 = (c >> 20) & 1;
        if ((c >> 27) & 1 && (c >> 28) & 1) {
            unsigned lo, hi;
            __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
            avx = (lo & 6) == 6;
            if (avx && __get_cpuid_count(7, 0, &a, &b, &c, &d)) {
                avx2 = (b >> 5) & 1;
                avx512 = (b >> 16) & 1 && (lo & 0xe6) == 0xe6;
            }
        }
    }
    // cores, threads, performance / efficiency cores (Intel 12th gen and up), biggest L3 cache (AMD X3D shows here)
    int cores = 0, threads = 0, eff_max = 0, per_class[8] = {0};
    unsigned long long l3 = 0;
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationAll, NULL, &len);
    uint8_t *buf = len ? malloc(len) : NULL;
    if (buf && GetLogicalProcessorInformationEx(RelationAll, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)buf, &len)) {
        for (uint8_t *p = buf; p < buf + len;) {
            SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *e = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)p;
            if (e->Relationship == RelationProcessorCore) {
                cores++;
                int th = 0;
                for (int g = 0; g < e->Processor.GroupCount; g++) th += __builtin_popcountll(e->Processor.GroupMask[g].Mask);
                threads += th;
                int cls = e->Processor.EfficiencyClass & 7;
                per_class[cls]++;
                if (cls > eff_max) eff_max = cls;
            } else if (e->Relationship == RelationCache && e->Cache.Level == 3 && e->Cache.CacheSize > l3) l3 = e->Cache.CacheSize;
            p += e->Size;
        }
    }
    free(buf);
    char split[64] = "";
    if (eff_max > 0) snprintf(split, sizeof split, " (%d performance + %d efficiency)", per_class[eff_max], cores - per_class[eff_max]);
    pc_line("processor: %s, %d cores%s, %d threads, L3 %llu MB; SSE4.2 %s, AVX %s, AVX2 %s, AVX-512 %s", t[0] ? t : "unknown",
            cores, split, threads, l3 >> 20, sse42 ? "yes" : "no", avx ? "yes" : "no", avx2 ? "yes" : "no", avx512 ? "yes" : "no");
}

static void pc_memory(void) {
    MEMORYSTATUSEX m;
    m.dwLength = sizeof m;
    if (!GlobalMemoryStatusEx(&m)) return;
    pc_line("memory: RAM %.1f GB (%.1f GB free now), RAM + page file %.1f GB", m.ullTotalPhys / 1073741824.0,
            m.ullAvailPhys / 1073741824.0, m.ullTotalPageFile / 1073741824.0);
}

static void pc_graphics_cards(void) {
    IDXGIFactory1 *f = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f)) || !f) { pc_line("graphics card: DXGI not available"); return; }
    IDXGIAdapter1 *ad;
    for (UINT i = 0; f->lpVtbl->EnumAdapters1(f, i, &ad) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        if (ad->lpVtbl->GetDesc1(ad, &d) == S_OK && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            char name[256];
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, NULL, NULL);
            LARGE_INTEGER umd;
            char drv[48] = "unknown";
            if (ad->lpVtbl->CheckInterfaceSupport(ad, &IID_IDXGIDevice, &umd) == S_OK)
                snprintf(drv, sizeof drv, "%u.%u.%u.%u", (unsigned)HIWORD(umd.HighPart), (unsigned)LOWORD(umd.HighPart),
                         (unsigned)HIWORD(umd.LowPart), (unsigned)LOWORD(umd.LowPart));
            const char *maker = d.VendorId == 0x10de ? "NVIDIA" : d.VendorId == 0x1002 ? "AMD" : d.VendorId == 0x8086 ? "Intel" : "other";
            // count the outputs: a card with none on a laptop is the one doing the work behind the screen card
            int outs = 0;
            IDXGIOutput *o;
            while (ad->lpVtbl->EnumOutputs(ad, (UINT)outs, &o) == S_OK) { o->lpVtbl->Release(o); outs++; }
            pc_line("graphics card %u: %s (%s, id %04x:%04x), %llu MB own memory, %llu MB shared, driver %s, %d screen(s) on it", i, name, maker,
                    d.VendorId, d.DeviceId, (unsigned long long)(d.DedicatedVideoMemory >> 20), (unsigned long long)(d.SharedSystemMemory >> 20), drv, outs);
        }
        ad->lpVtbl->Release(ad);
    }
    f->lpVtbl->Release(f);
}

static void pc_screens(void) {
    DISPLAY_DEVICEA dd;
    int n = 0;
    for (DWORD i = 0;; i++) {
        memset(&dd, 0, sizeof dd);
        dd.cb = sizeof dd;
        if (!EnumDisplayDevicesA(NULL, i, &dd, 0)) break;
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        DEVMODEA dm;
        memset(&dm, 0, sizeof dm);
        dm.dmSize = sizeof dm;
        if (!EnumDisplaySettingsA(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) continue;
        pc_line("screen %d: %lux%lu at %lu Hz, %lu-bit colour%s", ++n, dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency, dm.dmBitsPerPel,
                dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE ? ", main screen" : "");
    }
    if (!n) pc_line("screens: none found");
}

static void pc_power(void) {
    SYSTEM_POWER_STATUS s;
    if (!GetSystemPowerStatus(&s)) return;
    if (s.BatteryFlag == 128 || s.BatteryFlag == 255) { pc_line("power: no battery (desktop PC)"); return; }
    pc_line("power: battery (laptop?), %s, battery %u%%%s", s.ACLineStatus == 1 ? "plugged in" : s.ACLineStatus == 0 ? "ON BATTERY" : "plug unknown",
            (unsigned)s.BatteryLifePercent, s.SystemStatusFlag ? ", battery saver ON" : "");
}

// The drive the game is on: SSD or hard disk, how it is connected, free space.
static void pc_drive(const char *game) {
    char vol[MAX_PATH] = "", dev[16];
    ULARGE_INTEGER freeb, total;
    if (!GetVolumePathNameA(game, vol, sizeof vol) || !vol[0]) return;
    double free_gb = GetDiskFreeSpaceExA(vol, &freeb, &total, NULL) ? freeb.QuadPart / 1073741824.0 : -1;
    const char *kind = "unknown", *bus = "unknown";
    if (vol[1] == ':') {
        snprintf(dev, sizeof dev, "\\\\.\\%c:", vol[0]);
        HANDLE h = CreateFileA(dev, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            STORAGE_PROPERTY_QUERY q;
            memset(&q, 0, sizeof q);
            q.PropertyId = StorageDeviceSeekPenaltyProperty;
            q.QueryType = PropertyStandardQuery;
            DEVICE_SEEK_PENALTY_DESCRIPTOR sp;
            DWORD got;
            if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &sp, sizeof sp, &got, NULL) && got >= sizeof sp)
                kind = sp.IncursSeekPenalty ? "hard disk" : "SSD";
            q.PropertyId = StorageDeviceProperty;
            uint8_t out[1024];
            if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, out, sizeof out, &got, NULL) && got >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
                STORAGE_BUS_TYPE t = ((STORAGE_DEVICE_DESCRIPTOR *)out)->BusType;
                bus = t == BusTypeNvme ? "NVMe" : t == BusTypeSata ? "SATA" : t == BusTypeUsb ? "USB" : t == BusTypeSas ? "SAS"
                    : t == BusTypeRAID ? "RAID" : t == BusTypeSd || t == BusTypeMmc ? "memory card" : "other";
            }
            CloseHandle(h);
        }
    }
    pc_line("game drive: %s, %s, %.0f GB free", kind, bus, free_gb);
}

// The version marker in a DLL (e.g. JGdaCaesar's "AttilaCPUPerfFix-version:750"), 0 when there is none.
static int pc_marker(const char *path, const char *mark) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    static char buf[1 << 16];
    size_t ml = strlen(mark), keep = 0, n;
    int ver = 0;
    while (!ver && (n = fread(buf + keep, 1, sizeof buf - keep - 1, f)) > 0) {
        n += keep;
        buf[n] = 0;
        for (size_t i = 0; i + ml < n; i++)
            if (!memcmp(buf + i, mark, ml)) { ver = atoi(buf + i + ml); if (!ver) ver = -1; break; }
        keep = n > ml ? ml : 0;
        memmove(buf, buf + n - keep, keep);
    }
    fclose(f);
    return ver;
}

// Files that change how normal Attila runs, in the game folder: DLLs that are not CA's own, ReShade, the upscaler.
static void pc_game_folder(const char *game) {
    static const char *const ca[] = {"avcodec-53.dll", "avformat-53.dll", "avutil-51.dll", "avutil-ttv-51.dll", "clockwork.Release.x86.dll",
        "clockwork_crossplatform_eos.Release.x86.dll", "clockwork_eos.Release.x86.dll", "clockwork_steam.Release.x86.dll",
        "EOSSDK-Win32-Shipping.dll", "XCurl.dll", "empire.retail.dll", "icudt.dll", "icudt42.dll", "libeay32.dll", "libmfxsw32.dll",
        "libmp3lame-ttv.dll", "ssleay32.dll", "steam_api.dll", "swresample-ttv-0.dll", "twitchsdk_32_release.dll", NULL};
    char pat[MAX_PATH * 2], list[900] = "", p[MAX_PATH * 2];
    size_t used = 0;
    WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s\\*.dll", game);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            int known = 0;
            for (int i = 0; ca[i]; i++) if (!_stricmp(fd.cFileName, ca[i])) known = 1;
            if (known) continue;
            char extra[64] = "";
            snprintf(p, sizeof p, "%s\\%s", game, fd.cFileName);
            int v = pc_marker(p, "AttilaCPUPerfFix-version:");
            if (v > 0) snprintf(extra, sizeof extra, " (JGdaCaesar engine fix %d)", v);
            used += (size_t)snprintf(list + used, used < sizeof list ? sizeof list - used : 0, "%s%s%s", used ? ", " : "", fd.cFileName, extra);
        } while (FindNextFileA(h, &fd) && used < sizeof list);
        FindClose(h);
    }
    pc_line("game folder, DLLs not from CA: %s", list[0] ? list : "none");
    // ReShade, and JGdaCaesar's upscaler (FrameBridge, or moved out by the 64-bit tick)
    snprintf(p, sizeof p, "%s\\ReShade.ini", game);
    int reshade = GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
    char ver[64] = "", fb[MAX_PATH * 2];
    snprintf(fb, sizeof fb, "%s\\FrameBridge\\version.txt", game);
    FILE *f = fopen(fb, "rb");
    int in_game = f != NULL;
    if (f) { size_t n = fread(ver, 1, sizeof ver - 1, f); ver[n] = 0; fclose(f); ver[strcspn(ver, "\r\n")] = 0; }
    if (!_strnicmp(ver, "addon_version=", 14)) memmove(ver, ver + 14, strlen(ver + 14) + 1);
    char addon[32] = "";
    snprintf(fb, sizeof fb, "%s\\FrameBridge\\settings.txt", game);
    if ((f = fopen(fb, "r"))) {
        char line[256];
        while (fgets(line, sizeof line, f)) if (!_strnicmp(line, "addon=", 6)) { snprintf(addon, sizeof addon, "%s", line + 6); addon[strcspn(addon, "\r\n")] = 0; }
        fclose(f);
    }
    pc_line("ReShade: %s; upscaler (FrameBridge): %s%s%s%s", reshade ? "installed (ReShade.ini in the game folder)" : "no",
            in_game ? "in the game folder" : "not in the game folder", ver[0] ? ", version " : "", ver, addon[0] ? (strcmp(addon, "off") ? ", switched on" : ", switched off") : "");
}

// The 64-bit start window's choices (attila64\settings.txt), all on one line.
static void pc_settings(const char *exe_dir) {
    char p[MAX_PATH * 2], line[256], all[900] = "";
    size_t used = 0;
    snprintf(p, sizeof p, "%s\\settings.txt", exe_dir);
    FILE *f = fopen(p, "r");
    if (!f) { pc_line("64-bit settings: no settings.txt"); return; }
    while (fgets(line, sizeof line, f) && used < sizeof all) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#' || line[0] == ';') continue;
        used += (size_t)snprintf(all + used, sizeof all - used, "%s%s", used ? " " : "", line);
    }
    fclose(f);
    pc_line("64-bit settings: %s", all[0] ? all : "(empty)");
}

// Programs inside the game that are not Windows' own and not the game's: overlays (Steam, Discord, RivaTuner, MSI
// Afterburner, NVIDIA / AMD), recorders (OBS), ReShade, antivirus hooks. Names only. seen: those already written.
static char g_pc_seen[4096];
static void pc_programs_inside(const char *when) {
    char windir[MAX_PATH] = "", exe_dir[MAX_PATH], list[1400] = "";
    GetWindowsDirectoryA(windir, sizeof windir);
    GetModuleFileNameA(NULL, exe_dir, sizeof exe_dir);
    char *s = strrchr(exe_dir, '\\');
    if (s) *s = 0;
    size_t wl = strlen(windir), el = strlen(exe_dir), used = 0;
    int count = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32 me;
    me.dwSize = sizeof me;
    for (BOOL ok = Module32First(snap, &me); ok; ok = Module32Next(snap, &me)) {
        if (!_strnicmp(me.szExePath, windir, wl)) continue;    // Windows' own
        if (!_strnicmp(me.szExePath, exe_dir, el)) continue;   // ours (attila64 folder, plugins are logged on their own)
        char key[300];
        snprintf(key, sizeof key, "|%s|", me.szModule);
        if (strstr(g_pc_seen, key)) continue;
        if (strlen(g_pc_seen) + strlen(key) < sizeof g_pc_seen) strcat(g_pc_seen, key);
        // its folder's name too, which usually names the program (e.g. "RivaTuner Statistics Server")
        char dir[MAX_PATH], *d = dir;
        snprintf(dir, sizeof dir, "%s", me.szExePath);
        char *e = strrchr(dir, '\\');
        if (e) { *e = 0; char *f = strrchr(dir, '\\'); d = f ? f + 1 : dir; }
        used += (size_t)snprintf(list + used, used < sizeof list ? sizeof list - used : 0, "%s%s (%s)", used ? ", " : "", me.szModule, d);
        count++;
    }
    CloseHandle(snap);
    if (count) pc_line("programs inside the game (%s): %s", when, list);
    else if (!strcmp(when, "45 s after the start")) pc_line("programs inside the game (%s): none besides Windows and the game", when);
}

static DWORD WINAPI pc_profile_thread(void *unused) {
    (void)unused;
    char exe_dir[MAX_PATH], game[MAX_PATH + 8];
    GetModuleFileNameA(NULL, exe_dir, sizeof exe_dir);
    char *s = strrchr(exe_dir, '\\');
    if (s) *s = 0;
    snprintf(game, sizeof game, "%s\\..", exe_dir);
    pc_windows();
    pc_cpu();
    pc_memory();
    pc_graphics_cards();
    pc_screens();
    pc_power();
    pc_drive(exe_dir);
    pc_game_folder(game);
    pc_settings(exe_dir);
    Sleep(45000);
    pc_programs_inside("45 s after the start");
    Sleep(255000);
    pc_programs_inside("new ones at 5 minutes");
    return 0;
}

// The packs the game was given, in load order: the "mod" lines of used_mods_64.txt (in the game folder, the current folder
// at the start; loader.c writes it before the log opens, so its own lines there never reach the log). File names only.
// A crash report of 1.3.30 (a mod's grand campaign) could not be followed up because the log never named the mods.
static void pc_mod_list(void) {
    FILE *f = fopen("used_mods_64.txt", "r");
    if (!f) { log_line("mods (load order): none (no used_mods_64.txt)"); return; }
    char line[1024], *all = malloc(16384);
    size_t used = 0;
    int n = 0;
    if (all) all[0] = 0;
    while (all && fgets(line, sizeof line, f)) {
        char *q = !strncmp(line, "mod ", 4) ? strchr(line, '"') : NULL;
        if (!q) continue;
        char *e = strchr(q + 1, '"');
        if (!e) continue;
        *e = 0;
        char *name = q + 1, *sl = strrchr(name, '\\');   // never a folder
        if (sl) name = sl + 1;
        if ((sl = strrchr(name, '/'))) name = sl + 1;
        if (used + strlen(name) + 3 < 16384) used += (size_t)snprintf(all + used, 16384 - used, "%s%s", used ? ", " : "", name);
        n++;
    }
    fclose(f);
    if (all) log_line("mods (load order, %d packs): %s", n, n ? all : "none");
    free(all);
}

static void pc_profile_start(void) {
    if (getenv("ATTILA_NO_PC_PROFILE")) return;
    pc_mod_list();
    HANDLE t = CreateThread(NULL, 256 << 10, pc_profile_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}
