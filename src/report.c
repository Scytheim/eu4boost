// Sending the reports crash.c wrote ([report] upload: -1 = ask once, 0 = never, 1 = send). Kinds: crash,
// hang, exit (abnormal exit) and slow (at most one a day: crash_reports\last_slow.txt holds its date).
//
// A background thread asks once (a Yes/No message box listing what is sent, where to and for how long)
// and stores the answer in eu4boost.ini. With consent it sends up to 3 pending reports per launch to
// the report endpoint (a Cloudflare Worker that files them as issues in a private GitHub repository;
// the GitHub token stays on the Worker) and moves each sent payload to crash_reports\sent\, so the user
// can read exactly what left the PC. Before sending, the Windows user name, the profile folder and the
// computer name are replaced in every text.
// Payload (JSON, schema 1): kind, signature, the report text, the plugin version of the session the report
// is about (the issue title) and of the sender, the DLL hash, the
// effective config line, game version and mod list (the game's crash meta.yml or dlc_load.json), PC
// (CPU, threads, RAM, GPU, Windows build, locale), the files in plugins\ with EU4dll's hash, and the
// tails of the plugin log and the game's game.log / error.log / system.log of that session.
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h>
#include <winhttp.h>
#include <bcrypt.h>

#ifndef EB_REPORT_URL
#define EB_REPORT_URL L"https://eu4boost-report.eu4boost.workers.dev/report"   // worker/crash-report.js; [report] url= overrides
#endif

// ---- growing text buffer ----
typedef struct { char* p; size_t n, cap; } buf_t;
static void b_add(buf_t* b, const char* s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 4096; while (c < b->n + n + 1) c *= 2;
        char* q = (char*)realloc(b->p, c); if (!q) return;
        b->p = q; b->cap = c;
    }
    memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
}
static void b_str(buf_t* b, const char* s) { b_add(b, s, strlen(s)); }

// ---- personal data out: profile folder, user name and computer name (UTF-8 and ANSI spellings) ----
#define NSCRUB 12
static struct { char from[MAX_PATH * 3]; const char* to; int word; } g_scrub[NSCRUB];
static int g_nscrub;
// short names (under 5 bytes, e.g. "user") are replaced only as a whole word: "USER32.dll" stays
static void scrub_add(const wchar_t* w, const char* to, int whole_short) {
    if (!w || wcslen(w) < 3) return;
    for (int cp = 0; cp < 2; cp++) {
        for (int slash = 0; slash < 2; slash++) {
            wchar_t t[MAX_PATH]; wcsncpy(t, w, MAX_PATH - 1); t[MAX_PATH - 1] = 0;
            if (slash) for (wchar_t* q = t; *q; q++) if (*q == L'\\') *q = L'/';
            if (g_nscrub >= NSCRUB) return;
            if (WideCharToMultiByte(cp ? CP_ACP : CP_UTF8, 0, t, -1, g_scrub[g_nscrub].from, sizeof g_scrub[0].from, NULL, NULL) > 1) {
                int dup = 0;
                for (int i = 0; i < g_nscrub; i++) if (!strcmp(g_scrub[i].from, g_scrub[g_nscrub].from)) dup = 1;
                if (!dup) { g_scrub[g_nscrub].word = whole_short && strlen(g_scrub[g_nscrub].from) < 5; g_scrub[g_nscrub++].to = to; }
            }
        }
    }
}
static void scrub_init(void) {
    wchar_t v[MAX_PATH];
    if (GetEnvironmentVariableW(L"USERPROFILE", v, MAX_PATH)) scrub_add(v, "%USERPROFILE%", 0);   // longest first
    if (GetEnvironmentVariableW(L"USERNAME", v, MAX_PATH)) scrub_add(v, "<user>", 1);
    DWORD n = MAX_PATH; if (GetComputerNameW(v, &n)) scrub_add(v, "<pc>", 1);
}
static int ieq(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}
// values of account keys (the game's crash meta.yml has the launcher's arguments, e.g.
// --paradox-account-userid="..." --pdx-launcher-session-token="..."): the value becomes <removed>
static const char* const g_secret_keys[] = { "session-token", "sessiontoken", "session_token", "userid", "user-id", "user_id",
                                             "access_token", "access-token", "auth-token", "password", "steamid" };
