// Country update: statistics ([country] stats) and an experimental parallel RefreshKnowledge
// ([country] parallel_refresh, off by default).
//
// The serial country loop FUN_14075b0d0 runs the country daily update FUN_1402ee280 for every
// country in turn (gamestate+0x1d08, count +0x1d14). Part of that update is a "dirty bundle": when
// country+0x2882 is set it runs RefreshKnowledge FUN_14093fe30, CalculateModifier FUN_140327dc0,
// UpdateCasusBellis FUN_1404426f0 and FUN_1403d00c0. RefreshKnowledge and UpdateCasusBellis are also
// run from event options. Modded 1543 GER save, 500 days at speed 5: the loop takes 13.1 ms a day
// (280 countries); 11.6 countries a day run the bundle, RefreshKnowledge 151 us a call on average.
//
// parallel_refresh (EXPERIMENT - changes the order in which the game sees results):
//   At the start of the loop, RefreshKnowledge runs for every dirty country at once on worker
//   threads (and the main thread). RefreshKnowledge resets and rebuilds the country's knowledge
//   caches from provinces and relations; it uses no random numbers. Then the dirty flag is set to 2:
//   still "dirty" for the game (it tests != 0), but anything that marks the country dirty again
//   writes 1. When the bundle reaches RefreshKnowledge and the flag is still 2 (and +0x2885 is clear),
//   the call is skipped because its work is done; otherwise the original runs.
//   Differences from the game: other countries see the refreshed caches earlier in the loop, and the
//   refresh reads the state of the start of the loop instead of the country's turn.
//   Made safe for threads as far as known: UpdateOpinionCache FUN_14038b340, which RefreshKnowledge
//   calls for another country when a relation bit changes (and which can start the game's own
//   parallel for), is recorded on the workers and replayed on the main thread afterwards, in country
//   order. The player's (and observed) country is not precomputed (it raises UI messages).
//   Modes: 1 = on, 2 = on + verify (the original runs again at the country's turn and the country's
//   bytes and relation array are compared with the precomputed ones), 3 = serial precompute on the
//   main thread + verify (separates "the state changed in between" from thread effects), 4 = race
//   check (right after the parallel phase, on the same state, the original runs again serially for
//   every precomputed country and is compared; also checks the fixup below against the pre-state).
//   Known cross-country read: for countries at war (relation +0x46 bit 2) RefreshKnowledge asks
//   FUN_140b4a470 whether the leader of the country's group (+0x1948, first tag of its vector at
//   +0x20; tag 0 when empty) has that country in its +0x1bf0 list - a list the leader's own
//   RefreshKnowledge clears and rebuilds. The race check (mode 4) saw it as +0x1fdc differences
//   (2 of 7,563). A country whose group leader is another dirty country is therefore left out of the
//   parallel phase and refreshed by the original at its turn.
//   refresh_fixup=1 (default): the first step of RefreshKnowledge, FUN_1402f1120(country, 1), stores
//   cached values (+0xe1c..+0xe34; +0xe34 = a clamped opinion/modifier sum). Its inputs change between
//   the loop start and the country's turn (verify: ~95% of the remaining differences), so it runs
//   again at the country's turn when the precomputed refresh is skipped.
#include "common.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <mimalloc.h>
#include "MinHook.h"

