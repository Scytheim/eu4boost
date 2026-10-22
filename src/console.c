// Console commands in a single-player ironman game: Ctrl+Alt+C unlocks them until the next press or
// the next game start (loading a save locks them again).
// [console] ironman_hotkey is read once at launch (the user's on/off .bat files edit it before the
// game starts); with 0 nothing is installed.
//
// The console window opens in ironman too; what refuses the commands is one byte of the console
// manager (eu4.exe+0x2357b58, created on first use by FUN_1406e74c0): +0x19 "restricted". Every game
// start (FUN_1406e90f0) sets it to multiplayer || ironman (gamestate+0x24d0), and FUN_141721320 then
// runs only "stats" and "enable_all_commands" ("Command not available in multiplayer or ironman
// mode."). enable_all_commands clears the same byte when its argument hashes to a value built into the
// exe (a developer password), so the byte guards nothing else. +0x18 (developer-only commands) stays.
// Running a command other than "stats" sets +0x44, which only goes into the multiplayer sync checksum
// (FUN_140743aa0) and is cleared when a game is loaded (FUN_14074a260).
// Multiplayer is never unlocked: the commands run locally and would desync the other players.
#include "common.h"
#include <string.h>
#include "MinHook.h"

#define RVA_GAMESTART   0x6e90f0u    // sets the console restrictions of the game being started
#define RVA_CONSOLE_GET 0x6e74c0u    // returns the console manager (only checked: it reads RVA_CONSOLE)
#define RVA_IS_MP       0x6eaa10u    // FUN_1406eaa10(session): other human players in the session
#define RVA_CONSOLE     0x2357b58u   // console manager*
#define RVA_GAMESTATE   0x233fe78u
#define CON_RESTRICTED  0x19
#define GS_SESSION      0x1e00
#define GS_IRONMAN      0x24d0

// up to "mov byte ptr [rax+0x19], cl": also pins the offsets above
static const uint8_t sig_gamestart[119] = {
    0x48,0x83,0xec,0x28,0x48,0x89,0x74,0x24,0x40,0x48,0x8b,0xf1,0x48,0x8b,0x0d,0x75,0x6d,0xc5,0x01,0x48,
    0x85,0xc9,0x0f,0x84,0x9b,0x00,0x00,0x00,0x48,0x89,0x5c,0x24,0x30,0x48,0x8b,0xd6,0x48,0x89,0x6c,0x24,
    0x38,0x48,0x89,0x7c,0x24,0x20,0xe8,0x0d,0xd9,0x06,0x00,0xe8,0x98,0xe3,0xff,0xff,0x48,0x8b,0x0d,0x49,
    0x6d,0xc5,0x01,0xc6,0x40,0x18,0x01,0x48,0x8b,0x89,0x00,0x1e,0x00,0x00,0xe8,0xd1,0x18,0x00,0x00,0x48,
    0x8b,0x0d,0x32,0x6d,0xc5,0x01,0x0f,0xb6,0xd8,0x0f,0xb6,0xb9,0xd0,0x24,0x00,0x00,0xe8,0x6b,0xe3,0xff,
    0xff,0x84,0xdb,0x75,0x09,0x40,0x84,0xff,0x75,0x04,0x33,0xc9,0xeb,0x02,0xb1,0x01,0x88,0x48,0x19 };
static const uint8_t sig_console_get[16] = { 0x48,0x83,0xec,0x28,0x48,0x8b,0x05,0x8d,0x06,0xc7,0x01,0x48,0x85,0xc0,0x75,0x3d };
static const uint8_t sig_is_mp[16] = { 0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0x01,0x48,0x8b,0xf9 };

typedef void (*gamestart_t)(void*);
typedef char (*is_mp_t)(void*);
static gamestart_t o_gamestart;
static is_mp_t game_is_mp;
static volatile LONG g_sp_ironman;   // the game started last is a single-player ironman game
static volatile LONG g_unlocked;

static uint8_t* game_ptr(uint32_t rva) { return *(uint8_t* volatile*)((uint8_t*)g_exe + rva); }

// Main thread, when a game starts: the original sets the restrictions, then the same multiplayer test
// it just made decides whether the hotkey may lift them in this game.
static void hk_gamestart(void* idler) {
    o_gamestart(idler);
    uint8_t* gs = game_ptr(RVA_GAMESTATE);
    LONG sp_ironman = gs && gs[GS_IRONMAN] && !game_is_mp(*(void**)(gs + GS_SESSION));
    InterlockedExchange(&g_sp_ironman, sp_ironman);
    if (InterlockedExchange(&g_unlocked, 0)) eb_log("console: locked again (a game was started)");
}

static void toggle(void) {
    uint8_t* con = game_ptr(RVA_CONSOLE);
    uint8_t* gs = game_ptr(RVA_GAMESTATE);
    if (!con || !gs || !g_sp_ironman || !gs[GS_IRONMAN]) {
        eb_log("console: Ctrl+Alt+C ignored (not in a single-player ironman game)");
        Beep(400, 80); Beep(400, 80);
        return;
    }
    volatile uint8_t* restricted = con + CON_RESTRICTED;
    if (*restricted) {
        *restricted = 0; InterlockedExchange(&g_unlocked, 1);
        eb_log("console: commands unlocked (Ctrl+Alt+C again or loading a game locks them)");
        Beep(1200, 120);
    } else if (InterlockedExchange(&g_unlocked, 0)) {
        *restricted = 1;
        eb_log("console: commands locked");
        Beep(600, 200);
    } else {
        eb_log("console: Ctrl+Alt+C ignored (commands were unlocked by something else)");
    }
}

static int game_has_focus(void) {
    DWORD pid = 0;
    HWND w = GetForegroundWindow();
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

// Three key states read every 50 ms (a few microseconds); a global RegisterHotKey would take
// Ctrl+Alt+C away from every other program while the game runs.
static DWORD WINAPI hotkey_main(LPVOID arg) {
    (void)arg;
    int was_down = 0;
    for (;;) {
        Sleep(50);
        int down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000) &&
                   (GetAsyncKeyState('C') & 0x8000);
        if (down && !was_down && game_has_focus()) toggle();
        was_down = down;
    }
    return 0;
}

int eb_console_install(void) {
    if (g_cfg.console_hotkey != 1) return 1;
    uint8_t* b = (uint8_t*)g_exe;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(b + ((IMAGE_DOS_HEADER*)g_exe)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != 0x66FE6AB9u || memcmp(b + RVA_GAMESTART, sig_gamestart, sizeof sig_gamestart) ||
        memcmp(b + RVA_CONSOLE_GET, sig_console_get, sizeof sig_console_get) || memcmp(b + RVA_IS_MP, sig_is_mp, sizeof sig_is_mp)) {
        eb_log("console: signature mismatch - ironman hotkey not installed");
        return 0;
    }
    game_is_mp = (is_mp_t)(void*)(b + RVA_IS_MP);
    MH_STATUS s = MH_Initialize();
    if ((s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) ||
        MH_CreateHook(b + RVA_GAMESTART, (void*)hk_gamestart, (void**)&o_gamestart) != MH_OK || MH_EnableHook(b + RVA_GAMESTART) != MH_OK) {
        eb_log("console: hook failed - ironman hotkey not installed");
        return 0;
    }
    HANDLE t = CreateThread(NULL, 64 * 1024, hotkey_main, NULL, 0, NULL);
    if (!t) { eb_log("console: thread failed - ironman hotkey not installed"); return 0; }
    CloseHandle(t);
    eb_log("console: Ctrl+Alt+C unlocks console commands in single-player ironman");
    return 1;
}