static void mask_secrets(buf_t* b) {
    buf_t o = {0};
    size_t i = 0;
    while (i < b->n) {
        size_t kl = 0;
        for (size_t k = 0; k < sizeof g_secret_keys / sizeof g_secret_keys[0] && !kl; k++) {
            size_t l = strlen(g_secret_keys[k]);
            if (i + l <= b->n && ieq(b->p + i, g_secret_keys[k], l)) kl = l;
        }
        if (!kl) { b_add(&o, b->p + i, 1); i++; continue; }
        size_t j = i + kl;
        while (j < b->n && b->p[j] && strchr("=: \"'", b->p[j])) j++;
        size_t v = j;
        while (v < b->n && !strchr("\"' \r\n,", b->p[v])) v++;
        b_add(&o, b->p + i, j - i);
        if (v > j) b_add(&o, "<removed>", 9);
        i = v;
    }
    free(b->p); *b = o;
    if (!b->p) b_add(b, "", 0);
}
static void drop_lines(buf_t* b, const char* prefix) {       // whole lines that start with prefix
    buf_t o = {0};
    size_t pl = strlen(prefix);
    for (size_t i = 0; i < b->n; ) {
        size_t e = i; while (e < b->n && b->p[e] != '\n') e++;
        if (e < b->n) e++;
        if (!(e - i >= pl && !memcmp(b->p + i, prefix, pl))) b_add(&o, b->p + i, e - i);
        i = e;
    }
    free(b->p); *b = o;
    if (!b->p) b_add(b, "", 0);
}
static int alnum(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static void scrub(buf_t* b) {
    mask_secrets(b);
    for (int k = 0; k < g_nscrub; k++) {
        size_t fl = strlen(g_scrub[k].from), tl = strlen(g_scrub[k].to);
        buf_t o = {0};
        size_t i = 0;
        while (i < b->n) {
            if (i + fl <= b->n && ieq(b->p + i, g_scrub[k].from, fl) &&
                (!g_scrub[k].word || ((!i || !alnum(b->p[i - 1])) && (i + fl == b->n || !alnum(b->p[i + fl]))))) {
                b_add(&o, g_scrub[k].to, tl); i += fl;
            }
            else { b_add(&o, b->p + i, 1); i++; }
        }
        free(b->p); *b = o;
        if (!b->p) b_add(b, "", 0);
    }
}

// ---- JSON ----
// string value: valid UTF-8 is kept, every other byte becomes \u00XX (the game's logs mix code pages)
static void j_str(buf_t* o, const char* s, size_t n) {
    b_add(o, "\"", 1);
    for (size_t i = 0; i < n; ) {
        unsigned char c = (unsigned char)s[i];
        char e[8];
        if (c == '"' || c == '\\') { e[0] = '\\'; e[1] = (char)c; b_add(o, e, 2); i++; continue; }
        if (c == '\n') { b_add(o, "\\n", 2); i++; continue; }
        if (c == '\r') { b_add(o, "\\r", 2); i++; continue; }
        if (c == '\t') { b_add(o, "\\t", 2); i++; continue; }
        if (c < 0x20) { snprintf(e, sizeof e, "\\u%04x", c); b_add(o, e, 6); i++; continue; }
        if (c < 0x80) { b_add(o, (const char*)&s[i], 1); i++; continue; }
        int len = c >= 0xf0 && c < 0xf5 ? 4 : c >= 0xe0 ? 3 : c >= 0xc2 && c < 0xe0 ? 2 : 0;
        int ok = len && i + len <= n;
        for (int k = 1; ok && k < len; k++) ok = ((unsigned char)s[i + k] & 0xc0) == 0x80;
        if (ok) { b_add(o, s + i, len); i += len; }
        else { snprintf(e, sizeof e, "\\u%04x", c); b_add(o, e, 6); i++; }
    }
    b_add(o, "\"", 1);
}
static void j_kv(buf_t* o, const char* key, const char* val, size_t n, int comma) {
    if (comma) b_add(o, ",", 1);
    j_str(o, key, strlen(key)); b_add(o, ":", 1); j_str(o, val ? val : "", val ? n : 0);
}
static void j_kvs(buf_t* o, const char* key, const char* val, int comma) { j_kv(o, key, val, val ? strlen(val) : 0, comma); }

// ---- files ----
static int read_file(const wchar_t* path, buf_t* b, DWORD max_tail) {        // the last max_tail bytes, from a line start
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    DWORD n = sz.QuadPart > max_tail ? max_tail : (DWORD)sz.QuadPart;
    LARGE_INTEGER off; off.QuadPart = sz.QuadPart - n; SetFilePointerEx(h, off, NULL, FILE_BEGIN);
    char* t = (char*)malloc(n + 1); DWORD rd = 0;
    if (t && ReadFile(h, t, n, &rd, NULL)) {
        size_t s = 0;
        if (off.QuadPart > 0) { while (s < rd && t[s] != '\n') s++; if (s < rd) s++; }
        b_add(b, t + s, rd - s);
    }
    free(t); CloseHandle(h);
    return b->p != NULL;
}
static int sha256_file(const wchar_t* path, char* hex) {
    hex[0] = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    BCRYPT_ALG_HANDLE alg; BCRYPT_HASH_HANDLE hh; UCHAR d[32]; int ok = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0) {
        if (BCryptCreateHash(alg, &hh, NULL, 0, NULL, 0, 0) == 0) {
            static UCHAR chunk[1 << 16]; DWORD rd;
            while (ReadFile(h, chunk, sizeof chunk, &rd, NULL) && rd) BCryptHashData(hh, chunk, rd, 0);
            ok = BCryptFinishHash(hh, d, 32, 0) == 0;
            BCryptDestroyHash(hh);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    CloseHandle(h);
    if (ok) for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", d[i]);
    return ok;
}
static void line_value(const buf_t* b, const char* key, char* out, size_t n) {   // "key: value" in a report
    out[0] = 0;
    if (!b->p) return;
    const char* p = strstr(b->p, key);
    if (!p) return;
    p += strlen(key);
    size_t k = 0; while (p[k] && p[k] != '\r' && p[k] != '\n' && k < n - 1) { out[k] = p[k]; k++; }
    out[k] = 0;
}

// The version of the session the report is about: its "plugin: " line (crash, hang and slow reports are
// written by that session; an abnormal-exit report, written by the next launch, names the version from
// session.txt or the last heartbeat). A report is often sent by a newer DLL after an update (report 17:
// 0.4.2 sessions filed as 0.4.4). Without a usable line: the sender's version.
static void report_version(const buf_t* rep, char* out, size_t n) {
    line_value(rep, "\nplugin: ", out, n);
    size_t k = strlen(out);
    int ok = k >= 1 && k <= 32;
    for (size_t i = 0; ok && i < k; i++) {
        char c = out[i];
        ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '+' || c == '-';
    }
    if (!ok) snprintf(out, n, "%s", EU4BOOST_VERSION);
}

// ---- PC ----
static void pc_info(buf_t* o) {
    int r[4]; char cpu[49] = {0};
    __cpuid(r, 0x80000000);
    if ((unsigned)r[0] >= 0x80000004) for (int i = 0; i < 3; i++) { __cpuid(r, 0x80000002 + i); memcpy(cpu + 16 * i, r, 16); }
    char* c = cpu; while (*c == ' ') c++;
    SYSTEM_INFO si; GetSystemInfo(&si);
    MEMORYSTATUSEX ms; ms.dwLength = sizeof ms; GlobalMemoryStatusEx(&ms);
    typedef LONG (WINAPI *rtlgv_t)(OSVERSIONINFOW*);
    OSVERSIONINFOW ov; memset(&ov, 0, sizeof ov); ov.dwOSVersionInfoSize = sizeof ov;
    rtlgv_t gv = (rtlgv_t)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (gv) gv(&ov);
    wchar_t loc[LOCALE_NAME_MAX_LENGTH] = L""; GetUserDefaultLocaleName(loc, LOCALE_NAME_MAX_LENGTH);
    char gpu[256] = "", locs[64] = "", t[160];
    DISPLAY_DEVICEW dd; dd.cb = sizeof dd;
    for (DWORD i = 0; EnumDisplayDevicesW(NULL, i, &dd, 0); i++)
        if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) { WideCharToMultiByte(CP_UTF8, 0, dd.DeviceString, -1, gpu, sizeof gpu, NULL, NULL); break; }
    WideCharToMultiByte(CP_UTF8, 0, loc, -1, locs, sizeof locs, NULL, NULL);
    b_str(o, ",\"pc\":{"); j_kvs(o, "cpu", c, 0);
    snprintf(t, sizeof t, ",\"threads\":%lu,\"ram_mb\":%llu", (unsigned long)si.dwNumberOfProcessors, (unsigned long long)(ms.ullTotalPhys >> 20)); b_str(o, t);
    j_kvs(o, "gpu", gpu, 1);
    snprintf(t, sizeof t, "%lu.%lu.%lu", (unsigned long)ov.dwMajorVersion, (unsigned long)ov.dwMinorVersion, (unsigned long)ov.dwBuildNumber);
    j_kvs(o, "windows", t, 1); j_kvs(o, "locale", locs, 1);
    b_str(o, "}");
}

// ---- the game's crash folder of the same moment: Documents\...\crashes\eu4_YYYYMMDD_HHMMSS\ ----
static int game_crash_dir(const wchar_t* stamp, wchar_t* out) {
    wchar_t d[MAX_PATH], pat[MAX_PATH];
    if (!eb_docs_dir(d, MAX_PATH)) return 0;
    _snwprintf(pat, MAX_PATH, L"%lscrashes\\eu4_*", d);
    WIN32_FIND_DATAW fd; HANDLE f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    int found = 0;
    do {   // eu4_<stamp> written right after our report: same minute or the next few
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || wcslen(fd.cFileName) < 19) continue;
        const wchar_t* s = fd.cFileName + 4;
        if (wcsncmp(s, stamp, 9) || wcsncmp(s + 9, stamp + 9, 4) < 0) continue;    // same day, not earlier
        int dm = (_wtoi(s + 9) / 100) * 60 + _wtoi(s + 9) % 100 - ((_wtoi(stamp + 9) / 100) * 60 + _wtoi(stamp + 9) % 100);
        if (dm < 0 || dm > 3) continue;
        _snwprintf(out, MAX_PATH, L"%lscrashes\\%ls\\", d, fd.cFileName); found = 1; break;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return found;
}

// the save folder: how many saves and how big (the save list reads every one of them); no names
static void saves_info(buf_t* o) {
    wchar_t d[MAX_PATH], pat[MAX_PATH];
    if (!eb_docs_dir(d, MAX_PATH)) return;
    _snwprintf(pat, MAX_PATH, L"%lssave games\\*.eu4", d);
    WIN32_FIND_DATAW fd; HANDLE f = FindFirstFileW(pat, &fd);
    unsigned n = 0; ULONGLONG total = 0, big = 0;
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            ULONGLONG s = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            n++; total += s; if (s > big) big = s;
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    char t[160]; snprintf(t, sizeof t, "%u files, %.1f MB (largest %.1f MB)", n, total / 1048576.0, big / 1048576.0);
    j_kvs(o, "saves", t, 1);
}

static void add_file(buf_t* o, const char* key, const wchar_t* dir, const wchar_t* name, DWORD max) {
    wchar_t p[MAX_PATH]; _snwprintf(p, MAX_PATH, L"%ls%ls", dir, name);
    buf_t b = {0};
    if (read_file(p, &b, max)) { drop_lines(&b, "LaunchArguments"); scrub(&b); j_kv(o, key, b.p, b.n, 1); }
    free(b.p);
}
static int has(const char* s, size_t n, const char* k) {
    size_t kl = strlen(k);
    for (size_t i = 0; i + kl <= n; i++) if (!memcmp(s + i, k, kl)) return 1;
    return 0;
}
static void system_lines(buf_t* o, const wchar_t* dir) {      // the hardware lines of system.log
    wchar_t p[MAX_PATH]; _snwprintf(p, MAX_PATH, L"%lssystem.log", dir);
    buf_t b = {0}, k = {0};
    if (!read_file(p, &b, 64 * 1024)) return;
    static const char* keys[] = { "System memory", "CPU type", "Number of processors", "OS:", "Graphics device", "Graphics driver",
                                  "Driver version", "Video memory", "Resolution=", "Fullscreen=", "EU4 Version", "Language" };
    for (char* line = b.p; line && *line; ) {
        char* e = strchr(line, '\n'); size_t len = e ? (size_t)(e - line + 1) : strlen(line);
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
            if (has(line, len, keys[i])) { b_add(&k, line, len); break; }
        line = e ? e + 1 : NULL;
    }
    if (k.p) { scrub(&k); j_kv(o, "system_log", k.p, k.n, 1); }
    free(b.p); free(k.p);
}

// the plugins folder: file names and sizes (another plugin may hook the same functions), EU4dll's hash
static void plugins_info(buf_t* o) {
    wchar_t pat[MAX_PATH]; _snwprintf(pat, MAX_PATH, L"%ls*", g_dir);
    buf_t l = {0};
    WIN32_FIND_DATAW fd; HANDLE f = FindFirstFileW(pat, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            char n[MAX_PATH * 3], t[MAX_PATH * 3 + 32];
            WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, n, sizeof n, NULL, NULL);
            snprintf(t, sizeof t, "%s %lu\n", n, (unsigned long)fd.nFileSizeLow); b_str(&l, t);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    if (l.p) j_kv(o, "plugins_dir", l.p, l.n, 1);
    free(l.p);
    char h[65]; wchar_t p[MAX_PATH];
    _snwprintf(p, MAX_PATH, L"%lsplugin64.dll", g_dir); if (sha256_file(p, h)) j_kvs(o, "eu4dll_sha256", h, 1);
    GetModuleFileNameW(g_self, p, MAX_PATH); if (sha256_file(p, h)) j_kvs(o, "dll_sha256", h, 1);
}

static int build(const wchar_t* dir, const wchar_t* name, buf_t* o, char* title, size_t tn) {
    buf_t rep = {0}, log = {0};
    wchar_t p[MAX_PATH];
    _snwprintf(p, MAX_PATH, L"%lsreport.txt", dir); if (!read_file(p, &rep, 96 * 1024)) return 0;
    _snwprintf(p, MAX_PATH, L"%lseu4boost.log", dir); read_file(p, &log, 48 * 1024);
    scrub(&rep); scrub(&log);
    char kind[32], sig[200], cfg[600];
    line_value(&rep, "kind: ", kind, sizeof kind);
    line_value(&rep, "signature: ", sig, sizeof sig);
    line_value(&log, "] config: ", cfg, sizeof cfg);
    snprintf(title, tn, "%s", sig);
    char id[64]; WideCharToMultiByte(CP_UTF8, 0, name, -1, id, sizeof id, NULL, NULL);
    b_str(o, "{\"schema\":1"); j_kvs(o, "id", id, 1); j_kvs(o, "kind", kind, 1); j_kvs(o, "signature", sig, 1);
    char ver[40]; report_version(&rep, ver, sizeof ver);
    j_kvs(o, "plugin_version", ver, 1); j_kvs(o, "sender_version", EU4BOOST_VERSION, 1); j_kvs(o, "config", cfg, 1);
    plugins_info(o);
    pc_info(o);
    saves_info(o);
    j_kv(o, "report", rep.p, rep.n, 1);
    if (log.p) j_kv(o, "plugin_log", log.p, log.n, 1);
    wchar_t gd[MAX_PATH];
    if (!strcmp(kind, "crash") && game_crash_dir(name, gd)) {
        add_file(o, "game_exception", gd, L"exception.txt", 8 * 1024);
        add_file(o, "game_meta", gd, L"meta.yml", 8 * 1024);
    }
    add_file(o, "dlc_load", dir, L"dlc_load.json", 16 * 1024);
    add_file(o, "game_log", dir, L"game.log", 8 * 1024);
    add_file(o, "error_log", dir, L"error.log", 12 * 1024);
    system_lines(o, dir);
    b_str(o, "}");
    free(rep.p); free(log.p);
    return 1;
}

// ---- HTTP ----
static int post(const wchar_t* url, const char* body, size_t len, char* resp, size_t rn) {
    resp[0] = 0;
    wchar_t host[256], path[1024];
    URL_COMPONENTS uc; memset(&uc, 0, sizeof uc); uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = path; uc.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) return 0;
    int status = 0;
    HINTERNET s = WinHttpOpen(L"eu4boost/" EB_W(EU4BOOST_VERSION), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, uc.nPort, 0) : NULL;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"POST", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : NULL;
    if (r) {
        WinHttpSetTimeouts(r, 10000, 10000, 20000, 20000);
        if (WinHttpSendRequest(r, L"Content-Type: application/json\r\n", (DWORD)-1, (void*)body, (DWORD)len, (DWORD)len, 0) &&
            WinHttpReceiveResponse(r, NULL)) {
            DWORD code = 0, sz = sizeof code;
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
            status = (int)code;
            DWORD got = 0; size_t n = 0;
            while (n < rn - 1 && WinHttpReadData(r, resp + n, (DWORD)(rn - 1 - n), &got) && got) n += got;
            resp[n] = 0;
        }
        WinHttpCloseHandle(r);
    }
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return status;
}

