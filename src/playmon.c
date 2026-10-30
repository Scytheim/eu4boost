// Play monitor ([diag] play_monitor=1, off by default; a temporary diagnostic for long real sessions):
// what a session of many hours could run out of or slow down from, recorded while the user plays.
//
//   plugins\eu4boost_play_<start>.csv - one row every play_monitor_s seconds (default 30): game date,
//     days advanced, speed, seconds running / paused / in the menu / loading, seconds with the window in front /
//     behind / minimized; frames, fps, frames over 250 ms and 1 s, the longest frame; daily ticks with
//     their average and longest time; process memory (private = commit, working set, peak, page faults),
//     where the private memory is (mimalloc's regions, the Windows heaps, everything else by a walk of the
//     address space, mapped files, images), system RAM and commit left; handles, GDI and USER objects,
//     threads; process and main thread CPU; countries and wars of the game state; what the [leaks]
//     fixes deleted so far (leakfix.c); the GUI's window list (gui_windows: every window the GUI root
//     holds, gui_updated: the ones it updates every frame) and the time of its update per frame; frame
//     times from the QPC (fr_*: frames without a daily tick, fd_*: frames with one; 50th, 90th and 99th
//     percentile; frames over 50 and 100 ms) and the input delay (in_*: keys, buttons and the wheel taken
//     from the window's queue by PeekMessageW, from the time the message was made to the start of the
//     next frame, i.e. the frame that shows it was presented).
//   plugins\eu4boost_play_<start>_events.txt - what happened in between, with time and game date:
//     focus changes, pause / resume, menu, loads, saves, frames and days over 1 s, the slow and hang
//     reports, memory or handle jumps, low system memory; the GUI's windows by window type a minute after a load
//     and every play_monitor_census_s seconds (default 600), with the places the game was at in frames
//     over 100 ms (a sample of the main thread's stack once such a frame passes 100 ms).
//   eu4boost.log - a summary line every 10 minutes and at exit (growth per hour and per game year).
// The hooks only count (crash.c's frame counter, guard.c's daily tick, uitime.c's loads, savezip.c's
// saves, the GUI update below); the rest runs on its own low-priority thread that wakes once a second.
//
// The GUI root (DAT_1423494f0) keeps every window it made in a vector at +0x110 and the ones to update
// each frame at +0x170. Its update FUN_1414e9fa0 (called once a frame by FUN_1414c5810) calls those,
// then walks all of +0x110 to delete the windows marked dead (+0xc4). A window the game drops without
// closing stays in that walk for the rest of the session (leakfix.c: province_items, diplo_actions).
#include "common.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <mimalloc.h>
#include "MinHook.h"

int eb_pm_on;
static volatile LONG64 g_frames, g_hitch250, g_hitch1000, g_ticks, g_tick_sum;
static volatile LONG64 g_frame_max, g_tick_max;          // reset by each row
static LONG64 g_tot_hitch250, g_tot_hitch1000, g_tot_frames;
static double g_qms;                                       // ms per QPC tick
static CRITICAL_SECTION g_ev_cs;
static HANDLE g_csv = INVALID_HANDLE_VALUE, g_ev = INVALID_HANDLE_VALUE;
static HANDLE g_main_th;
static LONGLONG g_t0;

