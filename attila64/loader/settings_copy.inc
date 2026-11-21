// The one-time copy of graphics settings from normal Attila into the 64-bit game's settings.
// Part of loader.c (included there, not built by itself; the order of the includes matters).

// ---- Graphics settings from normal Attila, once. The 64-bit game keeps its own settings (HOME = %USERPROFILE%\attila64_home, see
// shim_environ), so it started on the game's automatic choice, often lower than what the player had set in normal Attila. Once per
// settings folder (marker file next to the settings), the graphics lines of normal Attila's
// %APPDATA%\The Creative Assembly\Attila\scripts\preferences.script.txt replace the same lines of the 64-bit file, or start that file
// when there is none yet. Only what the graphics options screen sets: nothing about the renderer, shader model, GPU or video memory.
// The 64-bit file as it was is kept as preferences.script.txt.before_copy. Deleting the marker copies again at the next start.
// ATTILA_NO_SETTINGS_COPY=1 skips it.
static const char *const k_copied_settings[] = {
    "x_res", "y_res", "gfx_fullscreen", "gfx_vsync", "gfx_aa", "gfx_texture_filtering", "gfx_texture_quality", "gfx_ssao",
    "gfx_distortion", "gfx_depth_of_field", "gfx_unlimited_video_memory", "gfx_sky_quality", "gfx_unit_quality",
    "gfx_building_quality", "gfx_water_quality", "gfx_shadow_quality", "gfx_tree_quality", "gfx_grass_quality",
    "gfx_terrain_quality", "gfx_unit_size", "gfx_fleet_size", "gfx_screen_space_reflections", "gfx_gamma_setting",
    "gfx_brightness_setting", "gfx_effects_quality", "gfx_vignette", "gfx_blood_effects", "porthole_3d", "proximity_fading",
};
#define N_COPIED_SETTINGS ((int)(sizeof k_copied_settings / sizeof k_copied_settings[0]))

// A whole settings file as narrow text (a UTF-16 one is turned into UTF-8). NULL if it is missing or odd.
static char *read_settings_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = n > 0 && n < (4L << 20) ? malloc((size_t)n + 2) : NULL;
    size_t got = b ? fread(b, 1, (size_t)n, f) : 0;
    fclose(f);
    if (!b) return NULL;
    b[got] = 0;
    b[got + 1] = 0;
    if (got >= 2 && (uint8_t)b[0] == 0xFF && (uint8_t)b[1] == 0xFE) {
        int wn = (int)((got - 2) / 2);
        int need = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)(b + 2), wn, NULL, 0, NULL, NULL);
        char *u = need > 0 ? malloc((size_t)need + 1) : NULL;
        if (u) { WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)(b + 2), wn, u, need, NULL, NULL); u[need] = 0; }
        free(b);
        return u;
    }
    if (got >= 3 && (uint8_t)b[0] == 0xEF && (uint8_t)b[1] == 0xBB && (uint8_t)b[2] == 0xBF) memmove(b, b + 3, got - 2);
    return b;
}