#define RVA_SERIAL  0x75b0d0u   // country serial loop (gamestate)
#define RVA_DAILY   0x2ee280u   // country daily update (country)
#define RVA_RK      0x93fe30u   // CCountry::RefreshKnowledge (country, const char* where)
#define RVA_CM      0x327dc0u   // CalculateModifier (country, const char* where, char)
#define RVA_UCB     0x4426f0u   // UpdateCasusBellis wrapper (casus belli object, const char* where)
#define RVA_OPCACHE 0x38b340u   // UpdateOpinionCache (country, relation key, char)
#define RVA_KCACHE  0x2f1120u   // FUN_1402f1120 (country, char): cached values +0xe1c..+0xe34, RefreshKnowledge's first step
#define RET_BUNDLE_RK 0x2f087bu // return address of the bundle's RefreshKnowledge call in FUN_1402ee280
#define DAILY_SIZE  11174u      // size of FUN_1402ee280 (return addresses inside it = called from the daily update)
static const uint8_t sig_serial[16]  = { 0x40,0x53,0x55,0x56,0x57,0x48,0x83,0xec,0x68,0x0f,0x29,0x74,0x24,0x50,0x48,0x8b };
static const uint8_t sig_daily[16]   = { 0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xac };
static const uint8_t sig_rk[16]      = { 0x48,0x89,0x5c,0x24,0x08,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57 };
static const uint8_t sig_cm[16]      = { 0x48,0x89,0x5c,0x24,0x10,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57 };
static const uint8_t sig_ucb[16]     = { 0x48,0x83,0xec,0x28,0x80,0x79,0x18,0x00,0x75,0x07,0x32,0xc0,0x48,0x83,0xc4,0x28 };
static const uint8_t sig_kcache[24]  = { 0x48,0x89,0x5c,0x24,0x08,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8b,0xec,0x48,0x83,0xec,0x60,0x0f };
static const uint8_t sig_opcache[16] = { 0x48,0x89,0x5c,0x24,0x08,0x44,0x88,0x44,0x24,0x18,0x48,0x89,0x54,0x24,0x10,0x57 };
#define C_TAG       0x20
#define C_PROVINCES 0x1344
#define C_REL       0x1510      // relation array (begin, end), 0x48 bytes per tag
#define C_KCACHE    0xe1c       // FUN_1402f1120's fields +0xe1c..+0xe37
#define C_KCACHE_N  0x1c
#define C_GROUP     0x1948      // group object; its vector at +0x20 holds tags, the first is the leader
#define C_DIRTY     0x2882
#define C_DIRTY2    0x2885
#define C_SIZE      0x9b50      // country object size at least (the save writer FUN_1402dc6b0 uses fields up to +0x9b48)
#define GS_COUNTRIES 0x1d08
#define GS_NCOUNTRIES 0x1d14
#define GS_SESSION  0x1e00

enum { F_SERIAL, F_DAILY, F_RK, F_CM, F_UCB, NF };
static const char* const fname[NF] = { "country loop", "country daily", "RefreshKnowledge", "CalculateModifier", "UpdateCasusBellis" };
#define NB 7
static const double bucket_us[NB - 1] = { 5, 20, 50, 200, 1000, 5000 };
typedef struct {
    LONG64 calls, in_daily, other_thread;   // in_daily: called from the country daily update
    double us, us_in_daily, max_us;
    LONG64 hist[NB];
} fstat_t;
static fstat_t g_st[NF];
static LONG64 g_rk_prov, g_dirty_seen, g_daily_dirty;
static double g_qpf_us;
static uint8_t* g_daily_lo;
static uint8_t* g_bundle_ret;

typedef void  (*v1_t)(void*);
typedef void  (*v2_t)(void*, const char*);
typedef void  (*cm_t)(void*, const char*, char);
typedef uint8_t (*ucb_t)(void*, void*);
typedef void  (*opc_t)(void*, uint64_t, char);
typedef void  (*kc_t)(void*, char);
static kc_t g_kcache;
static v1_t o_serial, o_daily;
static v2_t o_rk;
static cm_t o_cm;
static ucb_t o_ucb;
static opc_t o_opcache;

static inline LONGLONG now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

static void record(int f, LONGLONG t0, void* ret) {
    if (!g_cfg.country_stats) return;
    double us = (double)(now() - t0) * g_qpf_us;
    fstat_t* s = &g_st[f];
    if (GetCurrentThreadId() != g_main_tid) { InterlockedIncrement64(&s->other_thread); return; }
    s->calls++; s->us += us; if (us > s->max_us) s->max_us = us;
    int b = 0; while (b < NB - 1 && us >= bucket_us[b]) b++;
    s->hist[b]++;
    if (ret && (uint8_t*)ret >= g_daily_lo && (uint8_t*)ret < g_daily_lo + DAILY_SIZE) { s->in_daily++; s->us_in_daily += us; }
}

