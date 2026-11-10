// Where the time between two days goes ([diag] day_gap=1, read-only diagnostic, 2026-11-10).
//
// Why: in the user's late-game play a day takes 141-178 ms of wall time while its daily tick takes 49-65 ms;
// 11-18 frames pass per day (3.7 in the off-screen tests). The game's timer FUN_140757ad0(gamestate, dt),
// called from the idler, issues the next daily tick command only when
//     no daily tick is running (gs+0x1e10: set at 0x1407584a2 and cleared at 0x140758eee inside the tick, so
//     the tick's in-between frames see it set), the speed's interval has passed (gs+0x1df8 against the table
//     at 0x141c33e30 by gs+0x1e08: 2, 1, 0.5, 0.2, 0 s), the session's (gs+0x1e00) object from virtual +0x80
//     has +0x54 or +0x55 set, gs+0x1ed1 is 0, the session's virtuals +0x158 and +0x160 say no (the player's
//     pause is expected here), and the date of the last issued tick (gs+0x1e14, hours) is not ahead of the
//     earliest date in the list at gs+0xa50 (+0x20, next +0x60; one day of lead when session[0x6c]+0xcc is
//     set): the tick issued before must have been executed.
// Which of these keeps the days apart in real play is not known; it decides whether anything can be gained
// there without giving up frames.
// What: the timer is hooked (MinHook, the function is called as it is) and every call is counted by what
// kept it from issuing a day. With the daily tick's start and end (guard.c) and the frame counter (crash.c)
// the time from one tick's end to the next tick's start is split into
//     until the next day is issued  +  from the issue until its tick starts
// (a day can be issued before the tick has ended, in one of the tick's in-between frames after the flag is
// cleared: then the whole gap is waiting for the issued tick). Gaps in which the session said wait, or
// longer than 2 s, are pauses and are kept apart.
// Nothing of the game is changed and none of its functions is called besides the session's getter +0x80
// (the timer itself calls it, twice when it issues a day) and its getter of the pause object (+0x100, read_gate).
// Log lines every minute and at exit.
#include "common.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "MinHook.h"

#define RVA_TIMER 0x757ad0u           // FUN_140757ad0(gamestate, float dt)
#define RVA_INTERVALS 0x1c33e30u      // float[5]: seconds between days at speed 1..5
#define RVA_NO_DATE 0x1fd13c4u        // int: the "no date" value besides 0x29c77f8
static const uint8_t sig_timer[16] = { 0x40,0x55,0x48,0x83,0xec,0x50,0x80,0xb9,0x10,0x1e,0x00,0x00,0x00,0x0f,0x28,0xd1 };

enum { R_TICK, R_INTERVAL, R_NET, R_FLAG, R_SESSION, R_DATE, R_HELD, R_ISSUED, R_N };
// "in-between frame": gs+0x1e10 is also set by the frame drawn in the middle of a long step (FUN_140769ea0), in and before the tick
static const char* const R_NAME[R_N] = { "in-between frame", "speed interval", "session object flags", "flag 0x1ed1", "session says wait",
                                         "issued tick not run yet", "held back by speed5_gap_pct", "issued" };

typedef struct {
    LONG64 days, early;                                           // gaps (tick end -> next tick start) at speed 5; of them: issued before the tick ended
    LONG64 ticks, tick_t, tick_frames, tick_calls;                // the ticks themselves; frames and timer calls inside them
    LONG64 before_t, after_t;                                     // the gap: until the next day is issued, from the issue to the tick's start
    LONG64 before_frames, after_frames, before_calls, after_calls;
    LONG64 why_before[R_N], why_after[R_N];                       // what the timer calls in the two parts ran into
    LONG64 gate_before[4], gate_after[4];                         // frames of the two parts by the pause object's state (read_gate)
    LONG64 held, held_t;                                          // gaps with "session says wait" or over 2 s (pauses)
    LONG64 calls[5][R_N];                                         // every timer call, by speed
    double dt_sum; LONG64 wall_t;                                 // the dt the game gave its timer against the wall time, at speed 5
    LONG64 before_max, after_max;
} dg_t;
typedef struct { LONG64 frames[2], calls[2], why[2][R_N], gate[2][4]; int held; } gap_t;    // [0] before the issue, [1] after it
static dg_t g, g_all;
static gap_t c;                                                   // the gap in progress
int eb_dg_on;
static void (*o_timer)(uint8_t* gs, float dt);
static uint8_t* g_base;
static double g_ms;
static int g_in_tick; static LONGLONG g_t_tick0, g_t_tick1, g_t_issue, g_t_last, g_t_report, g_report_every, g_two_s;

