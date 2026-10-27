// Test autopilot ([test] autorun_until, testing only - not in the ini files). Runs a game test without
// anyone at the keyboard: start the game as `eu4.exe -skiplauncher -continuelastsave` (the front end
// loads the save named in continue_game.json), and after the load (uitime.c's "load:" line) this
// unpauses at speed 5 the way the console's "speed 5" does (FUN_140e4fa00): the session's pause object
// (session vtable +0x100), FUN_1407e0770(pause, 1, name) and FUN_1407579f0(gamestate, 4). No console
// command runs, so the game state is not marked. A popup that pauses the game is unpaused again after
// 2 s. When the date reaches autorun_until the game window gets WM_CLOSE (a normal exit, as its X button);
// autorun_stuck_s seconds without a new day also close it. Runs in uitime.c's frame hook (main thread).
// Started that way, the front end's continue FUN_1410ee950(front end, continue callback) asks "achievements
// will be disabled" (mods, no launcher) and waits for a click; here it runs the callback (vtable +0x18)
// at once, which is what its Agree button does. The game window is minimized as soon as it shows
// ([test] autorun_minimize, default 1) and again if it comes back without being the foreground window.
#include "common.h"
#include <stdio.h>
#include <string.h>
#include <intrin.h>
#include "MinHook.h"

#define RVA_GS       0x233fe78u  // gamestate pointer
#define RVA_UNPAUSE  0x7e0770u   // (pause object, char force, const std::string* by)
#define RVA_SPEED    0x7579f0u   // (gamestate, speed index 0..4)
#define RVA_CONTINUE 0x10ee950u  // front end continue: save warnings, then the callback
#define GS_DATE      0x1dd0      // today, in hours (year + 5000) * 8760 + day of year * 24
#define GS_SESSION   0x1e00
static const uint8_t sig_unpause[16] = { 0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x48,0x89,0x7c,0x24,0x18,0x4c };
static const uint8_t sig_speed[16]   = { 0x48,0x81,0xec,0x88,0x00,0x00,0x00,0x41,0xb8,0x04,0x00,0x00,0x00,0x41,0x3b,0xd0 };
static const uint8_t sig_continue[16] = { 0x48,0x89,0x5c,0x24,0x10,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57 };

typedef void (*unpause_t)(void*, char, void*);
typedef void (*speed_t)(void*, int);
typedef void* (*getpause_t)(void*);
static unpause_t g_unpause;
static speed_t g_setspeed;
static uint8_t** g_gs;
static volatile int g_loaded;
static int g_started, g_closed, g_kicks;
static int g_last_date, g_start_date;
static ULONGLONG g_last_change, g_t_start, g_next_kick;

int eb_autorun_date(const char* s) {            // "1877.01.01" -> the game's date value, 0 if not a date
    int y, m, d;
    static const int cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    if (sscanf(s, "%d.%d.%d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31) return 0;
    return ((y + 5000) * 365 + cum[m - 1] + d - 1) * 24;
}
static void fmt_date(int v, char* out, int cap) {
    static const int ml[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int days = v / 24, y = days / 365, doy = days - y * 365, m = 0;
    while (m < 11 && doy >= ml[m]) doy -= ml[m++];
    snprintf(out, cap, "%d.%02d.%02d", y - 5000, m + 1, doy + 1);
}

static void run(void) {
    uint8_t* gs = *g_gs;
    void** session = gs ? *(void***)(gs + GS_SESSION) : NULL;
    if (!session) return;
    void* pause = ((getpause_t)(*(void***)session)[0x100 / 8])(session);
    struct { char buf[16]; uint64_t size, cap; } by = { "eu4boost", 8, 15 };   // std::string (short form)
    if (pause) g_unpause(pause, 1, &by);
    g_setspeed(gs, 4);
}

static BOOL CALLBACK close_cb(HWND h, LPARAM l) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { PostMessageW(h, WM_CLOSE, 0, 0); (*(int*)l)++; }
    return TRUE;
}
static volatile int g_deep_stop;
static int __attribute__((noinline)) deep(volatile char* up) {   // testing: recursion until the stack overflows
    volatile char pad[4096];
    pad[0] = up ? up[0] : 1;
    return g_deep_stop ? 0 : deep(pad) + pad[1];
}
static void close_game(const char* why) {
    if (g_cfg.autorun_fastfail == 3) {                         // ... and a stack overflow on the main thread (report 40)
        eb_log("autorun: %s - TEST stack overflow", why);
        deep(NULL);
    }
    if (g_cfg.autorun_fastfail == 1) {                         // testing the abnormal-exit report: as a heap check would
        eb_log("autorun: %s - TEST __fastfail", why);
        __fastfail(7);
    }
    if (g_cfg.autorun_fastfail == 2) {                         // ... and as a program that ends itself would
        eb_log("autorun: %s - TEST TerminateProcess", why);
        TerminateProcess(GetCurrentProcess(), 3);
    }
    int n = 0; EnumWindows(close_cb, (LPARAM)&n);
    g_closed = 1;
    char a[16], b[16]; fmt_date(g_start_date, a, sizeof a); fmt_date(g_last_date, b, sizeof b);
    eb_log("autorun: %s - %s to %s in %.1f s (%d unpauses), WM_CLOSE to %d window(s)", why, a, b,
           (double)(GetTickCount64() - g_t_start) / 1000.0, g_kicks, n);
}

void eb_autorun_loaded(void) { if (g_unpause) g_loaded = 1; }

typedef void (*cont_t)(void*, void**);
static cont_t o_continue;
static void hk_continue(void* fe, void** cb) {
    eb_log("autorun: continue without the save warnings");
    ((void (*)(void*))(*(void***)cb)[0x18 / 8])(cb);
}

static BOOL CALLBACK min_cb(HWND h, LPARAM l) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && !GetWindow(h, GW_OWNER) && !IsIconic(h) &&
        (l || GetForegroundWindow() != h)) {
        ShowWindowAsync(h, SW_SHOWMINNOACTIVE);
        static int once; if (!once++) eb_log("autorun: game window minimized");
    }
    return TRUE;
}
static DWORD WINAPI min_thread(LPVOID p) {
    ULONGLONG t0 = GetTickCount64();
    while (!g_closed) {                         // the first 60 s always, later only when it is not the foreground window
        EnumWindows(min_cb, GetTickCount64() - t0 < 60000);
        Sleep(200);
    }
    return 0;
}

