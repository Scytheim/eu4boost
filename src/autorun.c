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
// ([test] autorun_minimize, default 1) and again if it comes back without being the foreground window,
// or kept drawn off the desktop with a simulated player ([test] autorun_offscreen=1, autorun_input=seed).
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
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


// ---- the off-screen window and the simulated player ([test] autorun_offscreen, autorun_input) ----
// To measure frames and input delay as in play while the PC is used for other work: the game window is
// shown (so it draws) but outside the desktop and never activated, and a simulated player posts input
// messages to it after the load: the pointer wanders and hovers over the map, the wheel zooms in and
// out, arrow keys pan, left clicks open provinces, F1..F8 open a view and close it again, now and then a
// pause. Never Esc, never the menu corner. Inside the game process only, the input and focus queries
// answer for the simulated player: GetCursorPos (its pointer), GetAsyncKeyState/GetKeyState (its keys
// and buttons), GetFocus/GetActiveWindow (the game window), WindowFromPoint (the game window over its
// own rect); ClipCursor, SetCursorPos and TrackMouseEvent do nothing, so the user's pointer is never
// confined, moved or tracked. CreateWindowExW/SetWindowPos/ShowWindow keep the game's top-level windows
// off the desktop and unactivated from the start.
#define OFF_X 20000
static HWND g_win;
static volatile LONG g_ox, g_oy, g_cw = 1280, g_ch = 720;  // client origin (screen) and size
static volatile LONG g_px = 640, g_py = 360;               // the simulated pointer (client)
static volatile uint8_t g_keys[256];
static int g_off;
typedef HWND (WINAPI *cwe_t)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
typedef BOOL (WINAPI *swp_t)(HWND, HWND, int, int, int, int, UINT);
typedef BOOL (WINAPI *sw_t)(HWND, int);
typedef BOOL (WINAPI *gcp_t)(LPPOINT);
typedef BOOL (WINAPI *scp_t)(int, int);
typedef BOOL (WINAPI *cc_t)(const RECT*);
typedef SHORT (WINAPI *gks_t)(int);
typedef HWND (WINAPI *gw_t)(void);
typedef HWND (WINAPI *wfp_t)(POINT);
typedef BOOL (WINAPI *tme_t)(LPTRACKMOUSEEVENT);
static cwe_t o_cwe; static swp_t o_swp; static sw_t o_sw; static gcp_t o_gcp; static scp_t o_scp; static cc_t o_cc;
static gks_t o_gaks, o_gks; static gw_t o_gf, o_gaw; static wfp_t o_wfp; static tme_t o_tme;
static int ours(HWND h) {                                  // a top-level window of this process
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    return h && pid == GetCurrentProcessId() && !GetParent(h) && !GetWindow(h, GW_OWNER);
}
static HWND WINAPI hk_cwe(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD st, int x, int y, int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID p) {
    if (!parent && !(st & WS_CHILD)) { x = OFF_X; y = 0; ex &= ~(DWORD)WS_EX_TOPMOST; }
    return o_cwe(ex, cls, name, st, x, y, w, h, parent, menu, inst, p);
}
static BOOL WINAPI hk_swp(HWND h, HWND after, int x, int y, int cx, int cy, UINT f) {
    if (ours(h)) {
        if (!(f & SWP_NOMOVE) && x < OFF_X - 1000) x = OFF_X;
        if (after == HWND_TOPMOST || after == HWND_TOP) f |= SWP_NOZORDER;
        f |= SWP_NOACTIVATE;
    }
    return o_swp(h, after, x, y, cx, cy, f);
}
static BOOL WINAPI hk_sw(HWND h, int cmd) {
    if (ours(h) && (cmd == SW_SHOW || cmd == SW_SHOWNORMAL || cmd == SW_RESTORE || cmd == SW_SHOWDEFAULT || cmd == SW_SHOWMAXIMIZED))
        cmd = SW_SHOWNOACTIVATE;
    return o_sw(h, cmd);
}
static BOOL WINAPI hk_gcp(LPPOINT p) { if (!p) return FALSE; p->x = g_ox + g_px; p->y = g_oy + g_py; return TRUE; }
static BOOL WINAPI hk_scp(int x, int y) { g_px = x - g_ox; g_py = y - g_oy; return TRUE; }
static BOOL WINAPI hk_cc(const RECT* r) { (void)r; return TRUE; }
static SHORT WINAPI hk_gaks(int vk) { return (vk >= 0 && vk < 256 && g_keys[vk]) ? (SHORT)0x8000 : 0; }
static SHORT WINAPI hk_gks(int vk) { return (vk >= 0 && vk < 256 && g_keys[vk]) ? (SHORT)0x8000 : 0; }
static HWND WINAPI hk_gf(void) { return g_win ? g_win : o_gf(); }
static HWND WINAPI hk_gaw(void) { return g_win ? g_win : o_gaw(); }
static HWND WINAPI hk_wfp(POINT pt) {
    if (g_win && pt.x >= g_ox && pt.x < g_ox + g_cw && pt.y >= g_oy && pt.y < g_oy + g_ch) return g_win;
    return o_wfp(pt);
}
static BOOL WINAPI hk_tme(LPTRACKMOUSEEVENT t) { (void)t; return TRUE; }
// all twelve or none: a pointer answered for the simulated player while the window is on the desktop, or a
// window kept off the desktop whose cursor queries are real, would be worse than the minimized fallback
static eb_hookset g_ohs;
static int hook_api(const char* name, void* fn, void* orig) {
    void* t = (void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), name);
    if (!t) { eb_log("autorun: %s not found", name); if (!g_ohs.failed) eb_hs_undo(&g_ohs, "a user32 function is missing"); return 0; }
    return eb_hs_add(&g_ohs, t, fn, (void**)orig);
}
static int offscreen_install(void) {
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return 0;
    eb_hs_init(&g_ohs, "autorun off-screen");
    return hook_api("CreateWindowExW", (void*)hk_cwe, &o_cwe) && hook_api("SetWindowPos", (void*)hk_swp, &o_swp) &&
           hook_api("ShowWindow", (void*)hk_sw, &o_sw) && hook_api("GetCursorPos", (void*)hk_gcp, &o_gcp) &&
           hook_api("SetCursorPos", (void*)hk_scp, &o_scp) && hook_api("ClipCursor", (void*)hk_cc, &o_cc) &&
           hook_api("GetAsyncKeyState", (void*)hk_gaks, &o_gaks) && hook_api("GetKeyState", (void*)hk_gks, &o_gks) &&
           hook_api("GetFocus", (void*)hk_gf, &o_gf) && hook_api("GetActiveWindow", (void*)hk_gaw, &o_gaw) &&
           hook_api("WindowFromPoint", (void*)hk_wfp, &o_wfp) && hook_api("TrackMouseEvent", (void*)hk_tme, &o_tme) &&
           eb_hs_enable(&g_ohs);
}
static BOOL CALLBACK find_cb(HWND h, LPARAM l) {
    RECT r;
    if (ours(h) && IsWindowVisible(h) && GetWindowRect(h, &r) && r.right - r.left >= 640) { *(HWND*)l = h; return FALSE; }
    return TRUE;
}
static DWORD WINAPI off_thread(LPVOID p) {                 // keeps the game window drawn but off the desktop
    (void)p;
    int told = 0, activated = 0;
    while (!g_closed) {
        HWND h = NULL; EnumWindows(find_cb, (LPARAM)&h);
        if (h) {
            g_win = h;
            if (IsIconic(h)) {                              // started minimized: restore straight to the off-screen place
                WINDOWPLACEMENT wp; wp.length = sizeof wp;
                if (GetWindowPlacement(h, &wp)) {
                    LONG w = wp.rcNormalPosition.right - wp.rcNormalPosition.left, hh = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
                    wp.rcNormalPosition.left = OFF_X; wp.rcNormalPosition.top = 0;
                    wp.rcNormalPosition.right = OFF_X + w; wp.rcNormalPosition.bottom = hh;
                    wp.showCmd = SW_SHOWNOACTIVATE; wp.flags = 0;
                    SetWindowPlacement(h, &wp);
                }
            }
            RECT r; GetWindowRect(h, &r);
            if (r.left < OFF_X - 1000) o_swp(h, NULL, OFF_X, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            RECT c; POINT o = { 0, 0 };
            if (GetClientRect(h, &c) && ClientToScreen(h, &o)) { g_ox = o.x; g_oy = o.y; g_cw = c.right; g_ch = c.bottom; }
            if (!told++) eb_log("autorun: game window kept off the desktop at x %d (client %ldx%ld)", OFF_X, (long)g_cw, (long)g_ch);
            if (g_loaded && !activated) { activated = 1; PostMessageW(h, WM_ACTIVATE, WA_ACTIVE, 0); PostMessageW(h, WM_SETFOCUS, 0, 0); }
        }
        Sleep(200);
    }
    return 0;
}

// the simulated player: its input messages and when each key, button or wheel message was posted (playmon's input delay)
typedef struct { UINT msg; LONGLONG t; } posted_t;
static posted_t g_q[512];
static volatile LONG g_qh, g_qt;                           // the main thread takes at g_qh, the player puts at g_qt
static SRWLOCK g_ql = SRWLOCK_INIT;
static LONGLONG qnow(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static void post_in(UINT msg, WPARAM w, LPARAM l) {
    AcquireSRWLockExclusive(&g_ql);
    if (g_qt - g_qh < 512) { g_q[g_qt & 511].msg = msg; g_q[g_qt & 511].t = qnow(); g_qt++; }
    ReleaseSRWLockExclusive(&g_ql);
    PostMessageW(g_win, msg, w, l);
}
LONGLONG eb_autorun_input_made(UINT msg) {
    if (!g_off) return 0;
    LONGLONG t = 0;
    AcquireSRWLockExclusive(&g_ql);
    while (g_qh < g_qt) { posted_t q = g_q[g_qh & 511]; g_qh++; if (q.msg == msg) { t = q.t; break; } }
    ReleaseSRWLockExclusive(&g_ql);
    return t;
}
static uint64_t g_rng;
static uint32_t rnd(uint32_t n) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return n ? (uint32_t)(g_rng % n) : 0; }
static WPARAM mk(void) { return (g_keys[VK_LBUTTON] ? MK_LBUTTON : 0) | (g_keys[VK_RBUTTON] ? MK_RBUTTON : 0); }
static void move_to(int x, int y, int steps, int ms) {
    int x0 = g_px, y0 = g_py;
    for (int i = 1; i <= steps && !g_closed; i++) {
        g_px = x0 + (x - x0) * i / steps; g_py = y0 + (y - y0) * i / steps;
        PostMessageW(g_win, WM_MOUSEMOVE, mk(), MAKELPARAM(g_px, g_py));
        Sleep((DWORD)(ms / steps));
    }
}
static void key(int vk, int down) {
    UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    int ext = vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN;
    LPARAM l = 1 | ((LPARAM)sc << 16) | (ext ? (1 << 24) : 0) | (down ? 0 : ((LPARAM)3 << 30));
    g_keys[vk] = (uint8_t)down;
    if (down) post_in(WM_KEYDOWN, (WPARAM)vk, l); else PostMessageW(g_win, WM_KEYUP, (WPARAM)vk, l);
}
static void click(int right) {
    int vk = right ? VK_RBUTTON : VK_LBUTTON;
    g_keys[vk] = 1; post_in(right ? WM_RBUTTONDOWN : WM_LBUTTONDOWN, mk(), MAKELPARAM(g_px, g_py));
    Sleep(60 + rnd(80));
    g_keys[vk] = 0; PostMessageW(g_win, right ? WM_RBUTTONUP : WM_LBUTTONUP, mk(), MAKELPARAM(g_px, g_py));
}
static void wheel(int notches) {
    for (int i = 0; i < (notches < 0 ? -notches : notches) && !g_closed; i++) {
        post_in(WM_MOUSEWHEEL, MAKEWPARAM(mk(), notches < 0 ? -WHEEL_DELTA : WHEEL_DELTA), MAKELPARAM(g_ox + g_px, g_oy + g_py));
        Sleep(40 + rnd(60));
    }
}
static DWORD WINAPI player(LPVOID p) {
    (void)p;
    while (!g_closed && (!g_started || !g_win)) Sleep(200);
    Sleep(3000);
    eb_log("autorun: simulated player starts (seed %d)", g_cfg.autorun_input);
    long n[8] = { 0 }; int open_f = 0; ULONGLONG close_at = 0, next_log = GetTickCount64() + 600000;
    while (!g_closed) {
        int W = g_cw, H = g_ch;
        int mx = W / 4 + (int)rnd((uint32_t)(W / 2)), my = H * 15 / 100 + (int)rnd((uint32_t)(H * 60 / 100));
        uint32_t a = rnd(100);
        if (open_f && GetTickCount64() >= close_at) { key(VK_F1 + open_f - 1, 1); Sleep(50); key(VK_F1 + open_f - 1, 0); open_f = 0; n[5]++; }
        else if (a < 28) { move_to(mx, my, 8 + (int)rnd(12), 250 + (int)rnd(600)); n[0]++; }             // hover over the map
        else if (a < 48) { move_to(mx, my, 4, 120); wheel((rnd(2) ? 1 : -1) * (3 + (int)rnd(6))); n[1]++; }   // zoom in or out there
        else if (a < 66) { int vk = VK_LEFT + (int)rnd(4); key(vk, 1); Sleep(300 + rnd(1200)); key(vk, 0); n[2]++; }   // pan
        else if (a < 80) { move_to(mx, my, 6, 200); click(0); n[3]++; }                                 // open a province
        else if (a < 85) { move_to(mx, my, 6, 200); click(1); n[4]++; }                                 // right click
        else if (a < 93 && !open_f) { open_f = 1 + (int)rnd(8); key(VK_F1 + open_f - 1, 1); Sleep(50); key(VK_F1 + open_f - 1, 0); close_at = GetTickCount64() + 2000 + rnd(5000); n[5]++; }   // a view
        else if (a < 96) { key(VK_SPACE, 1); Sleep(50); key(VK_SPACE, 0); Sleep(1000 + rnd(2000)); key(VK_SPACE, 1); Sleep(50); key(VK_SPACE, 0); n[6]++; }   // pause a moment
        else { Sleep(500 + rnd(1500)); n[7]++; }                                                       // just look
        Sleep(150 + rnd(700));
        if (GetTickCount64() >= next_log) {
            next_log += 600000;
            eb_log("autorun: player so far: hover %ld, zoom %ld, pan %ld, left %ld, right %ld, views %ld, pauses %ld, idle %ld", n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7]);
        }
    }
    eb_log("autorun: player: hover %ld, zoom %ld, pan %ld, left %ld, right %ld, views %ld, pauses %ld, idle %ld", n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7]);
    return 0;
}