// ---- consent ----
static const wchar_t g_ask[] =
    L"eu4boost가 게임 튕김과 느려짐을 고칠 수 있도록 보고를 보내도 될까요?\n\n"
    L"게임이 튕기거나, 멈추거나, 비정상 종료되거나, 눈에 띄게 느려지면(화면이 2초 넘게 멈칫, 저장 창·불러오기 목록이 0.3초 이상, "
    L"불러오기가 90초 이상) 다음 실행 때 원인 분석 정보를 개발자에게 자동으로 보냅니다. 느려짐 보고는 하루에 한 번까지입니다.\n\n"
    L"보내는 것: 오류·느려진 위치와 호출 기록, 걸린 시간, 플러그인 설정·로그, 게임 버전·모드 목록, 게임 로그 일부, "
    L"PC 사양(CPU·RAM·그래픽카드·Windows 버전·언어), 세이브 파일 수·크기, plugins 폴더의 파일 목록\n"
    L"보내지 않는 것: 세이브 파일, 메모리 덤프, Windows 사용자 이름·PC 이름(지운 뒤 보냄), 계정 정보\n"
    L"받는 곳: Cloudflare(전달), GitHub 비공개 저장소(보관, 미국). 해결 뒤 또는 최대 1년 뒤 삭제합니다.\n"
    L"보낸 내용은 plugins\\crash_reports\\sent 폴더에 그대로 남습니다. "
    L"eu4boost.ini의 [report] upload=0 으로 언제든 끌 수 있습니다.\n\n"
    L"Send crash and slowdown reports to the eu4boost developer? (error or slow location, times, plugin log and settings, game version and mods, "
    L"game log excerpts, PC specs; no saves, no user or PC names) Details: README, [report] in eu4boost.ini.";

