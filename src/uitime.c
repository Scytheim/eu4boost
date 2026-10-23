// Timing of the screens around saves and of loading. Two users:
//   [general] ui_timing=1 (diagnostics, default off): every timed call is logged with what ran inside
//     it and the zero-filled mallocs (alloc.c) it made, followed by the next frames; the Paradox cloud
//     sync after deleting is timed by a watcher thread.
//   [report] slow=1 (with crash reports, on by default): a save list over slow_ui_ms (the save dialog,
//     the load screen, deleting rebuilds it twice) or a load over slow_load_s becomes an event of the
//     slow report (crash.c); every load's time is logged in one line.
//
// Users of 0.4.0 reported that deleting a save, opening the save dialog and loading a game felt slower.
// What runs there (static analysis 2026-10-23):
//   * the in-game save dialog: show FUN_141172be0 -> list FUN_141172e30; the front end list
//     FUN_14112dce0. Both open every save file and read its meta (FUN_1411bfbc0 per file, zip entries
//     through FUN_1405cd120). 0.4.0 wrote ~68 MB of zeros per file there (alloc.c, fixed in 0.4.1).
//   * deleting: FUN_1411be490 (front end) / FUN_1411beea0 (in-game dialog) remove the file, rebuild
//     the list and start a Paradox cloud sync (FUN_1414af550) whose steps finish in POPS callbacks; the
//     client's busy flag (+0x30) stays set until the sync is over.
//   * loading: the load screen's start button runs FUN_14110d760, which reads and parses the save
//     right there (FUN_140d84b80 "LOADING_SAVEGAME"; the main thread does it, a late-game save takes
//     seconds); the in-game load dialog starts in FUN_141176020; a new game starts in the front end's
//     frame function FUN_1410da520 when its "start game" flag (+0x715) is set. It goes through
//     "Initializing savegame" FUN_1407751b0 and ends with "Applying savegame fixes" FUN_140777b80, or
//     for a new game with "Initializing new game" FUN_140773b20 (which runs the monthly update
//     FUN_14075d690 once). After that the game still sets up the map (a late-game save: one ~5 s
//     frame), so a load counts from that start until frames run smoothly - no gap over 500 ms for
//     1 s - outside the start functions (a frame = one POPS_RunCallbacks call with the first handle
//     seen; the game pumps it once per frame from CPopsWrapper::Update).
#include "common.h"
#include <string.h>
#include <stdio.h>
#include "MinHook.h"

typedef uintptr_t (*fn4_t)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);   // every target takes at most 4 integer args
enum { U_SHOW, U_GAME_LIST, U_MENU_LIST, U_DEL_MENU, U_DEL_GAME, U_FRONT, U_FLOAD, U_GLOAD,
       U_FILE, U_ZIP, U_INIT_SAVE, U_FIXES, U_INIT_GAME, U_MONTHLY, U_SYNC, NU };