#define RVA_GS 0x233fe78u
static int readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    return p && VirtualQuery(p, &mi, sizeof mi) && mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
           (const uint8_t*)p + n <= (const uint8_t*)mi.BaseAddress + mi.RegionSize;
}
static uint8_t* gamestate(void) {
    uint8_t* gs = *(uint8_t* volatile*)((uint8_t*)g_exe + RVA_GS);
    return gs && readable(gs + 0x1dd0, 0x60) ? gs : NULL;
}
static int game_hours(void) {                             // 0 = no game yet (while starting, the date is 1.1.1)
    uint8_t* gs = gamestate();
    int h = gs ? *(volatile int*)(gs + 0x1dd0) : 0;
    return h >= 43800000 + 2 * 8760 ? h : 0;
}
static void fmt_date(int hours, char* out, size_t n) {
    if (!hours) { snprintf(out, n, "-"); return; }
    int days = (hours - 43800000) / 24, y = days / 365, d = days - y * 365, m = 0;
    static const int ml[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    while (m < 11 && d >= ml[m]) d -= ml[m++];
    snprintf(out, n, "%d.%02d.%02d", y, m + 1, d + 1);
}
static LONGLONG qpc(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static void put(HANDLE h, const char* s, int n) { DWORD w; if (h != INVALID_HANDLE_VALUE && n > 0) WriteFile(h, s, (DWORD)n, &w, NULL); }

// ---- the GUI root's update (main thread, once a frame) ----
#define RVA_GUI_UPDATE 0x14e9fa0u                           // FUN_1414e9fa0(root, float dt)
static const uint8_t sig_gui_update[48] = {
    0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xec,0x30,0x0f,0x29,0x74,0x24,
    0x20,0x48,0x8b,0xf1,0x0f,0x28,0xf1,0x33,0xdb,0xf3,0x0f,0x11,0xb1,0x18,0x02,0x00,0x00,0x48,0x8b,0x89,0x40,0x01,0x00,0x00 };
typedef void (*gui_update_t)(uint8_t* root, float dt);
static gui_update_t o_gui_update;
static volatile LONG64 g_gui_calls, g_gui_ticks;           // reset by each row
static volatile LONG g_gui_windows = -1, g_gui_updated = -1;
static volatile LONG g_census_req;

typedef struct { const uint8_t* def; int n, shown; } cdef_t;
#define NCN 8192
static cdef_t g_cn[NCN], g_prev[NCN], g_grew[NCN];   // this census by type (a hash table, then sorted), the last one, the growth
static int g_have_prev;
static int str_at(const uint8_t* s, char* out, int max) {  // an MSVC std::string: 16 bytes or a pointer, size +0x10, capacity +0x18
    uint64_t size = *(const uint64_t*)(s + 0x10), cap = *(const uint64_t*)(s + 0x18);
    if (cap < 15 || size > cap || size > 4096) return -1;
    const char* p = cap > 15 ? *(const char* const*)s : (const char*)s;
    int n = (int)(size < (uint64_t)max - 1 ? size : (uint64_t)max - 1);
    if (!p || (cap > 15 && !readable(p, (size_t)n + 1))) return -1;
    for (int i = 0; i < n; i++) { char c = p[i]; out[i] = c > 0x20 && c < 0x7f ? c : '?'; }
    out[n] = 0;
    return n;
}
static int is_word(const char* s) {
    if (strlen(s) < 3) return 0;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_')) return 0;
    return 1;
}
// the window type (the GUI object's +0xb0, the definition from the .gui files): its kind (+8), vtable and the first two
// strings in it that look like names, with their offsets (the name's offset is not known yet)
static void def_name(const uint8_t* def, char* out, int max) {
    uintptr_t lo = (uintptr_t)g_exe, vt;
    if (!readable(def, 0x400) || (vt = *(const uintptr_t*)def) < lo || vt >= lo + 0x2600000) {
        snprintf(out, (size_t)max, "(+0xb0 %p)", (const void*)def);
        return;
    }
    int k = snprintf(out, (size_t)max, "%x/%llx", *(const int*)(def + 8), (unsigned long long)(vt - lo)), found = 0;
    for (int off = 0x10; off <= 0x3e0 && found < 2 && k < max - 60; off += 8) {
        char s[48];
        if (str_at(def + off, s, sizeof s) >= 3 && is_word(s)) { k += snprintf(out + k, (size_t)(max - k), " %s@%x", s, off); found++; }
    }
}
static int by_count(const void* a, const void* b) { return ((const cdef_t*)b)->n - ((const cdef_t*)a)->n; }
static uint32_t def_slot(const cdef_t* t, const uint8_t* def) {   // NCN when the table is full
    uint32_t i = (uint32_t)(((uintptr_t)def >> 4) * 0x9E3779B1u) & (NCN - 1), k = 0;
    while (k < NCN && t[i].n && t[i].def != def) { i = (i + 1) & (NCN - 1); k++; }
    return k == NCN ? NCN : i;
}
static int type_list(const cdef_t* t, int n, int with_shown, char* line, int max) {
    int k = 0;
    line[0] = 0;
    for (int i = 0; i < n && t[i].n && k < max - 160; i++) {
        char nm[160];
        def_name(t[i].def, nm, sizeof nm);
        k += snprintf(line + k, (size_t)(max - k), "%s%s %d", i ? ", " : "", nm, t[i].n);
        if (with_shown && t[i].shown) k += snprintf(line + k, (size_t)(max - k), " (%d shown)", t[i].shown);
        if (k >= max) k = max - 1;
    }
    return k;
}
// the windows of the root by window type (the GUI object's +0xb0, the type definition from the .gui files
// the window was made from): which screen leaves them behind
static int census_body(void* arg) {
    uint8_t* root = (uint8_t*)arg;
    uint8_t** b = *(uint8_t***)(root + 0x110);
    uint8_t** e = *(uint8_t***)(root + 0x118);
    if (!b || e < b) return -1;
    memset(g_cn, 0, sizeof g_cn);
    int total = 0, nodef = 0, full = 0, types = 0, shown = 0;
    for (uint8_t** q = b; q < e; q++) {
        uint8_t* w = *q;
        if (!w) continue;
        total++;
        if (w[0xc5]) shown++;
        const uint8_t* def = *(const uint8_t* const*)(w + 0xb0);
        if (!def) { nodef++; continue; }
        uint32_t i = def_slot(g_cn, def);
        if (i == NCN) { full++; continue; }
        if (!g_cn[i].n) { g_cn[i].def = def; types++; }
        g_cn[i].n++;
        if (w[0xc5]) g_cn[i].shown++;
    }
    // the types that grew since the last census (n = how many more)
    int ngrew = 0, gone = 0;
    if (g_have_prev) {
        for (int i = 0; i < NCN; i++) {
            if (!g_cn[i].n) continue;
            uint32_t j = def_slot(g_prev, g_cn[i].def);
            int before = j < NCN && g_prev[j].n ? g_prev[j].n : 0;
            if (g_cn[i].n > before) { g_grew[ngrew] = g_cn[i]; g_grew[ngrew].n = g_cn[i].n - before; ngrew++; }
        }
        for (int j = 0; j < NCN; j++) if (g_prev[j].n) { uint32_t i = def_slot(g_cn, g_prev[j].def); if (i == NCN || !g_cn[i].n) gone += g_prev[j].n; }
    }
    memcpy(g_prev, g_cn, sizeof g_cn);
    qsort(g_cn, NCN, sizeof g_cn[0], by_count);
    char line[1000];
    type_list(g_cn, 12, 1, line, sizeof line);
    eb_pm_event("gui: %d windows (%d shown; %d types, %d without one%s), %ld updated each frame; the most: %s", total, shown, types, nodef,
                full ? ", the type table full" : "", (long)g_gui_updated, line);
    if (g_have_prev) {
        int more = 0;
        for (int i = 0; i < ngrew; i++) more += g_grew[i].n;
        qsort(g_grew, (size_t)ngrew, sizeof g_grew[0], by_count);
        type_list(g_grew, ngrew < 20 ? ngrew : 20, 0, line, sizeof line);
        eb_pm_event("gui: since the last count +%d in %d types, %d of types now gone; the most new: %s", more, ngrew, gone, ngrew ? line : "-");
    }
    g_have_prev = 1;
    return 0;
}
static void hk_gui_update(uint8_t* root, float dt) {
    LONGLONG t = qpc();
    o_gui_update(root, dt);
    InterlockedAdd64(&g_gui_ticks, qpc() - t);
    InterlockedIncrement64(&g_gui_calls);
    uint8_t** b = *(uint8_t***)(root + 0x110);
    uint8_t** e = *(uint8_t***)(root + 0x118);
    uint8_t** ub = *(uint8_t***)(root + 0x170);
    uint8_t** ue = *(uint8_t***)(root + 0x178);
    g_gui_windows = e >= b ? (LONG)(e - b) : -1;
    g_gui_updated = ue >= ub ? (LONG)(ue - ub) : -1;
    if (g_census_req) {
        g_census_req = 0;
        if (eb_guarded(census_body, root) < 0) eb_pm_event("gui: the window list could not be read");
    }
}
static void gui_install(void) {
#ifndef EB_TEST
    uint8_t* b = (uint8_t*)g_exe;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != 0x66FE6AB9u || memcmp(b + RVA_GUI_UPDATE, sig_gui_update, sizeof sig_gui_update)) {
        eb_log("play: the GUI update is not the known one - no GUI columns");
        return;
    }
    MH_STATUS st = MH_Initialize();
    if ((st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) || MH_CreateHook(b + RVA_GUI_UPDATE, (void*)hk_gui_update, (void**)&o_gui_update) != MH_OK ||
        MH_EnableHook(b + RVA_GUI_UPDATE) != MH_OK) { o_gui_update = NULL; eb_log("play: GUI update hook failed - no GUI columns"); }
#endif
}

// ---- frame times and input delay ----
#define FH_N 600                                           // 0.5 ms buckets up to 300 ms, the last one longer
typedef struct { uint32_t n[FH_N]; uint32_t total, over50, over100; } fhist_t;
static fhist_t g_fh[2][2];                                 // [buffer][0 = frames without a daily tick, 1 = with]
static volatile LONG g_fh_cur;                             // the buffer the main thread fills; the monitor swaps it
#define IH_N 1000                                          // input delay: 1 ms buckets up to 1 s
typedef struct { uint32_t n[IH_N]; uint32_t total; } ihist_t;
static ihist_t g_ih[2];
static volatile LONGLONG g_frame_qpc;                      // the start of the current frame
static LONG64 g_ticks_at_frame;
static volatile LONGLONG g_in_first;                       // when the first input taken in this frame was made (QPC)
static LONGLONG g_qpf;
static void fh_add(fhist_t* h, double ms) {
    int b = (int)(ms * 2.0); if (b >= FH_N) b = FH_N - 1; if (b < 0) b = 0;
    h->n[b]++; h->total++; if (ms >= 50.0) h->over50++; if (ms >= 100.0) h->over100++;
}
static double h_pct(const uint32_t* n, int nb, uint32_t total, double q, double width) {   // the bucket's upper edge
    if (!total) return 0;
    uint32_t want = (uint32_t)(q * total), c = 0;
    if (want >= total) want = total - 1;
    for (int i = 0; i < nb; i++) { c += n[i]; if (c > want) return (i + 1) * width; }
    return nb * width;
}
// playmon frame hook part: the gap from the last frame start (QPC), whether a daily tick ran in it, the input delay
static void frame_times(void) {
    LONGLONG now = qpc(), last = g_frame_qpc;
    g_frame_qpc = now;
    if (!last) { g_ticks_at_frame = g_ticks; return; }
    LONG cur = g_fh_cur;
    LONG64 t = g_ticks; int day = t != g_ticks_at_frame; g_ticks_at_frame = t;
    fh_add(&g_fh[cur][day], (now - last) * g_qms);
    LONGLONG in = g_in_first;
    if (in) {
        g_in_first = 0;
        int b = (int)((now - in) * g_qms); if (b >= IH_N) b = IH_N - 1; if (b < 0) b = 0;
        g_ih[cur].n[b]++; g_ih[cur].total++;
    }
}
// the main thread takes input from the window's queue (SDL's WIN_PumpEvents)
typedef BOOL (WINAPI *peek_t)(LPMSG, HWND, UINT, UINT, UINT);
static peek_t o_peek;
static BOOL WINAPI hk_peek(LPMSG m, HWND w, UINT lo, UINT hi, UINT rm) {
    BOOL r = o_peek(m, w, lo, hi, rm);
    if (r && (rm & PM_REMOVE) && m && (m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN || m->message == WM_LBUTTONDOWN ||
                                      m->message == WM_RBUTTONDOWN || m->message == WM_MBUTTONDOWN || m->message == WM_MOUSEWHEEL)) {
        LONGLONG made = eb_autorun_input_made(m->message);    // a test's own input: when it was posted
        if (!made) {                                          // else from the message time (GetTickCount, ~16 ms steps)
            DWORD age = GetTickCount() - (DWORD)m->time;
            made = qpc() - (age < 60000 ? (LONGLONG)age * g_qpf / 1000 : 0);
        }
        if (!g_in_first || made < g_in_first) g_in_first = made;
    }
    return r;
}
// frames over 100 ms: where the main thread is once the frame passes 100 ms (one sample per frame)
#define HITCH_MS 100.0
#define NHS 1024
typedef struct { uint64_t key; uint32_t fr[8]; int nfr, n, in_day; } hsite_t;
static hsite_t g_hs[NHS];
static volatile LONG g_hs_lock;
static LONG64 g_hs_samples, g_hs_full;
static DWORD WINAPI hitch_thread(LPVOID u) {
    (void)u;
    LONGLONG done = 0;
    uintptr_t lo = (uintptr_t)g_exe;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)((uint8_t*)g_exe + ((IMAGE_DOS_HEADER*)g_exe)->e_lfanew);
    uintptr_t hi = lo + nt->OptionalHeader.SizeOfImage;
    for (;;) {
        Sleep(5);
        LONGLONG f = g_frame_qpc;
        if (!f || f == done || (qpc() - f) * g_qms < HITCH_MS) continue;
        done = f;
        int in_day = eb_in_day;
        uint64_t pcs[48]; int n = eb_sample_main_pcs(pcs, 48);
        if (n <= 0 || g_frame_qpc != f) continue;               // the frame ended meanwhile
        uint32_t fr[8]; int k = 0;
        for (int i = 0; i < n && k < 8; i++) if (pcs[i] >= lo && pcs[i] < hi) fr[k++] = (uint32_t)(pcs[i] - lo);
        if (!k) continue;
        uint64_t key = 1469598103934665603ull;
        for (int i = 0; i < k && i < 4; i++) { key ^= fr[i]; key *= 1099511628211ull; }
        while (InterlockedExchange(&g_hs_lock, 1)) Sleep(0);
        uint32_t i = (uint32_t)(key >> 20) & (NHS - 1), j = 0;
        while (j < NHS && g_hs[i].n && g_hs[i].key != key) { i = (i + 1) & (NHS - 1); j++; }
        if (j == NHS) g_hs_full++;
        else {
            if (!g_hs[i].n) { g_hs[i].key = key; memcpy(g_hs[i].fr, fr, sizeof fr); g_hs[i].nfr = k; }
            g_hs[i].n++; if (in_day) g_hs[i].in_day++;
        }
        g_hs_samples++;
        InterlockedExchange(&g_hs_lock, 0);
    }
    return 0;
}
static int by_hits(const void* a, const void* b) { return ((const hsite_t*)b)->n - ((const hsite_t*)a)->n; }
static void hitch_report(void) {                           // the monitor thread, with the census: since the last report
    static hsite_t t[NHS];
    while (InterlockedExchange(&g_hs_lock, 1)) Sleep(0);
    memcpy(t, g_hs, sizeof t); memset(g_hs, 0, sizeof g_hs);
    LONG64 ns = g_hs_samples, full = g_hs_full; g_hs_samples = 0; g_hs_full = 0;
    InterlockedExchange(&g_hs_lock, 0);
    if (!ns) return;
    qsort(t, NHS, sizeof t[0], by_hits);
    char line[1100]; int k = 0;
    line[0] = 0;
    for (int i = 0; i < NHS && t[i].n && i < 8 && k < (int)sizeof line - 160; i++) {
        k += snprintf(line + k, sizeof line - (size_t)k, "%s%d%s:", i ? " | " : "", t[i].n, t[i].in_day == t[i].n ? " (tick)" : t[i].in_day ? " (some tick)" : "");
        for (int j = 0; j < t[i].nfr && j < 6 && k < (int)sizeof line - 20; j++) k += snprintf(line + k, sizeof line - (size_t)k, " %x", t[i].fr[j]);
    }
    eb_pm_event("hitches: %lld frames over %.0f ms sampled%s; where (eu4 RVAs, innermost first): %s", (long long)ns, HITCH_MS,
                full ? " (site table full)" : "", line);
}