static void ask(void) {
    int r = MessageBoxW(NULL, g_ask, L"eu4boost - 크래시·느려짐 보고 (crash and slowdown reports)", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND);
    g_cfg.report_upload = r == IDYES ? 1 : 0;
    wchar_t ini[MAX_PATH]; _snwprintf(ini, MAX_PATH, L"%lseu4boost.ini", g_dir);
    WritePrivateProfileStringW(L"report", L"upload", g_cfg.report_upload ? L"1" : L"0", ini);
    eb_log("report: the user %s sending crash reports", g_cfg.report_upload ? "agreed to" : "declined");
}

static void today(char* out) { SYSTEMTIME st; GetLocalTime(&st); snprintf(out, 9, "%04d%02d%02d", st.wYear, st.wMonth, st.wDay); }
static int slow_sent_today(void) {
    wchar_t p[MAX_PATH]; _snwprintf(p, MAX_PATH, L"%lslast_slow.txt", eb_rep_dir);
    char d[16] = "", t[9]; today(t);
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD rd = 0; ReadFile(h, d, 8, &rd, NULL); CloseHandle(h);
    return rd == 8 && !memcmp(d, t, 8);
}
static void slow_mark_sent(void) {
    wchar_t p[MAX_PATH]; _snwprintf(p, MAX_PATH, L"%lslast_slow.txt", eb_rep_dir);
    char t[9]; today(t);
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, t, 8, &w, NULL); CloseHandle(h); }
}

