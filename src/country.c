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
//   UI messages: every message of the game goes through the handler FUN_1408c07b0(handler, message by
//   value, const std::function&), which adds it to the message log (FUN_141241af0: more than 100 entries
//   -> the oldest is deleted). Other countries' refreshes raise messages too: FUN_140a675b0 recomputes a
//   relation's truce bit (+0x45 bit 0) and, when a truce with the player ends, posts OURTRUCEENDS. Two
//   workers posting at once deleted the same log entry (report 24, 0.4.1: crash in FUN_141241ed0). So
//   on a worker the handler is not run: the message is moved out with the game's move constructor
//   FUN_1401af350 (0x90 bytes), the callback copied with its own _Copy, and both are recorded with the
//   job's UpdateOpinionCache calls; the main thread posts them in the same replay, in country order.
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
//
// parallel_annex (see "opinions when a country disappears" below): the opinions the game recomputes for
// every country when one disappears, at once on the worker threads.
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
#define RVA_MSG     0x8c07b0u   // message handler (handler, message by value - the callee destroys it, const std::function*)
#define RVA_MSGMOVE 0x1af350u   // message move constructor (dst, src): takes src's key list and text
#define RVA_STRFREE 0x095660u   // std::string tidy (the message text at +0x60)
#define RVA_LSTFREE 0x1af710u   // the message's key/value list (+0x10) free
#define RVA_WARREF  0xd8f790u   // (war): RefreshKnowledge of every attacker (war+0x48..0x50) and defender (+0x78..0x80) - joining or declaring a war
#define WAR_LEA1    0xd8f7cfu   // its lea rdx, "...war.cpp:1546" (attackers) and "...war.cpp:1551" (defenders)
#define WAR_LEA2    0xd8f827u
#define RVA_GSP     0x233fe78u  // gamestate pointer
#define RVA_TAGTAB  0x233d8d0u  // tag table object pointer
#define MSG_SIZE    0x90
#define FN_SIZE     0x40        // MSVC std::function: storage, impl pointer at +0x38 (vtable: [0] _Copy(where), [4] _Delete_this(bool))
#define DAILY_SIZE  11174u      // size of FUN_1402ee280 (return addresses inside it = called from the daily update)
static const uint8_t sig_serial[16]  = { 0x40,0x53,0x55,0x56,0x57,0x48,0x83,0xec,0x68,0x0f,0x29,0x74,0x24,0x50,0x48,0x8b };
static const uint8_t sig_daily[16]   = { 0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xac };
static const uint8_t sig_rk[16]      = { 0x48,0x89,0x5c,0x24,0x08,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57 };
static const uint8_t sig_cm[16]      = { 0x48,0x89,0x5c,0x24,0x10,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57 };
static const uint8_t sig_ucb[16]     = { 0x48,0x83,0xec,0x28,0x80,0x79,0x18,0x00,0x75,0x07,0x32,0xc0,0x48,0x83,0xc4,0x28 };
static const uint8_t sig_kcache[24]  = { 0x48,0x89,0x5c,0x24,0x08,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8b,0xec,0x48,0x83,0xec,0x60,0x0f };
static const uint8_t sig_opcache[16] = { 0x48,0x89,0x5c,0x24,0x08,0x44,0x88,0x44,0x24,0x18,0x48,0x89,0x54,0x24,0x10,0x57 };
static const uint8_t sig_warref[16] = { 0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x48 };
static const uint8_t sig_msg[16]     = { 0x48,0x89,0x5c,0x24,0x08,0x4c,0x89,0x44,0x24,0x18,0x48,0x89,0x54,0x24,0x10,0x55 };
static const uint8_t sig_msgmove[16] = { 0x45,0x33,0xc0,0x48,0x8d,0x05,0x86,0x33,0xa9,0x01,0x48,0x89,0x01,0x48,0x8b,0x42 };
static const uint8_t sig_strfree[16] = { 0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x51,0x18,0x48,0x8b,0xd9,0x48,0x83,0xfa };
static const uint8_t sig_lstfree[16] = { 0x48,0x89,0x5c,0x24,0x10,0x56,0x48,0x83,0xec,0x20,0x48,0x8b,0x19,0x48,0x8b,0xf1 };
#define C_TAG       0x20
#define C_PROVINCES 0x1344
#define C_REL       0x1510      // relation array (begin, end), 0x48 bytes per tag
#define C_KCACHE    0xe1c       // FUN_1402f1120's fields +0xe1c..+0xe37
#define C_KCACHE_N  0x1c
#define C_GROUP     0x1948      // group object; its vector at +0x20 holds tags, the first is the leader
#define C_OVERLORD  0x18f0      // the overlord's tag (top byte 0 = none)
#define C_DIRTY     0x2882
#define C_DIRTY2    0x2885
#define C_SIZE      0x9b50      // country object size at least (the save writer FUN_1402dc6b0 uses fields up to +0x9b48)
#define GS_COUNTRIES 0x1d08
#define GS_NCOUNTRIES 0x1d14
#define GS_SESSION  0x1e00
#define GS_DATE     0x1dd0      // today, in hours