// ---- parallel RefreshKnowledge -------------------------------------------------------------------
#define MAXJOBS 4096
#define MAXWORKERS 15
typedef struct { void* country; uint64_t key; char flag; } opcall_t;
typedef struct { uint8_t* c; opcall_t* ops; int nops, capops; uint8_t pre[C_KCACHE_N]; } job_t;
static job_t g_jobs[MAXJOBS];
static volatile LONG g_njobs, g_next, g_done, g_pending;   // g_pending: woken workers not yet back to waiting
static HANDLE g_wake[MAXWORKERS];
static int g_nworkers;
static __thread int t_job = -1;                 // job running on this thread (workers), -1 = none
static LONG64 g_pr_loops, g_pr_jobs, g_pr_skipped, g_pr_redirty, g_pr_leftover, g_pr_deferred, g_pr_ver, g_pr_ver_bad;
static double g_pr_us;                          // main-thread time of the parallel phase
static LONG64 g_pr_dep;                         // left out: the group leader is refreshed in the same phase
static uint8_t g_tagbits[65536 / 8];
static LONG64 g_fix_calls, g_fix_changed, g_kc_checked, g_kc_post_diff;
static double g_fix_us;
static const char g_where[] = "eu4boost parallel_refresh";

static void hk_opcache(void* c, uint64_t key, char flag) {
    int j = t_job;
    if (j < 0) { o_opcache(c, key, flag); return; }
    job_t* jb = &g_jobs[j];                     // worker: record, the main thread replays it later
    if (jb->nops == jb->capops) {
        int n = jb->capops ? jb->capops * 2 : 8;
        opcall_t* p = (opcall_t*)realloc(jb->ops, (size_t)n * sizeof *p);
        if (!p) { o_opcache(c, key, flag); return; }   // out of memory: run it here (best effort)
        jb->ops = p; jb->capops = n;
    }
    jb->ops[jb->nops].country = c; jb->ops[jb->nops].key = key; jb->ops[jb->nops].flag = flag; jb->nops++;
}

static void run_jobs(int deferred) {
    for (;;) {
        LONG i = InterlockedIncrement(&g_next) - 1;
        if (i >= g_njobs) break;
        if (deferred) t_job = (int)i;
        o_rk(g_jobs[i].c, g_where);
        t_job = -1;
        InterlockedIncrement(&g_done);
    }
}

static DWORD WINAPI worker(LPVOID arg) {
    HANDLE ev = (HANDLE)arg;
    for (;;) {
        WaitForSingleObject(ev, INFINITE);
        run_jobs(1);
        InterlockedDecrement(&g_pending);
    }
    return 0;
}

static void verify_rk(uint8_t* c, const char* w);