static void send_all(const wchar_t* url) {
    wchar_t pat[MAX_PATH]; _snwprintf(pat, MAX_PATH, L"%lspending\\*", eb_rep_dir);
    wchar_t names[64][MAX_PATH]; int n = 0;
    WIN32_FIND_DATAW fd; HANDLE f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.' && n < 64) wcscpy(names[n++], fd.cFileName);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    qsort(names, (size_t)n, sizeof names[0], (int (*)(const void*, const void*))wcscmp);
    int sent = 0;
    for (int i = 0; i < n && sent < 3; i++) {
        wchar_t dir[MAX_PATH], u[MAX_PATH]; _snwprintf(dir, MAX_PATH, L"%lspending\\%ls\\", eb_rep_dir, names[i]);
        _snwprintf(u, MAX_PATH, L"%lsunconfirmed", dir);
        if (GetFileAttributesW(u) != INVALID_FILE_ATTRIBUTES) continue;      // a hang or the slow events of this session
        size_t nl = wcslen(names[i]);
        int slow = nl > 5 && !wcscmp(names[i] + nl - 5, L"_slow");
        if (slow && slow_sent_today()) { eb_rm_dir(dir); continue; }        // one slow report a day
        buf_t o = {0}; char title[200];
        if (!build(dir, names[i], &o, title, sizeof title)) { eb_rm_dir(dir); continue; }
        char resp[512]; int st = post(url, o.p, o.n, resp, sizeof resp);
        if (st >= 200 && st < 300) {
            wchar_t sp[MAX_PATH]; _snwprintf(sp, MAX_PATH, L"%lssent\\%ls.json", eb_rep_dir, names[i]);
            HANDLE h = CreateFileW(sp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, o.p, (DWORD)o.n, &w, NULL); CloseHandle(h); }
            eb_rm_dir(dir); sent++;
            if (slow) slow_mark_sent();
            eb_log("report: sent %ls (%s, %zu bytes): %s", names[i], title, o.n, resp);
        } else if (st >= 400 && st < 500 && st != 408 && st != 429) {
            // refused (e.g. an older relay that does not know this kind yet): tried again at the next
            // launches, deleted after the fifth refusal
            wchar_t ap[MAX_PATH]; _snwprintf(ap, MAX_PATH, L"%lsrefused", dir);
            DWORD tries = 0, rw = 0;
            HANDLE h = CreateFileW(ap, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                ReadFile(h, &tries, sizeof tries, &rw, NULL); tries++;
                SetFilePointer(h, 0, NULL, FILE_BEGIN); WriteFile(h, &tries, sizeof tries, &rw, NULL); CloseHandle(h);
            }
            if (tries >= 5) { eb_log("report: %ls refused (HTTP %d: %s) a fifth time - deleted", names[i], st, resp); eb_rm_dir(dir); }
            else eb_log("report: %ls refused (HTTP %d: %s) - kept, try %lu of 5", names[i], st, resp, (unsigned long)tries);
        } else {
            eb_log("report: %ls not sent (HTTP %d) - kept for the next launch", names[i], st);
            free(o.p); break;                                                  // offline: try again next time
        }
        free(o.p);
    }
}

