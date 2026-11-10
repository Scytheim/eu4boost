// Warm-up of the game's and the mods' files at game start ([load] warm_files, experiment 2026-11-10).
//
// Why: the game opens tens of thousands of small files at start, one at a time (its file open
// FUN_141718300 holds a global spin lock, *0x14242b9c0, around PHYSFS_openRead, and PhysFS holds its own
// lock over the search path). The first open of a file costs 1.8-3.2 ms on this PC, a second one 0.02-0.05 ms
// (an antivirus real-time scan that remembers what it has looked at: Windows Defender here). It forgets at a
// PC start, when its signatures are updated (about 10 minutes after every PC start, and a few times a day)
// and after some hours (a play start 2 h 45 min after the last one was slow again; 90 minutes were not enough).
// So a first start took 106-133 s to the main menu (the user's own starts 113 and 122 s) and one right after
// another run 54-64 s, with the other threads spinning on that lock meanwhile. The same 40,540 opens take 8 s
// on 12 threads.
// What: right after the plugin loads, threads of its own open every file the game can ask for (the game
// folder and the folders or archives of the enabled mods), read one byte and close it. Nothing in the game
// is hooked or changed for that: when the game reaches a file, its own open is the cheap second one.
//   - afterwards, until the game's application loop runs (the main menu; WATCH_MS at most), one thread opens a
//     few of the files every 2 s: if a third of them take as long as a first open again (COLD_MS; the antivirus
//     got new signatures while the game starts: seen 14 s after a play start), all are opened once more (twice
//     at most). If a second open is not
//     faster on a PC at all (an antivirus that does not remember), the log says so and nothing more is done.
//     Nothing of this runs once the main menu is there.
//   - folders: the top-level folders the game reads from (TOP below, in the order it needs them), in the game
//     folder and in every enabled mod (dlc_load.json and the .mod files in the documents folder); the saves
//     and the rest of the documents folder are never touched.
//   - not opened: files that are not on this disk (cloud placeholders: OFFLINE / RECALL attributes), system
//     files, anything behind a reparse point (junction, symbolic link).
//   - the opens share everything (read, write, delete), so no open of the game or of anyone else can fail
//     because of ours.
// warm_files=2 (and [test] open_log=1) hooks PHYSFS_openRead to see how well it worked: for every file the
// game opens, whether the warm-up had already opened every copy of that relative path ("warm"), had not
// reached it yet ("late") or does not cover it ("other": files in archives, the documents folder), and how
// long the game's own opens took (the number that the warm-up is there to shrink). warm_files=3 = only those
// times, without the warm-up (the other side of a comparison).
// open_log writes those opens, in order, to plugins\eu4boost_opens.txt at exit (testing only).
#include "common.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include <winioctl.h>
#include "MinHook.h"

#define RVA_OPENREAD 0x19f7f60u       // PHYSFS_openRead(const char* fname) -> PHYSFS_File*
static const uint8_t sig_openread[16] = { 0x40,0x55,0x48,0x83,0xec,0x30,0x48,0x8d,0x6c,0x24,0x20,0x48,0x89,0x5d,0x20,0x48 };

#define MAX_FILES 400000
#define MAX_PATHW 1024
#define NOT_ON_DISK (FILE_ATTRIBUTE_OFFLINE | 0x00040000u /* RECALL_ON_OPEN */ | 0x00400000u /* RECALL_ON_DATA_ACCESS */)
#define PRIO_SKIP 255
#define WATCH_MS 150000.0             // how long after the plugin loaded the files are looked at again
#define PROBE_N 48                    // files of one look
#define SLOW_MS 0.25                  // a repeated open right after the pass that takes this long: opening again does not help on this PC
#define COLD_MS 1.0                   // an open this long is a first one again. Measured 2026-11-10: first opens 92-99% at 1 ms or more
                                      // (median 2.8-6.5 ms), repeated ones 0.02-0.08 ms, and 0.25-0.3 ms while the game loads on
                                      // all its threads (a limit of 0.25 ms opened everything again for nothing 4 s into a start)

// top-level folders the game loads from, in the order the start needs them (the order of the first opens
// in an open log of a start, 2026-11-10); a lower number is opened earlier
// (open log of a start with 12 mods, seconds after the plugin loaded: dlc 2.6, localisation 2.9-10.2, common from 11.1,
// sound and music 11.1, gfx 19.3, interface 20.1, map 20.6, events 34.7, missions 35.0, decisions 35.2, history 35.3;
// 31,358 opens of 20,120 paths in 55 s)
static const struct { const wchar_t* name; uint8_t prio; } TOP[] = {
    { L"dlc", 0 }, { L"builtin_dlc", 0 }, { L"localisation", 1 }, { L"common", 2 }, { L"sound", 3 }, { L"music", 3 },
    { L"gfx", 4 }, { L"interface", 4 }, { L"map", 4 }, { L"events", 5 }, { L"missions", 5 }, { L"decisions", 5 }, { L"history", 5 },
    { L"customizable_localization", 6 }, { L"hints", 6 }, { L"dlc_metadata", 6 }, { L"tutorial", 7 },
};
#define PRIO_ROOT_FILE 0              // files in a root itself (checksum_manifest.txt, descriptor.mod, a mod's archive)