typedef struct { const char* name; uint32_t rva; uint8_t sig[16]; int top; const char* slow; } target_t;
static const target_t g_tg[NU] = {
    { "in-game save dialog opened",    0x1172be0, { 0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0x48,0x8b,0x49,0x30,0x48,0x85,0xc9 }, 1, NULL },
    { "in-game save list built",       0x1172e30, { 0x88,0x54,0x24,0x10,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57 }, 1, "save dialog list" },
    { "save list built",               0x112dce0, { 0x48,0x8b,0xc4,0x48,0x89,0x58,0x10,0x48,0x89,0x70,0x18,0x55,0x57,0x41,0x54,0x41 }, 1, "load screen list" },
    { "saves deleted (front end)",     0x11be490, { 0x40,0x56,0x48,0x83,0xec,0x20,0x48,0x8b,0xf1,0x48,0x89,0x5c,0x24,0x30,0x48,0x8b }, 1, "delete (front end)" },
    { "save deleted (in-game dialog)", 0x11beea0, { 0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xb9,0xb8,0x01,0x00 }, 1, "delete (in-game)" },
    { "front end frame",               0x10da520, { 0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x55,0x57,0x41,0x54,0x41,0x56 }, 1, NULL },
    { "save read (load screen)",       0x110d760, { 0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x55,0x57,0x41,0x56,0x48,0x8d }, 1, NULL },
    { "in-game load started",          0x1176020, { 0x48,0x8b,0xc4,0x48,0x89,0x58,0x08,0x48,0x89,0x70,0x10,0x57,0x48,0x83,0xec,0x70 }, 1, NULL },
    { "save files read",               0x11bfbc0, { 0x48,0x89,0x4c,0x24,0x08,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57 }, 0, NULL },
    { "zip entries read",              0x05cd120, { 0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x55,0x57,0x41,0x54,0x41,0x56 }, 0, NULL },
    { "savegame init",                 0x07751b0, { 0x48,0x89,0x4c,0x24,0x08,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41 }, 1, NULL },
    { "savegame fixes",                0x0777b80, { 0x48,0x89,0x4c,0x24,0x08,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41 }, 0, NULL },
    { "new game init",                 0x0773b20, { 0x48,0x89,0x4c,0x24,0x08,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41 }, 0, NULL },
    { "monthly update",                0x075d690, { 0x88,0x54,0x24,0x10,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57 }, 0, NULL },
    { "cloud sync started",            0x14af550, { 0x40,0x53,0x48,0x83,0xec,0x30,0x83,0x79,0x30,0x00,0x48,0x8b,0xd9,0x74,0x35,0x48 }, 0, NULL },
};
#define FRONT_START_FLAG 0x715            // FUN_1410da520(front end): byte set while a game is to be started
static fn4_t g_orig[NU];
static volatile LONG64 g_n[NU], g_t[NU];
static double g_ms;                       // ms per QPC tick
static int g_diag;                        // [general] ui_timing

// ---- frames after an event (main thread) ----
typedef uintptr_t (*pops_rcb_t)(void*, void*);
static pops_rcb_t o_rcb;
static void* g_frame_key;                 // the first POPS handle pumped: one call per frame
static LONGLONG g_frame_last;
static int g_frames_left;
static char g_frame_line[256];
static int g_frame_len;

// ---- the current load (main thread) ----
static LONGLONG g_load_t0;                // QPC at its start, 0 = no load
static LONGLONG g_load_first, g_load_settle;   // first frame after its last step / start of the current smooth run
static int g_load_done, g_load_depth;     // its last step ran / start functions still running
static LONG64 g_load_n0[NU], g_load_tt0[NU];

// ---- cloud sync watcher: the client's busy flag (+0x30) stays set until the whole sync is over ----
static HANDLE g_sync_ev;
static volatile uintptr_t g_sync_client;
static volatile LONGLONG g_sync_t0;

