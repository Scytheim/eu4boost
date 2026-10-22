// Thread scheduling fixes.
//
// EU4's application loop (CApplication::Run, eu4.exe+0x14c2d00) hard-pins the main thread -
// the one that runs ~97% of the simulation serially - to a single logical CPU: the
// (N/2+1)-th CPU of the process mask (logical CPU 6 on a 12-thread CPU). That stops
// Windows from moving it to the fastest ("preferred") core, and when a worker or another
// program lands on the same core or its SMT sibling, the main thread cannot escape.
//
// We patch eu4.exe's import of SetThreadAffinityMask so the request can be ignored
// (main thread left free) or redirected to a core we choose.
#include "common.h"
#include <string.h>
#include "MinHook.h"

typedef DWORD_PTR (WINAPI *SetThreadAffinityMask_t)(HANDLE, DWORD_PTR);
static SetThreadAffinityMask_t o_SetThreadAffinityMask;

static int popcount64(DWORD_PTR m) { int n = 0; while (m) { m &= m - 1; n++; } return n; }

static DWORD_PTR WINAPI hk_SetThreadAffinityMask(HANDLE h, DWORD_PTR mask) {
    DWORD tid = GetThreadId(h);
    int self = (h == GetCurrentThread()) || tid == GetCurrentThreadId();
    int cpu = -1;
    for (int i = 0; i < 64; i++) if (mask == ((DWORD_PTR)1 << i)) { cpu = i; break; }
    if (self && g_cfg.main_affinity == 0) {
        DWORD_PTR proc = 0, sys = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
        eb_log("threads: game pins thread %lu to logical CPU %d (mask %llx) -> ignored, thread stays free on %d CPUs",
               (unsigned long)GetCurrentThreadId(), cpu, (unsigned long long)mask, popcount64(proc));
        return proc;   // "previous" mask; the game ignores the value
    }
    if (self && g_cfg.main_affinity > 0) {
        int want = g_cfg.main_affinity - 1;     // stored as cpu index + 1
        DWORD_PTR m = (DWORD_PTR)1 << want;
        eb_log("threads: game pins thread %lu to CPU %d -> using CPU %d instead", (unsigned long)GetCurrentThreadId(), cpu, want);
        return o_SetThreadAffinityMask(h, m);
    }
    eb_log("threads: SetThreadAffinityMask(thread %lu, %llx) passed through", (unsigned long)tid, (unsigned long long)mask);
    return o_SetThreadAffinityMask(h, mask);
}

// Replace one imported function in the IAT of `mod`. Returns the old pointer or NULL.
void* patch_iat(HMODULE mod, const char* dll, const char* func, void* repl) {
    uint8_t* base = (uint8_t*)mod;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY* d = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!d->VirtualAddress) return NULL;
    for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + d->VirtualAddress); imp->Name; imp++) {
        if (_stricmp((const char*)(base + imp->Name), dll) != 0) continue;
        IMAGE_THUNK_DATA64* names = (IMAGE_THUNK_DATA64*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        IMAGE_THUNK_DATA64* slots = (IMAGE_THUNK_DATA64*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, func) != 0) continue;
            DWORD old;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return NULL;
            void* prev = (void*)(uintptr_t)slots->u1.Function;
            slots->u1.Function = (ULONGLONG)(uintptr_t)repl;
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
            return prev;
        }
    }
    return NULL;
}

// ---- background throttle -----------------------------------------------------------------
// The frame function (eu4.exe+0x14c25a0) calls SDL_Delay(100) on every frame while the game
// window is not focused, so a game left running at speed 5 behind another window advances
// at most ~10 days per second. With background_full_speed=1 that sleep is skipped.
#define RVA_SDL_DELAY   0x1733fa0u   // SDL_Delay dynapi thunk: rex.w jmp qword ptr [rip+0x87ce51]
#define RVA_FRAME_FUNC  0x14c25a0u
#define FRAME_FUNC_SIZE 0x5a0u
static const uint8_t sig_sdl_delay[7] = { 0x48, 0xff, 0x25, 0x51, 0xce, 0x87, 0x00 };
typedef void (__cdecl *SDL_Delay_t)(uint32_t);
static SDL_Delay_t o_SDL_Delay;
static volatile LONG g_bg_skips;