enum { F_SERIAL, F_DAILY, F_RK, F_CM, F_UCB, F_WAR, NF };
static const char* const fname[NF] = { "country loop", "country daily", "RefreshKnowledge", "CalculateModifier", "UpdateCasusBellis", "war refresh" };
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
typedef void  (*msg_t)(void*, void*, void*);
typedef void* (*mmove_t)(void*, void*);
typedef void* (*fcopy_t)(void*, void*);
typedef void  (*fdel_t)(void*, char);
static kc_t g_kcache;
static msg_t o_msg;
static mmove_t g_msgmove;
static v1_t g_strfree, g_lstfree;
static v1_t o_serial, o_daily;
static v2_t o_rk;
static cm_t o_cm;
static ucb_t o_ucb;
static v1_t o_warref;
static uint8_t** g_gsp;
static uint8_t** g_tagtab;                     // DAT_14233d8d0: tag table object, +0x100 = tags, +0x118 = country by tag
static LONG64 g_war_parts;                     // stats: participants refreshed by the war refresh
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
// a recorded UpdateOpinionCache call, or (msg set) a message for the handler in .country
typedef struct { void* country; uint64_t key; char flag; uint8_t* msg; uint8_t* fn; } opcall_t;
typedef struct { uint8_t* c; const char* where; opcall_t* ops; int nops, capops; uint8_t pre[C_KCACHE_N]; } job_t;
static job_t g_jobs[MAXJOBS];                  // the loop start's refresh (finish_loop reads it until the loop ends)
static job_t g_wjobs[MAXJOBS];                 // a war refresh's participants
static volatile LONG g_njobs, g_next, g_done, g_pending;   // g_pending: woken workers not yet back to waiting
static job_t* volatile g_batch = g_jobs;       // the jobs the workers run now, and how many
static volatile LONG g_nbatch;
static int g_in_batch;
static HANDLE g_wake[MAXWORKERS];
static int g_nworkers;
static __thread int t_job = -1;                 // job running on this thread (workers), -1 = none
static LONG64 g_pr_loops, g_pr_jobs, g_pr_skipped, g_pr_redirty, g_pr_leftover, g_pr_deferred, g_pr_ver, g_pr_ver_bad;
static LONG64 g_pr_msgs, g_pr_msg_oom;          // messages posted from the replay; posted on a worker (out of memory)
static LONG64 g_pr_test;                        // test_truce_end: relations set to a truce that has just ended
static double g_pr_us;                          // main-thread time of the parallel phase
static LONG64 g_pr_dep;                         // left out: the group leader is refreshed in the same phase
static uint8_t g_tagbits[65536 / 8];
static LONG64 g_fix_calls, g_fix_changed, g_kc_checked, g_kc_post_diff;
static double g_fix_us;
static const char g_where[] = "eu4boost parallel_refresh";

static opcall_t* new_op(int j) {
    job_t* jb = &g_batch[j];
    if (jb->nops == jb->capops) {
        int n = jb->capops ? jb->capops * 2 : 8;
        opcall_t* p = (opcall_t*)realloc(jb->ops, (size_t)n * sizeof *p);
        if (!p) return NULL;
        jb->ops = p; jb->capops = n;
    }
    opcall_t* o = &jb->ops[jb->nops++];
    o->msg = NULL; o->fn = NULL;
    return o;
}

static int annex_call(void* c, uint64_t key, char flag, void* ret);
static void hk_opcache(void* c, uint64_t key, char flag) {
    int j = t_job;
    if (j < 0) {
        if (g_cfg.country_parallel_annex && annex_call(c, key, flag, __builtin_return_address(0))) return;
        o_opcache(c, key, flag);
        return;
    }
    opcall_t* o = new_op(j);                    // worker: record, the main thread replays it later
    if (!o) { o_opcache(c, key, flag); return; }   // out of memory: run it here (best effort)
    o->country = c; o->key = key; o->flag = flag;
}

static void hk_msg(void* h, void* msg, void* fn) {
    int j = t_job;
    if (j < 0 || g_cfg.country_test_no_defer) { o_msg(h, msg, fn); return; }
    // worker: keep the message for the main thread. The handler owns msg (by value): move it out with
    // the game's move constructor and free what is left, as the handler does when it drops a message.
    uint8_t* m = (uint8_t*)malloc(MSG_SIZE);
    uint8_t* f = (uint8_t*)malloc(FN_SIZE);
    opcall_t* o = (m && f) ? new_op(j) : NULL;
    if (!o) { free(m); free(f); InterlockedIncrement64(&g_pr_msg_oom); o_msg(h, msg, fn); return; }
    g_msgmove(m, msg);
    g_strfree((uint8_t*)msg + 0x60);
    g_lstfree((uint8_t*)msg + 0x10);
    void* impl = *(void**)((uint8_t*)fn + 0x38);   // the caller keeps (and destroys) its std::function
    *(void**)(f + 0x38) = impl ? ((fcopy_t)(*(void***)impl)[0])(impl, f) : NULL;
    o->country = h; o->msg = m; o->fn = f;
}

static void post_msg(opcall_t* o) {
    o_msg(o->country, o->msg, o->fn);           // the handler frees the message's contents
    void* impl = *(void**)(o->fn + 0x38);
    if (impl) ((fdel_t)(*(void***)impl)[4])(impl, impl != (void*)o->fn);
    free(o->msg); free(o->fn);
}