// ---- called from the hooks ----
void eb_pm_event(const char* fmt, ...) {
    if (!eb_pm_on) return;
    char b[1200], date[24];
    SYSTEMTIME st; GetLocalTime(&st);
    fmt_date(game_hours(), date, sizeof date);
    int k = snprintf(b, sizeof b, "[%02d:%02d:%02d %6.0f s] %-10s ", st.wHour, st.wMinute, st.wSecond, (qpc() - g_t0) * g_qms / 1000.0, date);
    va_list a; va_start(a, fmt); k += vsnprintf(b + k, sizeof b - (size_t)k - 2, fmt, a); va_end(a);
    if (k > (int)sizeof b - 3) k = (int)sizeof b - 3;
    b[k++] = '\r'; b[k++] = '\n';
    EnterCriticalSection(&g_ev_cs); put(g_ev, b, k); LeaveCriticalSection(&g_ev_cs);
}
static void lmax(volatile LONG64* m, LONG64 v) { for (LONG64 o = *m; v > o && InterlockedCompareExchange64(m, v, o) != o; o = *m) {} }
void eb_pm_frame(ULONGLONG gap_ms) {                       // crash.c, main thread, once a frame
    g_frames++;
    frame_times();
    if (gap_ms >= 250) { g_hitch250++; if (gap_ms >= 1000) { g_hitch1000++; eb_pm_event("long frame: %llu ms", (unsigned long long)gap_ms); } }
    if ((LONG64)gap_ms > g_frame_max) g_frame_max = (LONG64)gap_ms;
}
void eb_pm_day(LONGLONG ticks) {                           // guard.c, main thread, once a day
    g_ticks++; g_tick_sum += ticks;
    if (ticks > g_tick_max) g_tick_max = ticks;
    if (ticks * g_qms >= 1000.0) eb_pm_event("long day: %.0f ms", ticks * g_qms);
}