// ---- a scripted player ([test] autorun_script, with autorun_offscreen) ----
// For lag that needs a certain window open (2026-11-11: the building windows with 짭짭죽's many buildings): the game
// stays paused after the load and a script drives it through the same posted input as the simulated player.
// Commands, separated by ';':  wait MS | key K (A-Z, 0-9, F1-F12, SPACE, ESC, ENTER, TAB, or 0xNN) | move X Y |
// click X Y | rclick X Y | wheel N | shot NAME [DIV [X Y W H]] (the window, or a part of it, to
// plugins\eu4boost_shot_NAME.bmp, scaled down by DIV) | fps NAME MS (frame times over MS ms, logged) |
// scan (buildview.c: every building x every province, timed) | fr0 (start counting long frames) | fr NAME (the longest frame and the long ones since fr0, logged) |
// prof (Ctrl+Alt+P for the profiler thread only) | log TEXT | run (unpause at speed 5, as without a script) | end.
// X and Y are client pixels of the game window.
static volatile LONG g_script_run;                         // the script said "run": the usual autopilot from here
static volatile LONG g_script_scan;                        // "scan": the main thread runs eb_buildview_scan (game functions run there)
static volatile LONG g_fr_n, g_fr_hist[258];               // frame intervals in ms since the last reset (257 = longer)
static volatile LONGLONG g_fr_prev;
static double g_fr_ms;
static volatile LONG g_fr_max_us, g_fr_over[3];            // since "fr0": the longest frame, frames over 250 ms / 1 s / 3 s
static void fr_note(void) {                                // main thread, every frame
    LONGLONG t = qnow();
    if (g_fr_prev) {
        double ms = (double)(t - g_fr_prev) * g_fr_ms;
        int b = ms >= 257.0 ? 257 : (int)ms;
        g_fr_hist[b]++; g_fr_n++;
        if (ms * 1000.0 > (double)g_fr_max_us) g_fr_max_us = (LONG)(ms * 1000.0);
        if (ms >= 250.0) g_fr_over[0]++;
        if (ms >= 1000.0) g_fr_over[1]++;
        if (ms >= 3000.0) g_fr_over[2]++;
    }
    g_fr_prev = t;
}
static void shot(const char* name, int div, int x, int y, int w, int h) {
    int W = g_cw, H = g_ch;
    if (div < 1) div = 1;
    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > W || y + h > H) { x = 0; y = 0; w = W; h = H; }
    HDC wdc = GetDC(g_win);
    HDC mdc = CreateCompatibleDC(wdc);
    HBITMAP bm = CreateCompatibleBitmap(wdc, W, H);
    HGDIOBJ old = SelectObject(mdc, bm);
    BOOL ok = PrintWindow(g_win, mdc, 1 | 2);              // PW_CLIENTONLY | PW_RENDERFULLCONTENT (the composed picture)
    int ow = w / div, oh = h / div;
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = ow; bi.bmiHeader.biHeight = oh;   // bottom-up, as a .bmp is
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 24; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HDC sdc = CreateCompatibleDC(wdc);
    HBITMAP sb = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ old2 = SelectObject(sdc, sb);
    SetStretchBltMode(sdc, HALFTONE);
    StretchBlt(sdc, 0, 0, ow, oh, mdc, x, y, w, h, SRCCOPY);
    GdiFlush();
    DWORD stride = ((DWORD)ow * 3 + 3) & ~3u, size = stride * (DWORD)oh;
    wchar_t path[MAX_PATH]; _snwprintf(path, MAX_PATH, L"%lseu4boost_shot_%hs.bmp", g_dir, name);
    HANDLE f = bits ? CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    if (f != INVALID_HANDLE_VALUE) {
        BITMAPFILEHEADER fh; memset(&fh, 0, sizeof fh);
        fh.bfType = 0x4d42; fh.bfOffBits = sizeof fh + sizeof(BITMAPINFOHEADER); fh.bfSize = fh.bfOffBits + size;
        bi.bmiHeader.biSizeImage = size;
        DWORD wr; WriteFile(f, &fh, sizeof fh, &wr, NULL); WriteFile(f, &bi.bmiHeader, sizeof(BITMAPINFOHEADER), &wr, NULL); WriteFile(f, bits, size, &wr, NULL);
        CloseHandle(f);
    }
    eb_log("autorun: script shot %s: %dx%d of the window at %d,%d -> %dx%d (%s)", name, w, h, x, y, ow, oh, ok && f != INVALID_HANDLE_VALUE ? "written" : "FAILED");
    SelectObject(sdc, old2); DeleteObject(sb); DeleteDC(sdc);
    SelectObject(mdc, old); DeleteObject(bm); DeleteDC(mdc); ReleaseDC(g_win, wdc);
}
static int vk_of(const char* s) {
    if (!_stricmp(s, "SPACE")) return VK_SPACE;
    if (!_stricmp(s, "ESC")) return VK_ESCAPE;
    if (!_stricmp(s, "ENTER")) return VK_RETURN;
    if (!_stricmp(s, "TAB")) return VK_TAB;
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '1' && s[1] <= '9') { int n = atoi(s + 1); if (n >= 1 && n <= 12) return VK_F1 + n - 1; }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return (int)strtol(s, NULL, 16);
    if (s[0] && !s[1]) { char c = s[0]; if (c >= 'a' && c <= 'z') c = (char)(c - 32); return (unsigned char)c; }
    return 0;
}
static DWORD WINAPI scripted(LPVOID p) {
    (void)p;
    while (!g_closed && (!g_started || !g_win)) Sleep(200);
    Sleep(3000);
    eb_log("autorun: script starts: %s", g_cfg.autorun_script);
    char buf[sizeof g_cfg.autorun_script]; strncpy(buf, g_cfg.autorun_script, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    char* save = NULL;
    for (char* cmd = strtok_s(buf, ";", &save); cmd && !g_closed; cmd = strtok_s(NULL, ";", &save)) {
        while (*cmd == ' ') cmd++;
        char op[16] = { 0 }, a1[64] = { 0 }; int v[6] = { 0 };
        int n = sscanf(cmd, "%15s %63s %d %d %d %d %d", op, a1, &v[1], &v[2], &v[3], &v[4], &v[5]);
        if (n < 1) continue;
        int x = atoi(a1);
        if (!strcmp(op, "wait")) Sleep((DWORD)x);
        else if (!strcmp(op, "key")) { int vk = vk_of(a1); if (vk) { key(vk, 1); Sleep(60); key(vk, 0); } else eb_log("autorun: script: unknown key %s", a1); }
        else if (!strcmp(op, "move")) move_to(x, v[1], 6, 150);
        else if (!strcmp(op, "click") || !strcmp(op, "rclick")) { move_to(x, v[1], 6, 150); Sleep(120); click(op[0] == 'r'); }
        else if (!strcmp(op, "wheel")) wheel(x);
        else if (!strcmp(op, "shot")) shot(a1, n >= 3 ? v[1] : 3, v[2], v[3], v[4], v[5]);
        else if (!strcmp(op, "prof")) {                    // the profiler thread reads its hotkey through the hooked GetAsyncKeyState
            g_keys[VK_CONTROL] = g_keys[VK_MENU] = g_keys['P'] = 1; Sleep(300); g_keys[VK_CONTROL] = g_keys[VK_MENU] = g_keys['P'] = 0;
            eb_log("autorun: script: profiler hotkey");
        }
        else if (!strcmp(op, "log")) eb_log("autorun: script: %s", cmd + 4);
        else if (!strcmp(op, "fps")) {
            for (int i = 0; i < 258; i++) g_fr_hist[i] = 0;
            g_fr_n = 0;
            int ms = v[1] > 0 ? v[1] : 5000;
            Sleep((DWORD)ms);
            LONG total = 0, h[258]; for (int i = 0; i < 258; i++) { h[i] = g_fr_hist[i]; total += h[i]; }
            int p50 = -1, p90 = -1, p99 = -1, mx = 0; LONG acc = 0;
            for (int i = 0; i < 258; i++) {
                acc += h[i];
                if (h[i]) mx = i;
                if (p50 < 0 && acc * 2 >= total) p50 = i;
                if (p90 < 0 && acc * 10 >= total * 9) p90 = i;
                if (p99 < 0 && acc * 100 >= total * 99) p99 = i;
            }
            eb_log("autorun: script fps %s: %ld frames in %d ms = %.1f fps (%.1f ms a frame; median %d ms, 90%% %d ms, 99%% %d ms, longest %d%s ms)",
                   a1, (long)total, ms, total * 1000.0 / ms, total ? (double)ms / total : 0.0, p50, p90, p99, mx, mx >= 257 ? "+" : "");
        }
        else if (!strcmp(op, "fr0")) { g_fr_max_us = 0; g_fr_over[0] = g_fr_over[1] = g_fr_over[2] = 0; g_fr_n = 0; }
        else if (!strcmp(op, "fr"))
            eb_log("autorun: script frames %s: %ld frames since fr0, the longest %.0f ms; over 250 ms %ld, over 1 s %ld, over 3 s %ld",
                   a1, (long)g_fr_n, g_fr_max_us / 1000.0, (long)g_fr_over[0], (long)g_fr_over[1], (long)g_fr_over[2]);
        else if (!strcmp(op, "scan")) { g_script_scan = 1; while (g_script_scan && !g_closed) Sleep(50); }
        else if (!strcmp(op, "run")) g_script_run = 1;
        else if (!strcmp(op, "end")) { g_script_run = 2; break; }
        else eb_log("autorun: script: unknown command %s", op);
    }
    if (g_script_run != 1) g_script_run = 2;               // the main thread closes the game (close_game is its job)
    return 0;
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
    if (g_cfg.autorun_script[0]) fr_note();
    if (!g_loaded || g_closed) return;
    uint8_t* gs = *g_gs;
    if (!gs) return;
    int date = *(int*)(gs + GS_DATE);
    ULONGLONG now = GetTickCount64();
    if (!g_started) {
        g_started = 1; g_start_date = g_last_date = date; g_last_change = g_t_start = now;
        char a[16]; fmt_date(date, a, sizeof a);
        if (g_cfg.autorun_script[0] && g_off) { eb_log("autorun: game loaded at %s - left paused for the script", a); return; }
        eb_log("autorun: game loaded at %s - unpaused at speed 5", a);
        run(); g_kicks++;
        return;
    }
    if (g_cfg.autorun_script[0] && g_off) {
        if (g_script_scan == 1) { eb_buildview_scan(); g_script_scan = 0; }
        if (g_script_run == 2) { close_game("the script ended"); return; }
        if (!g_script_run) { g_last_change = now; return; }     // paused on purpose: no "stuck", no unpausing
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
    if (g_cfg.autorun_offscreen) {
        HANDLE h = NULL;
        if (!offscreen_install()) eb_log("autorun: off-screen hooks failed - minimized instead");
        else if (!(h = CreateThread(NULL, 0, off_thread, NULL, 0, NULL))) {
            eb_hs_undo(&g_ohs, "the off-screen thread could not start");
            eb_log("autorun: off-screen hooks failed - minimized instead");
        } else {
            g_off = 1;
            CloseHandle(h);
            if (g_cfg.autorun_input) {
                g_rng = 0x9E3779B97F4A7C15ull ^ (uint64_t)(unsigned)g_cfg.autorun_input;
                h = CreateThread(NULL, 0, player, NULL, 0, NULL); if (h) CloseHandle(h);
            }
            if (g_cfg.autorun_script[0]) {
                LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_fr_ms = 1000.0 / (double)f.QuadPart;
                g_rng = 0x9E3779B97F4A7C15ull;
                h = CreateThread(NULL, 0, scripted, NULL, 0, NULL); if (h) CloseHandle(h);
            }
            eb_log("autorun: TEST off-screen window%s", g_cfg.autorun_input ? " with a simulated player" : "");
        }
    }
    if (!g_off && g_cfg.autorun_minimize) { HANDLE h = CreateThread(NULL, 0, min_thread, NULL, 0, NULL); if (h) CloseHandle(h); }
    char a[16]; fmt_date(g_cfg.autorun_until, a, sizeof a);
    eb_log("autorun: TEST after the load: speed 5 until %s, then close the game (stuck after %d s)", a, g_cfg.autorun_stuck_s);
    return 1;
#endif
}