static void (*volatile g_run_one)(LONG i);      // set: the batch runs this for 0..g_nbatch instead of RefreshKnowledge
static void run_jobs(int deferred) {
    for (;;) {
        LONG i = InterlockedIncrement(&g_next) - 1;
        if (i >= g_nbatch) break;
        if (g_run_one) g_run_one(i);
        else {
            if (deferred) t_job = (int)i;
            o_rk(g_batch[i].c, g_batch[i].where);
            t_job = -1;
        }
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

static int verify_rk(uint8_t* c, const char* w, LONG64* ver, LONG64* bad);

// RefreshKnowledge of jobs[0..m) at once (workers and the main thread; serial = on the main thread in
// order, the verify baseline), then the recorded UpdateOpinionCache calls and messages in job order
// jobs 0..m at once: the workers and the main thread take the next index until none is left
static void run_all(LONG m, int serial) {
    while (g_pending) YieldProcessor();                         // every worker of the last batch is back
    g_next = 0; g_done = 0;
    g_nbatch = m;
    MemoryBarrier();
    if (serial) run_jobs(0);
    else {
        int wake = g_nworkers < m - 1 ? g_nworkers : m - 1;
        InterlockedExchange(&g_pending, wake);
        for (int w = 0; w < wake; w++) SetEvent(g_wake[w]);
        run_jobs(1);
        while (g_done < m) YieldProcessor();
    }
}
static void run_batch(job_t* jobs, LONG m, int serial) {
    g_in_batch = 1;
    g_batch = jobs; g_run_one = NULL;
    run_all(m, serial);
    for (LONG i = 0; i < m; i++)
        for (int k = 0; k < jobs[i].nops; k++) {
            opcall_t* o = &jobs[i].ops[k];
            if (o->msg) { post_msg(o); g_pr_msgs++; continue; }
            o_opcache(o->country, o->key, o->flag);
            g_pr_deferred++;
        }
    g_in_batch = 0;
}

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
        g_jobs[m].c = c; g_jobs[m].where = g_where; g_jobs[m].nops = 0; m++;
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
    if (g_cfg.country_test_truce_end > 0) {                     // testing only: their refresh posts truce-end messages
        int today = *(int*)(gs + GS_DATE);
        for (LONG i = 0, n = 0; i < m && n < g_cfg.country_test_truce_end; i++) {
            uint8_t* c = g_jobs[i].c;
            uint8_t* rb = *(uint8_t**)(c + C_REL); uint8_t* re = *(uint8_t**)(c + C_REL + 8);
            if (!*(int*)(c + C_PROVINCES) || shown < 0 || !rb || rb + ((size_t)shown + 1) * 0x48 > re) continue;
            uint8_t* r = rb + (size_t)shown * 0x48;
            r[0x45] |= 1;                                       // truce bit on, war ended 30 years ago
            *(int*)(r + 0x34) = today - 30 * 365 * 24;
            g_pr_test++; n++;
        }
    }
    int mode = g_cfg.country_parallel_refresh;
    if (mode == 4)                                              // the fixup's values on the state before the refresh
        for (LONG i = 0; i < m; i++) { g_kcache(g_jobs[i].c, 1); memcpy(g_jobs[i].pre, g_jobs[i].c + C_KCACHE, C_KCACHE_N); }
    LONGLONG t0 = now();
    g_njobs = m;
    run_batch(g_jobs, m, mode == 3);   // mode 3: serial precompute (verify baseline)
    g_pr_us += (double)(now() - t0) * g_qpf_us;
    if (mode == 4)
        for (LONG i = 0; i < m; i++) {
            uint8_t* c = g_jobs[i].c;
            g_kcache(c, 1);                                     // the fixup on the refreshed state = before?
            g_kc_checked++;
            if (memcmp(c + C_KCACHE, g_jobs[i].pre, C_KCACHE_N) != 0) g_kc_post_diff++;
            verify_rk(c, g_where, &g_pr_ver, &g_pr_ver_bad);    // serial redo on the same state: differences = races
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
static int verify_rk(uint8_t* c, const char* w, LONG64* ver, LONG64* badn) {
    size_t sz = C_SIZE;                                          // (mi_usable_size is wrong here: the country is not at the start of its block)
    uint8_t* rb = *(uint8_t**)(c + C_REL); uint8_t* re = *(uint8_t**)(c + C_REL + 8);
    size_t rsz = (rb && re > rb) ? (size_t)(re - rb) : 0;
    uint8_t* snap = sz ? (uint8_t*)malloc(sz) : NULL;
    uint8_t* rsnap = rsz ? (uint8_t*)malloc(rsz) : NULL;
    if (snap) { memcpy(snap, c, sz); snap[C_DIRTY] = 0; }       // the original clears the flag
    if (rsnap) memcpy(rsnap, rb, rsz);
    o_rk(c, w);
    (*ver)++;
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
        (*badn)++;
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
    return bad;
}

// ---- opinions when a country disappears ([country] parallel_annex) ----------------------------------
// A country that loses its last province (CProvince::Conquer -> SetOwner -> FUN_1403663b0) goes through
// FUN_140364f10, which walks every tag (tag table +0x100): it removes the opinion modifiers between the
// country and that tag both ways (FUN_14028c7a0) and calls UpdateOpinionCache for tag -> country and for
// country -> tag (return addresses 0x140365fe7 and 0x140365ff9). Each call computes one opinion -
// FUN_1403839b0(country, tag, NULL, 1) -> FUN_1403d32b0 - and stores it in the first country's cache
// (+0x2380, a short per tag). With many countries and opinion modifiers that is 2-8 s on the main
// thread (reports 26, 42, 59, 66, 68, 70: Anbennar, 짭짭죽).
// Here the loop's calls are recorded and, inside its last one, run at once on the worker threads and the
// main thread. The game runs FUN_1403839b0(.., NULL, 1) on its own job threads for many pairs at once
// (its full cache rebuild FUN_14038b1f0 -> FUN_140424380 with the lambdas FUN_140432de0/FUN_140432e70,
// and the AE sums of the AI). One opinion reads that pair's relation; what the loop changes between two
// calls are the modifiers of other pairs, so all of them computed after the loop are the values the
// game would store (mode 2 checks it: the calls run as in the game, and at the end they are computed
// again on all cores and compared; a difference keeps the game's value). A call whose country has no
// cache (the game would rebuild it through its own parallel for) runs on the main thread after the
// others. Anything else that reaches UpdateOpinionCache first, or the next country loop, runs the
// recorded calls before it. Multiplayer: as the game.
// Testing only: [country] test_annex=D - every D country loops, the 2 x N calls for one existing country
// are made one by one and then on all cores, the results compared, and the cache values put back.
#define RET_ANNEX1 0x365fe7u
#define RET_ANNEX2 0x365ff9u
static const uint8_t sig_annex[8] = { 0x48,0x8b,0xd3,0x4c,0x8b,0xad,0xc0,0x01 };   // after the first call: mov rdx,rbx; mov r13,[rbp+0x1c0]
typedef struct { uint8_t* c; uint64_t key; char flag; uint8_t main, has; int16_t v; } axcall_t;
static axcall_t *g_ax, *g_ax2;                          // recorded calls; the flush's order (all cores first)
static int g_ax_n, g_ax_cap, g_ax2_cap, g_ax_left;      // g_ax_left: the loop's second-site calls still to come
static uint8_t *g_ax_ret1, *g_ax_ret2;
static LONG64 g_ax_loops, g_ax_calls, g_ax_par, g_ax_main, g_ax_ver, g_ax_bad, g_ax_tests, g_ax_test_calls, g_ax_test_bad;
static double g_ax_us, g_ax_ser_us, g_ax_test_ser_us, g_ax_test_par_us;

static int16_t* opcache_at(uint8_t* c, uint64_t key) {   // the cache entry UpdateOpinionCache writes, or NULL
    int16_t* b = *(int16_t**)(c + 0x2380); int16_t* e = *(int16_t**)(c + 0x2388);
    uint32_t i = (uint32_t)(int)(int16_t)(key >> 32);
    return b && e - b >= 2 && i < (uint32_t)(e - b) ? b + i : NULL;
}
static void ax_one(LONG i) { axcall_t* a = &g_ax[i]; o_opcache(a->c, a->key, a->flag); }
static void ax_run(LONG m) {                             // the calls of g_ax[0..m) on all cores
    g_in_batch = 1;
    g_run_one = ax_one;
    run_all(m, 0);
    g_run_one = NULL;
    g_in_batch = 0;
}
// the recorded calls: those with a cache at once, the others after them on this thread in order
static void annex_flush(void) {
    int n = g_ax_n;
    g_ax_n = 0; g_ax_left = 0;
    if (!n) return;
    LONGLONG t0 = now();
    int verify = g_cfg.country_parallel_annex == 2;
    if (g_ax2_cap < n) {
        axcall_t* p = (axcall_t*)realloc(g_ax2, (size_t)n * sizeof *p);
        if (!p) { if (!verify) for (int i = 0; i < n; i++) ax_one(i); return; }   // out of memory: in order
        g_ax2 = p; g_ax2_cap = n;
    }
    // the calls with a cache first (ax_one takes indexes), then those for the main thread, each in order
    int k = 0, m = 0;
    for (int i = 0; i < n; i++) {
        g_ax[i].main = !opcache_at(g_ax[i].c, g_ax[i].key);
        if (!g_ax[i].main && (!verify || g_ax[i].has)) g_ax2[k++] = g_ax[i];
    }
    m = k;
    if (!verify) for (int i = 0; i < n; i++) if (g_ax[i].main) g_ax2[m++] = g_ax[i];
    axcall_t* sw = g_ax; g_ax = g_ax2; g_ax2 = sw;
    int cap = g_ax_cap; g_ax_cap = g_ax2_cap; g_ax2_cap = cap;
    if (k >= 2) ax_run(k);
    else for (int i = 0; i < k; i++) ax_one(i);
    if (verify) {
        for (int i = 0; i < k; i++) {
            int16_t* e = opcache_at(g_ax[i].c, g_ax[i].key);
            g_ax_ver++;
            if (e && *e != g_ax[i].v) {
                if (++g_ax_bad <= 10)
                    eb_log("country: VERIFY MISMATCH annexation opinion %.3s -> tag %d: the game stored %d, all cores %d",
                           (const char*)(g_ax[i].c + C_TAG), (int)(int16_t)(g_ax[i].key >> 32), (int)g_ax[i].v, (int)*e);
                *e = g_ax[i].v;                            // keep the game's value
            }
        }
    } else {
        for (int i = k; i < m; i++) ax_one(i);
        g_ax_par += k; g_ax_main += m - k;
    }
    g_ax_us += (double)(now() - t0) * g_qpf_us;
}
// UpdateOpinionCache from FUN_140364f10's loop: recorded (returns 1) or, verify, run and recorded
static int annex_call(void* cv, uint64_t key, char flag, void* ret) {
    if (ret != g_ax_ret1 && ret != g_ax_ret2) {
        if (g_ax_n && GetCurrentThreadId() == g_main_tid) annex_flush();   // something else first: the loop's calls go before it
        return 0;
    }
    uint8_t* gs = *g_gsp;
    void** session = gs ? *(void***)(gs + GS_SESSION) : NULL;
    if (GetCurrentThreadId() != g_main_tid || g_in_batch || !session || session[0x6c]) return 0;
    if (!g_ax_n && !g_ax_left) { g_ax_left = *(int*)(*g_tagtab + 0x100); g_ax_loops++; }
    if (g_ax_n == g_ax_cap) {
        int m = g_ax_cap ? g_ax_cap * 2 : 4096;
        axcall_t* p = (axcall_t*)realloc(g_ax, (size_t)m * sizeof *p);
        if (!p) { annex_flush(); return 0; }
        g_ax = p; g_ax_cap = m;
    }
    uint8_t* c = (uint8_t*)cv;
    axcall_t* a = &g_ax[g_ax_n++];
    a->c = c; a->key = key; a->flag = flag; a->main = 0; a->has = 0; a->v = 0;
    g_ax_calls++;
    if (g_cfg.country_parallel_annex == 2) {               // verify: as the game, and what it stored
        LONGLONG t0 = now();
        o_opcache(c, key, flag);
        g_ax_ser_us += (double)(now() - t0) * g_qpf_us;
        int16_t* e = opcache_at(c, key);
        if (e) { a->v = *e; a->has = 1; }
    }
    if (ret == g_ax_ret2 && --g_ax_left <= 0) annex_flush();
    return 1;
}
// test_annex: the loop's calls for one existing country, serial and on all cores, compared; the cache put back
static void annex_test(uint8_t* gs) {
    static int loops;
    if (++loops % g_cfg.country_test_annex || g_ax_n) return;
    uint8_t** list = *(uint8_t***)(gs + GS_COUNTRIES);
    int nl = *(int*)(gs + GS_NCOUNTRIES);
    uint8_t** tab = *(uint8_t***)(*g_tagtab + 0x118);
    int ntag = *(int*)(*g_tagtab + 0x100);
    if (nl < 2 || ntag < 2) return;
    uint8_t* x = NULL;
    for (int k = 0; k < nl && !x; k++) {
        uint8_t* c = list[(loops / g_cfg.country_test_annex * 7 + k) % nl];
        if (c && *(int*)(c + C_PROVINCES) > 0) x = c;
    }
    if (!x) return;
    int n = 2 * ntag;
    if (n > g_ax_cap) {
        axcall_t* p = (axcall_t*)realloc(g_ax, (size_t)n * sizeof *p);
        if (!p) return;
        g_ax = p; g_ax_cap = n;
    }
    int16_t* old = (int16_t*)malloc((size_t)n * sizeof *old);
    if (!old) return;
    uint64_t xkey = *(uint64_t*)(x + C_TAG);
    int m = 0;
    for (int i = 0; i < ntag; i++) {
        uint8_t* c = tab[i];
        if (!c) continue;
        axcall_t a1 = { c, xkey, 1, 0, 0, 0 }, a2 = { x, *(uint64_t*)(c + C_TAG), 1, 0, 0, 0 };
        if (opcache_at(a1.c, a1.key)) g_ax[m++] = a1;
        if (opcache_at(a2.c, a2.key)) g_ax[m++] = a2;
    }
    for (int i = 0; i < m; i++) old[i] = *opcache_at(g_ax[i].c, g_ax[i].key);
    LONGLONG t0 = now();
    for (int i = 0; i < m; i++) { ax_one(i); g_ax[i].v = *opcache_at(g_ax[i].c, g_ax[i].key); *opcache_at(g_ax[i].c, g_ax[i].key) = old[i]; }
    LONGLONG t1 = now();
    if (m >= 2) ax_run(m);
    LONGLONG t2 = now();
    for (int i = 0; i < m; i++) {
        int16_t* e = opcache_at(g_ax[i].c, g_ax[i].key);
        if (*e != g_ax[i].v && ++g_ax_test_bad <= 10)
            eb_log("country: TEST annexation opinion %.3s -> tag %d: serial %d, all cores %d",
                   (const char*)(g_ax[i].c + C_TAG), (int)(int16_t)(g_ax[i].key >> 32), (int)g_ax[i].v, (int)*e);
        *e = old[i];
    }
    free(old);
    g_ax_tests++; g_ax_test_calls += m;
    g_ax_test_ser_us += (double)(t1 - t0) * g_qpf_us; g_ax_test_par_us += (double)(t2 - t1) * g_qpf_us;
}

// ---- hooks ------------------------------------------------------------------------------------------
static void war_test(uint8_t* gs);
static void hk_serial(void* gs) {
    LONGLONG t0 = g_cfg.country_stats ? now() : 0;
    if (g_ax_n) annex_flush();                                  // a loop not seen to its end (should not happen)
    if (g_cfg.country_test_annex > 0 && g_cfg.country_parallel_annex) annex_test((uint8_t*)gs);
    if (g_cfg.country_parallel_refresh) precompute((uint8_t*)gs);
    o_serial(gs);
    if (g_cfg.country_parallel_refresh) finish_loop();
    if (g_cfg.country_test_war_all > 0 && g_cfg.country_parallel_war) war_test((uint8_t*)gs);
    if (g_cfg.test_warview) eb_warview_test((uint8_t*)gs);
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
            LONGLONG t0 = now(); verify_rk(c, w, &g_pr_ver, &g_pr_ver_bad); record(F_RK, t0, ret); return;
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
// ---- the war refresh in parallel ([country] parallel_war) ----------------------------------------
// FUN_140d8f790(war) runs RefreshKnowledge for every attacker (war+0x48..0x50, tags) and then every
// defender (+0x78..0x80), one after the other; joining or declaring a war calls it. An effect that joins
// many countries one by one (join_all_offensive_wars_of / join_all_defensive_wars_of in a country loop:
// 25 files of the 짭짭죽 mod) refreshes all participants again for every join: 5-31 s stalls (report 22).
// Here the participants are refreshed at once (run_batch: workers + main thread, their opinion-cache
// calls and messages replayed in participant order). In the original order after them, on the main
// thread: the player's and the observed country, and every participant that is the group leader of
// another participant or whose group leader takes part (the member reads the leader's +0x1bf0 list,
// which the leader's refresh rebuilds). Subjects read their overlord (liberty desire +0xe34:
// FUN_1403ca1a0 via FUN_1403d79f0 reads the overlord's values; the first verify of the all-countries test
// differed there in 219 of 2,812), so they run in phases (refresh_set). Nothing to share: the game's order.
// Mode 2 = + verify: each phase is refreshed again serially on its result and compared.
// Testing only: [country] test_war_all=D - every D country loops, all countries that are not dirty go
// through the same path (a war of hundreds of countries), with mode 2 also the serial time of the set.
static LONG64 g_war_calls, g_war_par, g_war_jobs, g_war_serial, g_war_ver, g_war_bad, g_war_tests;
static double g_war_us, g_war_ver_us, g_war_test_us, g_war_test_ser_us;
static LONG64 g_war_test_jobs, g_war_test_serial, g_war_test_ver, g_war_test_bad;
static const char* g_war_where[2];                     // the game's "where" texts of the two loops
static int16_t g_tagidx[65536];                         // participant index by tag (-1 = none; reset after use)

// RefreshKnowledge of cs[0..n) as the game would in this order; returns the parallel count.
// A subject S reads its overlord O (both in the set): in the game's order S sees O's old state when S
// comes first and O's new state when O comes first. Phase 0 = subjects before their overlord, phase 1 =
// overlords and everyone else, phase 2 = subjects after their overlord; each phase runs at once and is
// verified (mode 2) before the next one changes what it read. Serial, after the phases in the game's
// order: the player's and observed country, group leaders and members in the set, a country that is a
// subject and an overlord in the set, and every overlord/subject pair one of which is serial.
static uint8_t g_ph[MAXJOBS];                           // phase 0..2, 3 = serial
static int16_t g_olidx[MAXJOBS];                        // the overlord's index in the set, -1 = none
static int refresh_set(uint8_t* gs, uint8_t** cs, const char** ws, int n, double* par_us, double* ver_us) {
    short player = (short)(*(uint64_t*)(gs + 0x1e60) >> 32), shown = player;
    if (gs[0x1e66] == 7 && gs[0x1e6f]) shown = (short)(*(uint64_t*)(gs + 0x1e68) >> 32);
    static uint8_t has_sub[MAXJOBS];
    for (int i = 0; i < n; i++) { g_ph[i] = 1; g_olidx[i] = -1; has_sub[i] = 0; g_tagidx[(uint16_t)(*(uint64_t*)(cs[i] + C_TAG) >> 32)] = (int16_t)i; }
    for (int i = 0; i < n; i++) {
        uint8_t* c = cs[i];
        uint16_t tg = (uint16_t)(*(uint64_t*)(c + C_TAG) >> 32), lead = tg;
        if ((short)tg == player || (short)tg == shown) g_ph[i] = 3;
        uint8_t* grp = *(uint8_t**)(c + C_GROUP);
        if (grp) {
            uint64_t* vb = *(uint64_t**)(grp + 0x20); uint64_t* ve = *(uint64_t**)(grp + 0x28);
            if (vb && ve > vb) lead = (uint16_t)(vb[0] >> 32);
        }
        if (lead != tg && g_tagidx[lead] >= 0) { g_ph[i] = 3; g_ph[g_tagidx[lead]] = 3; }
        if (c[C_OVERLORD + 7]) {
            uint16_t ol = (uint16_t)(*(uint64_t*)(c + C_OVERLORD) >> 32);
            if (ol != tg && g_tagidx[ol] >= 0) { g_olidx[i] = g_tagidx[ol]; has_sub[g_tagidx[ol]] = 1; }
        }
    }
    for (int i = 0; i < n; i++) g_tagidx[(uint16_t)(*(uint64_t*)(cs[i] + C_TAG) >> 32)] = -1;
    for (int i = 0; i < n; i++) if (g_olidx[i] >= 0 && has_sub[i]) g_ph[i] = 3;   // in the middle of a chain
    for (int changed = 1; changed; ) {                  // a serial overlord makes its subjects serial and back
        changed = 0;
        for (int i = 0; i < n; i++) {
            int o = g_olidx[i];
            if (o >= 0 && (g_ph[i] == 3) != (g_ph[o] == 3)) { g_ph[i] = g_ph[o] = 3; changed = 1; }
        }
    }
    int cnt[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        if (g_ph[i] != 3 && g_olidx[i] >= 0) g_ph[i] = g_olidx[i] > i ? 0 : 2;
        cnt[g_ph[i]]++;
    }
    if (cnt[0] < 2 && cnt[1] < 2 && cnt[2] < 2) { for (int i = 0; i < n; i++) o_rk(cs[i], ws[i]); return 0; }   // nothing to share: as the game
    int total = 0;
    LONGLONG t0 = now();
    double vus = 0;
    for (int ph = 0; ph < 3; ph++) {
        LONG k = 0;
        for (int i = 0; i < n; i++)
            if (g_ph[i] == ph) { g_wjobs[k].c = cs[i]; g_wjobs[k].where = ws[i]; g_wjobs[k].nops = 0; k++; }
        if (k >= 2) { run_batch(g_wjobs, k, 0); total += k; }
        else for (LONG i = 0; i < k; i++) o_rk(g_wjobs[i].c, g_wjobs[i].where);
        if (g_cfg.country_parallel_war == 2 && k >= 2) {
            LONGLONG t1 = now();
            for (LONG i = 0; i < k; i++) verify_rk(g_wjobs[i].c, g_wjobs[i].where, &g_war_ver, &g_war_bad);
            vus += (double)(now() - t1) * g_qpf_us;
        }
    }
    for (int i = 0; i < n; i++) if (g_ph[i] == 3) o_rk(cs[i], ws[i]);
    *par_us += (double)(now() - t0) * g_qpf_us - vus;
    *ver_us += vus;
    g_war_jobs += total; g_war_serial += n - total;
    return total;
}

static uint8_t* g_wcs[MAXJOBS];
static const char* g_wws[MAXJOBS];
static void hk_warref(void* war) {
    uint8_t* w = (uint8_t*)war;
    uint64_t* ab = *(uint64_t**)(w + 0x48); uint64_t* ae = *(uint64_t**)(w + 0x50);
    uint64_t* db = *(uint64_t**)(w + 0x78); uint64_t* de = *(uint64_t**)(w + 0x80);
    int na = ae > ab ? (int)(ae - ab) : 0, nd = de > db ? (int)(de - db) : 0;
    g_war_parts += na + nd;
    LONGLONG t0 = now();
    uint8_t* gs = *g_gsp;
    void** session = gs ? *(void***)(gs + GS_SESSION) : NULL;
    int n = 0;
    if (g_cfg.country_parallel_war && GetCurrentThreadId() == g_main_tid && !g_in_batch && session && !session[0x6c] &&
        na + nd >= 3 && na + nd <= MAXJOBS && g_war_where[0]) {
        uint8_t** tab = *(uint8_t***)(*g_tagtab + 0x118);
        for (int i = 0; i < na + nd; i++) {
            uint64_t tag = i < na ? ab[i] : db[i - na];
            uint8_t* c = tab[(int16_t)(tag >> 32)];
            if (!c) { n = 0; break; }
            g_wcs[n] = c; g_wws[n] = g_war_where[i < na ? 0 : 1]; n++;
        }
    }
    g_war_calls++;
    if (n) { if (refresh_set(gs, g_wcs, g_wws, n, &g_war_us, &g_war_ver_us)) g_war_par++; }
    else o_warref(war);
    record(F_WAR, t0, NULL);
}

static void war_test(uint8_t* gs) {                    // test_war_all: a war of every country that is not dirty
    static int loops;
    if (++loops % g_cfg.country_test_war_all) return;
    uint8_t** list = *(uint8_t***)(gs + GS_COUNTRIES);
    int nl = *(int*)(gs + GS_NCOUNTRIES), n = 0;
    for (int i = 0; i < nl && n < MAXJOBS; i++) {
        uint8_t* c = list[i];
        if (!c || c[C_DIRTY] || c[C_DIRTY2] || *(int*)(c + C_PROVINCES) <= 0) continue;
        g_wcs[n] = c; g_wws[n] = g_where; n++;
    }
    if (n < 3) return;
    LONG64 j0 = g_war_jobs, s0 = g_war_serial, v0 = g_war_ver, b0 = g_war_bad;
    refresh_set(gs, g_wcs, g_wws, n, &g_war_test_us, &g_war_test_ser_us);
    g_war_tests++; g_war_test_jobs += g_war_jobs - j0; g_war_test_serial += g_war_serial - s0;
    g_war_test_ver += g_war_ver - v0; g_war_test_bad += g_war_bad - b0;
    g_war_jobs = j0; g_war_serial = s0; g_war_ver = v0; g_war_bad = b0;
}

int eb_country_install(void) {
    if (!g_cfg.country_stats && !g_cfg.country_parallel_refresh && !g_cfg.country_parallel_war && !g_cfg.country_parallel_annex) return 0;
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
        { RVA_MSG,     sig_msg,     (void*)hk_msg,     (void**)&o_msg,     "message handler" },
        { RVA_WARREF,  sig_warref,  (void*)hk_warref,  (void**)&o_warref,  "war refresh" },
    };
    int nt_ = (int)(sizeof t / sizeof t[0]);
    if (nt->FileHeader.TimeDateStamp != 0x66FE6AB9u) { eb_log("country: unexpected eu4.exe - off"); return 0; }
    for (int i = 0; i < nt_; i++)
        if (memcmp(b + t[i].rva, t[i].sig, 16) != 0) { eb_log("country: signature mismatch at %s - off", t[i].name); return 0; }
    // the bundle's call: E8 rel32 to RefreshKnowledge right before the return address
    if (memcmp(b + RVA_KCACHE, sig_kcache, sizeof sig_kcache) != 0) { eb_log("country: signature mismatch at FUN_1402f1120 - off"); return 0; }
    g_kcache = (kc_t)(b + RVA_KCACHE);
    if (memcmp(b + RVA_MSGMOVE, sig_msgmove, 16) != 0 || memcmp(b + RVA_STRFREE, sig_strfree, 16) != 0 ||
        memcmp(b + RVA_LSTFREE, sig_lstfree, 16) != 0) { eb_log("country: signature mismatch at the message helpers - off"); return 0; }
    g_msgmove = (mmove_t)(b + RVA_MSGMOVE);
    g_strfree = (v1_t)(b + RVA_STRFREE);
    g_lstfree = (v1_t)(b + RVA_LSTFREE);
    uint8_t* call = b + RET_BUNDLE_RK - 5;
    if (call[0] != 0xe8 || call + 5 + *(int32_t*)(call + 1) != b + RVA_RK) { eb_log("country: bundle call site mismatch - off"); return 0; }
    g_bundle_ret = b + RET_BUNDLE_RK;
    g_daily_lo = b + RVA_DAILY;
    g_gsp = (uint8_t**)(b + RVA_GSP);
    g_tagtab = (uint8_t**)(b + RVA_TAGTAB);
    memset(g_tagidx, 0xff, sizeof g_tagidx);
    for (int k = 0; k < 2; k++) {                       // lea rdx, [rip + text] before each RefreshKnowledge call
        uint8_t* p = b + (k ? WAR_LEA2 : WAR_LEA1);
        if (p[0] == 0x48 && p[1] == 0x8d && p[2] == 0x15) {
            const char* s = (const char*)(p + 7 + *(int32_t*)(p + 3));
            if (strstr(s, k ? "war.cpp:1551" : "war.cpp:1546")) g_war_where[k] = s;
        }
    }
    if (!g_war_where[0] || !g_war_where[1]) {
        g_war_where[0] = NULL;
        if (g_cfg.country_parallel_war) eb_log("country: war refresh texts not found - parallel war refresh off");
    }
    if (g_cfg.country_parallel_annex) {                 // the two calls of FUN_140364f10's loop
        uint8_t* r1 = b + RET_ANNEX1; uint8_t* r2 = b + RET_ANNEX2;
        if (r1[-5] == 0xe8 && r1 + *(int32_t*)(r1 - 4) == b + RVA_OPCACHE && r2[-5] == 0xe8 && r2 + *(int32_t*)(r2 - 4) == b + RVA_OPCACHE &&
            !memcmp(r1, sig_annex, sizeof sig_annex)) { g_ax_ret1 = r1; g_ax_ret2 = r2; }
        else { eb_log("country: annexation loop not found - parallel annexation opinions off"); g_cfg.country_parallel_annex = 0; }
    }
    if (g_cfg.country_parallel_refresh || g_cfg.country_parallel_war || g_cfg.country_parallel_annex) {
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
        if (t[i].rva == RVA_OPCACHE && !g_cfg.country_parallel_refresh && !g_cfg.country_parallel_war && !g_cfg.country_parallel_annex) continue;
        if (t[i].rva == RVA_MSG && !g_cfg.country_parallel_refresh && !g_cfg.country_parallel_war) continue;
        // timing-only hooks: not needed to play
        if ((t[i].rva == RVA_DAILY || t[i].rva == RVA_CM || t[i].rva == RVA_UCB) && !g_cfg.country_stats) continue;
        if (t[i].rva == RVA_WARREF && !g_cfg.country_stats && !g_cfg.country_parallel_war) continue;
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
    if (g_cfg.country_parallel_war && g_war_where[0])
        eb_log("country: war refresh (joining or declaring a war) on all cores, mode %d (%s), %d worker threads%s",
               g_cfg.country_parallel_war, g_cfg.country_parallel_war == 2 ? "on + verify" : "on", g_nworkers,
               g_cfg.country_test_war_all ? " | TEST: every country that is not dirty, as one war" : "");
    if (g_cfg.country_parallel_annex)
        eb_log("country: opinions recomputed when a country disappears on all cores, mode %d (%s), %d worker threads%s",
               g_cfg.country_parallel_annex, g_cfg.country_parallel_annex == 2 ? "on + verify" : "on", g_nworkers,
               g_cfg.country_test_annex ? " | TEST: the same for one country every few loops" : "");
    return 1;
#endif
}

void eb_country_report(void) {
    if (!o_serial) return;
    if (g_cfg.country_parallel_refresh)
        eb_log("country: parallel refresh: %lld loops, %lld precomputed (%.1f per loop, %.0f us per loop on the main thread), %lld skipped at the bundle, %lld marked again, %lld not reached, %lld opinion-cache calls replayed, %lld messages posted on the main thread (%lld on a worker: out of memory), %lld left out (group leader also dirty) | verify %lld, mismatches %lld",
               (long long)g_pr_loops, (long long)g_pr_jobs, g_pr_loops ? (double)g_pr_jobs / (double)g_pr_loops : 0.0,
               g_pr_loops ? g_pr_us / (double)g_pr_loops : 0.0, (long long)g_pr_skipped, (long long)g_pr_redirty,
               (long long)g_pr_leftover, (long long)g_pr_deferred, (long long)g_pr_msgs, (long long)g_pr_msg_oom,
               (long long)g_pr_dep, (long long)g_pr_ver, (long long)g_pr_ver_bad);
    if (g_cfg.country_test_truce_end || g_cfg.country_test_no_defer)
        eb_log("country: TEST truce_end %d no_defer %d: %lld relations set to a truce that has just ended",
               g_cfg.country_test_truce_end, g_cfg.country_test_no_defer, (long long)g_pr_test);
    if (g_cfg.country_parallel_war)
        eb_log("country: war refresh: %lld calls, %lld in parallel (%lld participants on all cores, %lld after them in order), %.1f ms in total | verify %lld, mismatches %lld (serial redo %.1f ms)",
               (long long)g_war_calls, (long long)g_war_par, (long long)g_war_jobs, (long long)g_war_serial, g_war_us / 1000.0,
               (long long)g_war_ver, (long long)g_war_bad, g_war_ver_us / 1000.0);
    if (g_cfg.country_parallel_annex)
        eb_log("country: annexation opinions: %lld loops, %lld calls (%lld on all cores, %lld on the main thread after them), %.1f ms in total | verify %lld, mismatches %lld (the game's own calls %.1f ms)",
               (long long)g_ax_loops, (long long)g_ax_calls, (long long)g_ax_par, (long long)g_ax_main, g_ax_us / 1000.0,
               (long long)g_ax_ver, (long long)g_ax_bad, g_ax_ser_us / 1000.0);
    if (g_ax_tests)
        eb_log("country: TEST annexation opinions: %lld runs, %lld calls, serial %.1f ms, all cores %.1f ms, mismatches %lld",
               (long long)g_ax_tests, (long long)g_ax_test_calls, g_ax_test_ser_us / 1000.0, g_ax_test_par_us / 1000.0, (long long)g_ax_test_bad);
    if (g_war_tests)
        eb_log("country: TEST war of all countries: %lld runs, %lld participants on all cores (%lld after them in order) in %.1f ms | verify %lld, mismatches %lld, serial redo %.1f ms",
               (long long)g_war_tests, (long long)g_war_test_jobs, (long long)g_war_test_serial, g_war_test_us / 1000.0,
               (long long)g_war_test_ver, (long long)g_war_test_bad, g_war_test_ser_us / 1000.0);
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
    if (g_st[F_WAR].calls)
        eb_log("country: war refresh: %lld participants refreshed in %lld calls (%.1f per call)", (long long)g_war_parts,
               (long long)g_st[F_WAR].calls, (double)g_war_parts / (double)g_st[F_WAR].calls);
}
