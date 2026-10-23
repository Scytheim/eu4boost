// Throttle per-frame polling of Paradox online services.
//
// At speed 5 the game advances one day per frame, so everything done once per frame costs
// simulation speed. Profiling (5x speed, 2026-10-14) showed on the main thread:
//   * POPS_RunCallbacks (pops_api.dll) called twice per frame: from CPopsWrapper::Update
//     (eu4.exe+0x14ada90) and from the scene update (eu4.exe+0x14c5810) - ~4-6%
//   * the Nakama client tick (eu4.exe+0x168dff0) - ~2.5%
// Both only pump asynchronous callbacks (login state, telemetry, online features), so running
// them every `online_poll_ms` instead of every frame just delays those callbacks a little.
//
// But every step of an online operation waits for such a pump: POPS_RunCallbacks first calls
// pdx_http_update (pops_api.dll), which hands finished HTTP requests to the game. Deleting a save
// starts a Paradox cloud sync (FUN_1414af550: list the files, then download, upload and delete them
// one at a time), and in 0.4.0, with pumps 100 ms apart even in menus, each of its steps finished up
// to 100 ms late (users reported slower deleting). Since 0.4.1 ([online] only_while_running=1, the
// default) the throttle applies only while game days are processed - the daily tick hook in guard.c
// records when the latest one ran; when none has run for RUN_WINDOW_MS (paused, menus, dialogs, the
// loading screen) every pump runs, as in the game. The speed-5 gain is unchanged.
#include "common.h"
#include <string.h>
#include "MinHook.h"

typedef uintptr_t (*pops_rcb_t)(void*, void*);
// Thread that first pumps callbacks (the game's main thread). Calls from any other thread are
// passed straight through, so the unsynchronised slot tables are only touched by one thread.
static DWORD g_pops_tid, g_nakama_tid;
typedef void (*nakama_tick_t)(void*);
static pops_rcb_t o_pops_rcb;
static nakama_tick_t o_nakama_tick;
static volatile LONG64 g_pops_calls, g_pops_skips, g_nakama_calls, g_nakama_skips, g_pops_still, g_nakama_still;

#define RUN_WINDOW_MS 300u
// 1 while days are being processed (the latest daily tick ended less than RUN_WINDOW_MS ago); without
// the daily tick hook the throttle always applies, as in 0.4.0
static int days_running(ULONGLONG now) {
    if (!g_cfg.online_running_only || !eb_day_hooked) return 1;
    ULONGLONG d = eb_day_ms;
    return d && now - d < RUN_WINDOW_MS;
}

// The two POPS call sites pass different handles, so the interval is tracked per handle
// (a single shared timer would let the first caller in each frame starve the second).
typedef struct { void* key; ULONGLONG last; } slot_t;
static slot_t g_pops_slots[8], g_nakama_slots[4];

// Returns 1 if `key` may run now. Unknown keys (table full) always run.
static int due(slot_t* tab, int n, void* key, ULONGLONG now) {
    for (int i = 0; i < n; i++) {
        if (tab[i].key == key) {
            if (now - tab[i].last < (ULONGLONG)g_cfg.online_poll_ms) return 0;
            tab[i].last = now; return 1;
        }
        if (!tab[i].key) { tab[i].key = key; tab[i].last = now; return 1; }
    }
    return 1;
}

#define RVA_NAKAMA_TICK 0x168dff0u
static const uint8_t sig_nakama[16] = { 0x48,0x89,0x5c,0x24,0x20,0x41,0x56,0x48,0x83,0xec,0x30,0x4c,0x8b,0xf1,0xbb,0x01 };

static int owner_thread(DWORD* tid) {
    DWORD me = GetCurrentThreadId();
    DWORD owner = (DWORD)InterlockedCompareExchange((volatile LONG*)tid, (LONG)me, 0);
    return owner == 0 || owner == me;
}

// Both call sites (eu4.exe+0x14ada9d, +0x14c5adb) ignore the return value.
// Called only from the main thread, so the slot tables need no locking.
static uintptr_t hk_pops_rcb(void* a, void* b) {
    if (owner_thread(&g_pops_tid)) {
        ULONGLONG now = GetTickCount64();
        if (!days_running(now)) g_pops_still++;
        else if (!due(g_pops_slots, 8, a, now)) { g_pops_skips++; return 0; }
    }
    g_pops_calls++;
    return o_pops_rcb(a, b);
}

static void hk_nakama_tick(void* self) {
    if (owner_thread(&g_nakama_tid)) {
        ULONGLONG now = GetTickCount64();
        if (!days_running(now)) g_nakama_still++;
        else if (!due(g_nakama_slots, 4, self, now)) { g_nakama_skips++; return; }
    }
    g_nakama_calls++;
    o_nakama_tick(self);
}

int eb_online_install(void) {
    if (g_cfg.online_poll_ms <= 0) { eb_log("online: polling left at every frame (poll_interval_ms=0)"); return 1; }
    int ok = 0;
    o_pops_rcb = (pops_rcb_t)patch_iat(g_exe, "pops_api.dll", "POPS_RunCallbacks", (void*)hk_pops_rcb);
    const char* when = g_cfg.online_running_only ? " while game days run (paused, menus, loading: every frame)" : " (always)";
    if (o_pops_rcb) { ok++; eb_log("online: POPS_RunCallbacks limited to every %d ms%s", g_cfg.online_poll_ms, when); }
    else eb_log("online: POPS_RunCallbacks import not found - not changed");

    uint8_t* t = (uint8_t*)g_exe + RVA_NAKAMA_TICK;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)((uint8_t*)g_exe + ((IMAGE_DOS_HEADER*)g_exe)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp == 0x66FE6AB9u && memcmp(t, sig_nakama, sizeof sig_nakama) == 0) {
        MH_STATUS s = MH_Initialize();
        if ((s == MH_OK || s == MH_ERROR_ALREADY_INITIALIZED) &&
            MH_CreateHook(t, (void*)hk_nakama_tick, (void**)&o_nakama_tick) == MH_OK && MH_EnableHook(t) == MH_OK) {
            ok++; eb_log("online: Nakama tick limited to every %d ms%s", g_cfg.online_poll_ms, when);
        } else eb_log("online: Nakama hook failed");
    } else eb_log("online: Nakama tick signature mismatch - not changed");
    return ok;
}

void eb_online_report(void) {
    if (g_cfg.online_poll_ms <= 0) return;
    eb_log("online: POPS callbacks run %lld / skipped %lld (%lld of the runs while the game stood still), Nakama ticks run %lld / skipped %lld (%lld)",
           (long long)g_pops_calls, (long long)g_pops_skips, (long long)g_pops_still,
           (long long)g_nakama_calls, (long long)g_nakama_skips, (long long)g_nakama_still);
}
