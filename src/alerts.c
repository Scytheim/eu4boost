// The alert icons at the top of the screen: fewer re-evaluations per frame ([render] alert_refresh_ms,
// experiment 2026-11-10).
//
// Why: the in-game idler calls FUN_140df5860 every frame (from 0x1408153c9). It walks the 114 alert kinds
// and evaluates, every frame, the one whose turn it is (this+0x4af8, +1 per frame) and EVERY alert that is
// shown at the moment (its item at this+0x28 + kind*0xa8 has a window at +0x20):
//     0x140df5ec8  cmp ebx, [r12+0x4af8]      ; this kind's turn?
//     0x140df5ed0  je  evaluate
//     0x140df5ed2  imul rax, r15, 0xa8
//     0x140df5ed9  cmp qword [rax+rdi+0x20], 0 ; shown?
//     0x140df5edf  jne evaluate               ; (0x140df5ef2: state 3 = remove, else the 114-way switch)
//     0x140df5ee1  ... nothing for this kind in this frame, next kind
// The evaluations are the game's full checks ("a building can be built" FUN_14032acb0 builds the reason
// texts too): 27% of the idler time outside the day ticks in a late game (1.2-1.5 ms a frame, more than the
// rendering; profile 2026-11-10, SPAIN 1704).
// What: a shown alert is evaluated again only when alert_refresh_ms have passed since its last evaluation
// (each kind on its own clock, spread over the interval so that they do not all fall into one frame). The
// kind whose turn it is, an alert in state 3 (to be removed: the player dismissed it) and everything else
// in the function (the icons' animation) run every frame as before. In the frames between, a shown alert
// takes the game's own "nothing for this kind in this frame" path, so its window just stays.
// Only what the player sees can change: an alert disappears or changes its text up to alert_refresh_ms later.
// How: MinHook at 0x140df5ed2 (the imul is moved to the trampoline; no jump targets inside it; all volatile
// registers and the flags are dead there: the last instruction before the compare pair is a call's return).
// alert_stats=1 also times every call of the function (log line at exit).
#include "common.h"
#include <stdio.h>
#include <string.h>
#include "MinHook.h"

#define RVA_ALERTS      0xdf5860u     // FUN_140df5860(this): the alerts update of a frame
#define RVA_ALERT_CHECK 0xdf5ec8u     // cmp ebx, [r12+0x4af8] ... (the signature starts here)
#define RVA_ALERT_HOOK  0xdf5ed2u     // imul rax, r15, 0xa8
#define RVA_ALERT_SKIP  0xdf5ee1u     // nothing for this kind in this frame
#define ALERT_KINDS 114
#define ITEM_SIZE 0xa8
#define ITEM_WINDOW 0x20
#define ITEM_STATE 0xa0

static const uint8_t sig_check[42] = {
    0x41,0x3b,0x9c,0x24,0xf8,0x4a,0x00,0x00,       // cmp ebx, [r12+0x4af8]
    0x74,0x20,                                     // je +0x20
    0x49,0x69,0xc7,0xa8,0x00,0x00,0x00,            // imul rax, r15, 0xa8
    0x48,0x83,0x7c,0x38,0x20,0x00,                 // cmp qword [rax+rdi+0x20], 0
    0x75,0x11,                                     // jne +0x11
    0x48,0x8d,0x8d,0x60,0x0c,0x00,0x00,            // lea rcx, [rbp+0xc60]
    0xe8,0x13,0xf9,0xff,0xff,                      // call FUN_140df5800
    0xe9,0x6e,0x0f,0x00,0x00 };                    // jmp 0x140df6e60
static const uint8_t sig_alerts[16] = { 0x48,0x89,0x4c,0x24,0x08,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41 };