static LONGLONG qpc(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

// ---- pacing ([render] speed5_gap_pct) ----
// The first real session with the alert throttle (2026-11-10): every frame got faster (5.0 instead of 7.0 ms,
// 69.9 instead of 56.9 fps) and the game 39% faster, but the user felt it "heavier": the game issues the next day
// a few frames after a tick, so with shorter frames the long tick frames (about 50 ms) came every 95 ms instead
// of every 133 ms and the stretch of normal frames between them shrank from about 75 to about 45 ms.
// speed5_gap_pct gives that stretch a minimum: the next day is not issued before the time since the last tick's
// end, plus the usual time from an issue to its tick's start, is pct % of a usual tick (medians of the last 15;
// at most 200 ms). 0 = off, 100 = the normal frames get as long as the tick took, about 140 = the rhythm before.
// A held call runs the game's timer with gs+0x1e10 set for the call, exactly as an in-between frame of a tick does:
// the timer adds its dt and issues nothing. Nothing of a day's content changes, only when it starts.
#define PACE_N 15
typedef struct { LONGLONG v[PACE_N]; int n, at; } ring_t;
static void ring_add(ring_t* r, LONGLONG x) { r->v[r->at] = x; r->at = (r->at + 1) % PACE_N; if (r->n < PACE_N) r->n++; }
static LONGLONG ring_median(const ring_t* r) {                    // 0 when empty
    LONGLONG s[PACE_N]; int n = r->n;
    for (int i = 0; i < n; i++) { LONGLONG x = r->v[i]; int j = i; while (j > 0 && s[j - 1] > x) { s[j] = s[j - 1]; j--; } s[j] = x; }
    return n ? s[n / 2] : 0;
}
static ring_t g_tick_ring, g_lat_ring;
static int g_pace_pct, g_diag; static LONGLONG g_pace_cap, g_med_tick, g_med_lat;
static LONG64 g_pace_held, g_pace_issues, g_pace_issue_t;
// how long after a tick's end the next day may be issued at the earliest (ticks)
static LONGLONG pace_wait(LONGLONG med_tick, LONGLONG med_lat, int pct, LONGLONG cap) {
    if (pct <= 0 || med_tick <= 0) return 0;
    LONGLONG w = med_tick * pct / 100;
    if (w > cap) w = cap;
    w -= med_lat;
    return w > 0 ? w : 0;
}

// why a timer call with this state does not issue a day (R_ISSUED = nothing in the way that can be read here).
// accum = gs+0x1df8 after adding dt; net = the session object's +0x54 | +0x55; issued / earliest = dates in days;
// lead = 1 when a day may be issued one ahead; no_list = the session has no date list (plVar3[0x6c] == 0)
static int why_not(int in_tick, float accum, float interval, int net, int flag, int no_list, int issued, int earliest, int lead) {
    if (in_tick) return R_TICK;
    if (!(interval <= accum)) return R_INTERVAL;
    if (!net) return R_NET;
    if (flag) return R_FLAG;
    if (!no_list && earliest + (lead != 0) < issued) return R_DATE;
    return R_ISSUED;
}
static void on_timer(int speed, int why, LONGLONG now, float dt) {
    if (speed < 0) speed = 0;
    if (speed > 4) speed = 4;
    g.calls[speed][why]++;
    if (speed != 4) { g_t_tick1 = 0; g_t_issue = 0; g_t_last = now; return; }     // only speed 5 is split into parts
    if (g_t_last) { g.wall_t += now - g_t_last; g.dt_sum += dt; }
    g_t_last = now;
    if (why == R_ISSUED) { g_t_issue = now; return; }
    if (g_in_tick) { g.tick_calls++; return; }
    int part = g_t_issue != 0;
    c.calls[part]++; c.why[part][why]++;
    if (why == R_SESSION) c.held = 1;
}
// The idler calls the timer only when FUN_1407e01b0(its +0x418) says the game is not paused: that object's flag
// +0x48 is clear and its list of pause requests (+0, +8) is empty. In real play the timer ran only 1.8 times a day
// (6.7 frames), so something pauses most frames. The session's pause object (virtual +0x100, the one autorun.c
// unpauses) is read once a frame between two ticks: 0 = nothing pauses, 1 = the flag, 2 = a pause request, 3 = not readable.
#define RVA_GS 0x233fe78u
static int read_gate(void* u) {
    (void)u;
    uint8_t* gs = *(uint8_t* volatile*)(g_base + RVA_GS);
    if (!gs) return 3;
    void** session = *(void***)(gs + 0x1e00);
    if (!session) return 3;
    void** pause = ((void** (*)(void*))(*(void***)session)[0x100 / 8])(session);
    if (!pause) return 3;
    if (*((uint8_t*)pause + 0x48)) return 1;
    return (((uintptr_t)pause[1] - (uintptr_t)pause[0]) & ~(uintptr_t)7) ? 2 : 0;
}
static void on_frame(void) {
    if (g_in_tick) { g.tick_frames++; return; }
    int part = g_t_issue != 0;
    c.frames[part]++;
    if (g_diag && g_base && g_t_tick1 && qpc() - g_t_tick1 < g_two_s) {       // a tick has just run: the game state is there
        int st = eb_guarded(read_gate, NULL);
        c.gate[part][st >= 0 && st < 3 ? st : 3]++;
    }
}
static void on_tick(int end, LONGLONG now) {
    if (end) {
        if (g_in_tick) { g.ticks++; g.tick_t += now - g_t_tick0; ring_add(&g_tick_ring, now - g_t_tick0); g_med_tick = ring_median(&g_tick_ring); }
        g_in_tick = 0; g_t_tick1 = now;
        memset(&c, 0, sizeof c);                                  // a gap begins; an issue seen during the tick stays (g_t_issue)
        return;
    }
    if (g_t_tick1 && g_t_issue > g_t_tick1 && now - g_t_issue < g_two_s) {       // the usual time from an issue to its tick's start
        ring_add(&g_lat_ring, now - g_t_issue); g_med_lat = ring_median(&g_lat_ring);
    }
    if (g_t_tick1 && g_t_issue) {                                 // a whole gap, with the issue of the tick that starts now
        LONGLONG gap = now - g_t_tick1;
        if (c.held || gap > g_two_s) { g.held++; g.held_t += gap; }
        else {
            LONGLONG before = g_t_issue > g_t_tick1 ? g_t_issue - g_t_tick1 : 0, after = gap - before;
            g.days++; if (!before) g.early++;
            g.before_t += before; g.after_t += after;
            if (before > g.before_max) g.before_max = before;
            if (after > g.after_max) g.after_max = after;
            g.before_frames += c.frames[0]; g.after_frames += c.frames[1]; g.before_calls += c.calls[0]; g.after_calls += c.calls[1];
            for (int r = 0; r < R_N; r++) { g.why_before[r] += c.why[0][r]; g.why_after[r] += c.why[1][r]; }
            for (int k = 0; k < 4; k++) { g.gate_before[k] += c.gate[0][k]; g.gate_after[k] += c.gate[1][k]; }
        }
    }
    g_in_tick = 1; g_t_tick0 = now; g_t_issue = 0;
}
void eb_dg_frame(void) { on_frame(); }                            // crash.c: a frame begins (main thread)
void eb_dg_day(int end) { on_tick(end, qpc()); }                  // guard.c: the daily tick starts (0) / has ended (1), main thread

static void line(const dg_t* s, const char* what) {
    char w1[300], w2[300]; int n1 = 0, n2 = 0;
    w1[0] = w2[0] = 0;
    for (int r = 0; r < R_ISSUED; r++) {
        if (s->why_before[r]) n1 += snprintf(w1 + n1, sizeof w1 - (size_t)n1, "%s%s %lld", n1 ? ", " : "", R_NAME[r], (long long)s->why_before[r]);
        if (s->why_after[r]) n2 += snprintf(w2 + n2, sizeof w2 - (size_t)n2, "%s%s %lld", n2 ? ", " : "", R_NAME[r], (long long)s->why_after[r]);
    }
    double d = s->days ? (double)s->days : 1.0, t = s->ticks ? (double)s->ticks : 1.0;
    eb_log("daygap: %s: %lld days at speed 5, %.1f ms a day = tick %.1f ms (%.1f frames, %.1f timer calls) + %.1f ms until the next day is issued "
           "(%.1f frames, %.1f timer calls, longest %.0f ms) + %.1f ms until its tick starts (%.1f frames, %.1f timer calls, longest %.0f ms)",
           what, (long long)s->days, (double)s->tick_t * g_ms / t + (double)(s->before_t + s->after_t) * g_ms / d,
           (double)s->tick_t * g_ms / t, (double)s->tick_frames / t, (double)s->tick_calls / t,
           (double)s->before_t * g_ms / d, (double)s->before_frames / d, (double)s->before_calls / d, (double)s->before_max * g_ms,
           (double)s->after_t * g_ms / d, (double)s->after_frames / d, (double)s->after_calls / d, (double)s->after_max * g_ms);
    eb_log("daygap:   %lld of the days were issued before the tick had ended | timer calls that did not issue a day: %s | calls between the issue "
           "and the tick: %s | pauses: %lld (%.1f s) | game timer %.1f s for %.1f s of wall time", (long long)s->early, n1 ? w1 : "none",
           n2 ? w2 : "none", (long long)s->held, (double)s->held_t * g_ms / 1000.0, s->dt_sum, (double)s->wall_t * g_ms / 1000.0);
    eb_log("daygap:   frames by the pause object's state - until the issue: nothing pauses %lld, pause flag %lld, pause request %lld, unreadable %lld | "
           "from the issue to the tick: %lld, %lld, %lld, %lld | usual tick %.1f ms, usual issue-to-tick %.1f ms, speed5_gap_pct %d",
           (long long)s->gate_before[0], (long long)s->gate_before[1], (long long)s->gate_before[2], (long long)s->gate_before[3],
           (long long)s->gate_after[0], (long long)s->gate_after[1], (long long)s->gate_after[2], (long long)s->gate_after[3],
           (double)g_med_tick * g_ms, (double)g_med_lat * g_ms, g_pace_pct);
}
static void add(dg_t* to, const dg_t* s) {
    const LONG64* a = (const LONG64*)s; LONG64* b = (LONG64*)to;
    size_t n = offsetof(dg_t, dt_sum) / sizeof(LONG64);
    for (size_t i = 0; i < n; i++) b[i] += a[i];
    to->dt_sum += s->dt_sum; to->wall_t += s->wall_t;
    if (s->before_max > to->before_max) to->before_max = s->before_max;
    if (s->after_max > to->after_max) to->after_max = s->after_max;
}

static void hk_timer(uint8_t* gs, float dt) {
    int in_tick = gs[0x1e10] != 0, speed = *(int*)(gs + 0x1e08), why;
    float accum = *(float*)(gs + 0x1df8) + dt;
    float interval = speed >= 0 && speed <= 4 ? *(float*)(g_base + RVA_INTERVALS + (size_t)speed * 4) : 1e9f;
    if (g_pace_pct && speed == 4 && !in_tick && g_t_tick1 && !g_t_issue && interval <= accum &&
        qpc() - g_t_tick1 < pace_wait(g_med_tick, g_med_lat, g_pace_pct, g_pace_cap)) {
        gs[0x1e10] = 1;                                           // as in an in-between frame of a tick: the timer counts its dt and issues nothing
        o_timer(gs, dt);
        gs[0x1e10] = 0;
        g_pace_held++;
        on_timer(speed, R_HELD, qpc(), dt);
        return;
    }
    if (in_tick || !(interval <= accum)) why = why_not(in_tick, accum, interval, 1, 0, 1, 0, 0, 0);
    else {
        void** session = *(void***)(gs + 0x1e00);
        uint8_t* net = ((uint8_t* (*)(void*))(*(void***)session)[0x80 / 8])(session);
        int no_date = *(int*)(g_base + RVA_NO_DATE);
        uint8_t* list = (uint8_t*)session[0x6c];
        int issued = *(int*)(gs + 0x1e14), earliest, lead = 0;
        if (issued == 0x29c77f8 || issued == no_date) issued = *(int*)(gs + 0x1dd0);
        earliest = issued / 24;
        if (list) {
            lead = *(int*)(list + 0xcc);
            for (uint8_t* n = *(uint8_t**)(gs + 0xa50); n; n = *(uint8_t**)(n + 0x60)) {
                int d = *(int*)(n + 0x20);
                if (d != 0x29c77f8 && d != no_date && d / 24 < earliest) earliest = d / 24;
            }
        }
        why = why_not(0, accum, interval, net[0x54] || net[0x55], gs[0x1ed1] != 0, list == NULL, issued / 24, earliest, lead);
    }
    o_timer(gs, dt);
    // issued = the timer set its count back to 0; when nothing readable was in the way and it still did not, the session said wait
    int did = *(float*)(gs + 0x1df8) == 0.0f && dt > 0.0f && !in_tick && interval <= accum;
    if (did) why = R_ISSUED; else if (why == R_ISSUED) why = R_SESSION;
    LONGLONG now = qpc();
    if (did && speed == 4 && g_t_tick1 && now - g_t_tick1 < g_two_s) { g_pace_issues++; g_pace_issue_t += now - g_t_tick1; }
    on_timer(speed, why, now, dt);
    if (g_diag && now - g_t_report >= g_report_every) {
        g_t_report = now;
        if (g.days >= 20) { line(&g, "last minute"); add(&g_all, &g); memset(&g, 0, sizeof g); }      // every minute: a session that crashes leaves them too
    }
}

int eb_daygap_install(void) {
    int pct = g_cfg.render_speed5_gap_pct;
    if (pct < 0) pct = g_cfg.render_speed5_gap_pct = 0;
    if (pct > 400) pct = g_cfg.render_speed5_gap_pct = 400;
    if (!g_cfg.diag_day_gap && !pct) return 0;
#ifdef EB_TEST
    return 0;
#else
    uint8_t* b = (uint8_t*)g_exe;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    g_ms = 1000.0 / (double)f.QuadPart; g_report_every = f.QuadPart * 60; g_two_s = f.QuadPart * 2; g_t_report = qpc(); g_base = b;
    if (nt->FileHeader.TimeDateStamp != 0x66FE6AB9u || memcmp(b + RVA_TIMER, sig_timer, sizeof sig_timer) || !eb_day_hooked) {
        eb_log("daygap: signature mismatch or no daily tick hook - no day gap diagnostic, no speed5_gap_pct");
        g_cfg.diag_day_gap = 0; g_cfg.render_speed5_gap_pct = 0;
        return 0;
    }
    MH_STATUS st = MH_Initialize();
    eb_hookset hs; eb_hs_init(&hs, "day timer");
    if ((st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) || !eb_hs_add(&hs, b + RVA_TIMER, (void*)hk_timer, (void**)&o_timer) || !eb_hs_enable(&hs)) {
        eb_log("daygap: hook failed - no day gap diagnostic, no speed5_gap_pct");
        o_timer = NULL; g_cfg.diag_day_gap = 0; g_cfg.render_speed5_gap_pct = 0;
        return 0;
    }
    eb_dg_on = 1; g_diag = g_cfg.diag_day_gap; g_pace_pct = pct; g_pace_cap = f.QuadPart / 5;
    if (g_diag) eb_log("daygap: DIAGNOSTIC what keeps the game from issuing the next day is counted every frame (nothing is changed)");
    if (pct) eb_log("daygap: at speed 5 the next day waits until the normal frames after a daily tick have had %d%% of a usual tick's time "
                    "(EXPERIMENT: more even frames for a little less speed; a day's content is unchanged)", pct);
    return 1;
#endif
}
void eb_daygap_report(void) {
    if (!o_timer) return;
    if (g_pace_pct)
        eb_log("daygap: speed5_gap_pct %d: %lld days issued %.1f ms after the last tick's end on average, %lld timer calls held back "
               "(a usual tick %.1f ms, from an issue to its tick %.1f ms at the end)", g_pace_pct, (long long)g_pace_issues,
               g_pace_issues ? (double)g_pace_issue_t * g_ms / (double)g_pace_issues : 0.0, (long long)g_pace_held, (double)g_med_tick * g_ms,
               (double)g_med_lat * g_ms);
    if (!g_diag) return;
    add(&g_all, &g);
    if (g_all.days) line(&g_all, "whole session");
    char s[700]; int n = 0;
    for (int sp = 0; sp < 5; sp++) {
        LONG64 t = 0; for (int r = 0; r < R_N; r++) t += g_all.calls[sp][r];
        if (!t) continue;
        n += snprintf(s + n, sizeof s - (size_t)n, " | speed %d:", sp + 1);
        for (int r = 0; r < R_N; r++)
            if (g_all.calls[sp][r] && n < (int)sizeof s - 60) n += snprintf(s + n, sizeof s - (size_t)n, " %s %lld", R_NAME[r], (long long)g_all.calls[sp][r]);
    }
    if (n) eb_log("daygap: timer calls by speed%s", s);
}

// ---- self-test (harness): the reasons and the split of a day ----
__declspec(dllexport) int eb_test_daygap(void) {
    int bad = 0;
    if (why_not(1, 5, 0, 1, 0, 0, 10, 10, 0) != R_TICK || why_not(0, 0.1f, 0.2f, 1, 0, 0, 10, 10, 0) != R_INTERVAL ||
        why_not(0, 0.2f, 0.2f, 0, 0, 0, 10, 10, 0) != R_NET || why_not(0, 1, 0, 1, 1, 0, 10, 10, 0) != R_FLAG ||
        why_not(0, 1, 0, 1, 0, 0, 11, 10, 0) != R_DATE || why_not(0, 1, 0, 1, 0, 0, 11, 10, 1) != R_ISSUED ||
        why_not(0, 1, 0, 1, 0, 0, 10, 10, 0) != R_ISSUED || why_not(0, 1, 0, 1, 0, 1, 99, 10, 0) != R_ISSUED) { puts("daygap: why_not"); bad++; }
    memset(&g, 0, sizeof g); memset(&c, 0, sizeof c); g_in_tick = 0; g_t_tick0 = g_t_tick1 = g_t_issue = g_t_last = 0; g_two_s = 2000;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_ms = 1000.0 / (double)f.QuadPart;
    // three days, 140 apart: tick 30..80 with an in-between frame, then 2 calls that do not issue, the issue at +110,
    // 1 call while the issued tick has not run; the next tick starts at +170 (= 30 of the next day)
    for (int day = 0; day < 3; day++) {
        LONGLONG t = 100 + day * 140;
        on_tick(0, t + 30);
        on_timer(4, R_TICK, t + 50, 0.01f); on_frame(); on_frame();
        on_tick(1, t + 80);
        on_frame(); on_timer(4, R_INTERVAL, t + 90, 0.01f); on_frame(); on_timer(4, R_FLAG, t + 100, 0.01f);
        on_frame(); on_timer(4, R_ISSUED, t + 110, 0.01f);
        on_frame(); on_frame(); on_timer(4, R_DATE, t + 120, 0.01f);
    }
    on_tick(0, 550);                                              // 3 gaps of 90 = 30 before the issue + 60 after it
    if (g.days != 3 || g.early != 0 || g.ticks != 3 || g.tick_t != 150 || g.before_t != 90 || g.after_t != 180 || g.tick_frames != 6 ||
        g.tick_calls != 3 || g.before_frames != 9 || g.after_frames != 6 || g.before_calls != 6 || g.after_calls != 3 ||
        g.why_before[R_INTERVAL] != 3 || g.why_before[R_FLAG] != 3 || g.why_after[R_DATE] != 3 || g.before_max != 30 || g.after_max != 60 ||
        g.calls[4][R_ISSUED] != 3 || g.held != 0) {
        printf("daygap: split %lld days, tick %lld, before %lld, after %lld, frames %lld %lld, calls %lld %lld\n", (long long)g.days, (long long)g.tick_t,
               (long long)g.before_t, (long long)g.after_t, (long long)g.before_frames, (long long)g.after_frames, (long long)g.before_calls, (long long)g.after_calls);
        bad++;
    }
    // a day issued while the tick (550..600) still runs: the whole gap (50) is waiting for its tick
    on_timer(4, R_ISSUED, 590, 0.01f); on_tick(1, 600); on_timer(4, R_DATE, 610, 0.01f); on_tick(0, 650);
    if (g.days != 4 || g.early != 1 || g.after_t != 230 || g.before_t != 90 || g.why_after[R_DATE] != 4) { puts("daygap: early issue"); bad++; }
    // a gap in which the session said wait (a pause), a gap with another speed in it, a gap of more than 2 s: not days
    on_tick(1, 700); on_timer(4, R_SESSION, 710, 0.01f); on_timer(4, R_ISSUED, 720, 0.01f); on_tick(0, 750);
    on_tick(1, 800); on_timer(2, R_INTERVAL, 810, 0.2f); on_timer(4, R_ISSUED, 820, 0.01f); on_tick(0, 850);
    on_tick(1, 900); on_timer(4, R_ISSUED, 950, 0.01f); on_tick(0, 2901);
    if (g.days != 4 || g.held != 2 || g.held_t != 2051 || g.ticks != 7 || g.tick_t != 350 || g.calls[4][R_ISSUED] != 7 || g.calls[2][R_INTERVAL] != 1) {
        printf("daygap: not days %lld, held %lld %lld, ticks %lld %lld\n", (long long)g.days, (long long)g.held, (long long)g.held_t, (long long)g.ticks,
               (long long)g.tick_t);
        bad++;
    }
    // the medians of the last 15 ticks (the loop above: 50 each, then 50, 50, 50, 50) and issue-to-tick times
    if (ring_median(&g_tick_ring) != 50 || g_med_tick != 50 || g_med_lat != 60) { printf("daygap: medians %lld %lld\n", (long long)g_med_tick, (long long)g_med_lat); bad++; }
    {
        ring_t r; memset(&r, 0, sizeof r);
        if (ring_median(&r) != 0) bad++;
        for (int i = 1; i <= 15; i++) ring_add(&r, i * 10);
        if (ring_median(&r) != 80) bad++;
        ring_add(&r, 5000); ring_add(&r, 7000);                   // two monthly ticks push out 10 and 20: 30..150, 5000, 7000
        if (ring_median(&r) != 100 || r.n != 15) bad++;
        // the wait after a tick's end: pct % of a usual tick, less the usual time from the issue to the tick, 0..cap
        if (pace_wait(50, 20, 100, 200) != 30 || pace_wait(50, 20, 140, 200) != 50 || pace_wait(50, 20, 0, 200) != 0 || pace_wait(0, 20, 100, 200) != 0 ||
            pace_wait(300, 20, 100, 200) != 180 || pace_wait(10, 20, 100, 200) != 0) { puts("daygap: pace_wait"); bad++; }
    }
    // a held call counts as a call that did not issue a day
    on_tick(1, 3000); on_timer(4, R_HELD, 3010, 0.01f); on_timer(4, R_ISSUED, 3020, 0.01f); on_tick(0, 3040);
    if (g.days != 5 || g.why_before[R_HELD] != 1) { puts("daygap: held call"); bad++; }
    g.days = 4; g.why_before[R_HELD] = 0; g.before_calls -= 1; g.before_t -= 20; g.after_t -= 20; g.ticks -= 1; g.tick_t -= 99; g.calls[4][R_HELD] = 0; g.calls[4][R_ISSUED] -= 1;
    dg_t sum; memset(&sum, 0, sizeof sum); add(&sum, &g); add(&sum, &g);
    if (sum.days != 8 || sum.after_t != 460 || sum.after_max != g.after_max || sum.calls[4][R_ISSUED] != 14 || sum.held_t != 4102) { puts("daygap: add"); bad++; }
    printf("day gap checks: %d wrong answers\n", bad);
    memset(&g, 0, sizeof g); memset(&c, 0, sizeof c); g_in_tick = 0; g_t_tick0 = g_t_tick1 = g_t_issue = g_t_last = 0;
    memset(&g_tick_ring, 0, sizeof g_tick_ring); memset(&g_lat_ring, 0, sizeof g_lat_ring); g_med_tick = g_med_lat = 0;
    return bad;
}
