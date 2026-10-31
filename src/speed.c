// Wall-clock game speed log.
//
// The game's own timer_dump only measures the daily tick itself; everything the main thread
// does between two days (rendering, scene update, online-service polling...) is not in it.
// At speed 5 the game advances one day per frame, so the real measure of "how fast does the
// game run" is the wall-clock time from the start of one day to the start of the next.
//
// Hooks CDailyTickCommand::Execute (eu4.exe+0xb79520, timercommands.cpp), which runs once per
// game day on the main thread. The current date is the CDate at gamestate+0x1dd0 (the daily
// tick 0x140758430 adds 24 hours to it first); the gamestate pointer is at eu4.exe+0x233fe78.
// (The command's own date at +0x50 is left at the default 1.1.1 in single player.) Writes
//   plugins\eu4boost_speed_<time>.csv : date, wall_ms (start of this day -> start of next), tick_ms
// plus a summary line in eu4boost.log every 100 days and at exit.
// Gaps longer than 1 s (pause, menus, saving) end a "run"; they are not counted.
#include "common.h"
#include <stdio.h>
#include <string.h>
#include "MinHook.h"

typedef void (*daily_cmd_t)(void*);
static daily_cmd_t o_daily;

#define RVA_DAILY_CMD 0xb79520u
static const uint8_t sig_daily[16] = { 0x48,0x8b,0xc4,0x48,0x89,0x58,0x08,0x48,0x89,0x68,0x18,0x48,0x89,0x70,0x20,0x57 };

#define RVA_GAMESTATE_PTR 0x233fe78u
#define GS_DATE_OFFSET 0x1dd0
static uint8_t** g_gs_ptr;

static HANDLE g_csv = INVALID_HANDLE_VALUE;
static double g_qpf_ms;
static void fmt_date(int v, char* out, size_t n);
static int g_seed_checked;
static void seed_check(void);

// ---- optional: daily game-state checksum (validation of logic changes) ----
// The game's own multiplayer OOS check: FUN_140746c40 fills a vector with 140 checksum entries
// {vtable, int 0x165, int value} (24 bytes each) and FUN_140743aa0(gamestate, &vector) computes
// them (entries 0 and 1 are the two random-generator states). Logging all 140 values every day
// lets two runs from the same save be compared category by category.
#define RVA_CS_INIT 0x746c40u
#define RVA_CS_CALC 0x743aa0u
static const uint8_t sig_cs_init[16] = { 0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x48 };
static const uint8_t sig_cs_calc[16] = { 0x48,0x89,0x54,0x24,0x10,0x48,0x89,0x4c,0x24,0x08,0x55,0x53,0x56,0x57,0x41,0x54 };
typedef void (*cs_init_t)(void* vec);
typedef void (*cs_calc_t)(void* gs, void* vec);
static cs_init_t g_cs_init;
static cs_calc_t g_cs_calc;
static struct { uint8_t* b; uint8_t* e; uint8_t* c; } g_csvec;
static void* g_cs_vtbl;
static int g_cs_n;
static HANDLE g_cs_file = INVALID_HANDLE_VALUE;
static double g_cs_ms; static long g_cs_days;

static void cs_write(const char* s) { DWORD w; WriteFile(g_cs_file, s, (DWORD)strlen(s), &w, NULL); }