static void precompute(uint8_t* gs) {
    void** session = *(void***)(gs + GS_SESSION);
    if (!session || session[0x6c]) return;                     // multiplayer: never
    uint8_t** list = *(uint8_t***)(gs + GS_COUNTRIES);
    int n = *(int*)(gs + GS_NCOUNTRIES);
    // the player's tag (gs+0x1e60) and, while observing a country (gs+0x1e66 == 7, +0x1e6f set), that
    // tag (gs+0x1e68), as FUN_1402fc910 picks them; their refresh can raise UI messages
    short player = (short)(*(uint64_t*)(gs + 0x1e60) >> 32), shown = player;
    if (gs[0x1e66] == 7 && gs[0x1e6f]) shown = (short)(*(uint64_t*)(gs + 0x1e68) >> 32);
    LONG m = 0;
    for (int i = 0; i < n && m < MAXJOBS; i++) {
        uint8_t* c = list[i];
        if (!c[C_DIRTY]) continue;
        short tag = (short)(*(uint64_t*)(c + C_TAG) >> 32);
        if (tag == player || tag == shown) continue;
        g_jobs[m].c = c; g_jobs[m].nops = 0; m++;
    }
    if (m < 2) return;
    for (LONG i = 0; i < m; i++) { uint16_t tg = (uint16_t)(*(uint64_t*)(g_jobs[i].c + C_TAG) >> 32); g_tagbits[tg >> 3] |= (uint8_t)(1 << (tg & 7)); }
    LONG k = 0;
    for (LONG i = 0; i < m; i++) {
        uint8_t* c = g_jobs[i].c;
        uint16_t tg = (uint16_t)(*(uint64_t*)(c + C_TAG) >> 32), lead = 0;
        uint8_t* grp = *(uint8_t**)(c + C_GROUP);
        if (grp) {
            uint64_t* vb = *(uint64_t**)(grp + 0x20); uint64_t* ve = *(uint64_t**)(grp + 0x28);
            if (vb && ve > vb) lead = (uint16_t)(vb[0] >> 32);
        }
        if (lead != tg && (g_tagbits[lead >> 3] & (1 << (lead & 7)))) { g_pr_dep++; continue; }
        if (k != i) { job_t tmp = g_jobs[k]; g_jobs[k] = g_jobs[i]; g_jobs[i] = tmp; }   // swap: each slot keeps its own ops buffer
        k++;
    }
    memset(g_tagbits, 0, sizeof g_tagbits);
    m = k;
    if (m < 2) return;
    int mode = g_cfg.country_parallel_refresh;
    if (mode == 4)                                              // the fixup's values on the state before the refresh
        for (LONG i = 0; i < m; i++) { g_kcache(g_jobs[i].c, 1); memcpy(g_jobs[i].pre, g_jobs[i].c + C_KCACHE, C_KCACHE_N); }
    LONGLONG t0 = now();
    while (g_pending) YieldProcessor();                         // every worker of the last phase is back
    g_next = 0; g_done = 0;
    MemoryBarrier();
    g_njobs = m;
    if (mode == 3) run_jobs(0);       // serial precompute (verify baseline)
    else {
        int wake = g_nworkers < m - 1 ? g_nworkers : m - 1;
        InterlockedExchange(&g_pending, wake);
        for (int w = 0; w < wake; w++) SetEvent(g_wake[w]);
        run_jobs(1);
        while (g_done < m) YieldProcessor();
    }
    // replay UpdateOpinionCache calls in country order, as the serial loop would have made them
    for (LONG i = 0; i < m; i++)
        for (int k = 0; k < g_jobs[i].nops; k++) {
            opcall_t* o = &g_jobs[i].ops[k];
            o_opcache(o->country, o->key, o->flag);
            g_pr_deferred++;
        }
    g_pr_us += (double)(now() - t0) * g_qpf_us;
    if (mode == 4)
        for (LONG i = 0; i < m; i++) {
            uint8_t* c = g_jobs[i].c;
            g_kcache(c, 1);                                     // the fixup on the refreshed state = before?
            g_kc_checked++;
            if (memcmp(c + C_KCACHE, g_jobs[i].pre, C_KCACHE_N) != 0) g_kc_post_diff++;
            verify_rk(c, g_where);                              // serial redo on the same state: differences = races
        }
    for (LONG i = 0; i < m; i++) {
        uint8_t* c = g_jobs[i].c;
        if (!c[C_DIRTY] && !c[C_DIRTY2]) c[C_DIRTY] = 2;       // done; still "dirty" so the bundle runs
        else g_pr_redirty++;                                    // marked again meanwhile: original at its turn
    }
    g_pr_loops++; g_pr_jobs += m;
}

static void finish_loop(void) {
    for (LONG i = 0; i < g_njobs; i++)
        if (g_jobs[i].c[C_DIRTY] == 2) { g_jobs[i].c[C_DIRTY] = 1; g_pr_leftover++; }   // the bundle did not run
    g_njobs = 0;
}