// ---- the monitor thread ----
typedef struct { SIZE_T cb, cbAllocated, cbCommitted, cbReserved, cbMaxReserve; } heap_summary_t;
typedef BOOL (WINAPI *heap_summary_fn)(HANDLE, DWORD, heap_summary_t*);
typedef struct { uint64_t mi, other, mapped, image, heap_c, heap_a; } where_t;
static void where_is(where_t* w) {
    memset(w, 0, sizeof *w);
    for (uintptr_t a = 0x10000; a < 0x7FFFFFFF0000ull; ) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery((void*)a, &m, sizeof m)) break;
        uintptr_t next = (uintptr_t)m.BaseAddress + m.RegionSize;
        if (next <= a) break;
        if (m.State == MEM_COMMIT) {
            if (m.Type == MEM_PRIVATE) { if (mi_is_in_heap_region((const char*)m.BaseAddress + 1)) w->mi += m.RegionSize; else w->other += m.RegionSize; }
            else if (m.Type == MEM_MAPPED) w->mapped += m.RegionSize;
            else w->image += m.RegionSize;
        }
        a = next;
    }
    static heap_summary_fn hsum;
    if (!hsum) hsum = (heap_summary_fn)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "HeapSummary");
    HANDLE hs[128]; DWORD n = GetProcessHeaps(128, hs);
    for (DWORD i = 0; i < n && i < 128 && hsum; i++) {
        heap_summary_t s; memset(&s, 0, sizeof s); s.cb = sizeof s;
        if (hsum(hs[i], 0, &s)) { w->heap_c += s.cbCommitted; w->heap_a += s.cbAllocated; }
    }
}
static int thread_count(void) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (s == INVALID_HANDLE_VALUE) return -1;
    THREADENTRY32 te; te.dwSize = sizeof te; int n = 0; DWORD pid = GetCurrentProcessId();
    if (Thread32First(s, &te)) do { if (te.th32OwnerProcessID == pid) n++; te.dwSize = sizeof te; } while (Thread32Next(s, &te));
    CloseHandle(s);
    return n;
}
static HWND g_wnd;
static BOOL CALLBACK find_wnd(HWND h, LPARAM lp) {
    DWORD pid; GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT r; GetWindowRect(h, &r);
    LONG64 area = (LONG64)(r.right - r.left) * (r.bottom - r.top), best = 0;
    if (g_wnd) { RECT q; GetWindowRect(g_wnd, &q); best = (LONG64)(q.right - q.left) * (q.bottom - q.top); }
    if (!g_wnd || area > best || IsIconic(h)) g_wnd = h;
    (void)lp;
    return TRUE;
}
static int focus_state(void) {                             // 0 in front, 1 behind, 2 minimized
    if (!g_wnd || !IsWindow(g_wnd)) { g_wnd = NULL; EnumWindows(find_wnd, 0); }
    if (g_wnd && IsIconic(g_wnd)) return 2;
    HWND f = GetForegroundWindow(); DWORD pid = 0;
    if (f) GetWindowThreadProcessId(f, &pid);
    return pid == GetCurrentProcessId() ? 0 : 1;
}
static uint64_t ft64(FILETIME f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }

typedef struct {                                           // the latest row's values, for the deltas and the summary
    LONGLONG t; uint64_t proc_cpu, main_cpu; DWORD faults; uint64_t priv; DWORD handles, gdi; int threads;
} last_t;
// growth is counted from the first row of the game played now: a load (or the first game) starts it again
static struct { uint64_t priv0, priv_max, ws_max; int hours0; LONGLONG t0; double days; int rows, loads; } g_sum;
static const char* const fname[3] = { "in front", "behind", "minimized" };

static DWORD WINAPI monitor(LPVOID u) {
    (void)u;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    SYSTEM_INFO si; GetSystemInfo(&si);
    int ncpu = (int)si.dwNumberOfProcessors;
    int interval = g_cfg.play_monitor_s >= 5 ? g_cfg.play_monitor_s : 5;
    last_t last; memset(&last, 0, sizeof last);
    int fsec[3] = { 0, 0, 0 }, ssec[4] = { 0, 0, 0, 0 };   // seconds per focus state / running, paused, menu, loading in this row
    int focus = -1, state = -1, hours_prev = game_hours(), still = 0, hours_row = hours_prev, load_seen = 0;
    LONG64 front_prev = eb_ui_front_frames();
    LONGLONG paused_since = 0, next_row = qpc() + (LONGLONG)(interval * 1000.0 / g_qms), next_log = qpc() + (LONGLONG)(600000.0 / g_qms);
    int low_cool = 0; uint64_t gb_logged = 0;
    LONGLONG census_at = 0;                                 // the GUI's windows by type a minute after a load, and every census_s
    int census_s = g_cfg.play_monitor_census_s >= 30 ? g_cfg.play_monitor_census_s : 30;
    LONGLONG next_census = qpc() + (LONGLONG)(census_s * 1000.0 / g_qms);
    for (;;) {
        Sleep(1000);
        // once a second: focus, and whether the game runs, is paused or is in the menu
        int f = focus_state();
        if (f != focus) { if (focus >= 0) eb_pm_event("window: %s -> %s", fname[focus], fname[f]); focus = f; }
        fsec[f]++;
        int h = game_hours();
        LONG64 front = eb_ui_front_frames();
        // speed 1 takes about 2 s a day: 5 s without a new day is a pause
        int st = eb_loading ? 3 : (front != front_prev || !h) ? 2 : h != hours_prev ? 0 : (++still >= 5 ? 1 : state < 0 || state == 3 ? 1 : state);
        if (h != hours_prev || eb_loading) still = 0;
        front_prev = front; hours_prev = h;
        if (st != state) {
            static const char* const sn[4] = { "running", "paused", "menu", "loading" };
            if (state == 1 && paused_since) eb_pm_event("%s (paused for %.0f s)", sn[st], (qpc() - paused_since) * g_qms / 1000.0);
            else if (state >= 0) eb_pm_event("%s", sn[st]);
            if (st == 1) paused_since = qpc(); else paused_since = 0;
            state = st;
        }
        ssec[st]++;
        if (st == 3) load_seen = 1;
        LONGLONG now = qpc();
        if (now < next_row) continue;
        next_row = now + (LONGLONG)(interval * 1000.0 / g_qms);
        // a row
        double dt = last.t ? (now - last.t) * g_qms / 1000.0 : interval;
        LONG64 frames = InterlockedExchange64(&g_frames, 0), h250 = InterlockedExchange64(&g_hitch250, 0), h1000 = InterlockedExchange64(&g_hitch1000, 0);
        LONG64 fmax = InterlockedExchange64(&g_frame_max, 0), ticks = InterlockedExchange64(&g_ticks, 0), tsum = InterlockedExchange64(&g_tick_sum, 0);
        LONG64 tmax = InterlockedExchange64(&g_tick_max, 0);
        g_tot_frames += frames; g_tot_hitch250 += h250; g_tot_hitch1000 += h1000;
        // a load: the first row with a date after the loading screen, or a date going back or far ahead (a new game
        // from the menu); a fast game can pass 400 days in 30 s
        int days = (h - hours_row) / 24, jump = 0;
        if (h && ((load_seen && !eb_loading) || days < 0 || days > interval * 1000)) { jump = 1; days = 0; load_seen = 0; }
        hours_row = h;
        PROCESS_MEMORY_COUNTERS_EX pm; memset(&pm, 0, sizeof pm); pm.cb = sizeof pm;
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm);
        MEMORYSTATUSEX ms; ms.dwLength = sizeof ms; GlobalMemoryStatusEx(&ms);
        where_t w; where_is(&w);
        DWORD handles = 0; GetProcessHandleCount(GetCurrentProcess(), &handles);
        DWORD gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS), usr = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
        int nth = thread_count();
        FILETIME c0, e0, k0, u0; uint64_t pcpu = 0, mcpu = 0;
        if (GetProcessTimes(GetCurrentProcess(), &c0, &e0, &k0, &u0)) pcpu = ft64(k0) + ft64(u0);
        if (g_main_th && GetThreadTimes(g_main_th, &c0, &e0, &k0, &u0)) mcpu = ft64(k0) + ft64(u0);
        double cpu = last.t ? (pcpu - last.proc_cpu) / 1e7 / dt / ncpu * 100.0 : 0, mainc = last.t ? (mcpu - last.main_cpu) / 1e7 / dt * 100.0 : 0;
        uint8_t* gs = gamestate();
        int countries = gs && readable(gs + 0x1d14, 4) ? *(volatile int*)(gs + 0x1d14) : -1;
        int wars = gs && readable(gs + 0x1e3c, 4) ? *(volatile int*)(gs + 0x1e3c) : -1;   // gamestate+0x1e28: data +8, count +0x14
        int speed = gs && readable(gs + 0x1e08, 4) ? *(volatile int*)(gs + 0x1e08) + 1 : 0;
        LONG64 lk_copies, lk_actions, lk_items, lk_entries, lk_popups; eb_leakfix_counts(&lk_copies, &lk_actions, &lk_items, &lk_entries, &lk_popups);
        LONG64 gcalls = InterlockedExchange64(&g_gui_calls, 0), gticks = InterlockedExchange64(&g_gui_ticks, 0);
        LONG old = g_fh_cur; InterlockedExchange(&g_fh_cur, old ^ 1);
        Sleep(0);
        fhist_t* f0 = &g_fh[old][0]; fhist_t* f1 = &g_fh[old][1]; ihist_t* ih = &g_ih[old];
        double fr50 = h_pct(f0->n, FH_N, f0->total, 0.5, 0.5), fr90 = h_pct(f0->n, FH_N, f0->total, 0.9, 0.5), fr99 = h_pct(f0->n, FH_N, f0->total, 0.99, 0.5);
        double fd50 = h_pct(f1->n, FH_N, f1->total, 0.5, 0.5), fd99 = h_pct(f1->n, FH_N, f1->total, 0.99, 0.5);
        unsigned o50 = f0->over50 + f1->over50, o100 = f0->over100 + f1->over100;
        double in50 = h_pct(ih->n, IH_N, ih->total, 0.5, 1.0), in90 = h_pct(ih->n, IH_N, ih->total, 0.9, 1.0), inmax = h_pct(ih->n, IH_N, ih->total, 1.0, 1.0);
        unsigned in_n = ih->total;
        memset(f0, 0, sizeof *f0); memset(f1, 0, sizeof *f1); memset(ih, 0, sizeof *ih);
        SYSTEMTIME lt; GetLocalTime(&lt);
        char date[24]; fmt_date(h, date, sizeof date);
        char row[1024];
        int k = snprintf(row, sizeof row,
            "%04d-%02d-%02d %02d:%02d:%02d,%.0f,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%lld,%.1f,%lld,%lld,%lld,%lld,%.2f,%.1f,"
            "%llu,%llu,%llu,%llu,%lu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%lu,%lu,%lu,%d,%.1f,%.1f,%d,%d,%lld,%lld,%lld,%lld,%lld,%ld,%ld,%.3f,%.1f,%.1f,%.1f,%.1f,%.1f,%u,%u,%u,%.0f,%.0f,%.0f\r\n",
            lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond, (now - g_t0) * g_qms / 1000.0, date, days, jump, speed,
            ssec[0], ssec[1], ssec[2], ssec[3], fsec[0], fsec[1], fsec[2], (long long)frames, frames / dt, (long long)h250, (long long)h1000, (long long)fmax,
            (long long)ticks, ticks ? tsum * g_qms / ticks : 0.0, tmax * g_qms,
            (unsigned long long)(pm.PrivateUsage >> 20), (unsigned long long)(pm.WorkingSetSize >> 20), (unsigned long long)(pm.PeakWorkingSetSize >> 20),
            (unsigned long long)(pm.PagefileUsage >> 20), (unsigned long)(pm.PageFaultCount - last.faults),
            (unsigned long long)(w.mi >> 20), (unsigned long long)(w.heap_c >> 20), (unsigned long long)(w.heap_a >> 20),
            (unsigned long long)(w.other >> 20), (unsigned long long)(w.mapped >> 20), (unsigned long long)(w.image >> 20),
            (unsigned long long)(ms.ullAvailPhys >> 20), (unsigned long long)(ms.ullTotalPhys >> 20), (unsigned long long)(ms.ullAvailPageFile >> 20),
            (unsigned long)handles, (unsigned long)gdi, (unsigned long)usr, nth, cpu, mainc, countries, wars,
            (long long)lk_copies, (long long)lk_actions, (long long)lk_items, (long long)lk_entries, (long long)lk_popups,
            (long)g_gui_windows, (long)g_gui_updated, gcalls ? gticks * g_qms / gcalls : 0.0,
            fr50, fr90, fr99, fd50, fd99, o50, o100, in_n, in50, in90, inmax);
        put(g_csv, row, k);
        // events from the row
        uint64_t priv = pm.PrivateUsage;
        if (last.t && priv > last.priv + (200ull << 20) && state <= 1) eb_pm_event("memory: private +%llu MB in %.0f s, now %llu MB", (unsigned long long)((priv - last.priv) >> 20), dt, (unsigned long long)(priv >> 20));
        if ((priv >> 30) > gb_logged) { gb_logged = priv >> 30; eb_pm_event("memory: private passed %llu GB (working set %llu MB, system RAM free %llu MB)", (unsigned long long)gb_logged, (unsigned long long)(pm.WorkingSetSize >> 20), (unsigned long long)(ms.ullAvailPhys >> 20)); }
        if (last.t && handles > last.handles + 500) eb_pm_event("handles: +%lu in %.0f s, now %lu", handles - last.handles, dt, handles);
        if (last.t && gdi > last.gdi + 200) eb_pm_event("GDI objects: +%lu in %.0f s, now %lu", gdi - last.gdi, dt, gdi);
        if (last.t && nth > last.threads + 20) eb_pm_event("threads: +%d in %.0f s, now %d", nth - last.threads, dt, nth);
        if (jump) eb_pm_event("date changed to %s (a load)", date);
        if (jump) census_at = now + (LONGLONG)(60000.0 / g_qms);
        if (census_at && now >= census_at && o_gui_update && h) { census_at = 0; g_census_req = 1; }
        if (low_cool) low_cool--;
        if ((ms.ullAvailPhys < (1ull << 30) || ms.ullAvailPageFile < (2ull << 30)) && !low_cool) {   // again after 20 rows at the most
            eb_pm_event("system memory low: RAM free %llu MB, commit left %llu MB", (unsigned long long)(ms.ullAvailPhys >> 20), (unsigned long long)(ms.ullAvailPageFile >> 20));
            low_cool = 20;
        }
        // the summary
        if (h && (!g_sum.hours0 || jump)) {
            if (jump) g_sum.loads++;
            g_sum.priv0 = priv; g_sum.t0 = now; g_sum.hours0 = h; g_sum.days = 0; g_sum.priv_max = 0; g_sum.ws_max = 0;
        }
        g_sum.rows++;
        if (priv > g_sum.priv_max) g_sum.priv_max = priv;
        if (pm.WorkingSetSize > g_sum.ws_max) g_sum.ws_max = pm.WorkingSetSize;
        g_sum.days += days;
        last.t = now; last.proc_cpu = pcpu; last.main_cpu = mcpu; last.faults = pm.PageFaultCount; last.priv = priv;
        last.handles = handles; last.gdi = gdi; last.threads = nth;
        memset(fsec, 0, sizeof fsec); memset(ssec, 0, sizeof ssec);
        if (now >= next_log) { next_log = now + (LONGLONG)(600000.0 / g_qms); eb_playmon_report(); }
        if (now >= next_census) { next_census = now + (LONGLONG)(census_s * 1000.0 / g_qms); if (o_gui_update && h) g_census_req = 1; hitch_report(); }
    }
    return 0;
}