static void prune_old(void) {                                 // pending reports older than 30 days are dropped
    wchar_t pat[MAX_PATH]; _snwprintf(pat, MAX_PATH, L"%lspending\\*", eb_rep_dir);
    FILETIME now; GetSystemTimeAsFileTime(&now);
    ULONGLONG t = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
    WIN32_FIND_DATAW fd; HANDLE f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        ULONGLONG c = ((ULONGLONG)fd.ftCreationTime.dwHighDateTime << 32) | fd.ftCreationTime.dwLowDateTime;
        if (t - c > 30ull * 24 * 3600 * 10000000ull) {
            wchar_t d[MAX_PATH]; _snwprintf(d, MAX_PATH, L"%lspending\\%ls\\", eb_rep_dir, fd.cFileName); eb_rm_dir(d);
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

static DWORD WINAPI sender(LPVOID unused) {
    (void)unused;
    Sleep(3000);                                               // the game is still loading: ask before its window opens
    if (g_cfg.report_upload < 0) ask();
    if (g_cfg.report_upload != 1) return 0;
    const wchar_t* url = g_cfg.report_url[0] ? g_cfg.report_url : EB_REPORT_URL;
    if (!url[0]) { eb_log("report: no report address in this build - reports stay in crash_reports\\pending"); return 0; }
    Sleep(12000);
    for (int i = 0; i < 200 && !eb_wer_done; i++) Sleep(100);   // crash.c is still adding Windows' record to an exit report
    scrub_init();
    prune_old();
    send_all(url);
    return 0;
}

void eb_report_start(void) {
#ifndef EB_TEST
    if (!g_cfg.report_capture || g_cfg.report_upload == 0) return;
    HANDLE h = CreateThread(NULL, 0, sender, NULL, 0, NULL);
    if (h) CloseHandle(h);
#endif
}

#ifdef EB_TEST
// harness: the version a report is filed under
__declspec(dllexport) int eb_test_report_version(void) {
    static const struct { const char* text; const char* want; } t[] = {
        { "eu4boost crash report\r\nkind: crash\r\nsignature: X\r\nthread: other\r\nplugin: 0.4.1\r\nuptime_s: 5\r\n", "0.4.1" },
        { "eu4boost crash report\r\nkind: slow\r\nsignature: SLOW x\r\nplugin: 0.4.2\r\n\r\n", "0.4.2" },
        { "eu4boost crash report\r\nkind: exit\r\nsignature: ABNORMAL_EXIT\r\nplugin: 0.4.2\r\nThe previous session\r\n"
          "last state of that session:\r\nplugin: 0.4.2\r\n", "0.4.2" },
        { "eu4boost crash report\r\nkind: exit\r\nsignature: ABNORMAL_EXIT\r\nThe previous session\r\n", EU4BOOST_VERSION },
        { "eu4boost crash report\r\nkind: crash\r\nplugin: 0.4.1 <x>\r\n", EU4BOOST_VERSION },     // not a version: the sender's
        { "eu4boost crash report\r\nsignature: no plugin: 9.9.9 here\r\n", EU4BOOST_VERSION },     // only at a line start
    };
    int bad = 0;
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        buf_t b = {0}; b_str(&b, t[i].text);
        char v[40]; report_version(&b, v, sizeof v);
        if (strcmp(v, t[i].want)) { printf("report version case %d: %s, want %s\n", (int)i, v, t[i].want); bad++; }
        free(b.p);
    }
    printf("report       %s (%d failures)\n", bad ? "FAILED" : "ALL TESTS PASSED", bad);
    return bad;
}
#endif