// verify: run the original again and compare the country's bytes and relation array
static int g_ver_logged;
static LONG64 g_off_hist[C_SIZE / 4];          // mismatching 4-byte words of the country object
static LONG64 g_rel_hist[0x48];                 // mismatching bytes by field of a relation entry
static void verify_rk(uint8_t* c, const char* w) {
    size_t sz = C_SIZE;                                          // (mi_usable_size is wrong here: the country is not at the start of its block)
    uint8_t* rb = *(uint8_t**)(c + C_REL); uint8_t* re = *(uint8_t**)(c + C_REL + 8);
    size_t rsz = (rb && re > rb) ? (size_t)(re - rb) : 0;
    uint8_t* snap = sz ? (uint8_t*)malloc(sz) : NULL;
    uint8_t* rsnap = rsz ? (uint8_t*)malloc(rsz) : NULL;
    if (snap) { memcpy(snap, c, sz); snap[C_DIRTY] = 0; }       // the original clears the flag
    if (rsnap) memcpy(rsnap, rb, rsz);
    o_rk(c, w);
    g_pr_ver++;
    int bad = 0, first = -1, nd = 0, rfirst = -1, rnd = 0;
    // 8-byte fields whose old and new values are both heap pointers are skipped: lists rebuilt by
    // RefreshKnowledge get new nodes (only their head/tail pointers are seen here, not their contents)
    if (snap) for (size_t i = 0; i + 8 <= sz; i += 8) {
        uint64_t a = *(uint64_t*)(snap + i), v = *(uint64_t*)(c + i);
        if (a == v) continue;
        if (mi_is_in_heap_region((void*)(uintptr_t)a) && mi_is_in_heap_region((void*)(uintptr_t)v)) { memcpy(snap + i, c + i, 8); continue; }
        for (int k = 0; k < 8; k++) if (snap[i + k] != c[i + k]) { if (first < 0) first = (int)(i + k); nd++; }
        if (*(uint32_t*)(snap + i) != *(uint32_t*)(c + i)) g_off_hist[i / 4]++;
        if (*(uint32_t*)(snap + i + 4) != *(uint32_t*)(c + i + 4)) g_off_hist[i / 4 + 1]++;
    }
    uint8_t* rb2 = *(uint8_t**)(c + C_REL);
    if (rsnap && rb2 == rb) for (size_t i = 0; i < rsz; i++) if (rsnap[i] != rb[i]) { if (rfirst < 0) rfirst = (int)i; rnd++; g_rel_hist[i % 0x48]++; }
    bad = nd || rnd || rb2 != rb;
    if (bad) {
        g_pr_ver_bad++;
        if (g_ver_logged++ < 40) {
            char offs[256]; int len = 0; offs[0] = 0;
            if (snap) for (size_t i = 0; i < sz && len < 200; i++)
                if (snap[i] != c[i]) { len += snprintf(offs + len, sizeof offs - len, "%zx ", i); while (i + 1 < sz && snap[i + 1] != c[i + 1]) i++; }
            eb_log("country: VERIFY MISMATCH %.3s: %d object bytes differ (first +0x%x; runs at %s), %d relation bytes differ (first +0x%x = tag %d field +0x%x)%s",
                   (const char*)(c + C_TAG), nd, first, offs, rnd, rfirst, rfirst >= 0 ? rfirst / 0x48 : -1, rfirst >= 0 ? rfirst % 0x48 : 0,
                   rb2 != rb ? ", relation array moved" : "");
        }
    }
    free(snap); free(rsnap);
}

// ---- hooks ------------------------------------------------------------------------------------------
static void hk_serial(void* gs) {
    LONGLONG t0 = g_cfg.country_stats ? now() : 0;
    if (g_cfg.country_parallel_refresh) precompute((uint8_t*)gs);
    o_serial(gs);
    if (g_cfg.country_parallel_refresh) finish_loop();
    record(F_SERIAL, t0, NULL);
}
static void hk_daily(void* c) {
    if (g_cfg.country_stats && *((uint8_t*)c + C_DIRTY)) g_daily_dirty++;
    LONGLONG t0 = now(); o_daily(c); record(F_DAILY, t0, NULL);
}
static void hk_rk(void* cv, const char* w) {
    uint8_t* c = (uint8_t*)cv;
    void* ret = __builtin_return_address(0);
    if (c[C_DIRTY] == 2 && (uint8_t*)ret == g_bundle_ret && GetCurrentThreadId() == g_main_tid) {
        if (!c[C_DIRTY2]) {
            if (g_cfg.country_refresh_fixup) {                  // the cached values as of now, as the original would compute them
                LONGLONG t1 = now();
                uint8_t old[C_KCACHE_N]; memcpy(old, c + C_KCACHE, C_KCACHE_N);
                g_kcache(c, 1);
                g_fix_calls++; if (memcmp(old, c + C_KCACHE, C_KCACHE_N) != 0) g_fix_changed++;
                g_fix_us += (double)(now() - t1) * g_qpf_us;
            }
            if (g_cfg.country_parallel_refresh != 2 && g_cfg.country_parallel_refresh != 3) { c[C_DIRTY] = 0; g_pr_skipped++; return; }
            LONGLONG t0 = now(); verify_rk(c, w); record(F_RK, t0, ret); return;
        }
        c[C_DIRTY] = 1;                                          // +0x2885 set meanwhile: original
    }
    if (g_cfg.country_stats && GetCurrentThreadId() == g_main_tid) { g_rk_prov += *(int*)(c + C_PROVINCES); if (c[C_DIRTY]) g_dirty_seen++; }
    if (!g_cfg.country_stats) { o_rk(c, w); return; }
    LONGLONG t0 = now(); o_rk(c, w); record(F_RK, t0, ret);
}
static void hk_cm(void* c, const char* w, char x) {
    void* ret = __builtin_return_address(0);
    LONGLONG t0 = now(); o_cm(c, w, x); record(F_CM, t0, ret);
}
static uint8_t hk_ucb(void* o, void* w) {
    void* ret = __builtin_return_address(0);
    LONGLONG t0 = now(); uint8_t r = o_ucb(o, w); record(F_UCB, t0, ret); return r;
}