void eb_playmon_report(void) {
    if (!eb_pm_on || !g_sum.hours0) return;
    PROCESS_MEMORY_COUNTERS_EX pm; memset(&pm, 0, sizeof pm); pm.cb = sizeof pm;
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm);
    double hrs = (qpc() - g_sum.t0) * g_qms / 3600000.0, years = g_sum.days / 365.0;
    double grow = ((double)pm.PrivateUsage - (double)g_sum.priv0) / 1048576.0;
    char date[24]; fmt_date(game_hours(), date, sizeof date);
    eb_log("play: since the latest load %.1f h, %s, %.1f game years; private %llu MB (then %llu, max %llu; %+.0f MB = %+.0f MB/h, %+.0f MB per game year), "
           "working set max %llu MB; loads %d; session frames %lld, over 250 ms %lld, over 1 s %lld",
           hrs, date, years, (unsigned long long)(pm.PrivateUsage >> 20), (unsigned long long)(g_sum.priv0 >> 20), (unsigned long long)(g_sum.priv_max >> 20),
           grow, hrs > 0.01 ? grow / hrs : 0.0, years > 0.05 ? grow / years : 0.0, (unsigned long long)(g_sum.ws_max >> 20), g_sum.loads,
           (long long)g_tot_frames, (long long)g_tot_hitch250, (long long)g_tot_hitch1000);
}