void eb_autorun_frame(void) {
    if (!g_loaded || g_closed) return;
    uint8_t* gs = *g_gs;
    if (!gs) return;
    int date = *(int*)(gs + GS_DATE);
    ULONGLONG now = GetTickCount64();
    if (!g_started) {
        g_started = 1; g_start_date = g_last_date = date; g_last_change = g_t_start = now;
        char a[16]; fmt_date(date, a, sizeof a);
        eb_log("autorun: game loaded at %s - unpaused at speed 5", a);
        run(); g_kicks++;
        return;
    }
    if (date != g_last_date) { g_last_date = date; g_last_change = g_next_kick = now; }
    if (date >= g_cfg.autorun_until) { close_game("reached the end date"); return; }
    if (now - g_last_change > (ULONGLONG)g_cfg.autorun_stuck_s * 1000) { close_game("STUCK (no new day)"); return; }
    if (now - g_last_change > 2000 && now >= g_next_kick) {     // paused by a popup: unpause again every 2 s
        run(); g_kicks++; g_next_kick = now + 2000;
    }
}

int eb_autorun_install(void) {
    if (!g_cfg.autorun_until) return 0;
#ifdef EB_TEST
    return 0;
#else
    uint8_t* b = (uint8_t*)g_exe;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != 0x66FE6AB9u || memcmp(b + RVA_UNPAUSE, sig_unpause, 16) || memcmp(b + RVA_SPEED, sig_speed, 16)) {
        eb_log("autorun: signature mismatch - off"); return 0;
    }
    if (!g_cfg.report_capture || !g_cfg.report_slow) { eb_log("autorun: needs [report] capture=1 and slow=1 (the load and frame hooks) - off"); return 0; }
    g_gs = (uint8_t**)(b + RVA_GS);
    g_unpause = (unpause_t)(b + RVA_UNPAUSE);
    g_setspeed = (speed_t)(b + RVA_SPEED);
    MH_STATUS st = MH_Initialize();
    if (memcmp(b + RVA_CONTINUE, sig_continue, 16) || (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) ||
        MH_CreateHook(b + RVA_CONTINUE, (void*)hk_continue, (void**)&o_continue) != MH_OK || MH_EnableHook(b + RVA_CONTINUE) != MH_OK)
        eb_log("autorun: the continue hook failed - the save warning would wait for a click");
    if (g_cfg.autorun_minimize) { HANDLE h = CreateThread(NULL, 0, min_thread, NULL, 0, NULL); if (h) CloseHandle(h); }
    char a[16]; fmt_date(g_cfg.autorun_until, a, sizeof a);
    eb_log("autorun: TEST after the load: speed 5 until %s, then close the game (stuck after %d s)", a, g_cfg.autorun_stuck_s);
    return 1;
#endif
}