void* eb_al_tramp;                    // MinHook trampoline: the imul, then on at 0x140df5ed9
void* eb_al_skip;                     // 0x140df5ee1
static double g_ms;                   // QueryPerformanceCounter ticks -> ms
static LONGLONG g_interval;           // alert_refresh_ms in ticks
static LONGLONG g_next[ALERT_KINDS];  // per kind: the tick count from which it is evaluated again (main thread only)
static LONG64 g_evaluated, g_skipped;
static void (*o_alerts)(void* self);
static LONG64 g_calls, g_call_ticks, g_call_max;

// 1 = evaluate this shown alert now, 0 = leave it for this frame. window = the item's +0x20, state = its +0xa0.
// A kind that was not evaluated yet (next == 0) starts somewhere inside the interval, by its number.
static int alert_due(uint32_t kind, const void* window, int state, LONGLONG now, LONGLONG interval, LONGLONG* next) {
    if (!window || state == 3 || kind >= ALERT_KINDS || interval <= 0) return 1;       // the game's own way
    if (!next[kind]) { next[kind] = now + (LONGLONG)(kind * 37u % 100u) * interval / 100 + 1; return 1; }
    if (now < next[kind]) return 0;
    next[kind] = now + interval;
    return 1;
}
// called from the stub at 0x140df5ed2: ebx, rdi (this + 0x28), r15 (the kind again, as the game indexes the items)
int eb_alert_due(uint32_t kind, const uint8_t* items, uint64_t index) {
    const uint8_t* it = items + index * ITEM_SIZE;
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    int due = alert_due(kind, *(void* const*)(it + ITEM_WINDOW), *(const int*)(it + ITEM_STATE), t.QuadPart, g_interval, g_next);
    if (*(void* const*)(it + ITEM_WINDOW)) { if (due) g_evaluated++; else g_skipped++; }
    return due;
}
void eb_al_stub(void);
__asm__(".text\n"
        ".globl eb_al_stub\n"
        "eb_al_stub:\n"
        "    subq $0x20, %rsp\n"                // rsp is 16-aligned here (the function's body, between two calls)
        "    movl %ebx, %ecx\n"
        "    movq %rdi, %rdx\n"
        "    movq %r15, %r8\n"
        "    callq eb_alert_due\n"              // rbx, rdi, rbp, r12-r15, xmm6-15 survive; the rest is dead here
        "    addq $0x20, %rsp\n"
        "    testl %eax, %eax\n"
        "    jz 1f\n"
        "    jmpq *eb_al_tramp(%rip)\n"         // due, or not shown: the game's own check goes on
        "1:  jmpq *eb_al_skip(%rip)\n");        // not due: nothing for this kind in this frame

static void hk_alerts(void* self) {
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    o_alerts(self);
    QueryPerformanceCounter(&b);
    LONG64 d = b.QuadPart - a.QuadPart;
    g_calls++; g_call_ticks += d;
    if (d > g_call_max) g_call_max = d;
}

int eb_alerts_install(void) {
    int ms = g_cfg.render_alert_refresh_ms, stats = g_cfg.render_alert_stats;
    if (ms < 0) ms = g_cfg.render_alert_refresh_ms = 0;
    if (ms > 2000) ms = g_cfg.render_alert_refresh_ms = 2000;
    if (!ms && !stats) return 0;
#ifdef EB_TEST
    return 0;
#else
    uint8_t* b = (uint8_t*)g_exe;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    g_ms = 1000.0 / (double)f.QuadPart; g_interval = f.QuadPart * ms / 1000;
    if (nt->FileHeader.TimeDateStamp != 0x66FE6AB9u || memcmp(b + RVA_ALERT_CHECK, sig_check, sizeof sig_check) ||
        memcmp(b + RVA_ALERTS, sig_alerts, sizeof sig_alerts)) {
        eb_log("alerts: signature mismatch - the alerts are evaluated as the game does it");
        g_cfg.render_alert_refresh_ms = 0;
        return 0;
    }
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) { g_cfg.render_alert_refresh_ms = 0; return 0; }
    if (stats) {
        eb_hookset hs; eb_hs_init(&hs, "alert statistics");
        if (!eb_hs_add(&hs, b + RVA_ALERTS, (void*)hk_alerts, (void**)&o_alerts) || !eb_hs_enable(&hs)) {
            eb_log("alerts: hook failed - no statistics");
            o_alerts = NULL;
        }
    }
    if (!ms) { if (o_alerts) eb_log("alerts: the time of the alerts update is measured (nothing is changed)"); return 0; }
    eb_al_skip = b + RVA_ALERT_SKIP;
    eb_hookset hs; eb_hs_init(&hs, "alerts");
    if (!eb_hs_add(&hs, b + RVA_ALERT_HOOK, (void*)eb_al_stub, &eb_al_tramp) || !eb_hs_enable(&hs)) {
        eb_log("alerts: hook failed - the alerts are evaluated as the game does it");
        eb_al_tramp = NULL; g_cfg.render_alert_refresh_ms = 0;
        return 0;
    }
    eb_log("alerts: a shown alert is evaluated again every %d ms instead of every frame (EXPERIMENT; the screen only)", ms);
    return 1;