static void __cdecl hk_SDL_Delay(uint32_t ms) {
    if (ms == 100) {
        uintptr_t ra = (uintptr_t)__builtin_return_address(0);
        uintptr_t f = (uintptr_t)g_exe + RVA_FRAME_FUNC;
        if (ra > f && ra < f + FRAME_FUNC_SIZE) {
            if (InterlockedIncrement(&g_bg_skips) == 1) eb_log("threads: background frame throttle skipped (logged once)");
            ms = 0;
        }
    }
    o_SDL_Delay(ms);
}

static void install_bg_unthrottle(void) {
    uint8_t* t = (uint8_t*)g_exe + RVA_SDL_DELAY;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)((uint8_t*)g_exe + ((IMAGE_DOS_HEADER*)g_exe)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != 0x66FE6AB9u || memcmp(t, sig_sdl_delay, sizeof sig_sdl_delay) != 0) {
        eb_log("threads: SDL_Delay signature mismatch - background throttle left as is");
        return;
    }
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) { eb_log("threads: MinHook init failed"); return; }
    if (MH_CreateHook(t, (void*)hk_SDL_Delay, (void**)&o_SDL_Delay) != MH_OK || MH_EnableHook(t) != MH_OK) {
        eb_log("threads: SDL_Delay hook failed");
        return;
    }
    eb_log("threads: background_full_speed on (no 100 ms sleep per frame when the window is not focused)");
}

// ---- mid-tick rendering -------------------------------------------------------------------
// The simulation runs on the main (render) thread. Between phases of a daily tick the game
// checks how long it has been since the last frame and, if more than a per-speed limit has
// passed, renders a whole frame in the middle of the tick (eu4.exe+0x769ea0). At speed 5 the
// limit is 50 ms, so every tick longer than 50 ms (late game, month starts) pays for extra
// frames. The limits are a float table in .rdata: speeds 1-4 = 0.02 s, speed 5 = 0.05 s.
#define RVA_MIDTICK_TABLE 0x1c33e18u
static const float midtick_expected[5] = { 0.02f, 0.02f, 0.02f, 0.02f, 0.05f };

static void patch_midtick_interval(void) {
    int ms = g_cfg.speed5_midtick_ms;
    if (ms <= 0 || ms == 50) { eb_log("render: speed-5 mid-tick frame interval left at 50 ms"); return; }
    float* t = (float*)((uint8_t*)g_exe + RVA_MIDTICK_TABLE);
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)((uint8_t*)g_exe + ((IMAGE_DOS_HEADER*)g_exe)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != 0x66FE6AB9u || memcmp(t, midtick_expected, sizeof midtick_expected) != 0) {
        eb_log("render: mid-tick interval table not found - left unchanged");
        return;
    }
    DWORD old;
    if (!VirtualProtect(&t[4], sizeof(float), PAGE_READWRITE, &old)) { eb_log("render: VirtualProtect failed"); return; }
    t[4] = (float)ms / 1000.0f;
    VirtualProtect(&t[4], sizeof(float), old, &old);
    eb_log("render: speed-5 mid-tick frame interval 50 ms -> %d ms", ms);
}

int eb_threads_install(void) {
    patch_midtick_interval();
    if (g_cfg.bg_full_speed) install_bg_unthrottle();
    if (g_cfg.main_affinity < 0) { eb_log("threads: main thread affinity left to the game (main_thread=game)"); return 1; }
    o_SetThreadAffinityMask = (SetThreadAffinityMask_t)patch_iat(g_exe, "KERNEL32.dll", "SetThreadAffinityMask", (void*)hk_SetThreadAffinityMask);
    if (!o_SetThreadAffinityMask) { eb_log("threads: SetThreadAffinityMask import not found - not patched"); return 0; }
    eb_log("threads: main thread affinity hook installed (mode: %s)", g_cfg.main_affinity == 0 ? "free" : "fixed CPU");
    return 1;
}