typedef struct { wchar_t* path; uint32_t rel, seq; uint8_t prio; } wfile_t;       // seq: its place in the walk
typedef struct { uint64_t h; volatile LONG pending; } rel_t;      // one relative path: copies not opened yet

static wfile_t* g_files; static int g_nfiles, g_cap;
static rel_t* g_rel; static uint32_t g_relmask;                   // only with stats or the open log
static volatile LONG g_next, g_opened, g_failed, g_done, g_listed, g_pass;
static volatile LONG64 g_open_ticks;                              // thread time of the warm-up's own opens
static int g_roots, g_listed_ms;
static LONGLONG g_t0, g_half_ms_ticks; static double g_ms;
static volatile LONG64 g_seen[3], g_calls[3], g_ticks[3], g_slow[3];   // game opens: warm, late, other
static void* (*o_openread)(const char*);
static char* g_olog; static volatile LONG64 g_olog_n; static LONG64 g_olog_cap;

static LONGLONG qpc(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

// ---- the list ----
static wchar_t* wdup(const wchar_t* s) {
    size_t n = (wcslen(s) + 1) * sizeof(wchar_t);
    wchar_t* p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
static uint64_t rel_hash(const char* s, size_t n) {               // lower case, '/' separators, no leading '/'
    uint64_t h = 1469598103934665603ull;
    while (n && (*s == '/' || *s == '\\')) { s++; n--; }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\') c = '/'; else if (c >= 'A' && c <= 'Z') c += 32;
        h = (h ^ c) * 1099511628211ull;
    }
    return h ? h : 1;
}
static uint32_t rel_slot(uint64_t h, int add) {                   // index + 1, 0 = not there; add = one more copy to open
    if (!g_rel) return 0;
    for (uint32_t i = (uint32_t)h & g_relmask, k = 0; k <= g_relmask; i = (i + 1) & g_relmask, k++) {
        if (g_rel[i].h == h) { if (add) InterlockedIncrement(&g_rel[i].pending); return i + 1; }
        if (!g_rel[i].h) {
            if (!add) return 0;
            g_rel[i].pending = 1; g_rel[i].h = h;                 // the hook may look while the list is made: count first
            return i + 1;
        }
    }
    return 0;
}
static void add_file(const wchar_t* path, const wchar_t* rel, uint8_t prio) {
    if (g_nfiles >= MAX_FILES) return;
    if (g_nfiles == g_cap) {
        int cap = g_cap ? g_cap * 2 : 16384;
        wfile_t* p = realloc(g_files, (size_t)cap * sizeof *p);
        if (!p) return;
        g_files = p; g_cap = cap;
    }
    wfile_t* f = &g_files[g_nfiles];
    f->path = wdup(path); f->prio = prio; f->rel = 0; f->seq = (uint32_t)g_nfiles;
    if (!f->path) return;
    if (g_rel && rel) {
        char u[MAX_PATHW * 3];
        int n = WideCharToMultiByte(CP_UTF8, 0, rel, -1, u, sizeof u, NULL, NULL);
        if (n > 1) f->rel = rel_slot(rel_hash(u, (size_t)n - 1), 1);
    }
    g_nfiles++;
}
static int top_prio(const wchar_t* name) {
    for (size_t i = 0; i < sizeof TOP / sizeof TOP[0]; i++) if (!_wcsicmp(name, TOP[i].name)) return TOP[i].prio;
    return PRIO_SKIP;
}
// every file under dir (rel = its path below the root); the folders of a root's top level pick the priority
static void walk(const wchar_t* dir, const wchar_t* rel, int prio, int depth) {
    wchar_t pat[MAX_PATHW], sub[MAX_PATHW], subrel[MAX_PATHW];
    if (depth > 24 || _snwprintf(pat, MAX_PATHW, L"%ls\\*", dir) < 0) return;
    pat[MAX_PATHW - 1] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2]))) continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_SYSTEM | NOT_ON_DISK)) continue;
        if (_snwprintf(sub, MAX_PATHW, L"%ls\\%ls", dir, fd.cFileName) < 0) continue;
        if (_snwprintf(subrel, MAX_PATHW, L"%ls%ls%ls", rel, rel[0] ? L"/" : L"", fd.cFileName) < 0) continue;
        sub[MAX_PATHW - 1] = subrel[MAX_PATHW - 1] = 0;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            int p = depth ? prio : top_prio(fd.cFileName);
            if (p != PRIO_SKIP) walk(sub, subrel, p, depth + 1);
        } else if (depth) add_file(sub, subrel, (uint8_t)prio);
        else {                                                    // a file in the root itself: only what the game reads there
            const wchar_t* e = wcsrchr(fd.cFileName, L'.');
            if (e && (!_wcsicmp(e, L".txt") || !_wcsicmp(e, L".mod") || !_wcsicmp(e, L".asset") || !_wcsicmp(e, L".gui") || !_wcsicmp(e, L".gfx")))
                add_file(sub, subrel, PRIO_ROOT_FILE);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
static int add_root(const wchar_t* dir) {
    wchar_t d[MAX_PATHW];
    wcsncpy(d, dir, MAX_PATHW - 1); d[MAX_PATHW - 1] = 0;
    for (wchar_t* c = d; *c; c++) if (*c == L'/') *c = L'\\';
    size_t n = wcslen(d);
    while (n > 3 && d[n - 1] == L'\\') d[--n] = 0;
    DWORD a = GetFileAttributesW(d);
    if (a == INVALID_FILE_ATTRIBUTES) return 0;
    int before = g_nfiles;
    if (a & FILE_ATTRIBUTE_DIRECTORY) walk(d, L"", PRIO_SKIP, 0);
    else if (!(a & NOT_ON_DISK)) add_file(d, NULL, PRIO_ROOT_FILE);          // a mod's archive
    g_roots++;
    return g_nfiles - before;
}
static int by_prio(const void* a, const void* b) {
    const wfile_t* x = a, *y = b;
    if (x->prio != y->prio) return (int)x->prio - (int)y->prio;
    return x->seq < y->seq ? -1 : x->seq > y->seq;                // the walk's order within a priority
}

// the value of key="..." at the start of a line of a .mod descriptor (UTF-8)
static int mod_value(const char* t, const char* key, char* out, int n) {
    size_t kl = strlen(key); out[0] = 0;
    for (const char* p = t; (p = strstr(p, key)); p += kl) {
        if (p != t && p[-1] != '\n' && p[-1] != '\r' && p[-1] != ' ' && p[-1] != '\t') continue;
        const char* q = p + kl; while (*q == ' ' || *q == '\t') q++;
        if (*q != '=') continue;
        q++; while (*q == ' ' || *q == '\t') q++;
        if (*q != '"') continue;
        int k = 0; q++;
        while (*q && *q != '"' && *q != '\r' && *q != '\n' && k < n - 1) out[k++] = *q++;
        out[k] = 0;
        return k;
    }
    return 0;
}
static int read_text(const wchar_t* p, char* buf, int cap) {
    DWORD rd = 0;
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { if (!ReadFile(h, buf, (DWORD)cap - 1, &rd, NULL)) rd = 0; CloseHandle(h); }
    buf[rd] = 0;
    return (int)rd;
}
// the folders (or archives) of the enabled mods: dlc_load.json's enabled_mods -> each .mod file's path / archive
static void add_mods(const wchar_t* docs) {
    static char js[64 * 1024], t[16 * 1024];
    wchar_t p[MAX_PATHW];
    _snwprintf(p, MAX_PATHW, L"%lsdlc_load.json", docs); p[MAX_PATHW - 1] = 0;
    if (!read_text(p, js, sizeof js)) return;
    const char* e = strstr(js, "\"enabled_mods\""), *end = e ? strchr(e, ']') : NULL, *q = e ? strchr(e, '[') : NULL;
    while (q && end && (q = strchr(q + 1, '"')) && q < end) {
        const char* z = strchr(q + 1, '"'); if (!z || z > end) break;
        char file[512]; int fl = (int)(z - q - 1); if (fl >= (int)sizeof file) fl = sizeof file - 1;
        memcpy(file, q + 1, (size_t)fl); file[fl] = 0; q = z;
        wchar_t wf[512]; if (MultiByteToWideChar(CP_UTF8, 0, file, -1, wf, 512) <= 0) continue;
        _snwprintf(p, MAX_PATHW, L"%ls%ls", docs, wf); p[MAX_PATHW - 1] = 0;
        if (!read_text(p, t, sizeof t)) continue;
        char v[1024];
        if (!mod_value(t, "path", v, sizeof v) && !mod_value(t, "archive", v, sizeof v)) continue;
        wchar_t wv[MAX_PATHW]; if (MultiByteToWideChar(CP_UTF8, 0, v, -1, wv, MAX_PATHW) <= 0) continue;
        int absolute = (wv[0] && wv[1] == L':') || wv[0] == L'\\' || wv[0] == L'/';
        if (absolute) add_root(wv);
        else { _snwprintf(p, MAX_PATHW, L"%ls%ls", docs, wv); p[MAX_PATHW - 1] = 0; add_root(p); }
    }
}

// the game's user folder (with a trailing backslash): the one that holds dlc_load.json. A non-empty userdir.txt in
// the game folder names another place than Documents\Paradox Interactive\Europa Universalis IV.
static int has_dlc_load(const wchar_t* dir) {
    wchar_t p[MAX_PATHW];
    if (_snwprintf(p, MAX_PATHW, L"%lsdlc_load.json", dir) < 0) return 0;
    p[MAX_PATHW - 1] = 0;
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static int is_space(char c) { return c == ' ' || c == 9 || c == 13 || c == 10; }
static int user_dir(const wchar_t* game, wchar_t* out, size_t n) {
    wchar_t p[MAX_PATHW], w[MAX_PATHW]; char t[2048];
    _snwprintf(p, MAX_PATHW, L"%ls/userdir.txt", game); p[MAX_PATHW - 1] = 0;
    int k = read_text(p, t, sizeof t);
    char* s = t;
    if (k >= 3 && (unsigned char)s[0] == 0xef && (unsigned char)s[1] == 0xbb && (unsigned char)s[2] == 0xbf) s += 3;
    while (is_space(*s)) s++;
    char* e = s + strlen(s);
    while (e > s && (is_space(e[-1]) || e[-1] == 92 || e[-1] == '/')) *--e = 0;       // 92 = a backslash
    if (*s && MultiByteToWideChar(CP_UTF8, 0, s, -1, w, MAX_PATHW) > 0) {
        if (_snwprintf(out, n, L"%ls/", w) > 0 && has_dlc_load(out)) return 1;
        if (_snwprintf(out, n, L"%ls/Europa Universalis IV/", w) > 0 && has_dlc_load(out)) return 1;
    }
    if (eb_docs_dir(out, n) && has_dlc_load(out)) return 1;
    out[0] = 0;
    return 0;
}

// ---- the opens ----
static LONGLONG open_once(const wchar_t* path) {                  // the ticks it took, -1 = it could not be opened
    LONGLONG t = qpc();
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    char c; DWORD rd;
    ReadFile(h, &c, 1, &rd, NULL);
    CloseHandle(h);
    return qpc() - t;
}
static void warm_one(const wfile_t* f) {
    LONGLONG t = open_once(f->path);
    if (t >= 0) { InterlockedIncrement(&g_opened); InterlockedExchangeAdd64(&g_open_ticks, t); }
    else InterlockedIncrement(&g_failed);
    if (f->rel && !g_pass) InterlockedDecrement(&g_rel[f->rel - 1].pending);
}
static DWORD WINAPI worker(LPVOID u) {
    (void)u;
    for (;;) {
        LONG i = InterlockedIncrement(&g_next) - 1;
        if (i >= g_nfiles) break;
        warm_one(&g_files[i]);
    }
    return 0;
}
// every file of the list once, on th threads; returns when all are done
static void run_pass(int th) {
    HANDLE h[32]; int n = 0;
    InterlockedExchange(&g_next, 0);
    for (int i = 0; i < th && i < 32; i++) { h[n] = CreateThread(NULL, 64 << 10, worker, NULL, 0, NULL); if (h[n]) n++; }
    if (!n) { worker(NULL); return; }                             // no thread could be made: this one does it
    WaitForMultipleObjects((DWORD)n, h, TRUE, INFINITE);
    for (int i = 0; i < n; i++) CloseHandle(h[i]);
}

// ---- are the files still quick to open? ----
static double median_ms(float* v, int n) {                        // sorts v
    for (int i = 1; i < n; i++) { float x = v[i]; int j = i; while (j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; j--; } v[j] = x; }
    return n ? (n & 1 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0) : 0;
}
// how many of n open times (ms) are far above what a repeated open costs on this PC (again_ms)
static int slow_count(const float* ms, int n, double again_ms) {
    double lim = again_ms * 6 > COLD_MS ? again_ms * 6 : COLD_MS;
    int k = 0;
    for (int i = 0; i < n; i++) if (ms[i] >= lim) k++;
    return k;
}
static char g_looks[1200]; static int g_looks_n;                  // with statistics: every look as "seconds:median ms/slow files"
// opens PROBE_N files (from = the first one, step = the distance in the list) and gives their times; returns how many
static int probe(int from, int step, float* ms) {
    int n = 0;
    if (g_nfiles <= 0) return 0;
    if (step < 1) step = 1;
    for (int j = 0; j < PROBE_N && j < g_nfiles; j++) {
        LONGLONG t = open_once(g_files[(int)(((LONGLONG)from + (LONGLONG)j * step) % g_nfiles)].path);
        if (t >= 0) ms[n++] = (float)(t * g_ms);
    }
    return n;
}
// 1 = a third or more of a look's files are slow to open again (round picks other files every time)
static int slow_again(int round, double again_ms, double* mean) {
    float ms[PROBE_N]; double sum = 0;
    int n = probe((int)(((LONGLONG)round * 7919) % (g_nfiles > 0 ? g_nfiles : 1)), g_nfiles / PROBE_N, ms);
    for (int i = 0; i < n; i++) sum += ms[i];
    *mean = n ? sum / n : 0;
    int slow = slow_count(ms, n, again_ms);
    if (g_cfg.load_warm_files == 2 && g_looks_n < (int)sizeof g_looks - 40)
        g_looks_n += snprintf(g_looks + g_looks_n, sizeof g_looks - (size_t)g_looks_n, " %.0f:%.2f/%d", (qpc() - g_t0) * g_ms / 1000.0, median_ms(ms, n), slow);
    return n >= 8 && slow * 3 >= n;
}
// 1 = the drive of this path has a seek penalty (a hard disk), 0 = it has none, -1 = unknown
static int seek_penalty(const wchar_t* path) {
    if (!path[0] || path[1] != L':') return -1;
    wchar_t vol[8] = L"\\\\.\\X:"; vol[4] = path[0];
    HANDLE h = CreateFileW(vol, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    STORAGE_PROPERTY_QUERY q; memset(&q, 0, sizeof q);
    q.PropertyId = StorageDeviceSeekPenaltyProperty; q.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR d; memset(&d, 0, sizeof d);
    DWORD rd = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &d, sizeof d, &rd, NULL);
    CloseHandle(h);
    return ok && rd >= sizeof d ? (d.IncursSeekPenalty ? 1 : 0) : -1;
}
static int list_all(const wchar_t* game, const wchar_t* docs) {
    add_root(game);
    if (docs && docs[0]) add_mods(docs);
    if (g_nfiles > 1) qsort(g_files, (size_t)g_nfiles, sizeof *g_files, by_prio);
    InterlockedExchange(&g_listed, 1);
    return g_nfiles;
}
static int rel_table(void) {
    g_relmask = (1u << 20) - 1;
    g_rel = VirtualAlloc(NULL, ((size_t)g_relmask + 1) * sizeof *g_rel, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return g_rel != NULL;
}
static DWORD WINAPI starter(LPVOID u) {
    (void)u;
    wchar_t game[MAX_PATHW], docs[MAX_PATHW];
    DWORD n = GetModuleFileNameW(NULL, game, MAX_PATHW);
    if (!n || n >= MAX_PATHW) return 0;
    wchar_t* s = wcsrchr(game, L'\\'); if (!s) return 0;
    *s = 0;
    user_dir(game, docs, MAX_PATHW);
    LONGLONG t = qpc();
    list_all(game, docs);
    g_listed_ms = (int)((qpc() - t) * g_ms);
    if (!g_nfiles) { eb_log("warm: no files found - nothing is opened ahead"); InterlockedExchange(&g_done, 1); return 0; }
    int th = g_cfg.load_warm_threads;
    if (th <= 0) {
        SYSTEM_INFO si; GetSystemInfo(&si);
        int hd = seek_penalty(game), hd2 = docs[0] ? seek_penalty(docs) : 0;
        th = (hd == 1 || hd2 == 1) ? 2 : (hd < 0 || hd2 < 0) ? 4 : (int)si.dwNumberOfProcessors - 2;   // a hard disk gains nothing from many seeks
    }
    if (th < 1) th = 1;
    if (th > 32) th = 32;
    t = qpc();
    run_pass(th);
    double pass_s = (qpc() - t) * g_ms / 1000.0, first_ms = g_opened ? (double)g_open_ticks * g_ms / g_opened : 0;
    // what a repeated open costs here: the files the pass opened last, once more
    float ms[PROBE_N];
    int k = probe(g_nfiles > PROBE_N ? g_nfiles - PROBE_N : 0, 1, ms);
    double again = median_ms(ms, k);
    InterlockedExchange(&g_done, 1);
    eb_log("warm: %ld files opened in %.1f s on %d threads, done %.1f s after the plugin loaded (%.2f ms per open, %.2f ms when opened again; "
           "%ld could not be opened; %d folders and archives, listed in %d ms)",
           g_opened, pass_s, th, (qpc() - g_t0) * g_ms / 1000.0, first_ms, again, g_failed, g_roots, g_listed_ms);
    if (again >= SLOW_MS) {
        eb_log("warm: opening a file a second time is not faster on this PC (%.2f ms) - the game gains nothing from the warm-up", again);
        return 0;
    }
    // the antivirus can forget what it has looked at while the game still starts (new signatures): look every 2 s,
    // until the game's application loop runs (the main menu: the loading is over, and nothing of ours may disturb play)
    for (int round = 1, redo = 0; redo < 2 && !eb_app_loop && (qpc() - g_t0) * g_ms < WATCH_MS; round++) {
        Sleep(2000);
        double m = 0, m2 = 0;
        if (eb_app_loop || !slow_again(round * 2, again, &m) || !slow_again(round * 2 + 1, again, &m2)) continue;
        redo++;
        InterlockedIncrement(&g_pass);
        LONG before = g_opened; InterlockedExchange64(&g_open_ticks, 0);
        eb_log("warm: %.0f s after the plugin loaded the files are slow to open again (%.2f ms per open: the antivirus forgot them) - opening all once more",
               (qpc() - g_t0) * g_ms / 1000.0, (m + m2) / 2);
        t = qpc();
        run_pass(th);
        LONG n2 = g_opened - before;
        eb_log("warm: %ld files opened once more in %.1f s (%.2f ms per open)", n2, (qpc() - t) * g_ms / 1000.0,
               n2 > 0 ? (double)g_open_ticks * g_ms / n2 : 0.0);
    }
    if (g_looks_n) eb_log("warm: looks until the main menu (seconds after the plugin loaded:median ms of %d opens/how many took %.1f ms or more):%s", PROBE_N, COLD_MS, g_looks);
    return 0;
}

// ---- how well it worked (warm_files=2, [test] open_log) ----
static void* hk_openread(const char* fname) {
    int st = 2;
    if (fname) {
        uint32_t k = rel_slot(rel_hash(fname, strlen(fname)), 0);
        if (k) st = g_rel[k - 1].pending > 0 ? 1 : 0;
        else if (!g_listed) st = 1;                               // the list is still being made: not reached yet
    }
    LONGLONG t = qpc();
    void* r = o_openread(fname);
    t = qpc() - t;
    InterlockedIncrement64(&g_calls[st]); InterlockedExchangeAdd64(&g_ticks[st], t);
    if (t >= g_half_ms_ticks) InterlockedIncrement64(&g_slow[st]);
    if (r) InterlockedIncrement64(&g_seen[st]);
    if (g_olog && fname) {
        char line[600];
        int n = snprintf(line, sizeof line, "%.0f\t%lu\t%c\t%d\t%.500s\t%.0f\n", (qpc() - g_t0) * g_ms, GetCurrentThreadId(), "WLO"[st], r != NULL, fname,
                         t * g_ms * 1000.0);                      // the last column: microseconds in the open
        LONG64 at = InterlockedExchangeAdd64(&g_olog_n, n);
        if (at + n <= g_olog_cap) memcpy(g_olog + at, line, (size_t)n);
    }
    return r;
}

int eb_warm_install(void) {
    int want = g_cfg.load_warm_files, olog = g_cfg.test_open_log;
    if (want < 0 || want > 3) want = g_cfg.load_warm_files = 0;
    if (!want && !olog) return 0;
#ifdef EB_TEST
    return 0;
#else
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_ms = 1000.0 / (double)f.QuadPart;
    g_half_ms_ticks = f.QuadPart / 2000;
    g_t0 = qpc();
    if (want >= 2 || olog) {                                      // statistics: the game's own opens
        uint8_t* b = (uint8_t*)g_exe;
        IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
        MH_STATUS st = MH_Initialize();
        eb_hookset hs; eb_hs_init(&hs, "warm statistics");
        if (nt->FileHeader.TimeDateStamp != 0x66FE6AB9u || memcmp(b + RVA_OPENREAD, sig_openread, sizeof sig_openread) || !rel_table())
            eb_log("warm: signature mismatch - no statistics of the game's opens");
        else {
            if (olog) { g_olog_cap = 32 << 20; g_olog = VirtualAlloc(NULL, (size_t)g_olog_cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }
            if ((st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) || !eb_hs_add(&hs, b + RVA_OPENREAD, (void*)hk_openread, (void**)&o_openread) ||
                !eb_hs_enable(&hs)) {
                eb_log("warm: hook failed - no statistics of the game's opens");
                g_olog = NULL;
            }
        }
    }
    if (!want) { eb_log("warm: TEST the game's opens are logged to eu4boost_opens.txt (no warm-up)"); return 0; }
    if (want == 3) { eb_log("warm: only the time of the game's own file opens is measured (no warm-up, for comparison)"); return 0; }
    HANDLE h = CreateThread(NULL, 256 << 10, starter, NULL, 0, NULL);
    if (!h) { eb_log("warm: could not start - the game opens its files as usual"); g_cfg.load_warm_files = 0; return 0; }
    CloseHandle(h);
    eb_log("warm: the game's and the mods' files are opened ahead on threads of our own (EXPERIMENT%s)",
           want >= 2 ? ", with statistics of the game's opens" : "");
    return 1;
#endif
}
void eb_warm_report(void) {
    int want = g_cfg.load_warm_files;
    if ((want == 1 || want == 2) && !g_done)
        eb_log("warm: not finished at exit: %ld of %d files opened", g_opened, g_nfiles);
    if (o_openread && want) {
        LONG64 w = g_seen[0], l = g_seen[1], o = g_seen[2];
        double s[3]; for (int i = 0; i < 3; i++) s[i] = (double)g_ticks[i] * g_ms / 1000.0;
        if (want != 3)
            eb_log("warm: the game opened %lld files: %lld were opened ahead, %lld before the warm-up reached them, %lld not covered (inside archives, documents folder)",
                   w + l + o, w, l, o);
        eb_log("warm: the game's own opens took %.1f s in all (%lld calls, %lld of them 0.5 ms or longer)%s", s[0] + s[1] + s[2],
               g_calls[0] + g_calls[1] + g_calls[2], g_slow[0] + g_slow[1] + g_slow[2], want == 3 ? " - no warm-up in this run" : "");
        if (want != 3)
            eb_log("warm:   opened ahead %.1f s (%lld slow), not reached yet %.1f s (%lld slow), not covered %.1f s (%lld slow)",
                   s[0], g_slow[0], s[1], g_slow[1], s[2], g_slow[2]);
    }
    if (g_olog && g_olog_n > 0) {
        wchar_t p[MAX_PATH]; _snwprintf(p, MAX_PATH, L"%lseu4boost_opens.txt", g_dir);
        HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD wr; LONG64 n = g_olog_n < g_olog_cap ? g_olog_n : g_olog_cap;
            WriteFile(h, g_olog, (DWORD)n, &wr, NULL);
            CloseHandle(h);
        }
    }
}

// ---- self-test (harness): the list of a made-up game folder and mod, the order, the opens ----
static int tmk(const wchar_t* base, const wchar_t* rel, int dir) {
    wchar_t p[MAX_PATHW]; _snwprintf(p, MAX_PATHW, L"%ls\\%ls", base, rel);
    if (dir) return CreateDirectoryW(p, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD wr; WriteFile(h, "x", 1, &wr, NULL); CloseHandle(h);
    return 1;
}
__declspec(dllexport) int eb_test_warm(void) {
    int bad = 0;
    wchar_t tmp[MAX_PATH], base[MAX_PATHW], docs[MAX_PATHW], game[MAX_PATHW];
    if (!GetTempPathW(MAX_PATH, tmp)) return 1;
    _snwprintf(base, MAX_PATHW, L"%lseb_warm_test_%lu", tmp, GetCurrentProcessId());
    _snwprintf(game, MAX_PATHW, L"%ls\\game", base); _snwprintf(docs, MAX_PATHW, L"%ls\\docs\\", base);
    static const wchar_t* dirs[] = { L"", L"game", L"game\\common", L"game\\common\\ideas", L"game\\gfx", L"game\\localisation", L"game\\soundtrack",
                                     L"game\\plugins", L"docs", L"docs\\mod", L"docs\\mod\\m1", L"docs\\mod\\m1\\common", L"docs\\mod\\m1\\common\\ideas",
                                     L"docs\\mod\\m1\\junk", L"docs\\save games" };
    static const wchar_t* files[] = { L"game\\common\\ideas\\a.txt", L"game\\common\\b.txt", L"game\\gfx\\c.dds", L"game\\localisation\\l.yml",
                                      L"game\\soundtrack\\s.mp3", L"game\\plugins\\p.dll", L"game\\eu4.exe", L"game\\checksum_manifest.txt",
                                      L"docs\\mod\\m1\\common\\ideas\\a.txt", L"docs\\mod\\m1\\junk\\j.txt", L"docs\\mod\\m1\\descriptor.mod",
                                      L"docs\\mod\\m2.zip", L"docs\\mod\\m3.zip", L"docs\\save games\\s.eu4" };
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) if (!tmk(base, dirs[i], 1)) bad++;
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) if (!tmk(base, files[i], 0)) bad++;
    // m1 = a folder with a relative path, m2 = an archive, m3 is installed but not enabled, m4's folder is gone
    static const char m1[] = "name=\"one\"\r\npath=\"mod/m1\"\r\n", m2[] = "name=\"two\"\narchive=\"mod/m2.zip\"\n", m4[] = "name=\"four\"\npath=\"mod/m4\"\n";
    static const char js[] = "{\"disabled_dlcs\":[],\"enabled_mods\":[\"mod/m1.mod\",\"mod/m2.mod\",\"mod/m4.mod\",\"mod/missing.mod\"]}";
    struct { const wchar_t* rel; const char* text; } tx[] = { { L"docs\\mod\\m1.mod", m1 }, { L"docs\\mod\\m2.mod", m2 }, { L"docs\\mod\\m4.mod", m4 },
                                                               { L"docs\\dlc_load.json", js } };
    for (size_t i = 0; i < sizeof tx / sizeof tx[0]; i++) {
        wchar_t p[MAX_PATHW]; _snwprintf(p, MAX_PATHW, L"%ls\\%ls", base, tx[i].rel);
        HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) { bad++; continue; }
        DWORD wr; WriteFile(h, tx[i].text, (DWORD)strlen(tx[i].text), &wr, NULL); CloseHandle(h);
    }
    // userdir.txt of the game folder: names the folder that holds dlc_load.json (itself, or its parent)
    {
        wchar_t u[MAX_PATHW], want[MAX_PATHW], p[MAX_PATHW]; char a[MAX_PATHW * 3];
        _snwprintf(p, MAX_PATHW, L"%ls/userdir.txt", game);
        _snwprintf(want, MAX_PATHW, L"%ls/docs/", base);
        for (int k = 0; k < 2; k++) {
            int n = WideCharToMultiByte(CP_UTF8, 0, base, -1, a, sizeof a, NULL, NULL) - 1;
            static const char with_line_end[] = { '/', 'd', 'o', 'c', 's', '/', ' ', 13, 10, 0 };
            n += snprintf(a + n, sizeof a - (size_t)n, "%s", k ? with_line_end : "/docs");
            HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h == INVALID_HANDLE_VALUE) { bad++; continue; }
            DWORD wr; WriteFile(h, a, (DWORD)n, &wr, NULL); CloseHandle(h);
            if (!user_dir(game, u, MAX_PATHW) || _wcsicmp(u, want)) { printf("warm: userdir.txt case %d gives %ls", k, u); puts(""); bad++; }
        }
        DeleteFileW(p);
    }
    // the "slow again" decision: times far above a repeated open, the median
    {
        static const float a[8] = { 0.02f, 0.03f, 0.3f, 2.0f, 0.02f, 0.02f, 0.05f, 0.04f };
        float b[5] = { 5.0f, 0.03f, 0.02f, 0.04f, 0.05f }, c[4] = { 4.0f, 1.0f, 3.0f, 2.0f };
        // 1 ms or more counts (0.3 ms is a busy repeated open), or six times a repeated open where that is slower
        if (slow_count(a, 8, 0.03) != 1 || slow_count(a, 8, 0.5) != 0 || slow_count(c, 4, 0.03) != 4 || slow_count(c, 4, 0.4) != 2 ||
            slow_count(a, 0, 0.03) != 0) { puts("warm: slow_count"); bad++; }
        if (median_ms(b, 5) != 0.04f || median_ms(c, 4) != 2.5 || median_ms(c, 0) != 0) { puts("warm: median"); bad++; }
    }
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq); g_ms = 1000.0 / (double)fq.QuadPart;
    g_files = NULL; g_nfiles = g_cap = 0; g_roots = 0; g_next = g_opened = g_failed = g_listed = g_pass = 0; g_open_ticks = 0;
    if (!rel_table()) return bad + 1;
    int n = list_all(game, docs);
    // expected: 5 of the game (a, b, c, l, checksum_manifest; not soundtrack, plugins, eu4.exe), 2 of m1 (a.txt,
    // descriptor.mod; not junk), the archive m2.zip; nothing of m3, m4, the saves
    if (n != 8) { printf("warm: %d files listed, expected 8\n", n); bad++; }
    if (g_roots != 3) { printf("warm: %d roots, expected 3\n", g_roots); bad++; }
    int last = -1, seen_loc = -1, seen_gfx = -1;
    for (int i = 0; i < g_nfiles; i++) {
        if (g_files[i].prio < last) bad++;
        last = g_files[i].prio;
        if (wcsstr(g_files[i].path, L"\\localisation\\")) seen_loc = i;
        if (wcsstr(g_files[i].path, L"\\gfx\\")) seen_gfx = i;
        if (wcsstr(g_files[i].path, L"soundtrack") || wcsstr(g_files[i].path, L"plugins") || wcsstr(g_files[i].path, L".exe") ||
            wcsstr(g_files[i].path, L"junk") || wcsstr(g_files[i].path, L"save games") || wcsstr(g_files[i].path, L"m3")) { printf("warm: listed %ls\n", g_files[i].path); bad++; }
    }
    if (seen_loc < 0 || seen_gfx < 0 || seen_loc > seen_gfx) { printf("warm: order localisation %d, gfx %d\n", seen_loc, seen_gfx); bad++; }
    // the relative path common/ideas/a.txt has two copies; any spelling of it finds the entry
    uint32_t k = rel_slot(rel_hash("common/ideas/a.txt", 18), 0), k2 = rel_slot(rel_hash("/Common\\Ideas/A.TXT", 19), 0);
    if (!k || k != k2 || g_rel[k - 1].pending != 2) { printf("warm: relative path entry %u %u\n", k, k2); bad++; }
    if (rel_slot(rel_hash("save games/s.eu4", 16), 0)) bad++;
    for (int i = 0; i < g_nfiles; i++) warm_one(&g_files[i]);
    if (g_opened != 8 || g_failed != 0) { printf("warm: %ld opened, %ld failed\n", g_opened, g_failed); bad++; }
    if (k && g_rel[k - 1].pending != 0) bad++;
    // a second pass on threads opens all again and leaves the per-path counts alone; a look opens every file of a short list
    {
        float ms[PROBE_N]; double mean = 0;
        g_pass = 1; run_pass(3); g_pass = 0;
        if (g_opened != 16 || g_failed != 0 || (k && g_rel[k - 1].pending != 0)) { printf("warm: second pass %ld opened, %ld failed\n", g_opened, g_failed); bad++; }
        if (probe(5, 3, ms) != 8) { puts("warm: probe"); bad++; }
        slow_again(1, 1000.0, &mean);                             // with a huge "repeated open" time nothing counts as slow
        if (slow_again(1, 1000.0, &mean) || mean <= 0) { puts("warm: slow_again"); bad++; }
    }
    // a file of the game can still be opened for writing without sharing and deleted afterwards
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        wchar_t p[MAX_PATHW]; _snwprintf(p, MAX_PATHW, L"%ls\\%ls", base, files[i]);
        if (!DeleteFileW(p)) bad++;
    }
    for (size_t i = 0; i < sizeof tx / sizeof tx[0]; i++) { wchar_t p[MAX_PATHW]; _snwprintf(p, MAX_PATHW, L"%ls\\%ls", base, tx[i].rel); DeleteFileW(p); }
    for (int i = (int)(sizeof dirs / sizeof dirs[0]) - 1; i >= 0; i--) { wchar_t p[MAX_PATHW]; _snwprintf(p, MAX_PATHW, L"%ls\\%ls", base, dirs[i]); RemoveDirectoryW(p); }
    for (int i = 0; i < g_nfiles; i++) free(g_files[i].path);
    free(g_files); g_files = NULL; g_nfiles = g_cap = 0;
    VirtualFree(g_rel, 0, MEM_RELEASE); g_rel = NULL;
    printf("file warm-up checks: %d wrong answers\n", bad);
    return bad;
}