static LONGLONG qpc(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

static DWORD WINAPI sync_watch(LPVOID unused) {
    (void)unused;
    for (;;) {
        WaitForSingleObject(g_sync_ev, INFINITE);
        uintptr_t cl = g_sync_client;
        LONGLONG t0 = g_sync_t0, idle_since = 0, t;
        // a step's callback clears the flag and may start the next step at once: the sync is over when
        // the flag has stayed clear for 500 ms
        for (;;) {
            t = qpc();
            if (*(volatile int*)(cl + 0x30)) idle_since = 0;
            else if (!idle_since) idle_since = t;
            else if ((t - idle_since) * g_ms > 500) break;
            if ((t - t0) * g_ms > 120000) { idle_since = t; eb_log("ui: cloud sync still busy after 2 minutes"); break; }
            Sleep(1);
        }
        eb_log("ui: cloud sync finished %.1f ms after it started", (idle_since - t0) * g_ms);
    }
    return 0;
}

// what ran inside a call: "| save files read 10 in 2.1 ms | ..." since the snapshot n0/t0
static int inner_text(char* out, int cap, int self, const LONG64* n0, const LONG64* t0, LONG64 z0, LONG64 zb0) {
    int k = 0; out[0] = 0;
    for (int j = 0; j < NU && k < cap - 96; j++)
        if (j != self && g_n[j] != n0[j])
            k += snprintf(out + k, cap - k, " | %s %lld in %.1f ms", g_tg[j].name, (long long)(g_n[j] - n0[j]), (g_t[j] - t0[j]) * g_ms);
    LONG64 z, zb; eb_alloc_zero_stats(&z, &zb);
    if (z != z0 && k < cap - 64)
        k += snprintf(out + k, cap - k, " | zero-filled mallocs %lld (%.1f MB)", (long long)(z - z0), (zb - zb0) / 1048576.0);
    return k;
}

static void load_begin(const char* how) {
    g_load_t0 = qpc(); g_load_done = 0; g_load_first = g_load_settle = 0;
    for (int j = 0; j < NU; j++) { g_load_n0[j] = g_n[j]; g_load_tt0[j] = g_t[j]; }
    eb_loading = 1;
    eb_load_samples_reset();
    if (g_diag) eb_log("ui: load started (%s)", how);
}
static void load_end(LONGLONG t) {
    double ms = (t - g_load_t0) * g_ms, first = (g_load_first - g_load_t0) * g_ms;
    char in[768]; inner_text(in, sizeof in, -1, g_load_n0, g_load_tt0, 0, 0);
    eb_log("load: %.1f s from the start until frames run smoothly (first game frame after %.1f s)%s", ms / 1000.0, first / 1000.0, in);
    if (g_cfg.report_slow && ms >= g_cfg.slow_load_s * 1000.0) {
        char d[4096]; int k = snprintf(d, sizeof d, "steps%s\r\nmain thread every 5 s (a load reads the save on other threads too):\r\n", in);
        eb_load_samples_take(d + k, sizeof d - k);
        eb_slow_event("load", ms, g_cfg.slow_load_s * 1000.0, d);
    }
    g_load_t0 = 0; g_load_done = 0; g_load_first = g_load_settle = 0;
    eb_loading = 0;
}

static uintptr_t hk_rcb(void* a, void* b) {
    if (GetCurrentThreadId() == g_main_tid) {
        if (!g_frame_key) g_frame_key = a;
        if (a == g_frame_key) {
            LONGLONG t = qpc();
            // after the last step the game still sets up the map for a few seconds (a late-game save: one
            // frame of ~5 s): the load ends when frames have run smoothly (no gap over 500 ms) for 1 s
            if (g_load_t0 && g_load_done && !g_load_depth) {
                if (!g_load_first) g_load_first = g_load_settle = t;
                else if ((t - g_frame_last) * g_ms > 500.0) g_load_settle = t;
                else if ((t - g_load_settle) * g_ms >= 1000.0) load_end(g_load_settle);
            } else if (g_load_t0 && (t - g_load_t0) * g_ms > 30 * 60 * 1000.0) { g_load_t0 = 0; eb_loading = 0; }   // never finished
            if (g_diag) {
                if (g_frames_left > 0) {
                    g_frame_len += snprintf(g_frame_line + g_frame_len, sizeof g_frame_line - g_frame_len, " %.1f", (t - g_frame_last) * g_ms);
                    if (--g_frames_left == 0) eb_log("ui:   next frames (ms after the event, then frame intervals):%s", g_frame_line);
                } else if (g_frame_last && (t - g_frame_last) * g_ms >= 100.0)
                    eb_log("ui: long frame %.1f ms", (t - g_frame_last) * g_ms);   // e.g. the loading screen's steps
            }
            g_frame_last = t;
        }
    }
    return o_rcb(a, b);
}

static uintptr_t timed(int i, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    LONG64 n0[NU], t0[NU], z0 = 0, zb0 = 0;
    int main = GetCurrentThreadId() == g_main_tid;
    if (g_tg[i].top) {
        for (int j = 0; j < NU; j++) { n0[j] = g_n[j]; t0[j] = g_t[j]; }
        eb_alloc_zero_stats(&z0, &zb0);
    }
    int starter = main && (i == U_FRONT || i == U_FLOAD || i == U_GLOAD);
    if (starter) {
        if (!g_load_t0 && (i != U_FRONT || *(volatile uint8_t*)(a + FRONT_START_FLAG)))
            load_begin(i == U_FLOAD ? "load screen" : i == U_GLOAD ? "in-game load dialog" : "new game");
        g_load_depth++;
    }
    LONGLONG s = qpc();
    uintptr_t r = g_orig[i](a, b, c, d);
    LONGLONG e = qpc(), dt = e - s;
    InterlockedIncrement64(&g_n[i]); InterlockedAdd64(&g_t[i], dt);
    if (starter) g_load_depth--;
    if ((i == U_FIXES || i == U_INIT_GAME) && g_load_t0) g_load_done = 1;
    if (i == U_SYNC && g_diag && g_sync_ev && *(volatile int*)(a + 0x30)) {
        g_sync_client = a; g_sync_t0 = s; SetEvent(g_sync_ev);
    }
    if (g_tg[i].top) {
        double ms = dt * g_ms;
        int slow = g_tg[i].slow && g_cfg.report_slow && ms >= g_cfg.slow_ui_ms;
        // the front end runs FUN_1410da520 every frame: only calls that did something are logged
        int inner = 0;
        for (int j = 0; j < NU; j++) if (j != i && g_n[j] != n0[j]) inner = 1;
        if ((g_diag && (inner || ms >= 5.0)) || slow) {
            char in[768]; inner_text(in, sizeof in, i, n0, t0, z0, zb0);
            if (g_diag) {
                eb_log("ui: %s in %.1f ms%s", g_tg[i].name, ms, in);
                if (main && o_rcb) { g_frames_left = 4; g_frame_len = 0; g_frame_line[0] = 0; g_frame_last = e; }
            }
            if (slow) eb_slow_event(g_tg[i].slow, ms, g_cfg.slow_ui_ms, in[0] ? in + 3 : "");
        }
    }
    return r;
}

#define HK(i) static uintptr_t hk_##i(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) { return timed(i, a, b, c, d); }
HK(0) HK(1) HK(2) HK(3) HK(4) HK(5) HK(6) HK(7) HK(8) HK(9) HK(10) HK(11) HK(12) HK(13) HK(14)
static void* const g_hk[NU] = { (void*)hk_0, (void*)hk_1, (void*)hk_2, (void*)hk_3, (void*)hk_4, (void*)hk_5, (void*)hk_6, (void*)hk_7,
                                (void*)hk_8, (void*)hk_9, (void*)hk_10, (void*)hk_11, (void*)hk_12, (void*)hk_13, (void*)hk_14 };
_Static_assert(NU == 15, "one hook function per target");

int eb_uitime_install(void) {
    g_diag = g_cfg.ui_timing;
    if (!g_diag && !(g_cfg.report_capture && g_cfg.report_slow)) return 0;
#ifdef EB_TEST
    return 0;
#else
    uint8_t* b = (uint8_t*)g_exe;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(b + ((IMAGE_DOS_HEADER*)g_exe)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != 0x66FE6AB9u) { eb_log("ui: unknown eu4.exe - timing off"); return 0; }
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_ms = 1000.0 / (double)f.QuadPart;
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) { eb_log("ui: MinHook init failed"); return 0; }
    int ok = 0;
    for (int i = 0; i < NU; i++) {
        uint8_t* t = b + g_tg[i].rva;
        if (memcmp(t, g_tg[i].sig, sizeof g_tg[i].sig)) { eb_log("ui: %s: signature mismatch - not timed", g_tg[i].name); continue; }
        if (MH_CreateHook(t, g_hk[i], (void**)&g_orig[i]) != MH_OK || MH_EnableHook(t) != MH_OK) {
            eb_log("ui: %s: hook failed - not timed", g_tg[i].name); continue;
        }
        ok++;
    }
    o_rcb = (pops_rcb_t)patch_iat(g_exe, "pops_api.dll", "POPS_RunCallbacks", (void*)hk_rcb);   // after online.c: chains to its hook
    if (g_diag && g_orig[U_SYNC]) {
        g_sync_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
        HANDLE h = g_sync_ev ? CreateThread(NULL, 0, sync_watch, NULL, 0, NULL) : NULL;
        if (h) CloseHandle(h); else g_sync_ev = NULL;
    }
    if (g_diag) eb_log("ui: DIAGNOSTICS timing of save lists, the save dialog, deleting and loading on (%d of %d functions)", ok, NU);
    else eb_log("report: save lists and loads are timed for slow reports (lists over %d ms, loads over %d s; %d of %d functions)",
                g_cfg.slow_ui_ms, g_cfg.slow_load_s, ok, NU);
    return ok;
#endif
}