int eb_country_install(void) {
    if (!g_cfg.country_stats && !g_cfg.country_parallel_refresh) return 0;
#ifdef EB_TEST
    eb_log("country: hooks are not available in the test build");
    return 0;
#else
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpf_us = 1e6 / (double)f.QuadPart;
    uint8_t* b = (uint8_t*)g_exe;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    struct { uint32_t rva; const uint8_t* sig; void* hook; void** orig; const char* name; } t[] = {
        { RVA_SERIAL,  sig_serial,  (void*)hk_serial,  (void**)&o_serial,  "country loop" },
        { RVA_DAILY,   sig_daily,   (void*)hk_daily,   (void**)&o_daily,   "country daily" },
        { RVA_RK,      sig_rk,      (void*)hk_rk,      (void**)&o_rk,      "RefreshKnowledge" },
        { RVA_CM,      sig_cm,      (void*)hk_cm,      (void**)&o_cm,      "CalculateModifier" },
        { RVA_UCB,     sig_ucb,     (void*)hk_ucb,     (void**)&o_ucb,     "UpdateCasusBellis" },
        { RVA_OPCACHE, sig_opcache, (void*)hk_opcache, (void**)&o_opcache, "UpdateOpinionCache" },
    };
    int nt_ = (int)(sizeof t / sizeof t[0]);
    if (nt->FileHeader.TimeDateStamp != 0x66FE6AB9u) { eb_log("country: unexpected eu4.exe - off"); return 0; }
    for (int i = 0; i < nt_; i++)
        if (memcmp(b + t[i].rva, t[i].sig, 16) != 0) { eb_log("country: signature mismatch at %s - off", t[i].name); return 0; }
    // the bundle's call: E8 rel32 to RefreshKnowledge right before the return address
    if (memcmp(b + RVA_KCACHE, sig_kcache, sizeof sig_kcache) != 0) { eb_log("country: signature mismatch at FUN_1402f1120 - off"); return 0; }
    g_kcache = (kc_t)(b + RVA_KCACHE);
    uint8_t* call = b + RET_BUNDLE_RK - 5;
    if (call[0] != 0xe8 || call + 5 + *(int32_t*)(call + 1) != b + RVA_RK) { eb_log("country: bundle call site mismatch - off"); return 0; }
    g_bundle_ret = b + RET_BUNDLE_RK;
    g_daily_lo = b + RVA_DAILY;
    if (g_cfg.country_parallel_refresh) {
        SYSTEM_INFO si; GetSystemInfo(&si);
        int nw = (int)si.dwNumberOfProcessors - 1;
        if (nw > MAXWORKERS) nw = MAXWORKERS;
        for (int i = 0; i < nw; i++) {
            g_wake[i] = CreateEventW(NULL, FALSE, FALSE, NULL);
            HANDLE h = g_wake[i] ? CreateThread(NULL, 256 << 10, worker, g_wake[i], 0, NULL) : NULL;
            if (!h) break;
            CloseHandle(h); g_nworkers++;
        }
    }
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) { eb_log("country: MinHook init failed"); return 0; }
    for (int i = 0; i < nt_; i++) {
        if (t[i].rva == RVA_OPCACHE && !g_cfg.country_parallel_refresh) continue;
        // timing-only hooks: not needed to play
        if ((t[i].rva == RVA_DAILY || t[i].rva == RVA_CM || t[i].rva == RVA_UCB) && !g_cfg.country_stats) continue;
        if (MH_CreateHook(b + t[i].rva, t[i].hook, t[i].orig) != MH_OK || MH_EnableHook(b + t[i].rva) != MH_OK) {
            eb_log("country: hook %s failed", t[i].name);
            return 0;
        }
    }
    if (g_cfg.country_stats) eb_log("country: statistics hooks on (country loop, daily update, RefreshKnowledge, CalculateModifier, UpdateCasusBellis)");
    if (g_cfg.country_parallel_refresh)
        eb_log("country: EXPERIMENT parallel RefreshKnowledge mode %d (%s), fixup %d, %d worker threads",
               g_cfg.country_parallel_refresh,
               g_cfg.country_parallel_refresh == 1 ? "on" : g_cfg.country_parallel_refresh == 2 ? "on + verify" :
               g_cfg.country_parallel_refresh == 3 ? "serial precompute + verify" : "on + race check",
               g_cfg.country_refresh_fixup, g_nworkers);
    return 1;