int eb_playmon_install(void) {
    if (!g_cfg.play_monitor) return 0;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qms = 1000.0 / (double)f.QuadPart;
    g_t0 = qpc();
    InitializeCriticalSection(&g_ev_cs);
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t p[MAX_PATH];
    _snwprintf(p, MAX_PATH, L"%lseu4boost_play_%04d%02d%02d_%02d%02d%02d.csv", g_dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    g_csv = CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    _snwprintf(p, MAX_PATH, L"%lseu4boost_play_%04d%02d%02d_%02d%02d%02d_events.txt", g_dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    g_ev = CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_csv == INVALID_HANDLE_VALUE || g_ev == INVALID_HANDLE_VALUE) { eb_log("play: DIAGNOSTICS - the files could not be made, off"); return 0; }
    static const char hdr[] =
        "time,uptime_s,date,days,date_jump,speed,run_s,paused_s,menu_s,loading_s,front_s,behind_s,minimized_s,frames,fps,frames_250ms,frames_1s,longest_frame_ms,"
        "days_ticked,day_avg_ms,day_max_ms,private_mb,working_set_mb,peak_ws_mb,pagefile_mb,page_faults,mimalloc_mb,heaps_commit_mb,"
        "heaps_alloc_mb,other_private_mb,mapped_mb,image_mb,ram_free_mb,ram_total_mb,commit_left_mb,handles,gdi,user,threads,cpu_pct,main_cpu_pct,countries,wars,leak_copies,leak_actions,leak_items,leak_entries,leak_popups,"
        "gui_windows,gui_updated,gui_ms,fr_p50,fr_p90,fr_p99,fd_p50,fd_p99,frames_50ms,frames_100ms,inputs,in_p50,in_p90,in_max\r\n";
    put(g_csv, hdr, (int)sizeof hdr - 1);
    g_main_th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, g_main_tid);
    g_qpf = f.QuadPart;
    gui_install();
#ifndef EB_TEST
    {
        void* pk = (void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "PeekMessageW");
        MH_STATUS ms = MH_Initialize();
        if (!pk || (ms != MH_OK && ms != MH_ERROR_ALREADY_INITIALIZED) || MH_CreateHook(pk, (void*)hk_peek, (void**)&o_peek) != MH_OK ||
            MH_EnableHook(pk) != MH_OK) eb_log("play: PeekMessageW hook failed - no input delay columns");
        HANDLE ht = CreateThread(NULL, 64 << 10, hitch_thread, NULL, 0, NULL);
        if (ht) { SetThreadPriority(ht, THREAD_PRIORITY_ABOVE_NORMAL); CloseHandle(ht); }
    }
#endif
    eb_pm_on = 1;
    eb_pm_event("start: eu4boost %s, a row every %d s", EU4BOOST_VERSION, g_cfg.play_monitor_s >= 5 ? g_cfg.play_monitor_s : 5);
    HANDLE t = CreateThread(NULL, 64 << 10, monitor, NULL, 0, NULL);
    if (!t) { eb_pm_on = 0; return 0; }
    CloseHandle(t);
    eb_log("play: DIAGNOSTICS - a row every %d s in eu4boost_play_*.csv and the events in eu4boost_play_*_events.txt",
           g_cfg.play_monitor_s >= 5 ? g_cfg.play_monitor_s : 5);
    return 1;
}