// One line "name value; # note" between s and e: where its name and value sit. 0 when the line is not a setting.
static int settings_line(const char *s, const char *e, const char **name, int *name_len, const char **val, int *val_len) {
    while (s < e && (*s == ' ' || *s == '\t')) s++;
    const char *n = s;
    while (s < e && ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_')) s++;
    if (s == n || s >= e || (*s != ' ' && *s != '\t')) return 0;
    *name = n;
    *name_len = (int)(s - n);
    while (s < e && (*s == ' ' || *s == '\t')) s++;
    const char *v = s;
    while (s < e && *s != ';') s++;
    if (s >= e) return 0;
    while (s > v && (s[-1] == ' ' || s[-1] == '\t')) s--;
    *val = v;
    *val_len = (int)(s - v);
    if (*val_len < 1 || *val_len > 31) return 0;
    for (int i = 0; i < *val_len; i++) {   // numbers, true/false: nothing else is ever copied
        char c = v[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+')) return 0;
    }
    return 1;
}

static int copied_setting_index(const char *name, int len) {
    for (int i = 0; i < N_COPIED_SETTINGS; i++)
        if ((int)strlen(k_copied_settings[i]) == len && !memcmp(name, k_copied_settings[i], (size_t)len)) return i;
    return -1;
}

// Where the line starting at s ends (its CR, LF or the end of the text) and where the next one starts. The 64-bit game writes its
// settings with a lone CR between lines, Windows Attila with CR LF.
static const char *settings_line_end(const char *s, const char **next) {
    const char *e = s;
    while (*e && *e != '\r' && *e != '\n') e++;
    *next = e + (*e == '\r' && e[1] == '\n' ? 2 : *e ? 1 : 0);
    return e;
}

static void copy_normal_graphics_settings(void) {
    if (getenv("ATTILA_NO_SETTINGS_COPY")) return;
    const char *up = getenv("USERPROFILE"), *ad = getenv("APPDATA");
    if (!up || !ad) return;
    char dir[MAX_PATH], ours[MAX_PATH], marker[MAX_PATH], theirs[MAX_PATH], tmp[MAX_PATH], keep[MAX_PATH];
    { extern const char *attila_home_dir(void); snprintf(dir, sizeof dir, "%s\\.Creative Assembly\\Attila\\scripts", attila_home_dir()); }   // HOME as shim_environ makes it
    snprintf(ours, sizeof ours, "%s\\preferences.script.txt", dir);
    snprintf(marker, sizeof marker, "%s\\attila64_graphics_copied.txt", dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", ours);
    snprintf(keep, sizeof keep, "%s.before_copy", ours);
    snprintf(theirs, sizeof theirs, "%s\\The Creative Assembly\\Attila\\scripts\\preferences.script.txt", ad);
    if (GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES) return;
    char *src = read_settings_text(theirs);
    if (!src) { log_line("graphics settings: no normal Attila settings on this PC, nothing copied"); return; }
    const char *val[N_COPIED_SETTINGS] = {0};
    int vlen[N_COPIED_SETTINGS] = {0}, found = 0;
    for (const char *s = src, *next; *s; s = next) {
        const char *e = settings_line_end(s, &next), *n, *v;
        int nl, vl, i;
        if (settings_line(s, e, &n, &nl, &v, &vl) && (i = copied_setting_index(n, nl)) >= 0) { if (!val[i]) found++; val[i] = v; vlen[i] = vl; }
    }
    if (!found) { log_line("graphics settings: normal Attila's file has none of them, nothing copied"); free(src); return; }

    char *cur = read_settings_text(ours);
    size_t cap = (cur ? strlen(cur) : 0) + 8192;   // each listed line grows by at most 31, plus the added lines
    char *out = malloc(cap), note[1024];
    int changed = 0, seen[N_COPIED_SETTINGS] = {0};
    size_t o = 0, nn = 0;
    note[0] = 0;
    if (!out) { free(src); free(cur); return; }
    const char *nl_text = "\n";   // added lines end the way the file's own lines do
    if (cur) {
        const char *next, *e = settings_line_end(cur, &next);
        if (*e) nl_text = *e == '\r' ? (e[1] == '\n' ? "\r\n" : "\r") : "\n";
    }
    if (cur) {   // the 64-bit file line by line; listed settings get normal Attila's value, everything else stays as it is
        for (const char *s = cur, *next; *s; s = next) {
            const char *e = settings_line_end(s, &next), *n, *v;
            int nl, vl, i;
            if (settings_line(s, e, &n, &nl, &v, &vl) && (i = copied_setting_index(n, nl)) >= 0 && val[i]) {
                seen[i] = 1;
                if (vl != vlen[i] || memcmp(v, val[i], (size_t)vl)) {
                    changed++;
                    if (nn + 80 < sizeof note)
                        nn += (size_t)snprintf(note + nn, sizeof note - nn, " %s %.*s->%.*s", k_copied_settings[i], vl, v, vlen[i], val[i]);
                    memcpy(out + o, s, (size_t)(v - s)); o += (size_t)(v - s);
                    memcpy(out + o, val[i], (size_t)vlen[i]); o += (size_t)vlen[i];
                    memcpy(out + o, v + vl, (size_t)(next - (v + vl))); o += (size_t)(next - (v + vl));
                    continue;
                }
            }
            memcpy(out + o, s, (size_t)(next - s));
            o += (size_t)(next - s);
        }
        if (o && out[o - 1] != '\n' && out[o - 1] != '\r') o += (size_t)snprintf(out + o, cap - o, "%s", nl_text);
    } else {   // no 64-bit settings yet: start them, and skip the game's first-start automatic choice that would replace these
        o += (size_t)snprintf(out + o, cap - o, "gfx_first_run false;%s", nl_text);
    }
    for (int i = 0; i < N_COPIED_SETTINGS; i++)
        if (val[i] && !seen[i]) {
            changed++;
            o += (size_t)snprintf(out + o, cap - o, "%s %.*s;%s", k_copied_settings[i], vlen[i], val[i], nl_text);
            if (cur && nn + 80 < sizeof note) nn += (size_t)snprintf(note + nn, sizeof note - nn, " %s (new) %.*s", k_copied_settings[i], vlen[i], val[i]);
        }
    int ok = 1;
    if (changed) {
        if (!cur) {   // make the folders the game would make
            char path[MAX_PATH];
            snprintf(path, sizeof path, "%s", dir);
            for (char *p = path + strlen(up) + 1; *p; p++)
                if (*p == '\\') { *p = 0; CreateDirectoryA(path, NULL); *p = '\\'; }
            CreateDirectoryA(path, NULL);
        }
        FILE *f = fopen(tmp, "wb");
        ok = f && fwrite(out, 1, o, f) == o;
        if (f && fclose(f)) ok = 0;
        if (ok && cur && GetFileAttributesA(keep) == INVALID_FILE_ATTRIBUTES) CopyFileA(ours, keep, TRUE);
        if (ok) ok = MoveFileExA(tmp, ours, MOVEFILE_REPLACE_EXISTING) != 0;
        if (!ok) DeleteFileA(tmp);
    }
    if (ok) {
        FILE *m = fopen(marker, "w");
        if (m) {
            SYSTEMTIME t;
            GetLocalTime(&t);
            fprintf(m, "64-bit Attila copied the graphics settings from normal Attila once, on %04d-%02d-%02d.\n"
                       "Delete this file to copy them again at the next start.\n", t.wYear, t.wMonth, t.wDay);
            fclose(m);
        }
    }
    if (!ok) log_line("graphics settings: could not write %s", ours);
    else if (!cur) log_line("graphics settings: first start, %d settings taken from normal Attila", changed);
    else if (changed) log_line("graphics settings taken from normal Attila (%d changed, old file kept as .before_copy):%s", changed, note);
    else log_line("graphics settings: already the same as normal Attila");
    free(out);
    free(src);
    free(cur);
}