#endif
}

void eb_country_report(void) {
    if (!o_serial) return;
    if (g_cfg.country_parallel_refresh)
        eb_log("country: parallel refresh: %lld loops, %lld precomputed (%.1f per loop, %.0f us per loop on the main thread), %lld skipped at the bundle, %lld marked again, %lld not reached, %lld opinion-cache calls replayed, %lld left out (group leader also dirty) | verify %lld, mismatches %lld",
               (long long)g_pr_loops, (long long)g_pr_jobs, g_pr_loops ? (double)g_pr_jobs / (double)g_pr_loops : 0.0,
               g_pr_loops ? g_pr_us / (double)g_pr_loops : 0.0, (long long)g_pr_skipped, (long long)g_pr_redirty,
               (long long)g_pr_leftover, (long long)g_pr_deferred, (long long)g_pr_dep, (long long)g_pr_ver, (long long)g_pr_ver_bad);
    if (g_cfg.country_parallel_refresh && g_cfg.country_refresh_fixup)
        eb_log("country: fixup: %lld calls (%.1f us each), values changed %lld", (long long)g_fix_calls,
               g_fix_calls ? g_fix_us / (double)g_fix_calls : 0.0, (long long)g_fix_changed);
    if (g_cfg.country_parallel_refresh == 4)
        eb_log("country: race check: FUN_1402f1120 after the refresh differs from before it in %lld of %lld countries",
               (long long)g_kc_post_diff, (long long)g_kc_checked);
    if (g_pr_ver_bad) {
        char buf[600]; int len = 0; buf[0] = 0;
        for (int top = 0; top < 12; top++) {                    // the most frequent differing words
            int best = -1;
            for (int i = 0; i < C_SIZE / 4; i++)
                if (g_off_hist[i] > 0 && (best < 0 || g_off_hist[i] > g_off_hist[best])) {
                    best = i;
                }
            if (best < 0) break;
            len += snprintf(buf + len, sizeof buf - len, "+0x%x:%lld ", best * 4, (long long)g_off_hist[best]);
            g_off_hist[best] = -g_off_hist[best];               // mark as printed
        }
        for (int i = 0; i < C_SIZE / 4; i++) if (g_off_hist[i] < 0) g_off_hist[i] = -g_off_hist[i];
        char rbuf[400]; int rl = 0; rbuf[0] = 0;
        for (int i = 0; i < 0x48; i++) if (g_rel_hist[i]) rl += snprintf(rbuf + rl, sizeof rbuf - rl, "+0x%x:%lld ", i, (long long)g_rel_hist[i]);
        eb_log("country: verify differences by object field: %s| by relation field: %s", buf, rl ? rbuf : "none");
    }
    if (!g_cfg.country_stats) return;
    double loops = (double)(g_st[F_SERIAL].calls ? g_st[F_SERIAL].calls : 1);
    eb_log("country: %lld loops, %lld country updates (%lld with the dirty flag set), RefreshKnowledge avg %.0f provinces, dirty at entry %lld",
           (long long)g_st[F_SERIAL].calls, (long long)g_st[F_DAILY].calls, (long long)g_daily_dirty,
           g_st[F_RK].calls ? (double)g_rk_prov / (double)g_st[F_RK].calls : 0.0, (long long)g_dirty_seen);
    for (int i = 0; i < NF; i++) {
        fstat_t* s = &g_st[i];
        eb_log("country:   %-18s %8lld calls %7.2f ms/loop  avg %7.1f us  max %8.1f us | from daily update %lld calls %.2f ms/loop | other threads %lld | <5 %lld <20 %lld <50 %lld <200 %lld <1000 %lld <5000 %lld >=5000 %lld us",
               fname[i], (long long)s->calls, s->us / 1000.0 / loops, s->calls ? s->us / (double)s->calls : 0.0, s->max_us,
               (long long)s->in_daily, s->us_in_daily / 1000.0 / loops, (long long)s->other_thread,
               (long long)s->hist[0], (long long)s->hist[1], (long long)s->hist[2], (long long)s->hist[3],
               (long long)s->hist[4], (long long)s->hist[5], (long long)s->hist[6]);
    }
}