#endif
}
void eb_alerts_report(void) {
    if (eb_al_tramp)
        eb_log("alerts: shown alerts evaluated %lld times, left for a later frame %lld times", (long long)g_evaluated, (long long)g_skipped);
    if (o_alerts && g_calls)
        eb_log("alerts: the alerts update ran %lld times, %.1f us on average (%.1f s in all, longest %.1f ms)", (long long)g_calls,
               (double)g_call_ticks * g_ms * 1000.0 / (double)g_calls, (double)g_call_ticks * g_ms / 1000.0, (double)g_call_max * g_ms);
}

// ---- self-test (harness): when a shown alert is evaluated ----
__declspec(dllexport) int eb_test_alerts(void) {
    int bad = 0;
    static LONGLONG next[ALERT_KINDS];
    memset(next, 0, sizeof next);
    int w = 1;                                                    // any non-null "window"
    // not shown, to be removed, an unknown kind, the feature off: always the game's own way, and no clock is started
    if (!alert_due(5, NULL, 0, 1000, 100, next) || !alert_due(5, &w, 3, 1000, 100, next) || !alert_due(200, &w, 0, 1000, 100, next) ||
        !alert_due(5, &w, 0, 1000, 0, next) || next[5]) { puts("alerts: pass-through"); bad++; }
    // the first evaluation of a shown alert is at once; the next one within the interval, by the kind's number
    if (!alert_due(5, &w, 0, 1000, 100, next) || next[5] <= 1000 || next[5] > 1101) { printf("alerts: first %lld\n", (long long)next[5]); bad++; }
    LONGLONG first = next[5];
    if (alert_due(5, &w, 0, first - 1, 100, next) || !alert_due(5, &w, 0, first, 100, next) || next[5] != first + 100) { puts("alerts: interval"); bad++; }
    if (alert_due(5, &w, 0, first + 99, 100, next) || !alert_due(5, &w, 0, first + 250, 100, next) || next[5] != first + 350) { puts("alerts: later"); bad++; }
    // the kinds start at different points of the interval (not all in one frame)
    int same = 0;
    for (uint32_t k = 0; k < ALERT_KINDS; k++) { next[k] = 0; alert_due(k, &w, 0, 5000, 100, next); }
    for (uint32_t k = 1; k < ALERT_KINDS; k++) if (next[k] == next[0]) same++;
    if (same > 3) { printf("alerts: %d kinds share a start\n", same); bad++; }
    // over 1000 ticks with an interval of 100, a shown alert looked at every tick is evaluated 10 or 11 times
    memset(next, 0, sizeof next);
    int n = 0;
    for (LONGLONG t = 1; t <= 1000; t++) n += alert_due(7, &w, 0, t, 100, next);
    if (n < 10 || n > 11) { printf("alerts: %d evaluations in 1000 ticks\n", n); bad++; }
    printf("alert refresh checks: %d wrong answers\n", bad);
    return bad;
}