static void state_checksum(uint8_t* gs, int date) {
    if (!g_cs_init || !gs) return;
    if (!g_csvec.b) {
        g_cs_init(&g_csvec);
        g_cs_n = (int)((g_csvec.e - g_csvec.b) / 24);
        if (g_cs_n <= 0 || g_cs_n > 1000) { g_cs_init = NULL; eb_log("checksum: unexpected vector size %d - off", g_cs_n); return; }
        g_cs_vtbl = *(void**)g_csvec.b;
        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t path[MAX_PATH];
        _snwprintf(path, MAX_PATH, L"%lseu4boost_state_%04d%02d%02d_%02d%02d%02d.csv", g_dir,
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        g_cs_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        char hdr[32]; cs_write("date");
        for (int i = 0; i < g_cs_n; i++) { snprintf(hdr, sizeof hdr, ",c%d", i); cs_write(hdr); }
        cs_write("\r\n");
        eb_log("checksum: %d categories per day -> eu4boost_state_*.csv", g_cs_n);
    }
    for (int i = 0; i < g_cs_n; i++) {           // same state as the constructor leaves it
        uint8_t* e = g_csvec.b + 24 * i;
        *(void**)e = g_cs_vtbl; *(int*)(e + 8) = 0x165; *(int*)(e + 16) = 0;
    }
    LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
    g_cs_calc(gs, &g_csvec);
    QueryPerformanceCounter(&b);
    g_cs_ms += (double)(b.QuadPart - a.QuadPart) * g_qpf_ms; g_cs_days++;
    char line[16 * 1024]; int n = 0;
    char d[24]; fmt_date(date, d, sizeof d);
    n += snprintf(line + n, sizeof line - n, "%s", d);
    for (int i = 0; i < g_cs_n && n < (int)sizeof line - 16; i++)
        n += snprintf(line + n, sizeof line - n, ",%08x", (unsigned)*(int*)(g_csvec.b + 24 * i + 16));
    n += snprintf(line + n, sizeof line - n, "\r\n");
    DWORD w; WriteFile(g_cs_file, line, (DWORD)n, &w, NULL);
}
static LONGLONG g_prev_start;          // QPC at start of previous day (0 = none)
static int g_prev_date;
static volatile LONG g_ticks;          // days processed so far (for speed-5 pacing)
static void pacing_report(void);
static double g_prev_tick_ms;
// current run
static int g_run_days, g_run_first, g_run_last;
static double g_run_wall, g_run_tick;
static int g_blk_days, g_blk_first;
static double g_blk_wall, g_blk_tick;
// totals
static long g_tot_days, g_runs;
static double g_tot_wall, g_tot_tick;

static void fmt_date(int v, char* out, size_t n) {
    int hours = v - 43800000;
    int days = hours >= 0 ? hours / 24 : -((-hours + 23) / 24);
    int year = days >= 0 ? days / 365 : -((-days + 364) / 365);
    int doy = days - year * 365;
    static const int ml[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int m = 0;
    while (m < 11 && doy >= ml[m]) { doy -= ml[m]; m++; }
    snprintf(out, n, "%d.%02d.%02d", year, m + 1, doy + 1);
}

static int day_of_month(int v) {
    int hours = v - 43800000;
    int days = hours >= 0 ? hours / 24 : -((-hours + 23) / 24);
    int year = days >= 0 ? days / 365 : -((-days + 364) / 365);
    int doy = days - year * 365;
    static const int ml[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int m = 0;
    while (m < 11 && doy >= ml[m]) { doy -= ml[m]; m++; }
    return doy + 1;
}

static void csv_write(const char* s) {
    if (g_csv == INVALID_HANDLE_VALUE) return;
    DWORD w; WriteFile(g_csv, s, (DWORD)strlen(s), &w, NULL);
}

static void open_csv(void) {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%lseu4boost_speed_%04d%02d%02d_%02d%02d%02d.csv", g_dir,
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    g_csv = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    csv_write("date,wall_ms,tick_ms\r\n");
}

static void log_span(const char* what, int first, int last, int days, double wall, double tick) {
    if (days <= 0 || wall <= 0) return;
    char a[24], b[24]; fmt_date(first, a, sizeof a); fmt_date(last, b, sizeof b);
    eb_log("speed: %s %s - %s: %d days in %.1f s = %.2f days/s (%.1f ms per day, of which daily tick %.1f ms)",
           what, a, b, days, wall / 1000.0, days * 1000.0 / wall, wall / days, tick / days);
}

static void end_run(void) {
    if (g_run_days >= 20) log_span("run", g_run_first, g_run_last, g_run_days, g_run_wall, g_run_tick);
    g_run_days = 0; g_run_wall = g_run_tick = 0;
    g_blk_days = 0; g_blk_wall = g_blk_tick = 0;
}

static void hk_daily(void* cmd) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    o_daily(cmd);
    QueryPerformanceCounter(&t1);
    uint8_t* gs = g_gs_ptr ? *g_gs_ptr : NULL;
    int date = gs ? *(int*)(gs + GS_DATE_OFFSET) : 0;     // the day just processed
    if (!g_seed_checked && g_cfg.fixed_seed) seed_check();
    if (g_cs_init) state_checksum(gs, date);
    double tick_ms = (double)(t1.QuadPart - t0.QuadPart) * g_qpf_ms;
    InterlockedIncrement(&g_ticks);

    if (g_cfg.speed_log && g_csv == INVALID_HANDLE_VALUE) open_csv();
    if (g_prev_start) {
        double wall = (double)(t0.QuadPart - g_prev_start) * g_qpf_ms;
        char d[24], line[96]; fmt_date(g_prev_date, d, sizeof d);
        if (wall > 1000.0) {                       // paused / menu / saving: not a speed sample
            snprintf(line, sizeof line, "%s,,%.2f\r\n", d, g_prev_tick_ms);
            csv_write(line);
            end_run();
        } else {
            snprintf(line, sizeof line, "%s,%.2f,%.2f\r\n", d, wall, g_prev_tick_ms);
            csv_write(line);
            if (!g_run_days) g_run_first = g_prev_date;
            if (!g_blk_days) g_blk_first = g_prev_date;
            g_run_days++; g_run_wall += wall; g_run_tick += g_prev_tick_ms; g_run_last = g_prev_date;
            g_blk_days++; g_blk_wall += wall; g_blk_tick += g_prev_tick_ms;
            g_tot_days++; g_tot_wall += wall; g_tot_tick += g_prev_tick_ms;
            if (g_run_days == 1) g_runs++;
            if (g_blk_days == 100) {
                log_span("100d", g_blk_first, g_prev_date, g_blk_days, g_blk_wall, g_blk_tick);
                pacing_report();
                g_blk_days = 0; g_blk_wall = g_blk_tick = 0;
            }
            if (g_cfg.par_stats && g_tot_days % 365 == 0) eb_parallel_report();   // cumulative, also when the game is killed
            if ((g_cfg.mod_fast || g_cfg.mod_verify) && g_tot_days % 100 == 0) eb_modifier_report();
            if ((g_cfg.country_stats || g_cfg.country_parallel_refresh) && g_tot_days % 100 == 0) eb_country_report();
        }
    }
    g_prev_start = t0.QuadPart; g_prev_date = date; g_prev_tick_ms = tick_ms;
}

// ---- optional: fixed random seed (same as the game's own "-seed=N" command-line option) ----
// main (eu4.exe+0x8757e0) handles "-seed=N" by setting a fixed-seed flag (+0x23581c0) and value
// (+0x23581c4) plus the synchronized RNG state (seed +0x23494b4, counter +0x23494b0). With the flag
// set, loading or starting a game (FUN_140ad0880) reseeds from the fixed value instead of the
// clock, and about 150 other places that seed local RNGs with _time64() use it too. Without it,
// two runs from the same save diverge on the first day, so logic changes cannot be validated.
#define RVA_FIXED_SEED_FLAG 0x23581c0u
#define RVA_FIXED_SEED_VAL  0x23581c4u
#define RVA_RNG_SEED        0x23494b4u
#define RVA_RNG_COUNT       0x23494b0u
static void seed_check(void) {
    g_seed_checked = 1;
    uint8_t* b = (uint8_t*)g_exe;
    eb_log("seed: fixed-seed flag %d, fixed seed %d, current RNG seed %d",
           *(volatile uint8_t*)(b + RVA_FIXED_SEED_FLAG), *(volatile int*)(b + RVA_FIXED_SEED_VAL), *(volatile int*)(b + RVA_RNG_SEED));
}

static void seed_install(void) {
    if (!g_cfg.fixed_seed) return;
#ifndef EB_TEST
    uint8_t* b = (uint8_t*)g_exe;
    *(volatile int*)(b + RVA_FIXED_SEED_VAL) = g_cfg.fixed_seed;
    *(volatile uint8_t*)(b + RVA_FIXED_SEED_FLAG) = 1;
    *(volatile int*)(b + RVA_RNG_SEED) = g_cfg.fixed_seed;
    *(volatile int*)(b + RVA_RNG_COUNT) = 0;
    eb_log("seed: fixed random seed %d (like -seed=%d; for validation runs only)", g_cfg.fixed_seed, g_cfg.fixed_seed);
#endif
}

// ---- optional: several days per frame at speed 5 ([render] speed5_days_per_frame) ----
// A frame (eu4.exe+0x8137c0) runs the session update - at speed 5 that executes one daily tick - and
// then updates the map objects and renders. At speed 5 a frame therefore costs a whole render per
// day (8% + ~6% map updates of the main thread in the modded 1544 save). The session update wrapper
// FUN_14020f810(app session) only sets up and runs CSession::Update (+ a little time keeping). The
// next day is queued by the game timer FUN_140757ad0(gamestate, frame seconds), which the frame calls
// after the session update: it adds the frame time and, once the per-speed day length (table
// 0x141c33e30 = 2, 1, 0.5, 0.2, 0 s) is reached, posts one CDailyTickCommand. So after the session
// update the hook calls the timer with 0 s (at speed 5 the day length is 0) and the session update
// again: the next day runs in the same frame - the same commands in the same order, just fewer
// frames. It is repeated only while
//   - the previous call actually ran a day (nothing happens while paused or at slower speeds),
//   - the game speed is 5 (gamestate+0x1e08 == 4, the index the mid-tick render table uses),
//   - the day just processed was not the 1st of a month (the monthly update and the quarterly
//     autosave keep their own frame, so saves happen on the same date as before),
//   - the frame has used less than speed5_frame_budget_ms (the map still redraws ~10 times a second),
//   - it is a single-player game (the timer's multiplayer date sync, session+0x360, is not in use).
#define RVA_SESSION_FRAME 0x20f810u
#define RVA_GAME_TIMER    0x757ad0u
static const uint8_t sig_session_frame[16] = { 0x48,0x89,0x5c,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xf9,0x48,0x8b,0x81 };
static const uint8_t sig_game_timer[16]    = { 0x40,0x55,0x48,0x83,0xec,0x50,0x80,0xb9,0x10,0x1e,0x00,0x00,0x00,0x0f,0x28,0xd1 };
#define GS_SPEED_OFFSET   0x1e08
#define GS_SESSION_OFFSET 0x1e00
typedef void (*session_frame_t)(void*);
typedef void (*game_timer_t)(void* gs, float seconds);
static session_frame_t o_session_frame;
static game_timer_t game_timer;
static volatile LONG64 g_frames, g_extra_days, g_stop_noday, g_stop_speed, g_stop_month, g_stop_budget, g_repeat_noday;
static volatile int g_last_speed = -1;

static void hk_session_frame(void* s) {
    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
    LONG ticks = g_ticks;
    o_session_frame(s);
    g_frames++;
    for (int d = 1; d < g_cfg.days_per_frame; d++) {
        if (g_ticks == ticks) { if (d == 1) g_stop_noday++; else g_repeat_noday++; break; }   // no day ran
        uint8_t* gs = g_gs_ptr ? *g_gs_ptr : NULL;
        if (!gs) break;
        g_last_speed = *(int*)(gs + GS_SPEED_OFFSET);
        if (g_last_speed != 4) { g_stop_speed++; break; }            // speed 5 only
        if (day_of_month(g_prev_date) == 1) { g_stop_month++; break; } // month start keeps its frame
        LARGE_INTEGER t; QueryPerformanceCounter(&t);
        if ((double)(t.QuadPart - t0.QuadPart) * g_qpf_ms >= g_cfg.frame_budget_ms) { g_stop_budget++; break; }
        void** session = *(void***)(gs + GS_SESSION_OFFSET);
        if (!session || session[0x6c]) break;                         // multiplayer date sync in use
        ticks = g_ticks;
        game_timer(gs, 0.0f);                                         // queue the next day (day length 0 at speed 5)
        o_session_frame(s);
        if (g_ticks != ticks) g_extra_days++;
    }
}

static void pacing_report(void) {
    if (o_session_frame)
        eb_log("render: %lld frames, %lld extra days | stops: no day %lld, speed %lld (last speed index %d), month start %lld, budget %lld, repeat without day %lld",
               (long long)g_frames, (long long)g_extra_days, (long long)g_stop_noday, (long long)g_stop_speed, g_last_speed,
               (long long)g_stop_month, (long long)g_stop_budget, (long long)g_repeat_noday);
}

int eb_speed_install(void) {
    if (!g_cfg.speed_log && !g_cfg.state_checksum && !g_cfg.fixed_seed && g_cfg.days_per_frame <= 1) return 0;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpf_ms = 1000.0 / (double)f.QuadPart;
    uint8_t* t;
#ifdef EB_TEST
    t = (uint8_t*)GetProcAddress(g_exe, "t_daily_cmd");
    g_gs_ptr = (uint8_t**)GetProcAddress(g_exe, "t_gs");
    if (!t || !g_gs_ptr) { eb_log("speed: test target not found"); return 0; }
#else
    t = (uint8_t*)g_exe + RVA_DAILY_CMD;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)((uint8_t*)g_exe + ((IMAGE_DOS_HEADER*)g_exe)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != 0x66FE6AB9u || memcmp(t, sig_daily, sizeof sig_daily) != 0) {
        eb_log("speed: daily tick command signature mismatch - speed log off");
        return 0;
    }
    g_gs_ptr = (uint8_t**)((uint8_t*)g_exe + RVA_GAMESTATE_PTR);
    seed_install();
    if (g_cfg.state_checksum) {
        uint8_t* ci = (uint8_t*)g_exe + RVA_CS_INIT; uint8_t* cc = (uint8_t*)g_exe + RVA_CS_CALC;
        if (memcmp(ci, sig_cs_init, 16) == 0 && memcmp(cc, sig_cs_calc, 16) == 0) {
            g_cs_init = (cs_init_t)ci; g_cs_calc = (cs_calc_t)cc;
            eb_log("checksum: daily game-state checksum ON (validation mode, slows the game)");
        } else eb_log("checksum: signature mismatch - off");
    }
#endif
    MH_STATUS s = MH_Initialize();
    if ((s == MH_OK || s == MH_ERROR_ALREADY_INITIALIZED) &&
        MH_CreateHook(t, (void*)hk_daily, (void**)&o_daily) == MH_OK && MH_EnableHook(t) == MH_OK) {
        if (g_cfg.speed_log) eb_log("speed: logging real time per game day (eu4boost_speed_*.csv)");
        if (g_cfg.days_per_frame > 1) {
            uint8_t* sf;
#ifdef EB_TEST
            sf = (uint8_t*)GetProcAddress(g_exe, "t_session_frame");
            game_timer = (game_timer_t)(void*)GetProcAddress(g_exe, "t_game_timer");
#else
            sf = (uint8_t*)g_exe + RVA_SESSION_FRAME;
            game_timer = (game_timer_t)(void*)((uint8_t*)g_exe + RVA_GAME_TIMER);
            if (memcmp(sf, sig_session_frame, sizeof sig_session_frame) != 0 ||
                memcmp((void*)game_timer, sig_game_timer, sizeof sig_game_timer) != 0) sf = NULL;
#endif
            if (!game_timer) sf = NULL;
            if (sf && MH_CreateHook(sf, (void*)hk_session_frame, (void**)&o_session_frame) == MH_OK && MH_EnableHook(sf) == MH_OK)
                eb_log("render: up to %d days per frame at speed 5 (frame budget %d ms)", g_cfg.days_per_frame, g_cfg.frame_budget_ms);
            else { eb_log("render: session update signature mismatch - one day per frame"); g_cfg.days_per_frame = 1; }
        }
        return 1;
    }
    eb_log("speed: hook failed - speed log off");
    return 0;
}

void eb_speed_report(void) {
    if (!o_daily) return;
    pacing_report();
    end_run();
    if (g_tot_days > 0)
        eb_log("speed: total %ld days in %d run(s), %.1f s = %.2f days/s (%.1f ms per day, of which daily tick %.1f ms)",
               g_tot_days, (int)g_runs, g_tot_wall / 1000.0, g_tot_days * 1000.0 / g_tot_wall,
               g_tot_wall / g_tot_days, g_tot_tick / g_tot_days);
    if (g_csv != INVALID_HANDLE_VALUE) { CloseHandle(g_csv); g_csv = INVALID_HANDLE_VALUE; }
    if (g_cs_days) eb_log("checksum: %ld days, %.2f ms per checksum", g_cs_days, g_cs_ms / g_cs_days);
    if (g_cs_file != INVALID_HANDLE_VALUE) { CloseHandle(g_cs_file); g_cs_file = INVALID_HANDLE_VALUE; }
}
