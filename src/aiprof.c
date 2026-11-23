// Diagnostics ([parallel] ai_item_stats=1, default off): time of every item of the AI loop.
//
// FUN_14075b4b0 collects the AI object of every living country (gamestate+0xb78 vector; the country
// tag {char tag[4]; short index} at +0x20) and runs FUN_1400b0fa0(ai) for each on all cores through
// FUN_14077d8a0. The loop's wall time is set by its slowest country: in the 10-19 profile the main
// thread waits 6.4% of its time there while the workers have long finished. Every 100 loops the log
// gets the average wall time, the average slowest item, and the countries with the most total time.
//
// Inside a country's update, FUN_14014b370 walks a tree of AI tasks: FUN_1400e4890(node, ctx) runs
// the node (vt+0x60) when vt+0x50 says it is active and its countdown at +0x20 reaches 0, then calls
// vt+0x58 of each child (list at +0x40: {child, ?, next}). hk_node does exactly that and times each
// run by the node's class (vtable; RTTI name in the report): self time without nested runs, the
// longest run, and runs over 5 ms. Every 500 loops the log gets the classes with the most self time.
// The foreign minister's runs (vt 0x141c3df20, peace deals) are also broken down: calls and time of 14
// of its functions (g_fmf) in the longest run and per run over 100 ms.
//
// EXPERIMENT [parallel] ai_split=1 (default off): one country's AI work on two threads.
//   The king's update FUN_14014b370(king, ctx) ends with its own run, then the children at king+0x40 -
//   always one node each of 7 classes, the first the military one (vt 0x141c3f0e8, ai_admiral) - then
//   the army/fleet AI nodes at king+0xc50 (vt 0x141c3cdb8, the largest share: 31 of 79 ms a day in
//   the late game), then FUN_140150740 x6. The AI loop waits for its slowest country (99% of its wall
//   time in the late game) while the other workers idle.
//   With the experiment, once the first child has run, the army list (if it has ai_split_min nodes
//   or more) is posted as a job; the king's thread goes on with the other six children. Workers that
//   ran out of countries in the AI loop (parallel.c my_task) and the main thread while it waits
//   (my_wait) take posted jobs. When the original reaches the army list, the king's thread runs the
//   job itself if nobody took it yet, or waits for it, and the original's calls of those nodes return
//   at once. So each army runs exactly once and before the king's update ends, as before; what
//   changes is that it runs at the same time as the six other children of its own country.
//   Known shared state: ctx (ai+0x60) is the country's access cache (flat hash map {?, entries,
//   count, mask, max lookups, max load}) used through FUN_140415c70 (lookup) and FUN_140415b00
//   (insert, may rehash) by the army code and the other children; while a job is out, both take a
//   striped SRW lock (shared / exclusive). Other king state the two sides may both touch is not known.
//   Off with the fixed-seed option (the game itself runs the AI loop serially then).
//   Result (10-21): no crashes, no gain - the slow days come from single runs of other children.
//
// EXPERIMENT [parallel] ai_prov_split=1 (2 = + verify, default off): the province evaluation on all
// cores. The child 0x141c3d368 (FUN_14011dc00 -> FUN_140119320) calls FUN_140118d20(node, country,
// target, flag) for the country and its subjects: for every province of `country` and every building
// that FUN_14032acb0 allows it scores the pair with FUN_140118470(int out[2], country, target,
// province, building, buf, int* a, int* b, log) (82% of the time; no RNG within 4 call levels, writes
// only out) and keeps the best in node+0xa50/+0xa58/+0xa60 (float)/+0xa68. One run takes up to
// 300 ms in the late game. For countries with ai_prov_min provinces or more:
//   1. record: the original runs with FUN_140118470 returning {0,0} and noting its arguments (the
//      node's best fields are restored afterwards; FUN_1401174d0, which the loop calls first for the
//      own country and which uses the RNG, is skipped so that it still runs once);
//   2. the noted calls are scored in chunks by this thread and by idle AI-loop threads (the job
//      queue of ai_split);
//   3. replay: the original runs again and every call that matches a noted one (same province,
//      building, target, *a, *b and buf; looked up by hash) gets its precomputed result; others are
//      computed.
//   The choice of the best pair, its order and ties are the original code's; only the scoring calls
//   ran earlier and elsewhere. Verify mode scores every replayed call again and compares.
//   ai_prov_split=3 (A/B): even AI loops run the original, odd ones the experiment, and the wall time
//   of every big evaluation is summed per side (same game, same days - runs differ by +-20%).
//
// EXPERIMENT [parallel] ai_ae_cache=1 (2 = + verify, 3 = A/B, default off): a faster AE cache for the
// foreign minister. Building a peace deal (FUN_14098c0e0 -> FUN_14098b670) values every candidate
// province with the coalition check FUN_14099fc60 -> FUN_14038a530, which sums the aggressive
// expansion of every country (FUN_140388b40) over the deal's provinces (FUN_140388880 per province).
// In the late game one deal makes ~10 million FUN_140388880 calls and takes up to 1.9 s; ~99.9% of
// them are hits in the AE cache. That cache is one std::unordered_map that FUN_14075b4b0 creates for
// each AI loop (FUN_140478280, lock flag 1) and that FUN_140786150 puts into every AI thread's TBB slot
// (ETS 0x141fd1ab8): each lookup takes the slot through FUN_1400df320, a spinlock (FUN_141581820) and
// a linked bucket walk. With the experiment, while that cache is the one in use, FUN_140388880 looks up
// a per-thread open-addressing table instead, valid for the same AI loop; a miss computes the value as
// the game's cached path does - FUN_140385d40 with the constant 100000 (0x141c337fc) - and the result is
// scaled with the game's integer formula. Negative values are not kept (the game recomputes those
// too). The game's own map is then no longer filled during the AI loop; nothing else reads it.
//   The AE sum itself, FUN_14038a530 (with FUN_140388b40 for each country), is redone in C on the same
//   tables (ae_sum: the same countries, filters, order and integer arithmetic; the rare branch for a
//   country that would join a coalition calls the game's own functions), so the ~17 lookups per
//   country need no call each. Verify mode runs the game's sum with the game's map after every sum
//   and compares, and compares every single lookup the same way.
//   ai_ae_cache=3 (A/B): the AE sums alternate on each thread between the game's code with the game's
//   map and ae_sum, and the time of each sum is added up per side (big peace deals are rare and vary
//   from run to run, so alternating whole days compares little).
//
// EXPERIMENT [parallel] ai_army_fast=1 (2 = + verify, 3 = A/B, default off): the army AI's move check.
// In the late game the army/fleet AI (FUN_1400ea250) is 40% of all AI work and about half of the
// slowest country's update on ordinary days. 21% of it is FUN_140d45250(unit, from, to, flag), asked
// for many candidate moves, which returns false at once for one special owner, then if the unit has
// modifier 0xe0 (FUN_140d40ae0: leader, country and regiment modifiers, built from scratch on every
// call - 19% of the army AI), then if either province lacks flag 8 at +0x28 -> +0x228. Only when all
// pass does it do its real work. The experiment
//   1. tests the two flags first and returns false when one is missing, which is what the original
//      returns then whatever the other two say (they only read); only ~12% of the checks end there;
//   2. keeps the answer of FUN_140d40ae0(unit, 0xe0, 0) asked from inside FUN_140d45250 for the rest
//      of the same AI task run (hk_node bumps a per-thread run number before each run): ~22,000 checks
//      a day in the late game, mostly a burst for one army deciding where to go.
// Verify mode asks the original again for every early false and every kept answer and compares.
#include "common.h"
#include <stdio.h>
#include <string.h>
#include "MinHook.h"
#include <stdlib.h>

#define RVA_AI_ITEM 0xb0fa0u     // one country's AI update (ai object)
#define RVA_AI_LOOP 0x77d8a0u    // parallel for over the AI objects (vector {vt, data, cap, count at +0x14}, ctx)
static const uint8_t sig_item[24] = { 0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0x6c,0x24,0xf8,0x48,0x81,0xec,0x08,0x01,0x00 };
static const uint8_t sig_loop[24] = { 0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x20,0x55,0x57,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x8d,0x6c,0x24,0xc9,0x48 };
#define RVA_AI_NODE 0xe4890u     // AI task tree node update (node, ctx)
static const uint8_t sig_node[24] = { 0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0x01,0x48,0x8b,0xfa,0x48,0x8b,0xd9,0xff,0x50,0x50,0x84,0xc0 };
#define RVA_KING    0x14b370u     // king update FUN_14014b370(king, ctx)
#define RVA_CGET    0x415c70u     // access cache lookup (map, byte* out, tag a, tag b)
#define RVA_CPUT    0x415b00u     // access cache insert (map, tag a, tag b, byte value)
#define RVA_FIXED_SEED_FLAG 0x23581c0u
#define VT_KING     0x1c3e9e0u
#define VT_MILITARY 0x1c3f0e8u
#define K_KIDS      0x40
#define K_ARMIES    0xc50
static const uint8_t sig_king[24] = { 0x40,0x55,0x57,0x48,0x83,0xec,0x28,0xff,0x81,0xc0,0x0a,0x00,0x00,0x48,0x8b,0xea,0x48,0x8b,0x81,0x28,0x0d,0x00,0x00,0x48 };
static const uint8_t sig_cget[24] = { 0x48,0x89,0x5c,0x24,0x10,0x49,0xc1,0xe9,0x20,0x48,0x8b,0xda,0x49,0xc1,0xe8,0x20,0x48,0x8d,0x54,0x24,0x08,0x4d,0x0f,0xbf };
static const uint8_t sig_cput[24] = { 0x44,0x88,0x4c,0x24,0x20,0x53,0x48,0x83,0xec,0x40,0x49,0xc1,0xe8,0x20,0x45,0x0f,0xb6,0xd9,0x48,0xc1,0xea,0x20,0x48,0x8b };
#define NTAG 4096
#define NCLS 512

typedef void (*item_t)(uint8_t* ai);
typedef void (*loop_t)(uint8_t* items, void* ctx);
static item_t o_item;
static loop_t o_loop;
static volatile LONG64 g_total[NTAG], g_max[NTAG];
static volatile LONG g_calls[NTAG], g_slowest[NTAG];
static char g_tag[NTAG][4];
static volatile LONG64 g_loop_max;
static volatile LONG g_loop_max_tag;
static LONG64 g_wall, g_slow, g_items, g_loops;   // main thread only
static double g_ms;                               // milliseconds per QPC tick
typedef struct { void* volatile vt; volatile LONG64 runs, self, max; volatile LONG spikes; } cls_t;
static cls_t g_cls[NCLS];
static __thread LONG64 t_nested;                  // time of the runs nested in the current run
static LONG64 g_spike;                            // 5 ms in QPC ticks
typedef void (*node_t)(uint8_t* node, void* ctx);
static node_t o_node;
static int g_stats, g_split;

// ---- split experiment state ----
typedef struct job_s {
    void (*run)(struct job_s*);   // army list or province-evaluation chunks
    void* arg;
    void* ctx;
    volatile LONG state;          // 0 posted, 1 running, 2 done
    volatile LONG refs;           // the queue and the king's thread
    int n;
    uint8_t* nodes[];             // the army list when it was posted
} job_t;
typedef struct { uint8_t* king; void* ctx; uint8_t* first; job_t* job; int next; LONG64 t0, t1, t2; } split_t;
static __thread split_t t_split;  // the king update running on this thread
#define QCAP 1024
static job_t* g_q[QCAP];
static int g_qn;
static SRWLOCK g_qlock = SRWLOCK_INIT;
static HANDLE g_qsem;
static SRWLOCK g_done_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_done_cv = CONDITION_VARIABLE_INIT;
static volatile LONG g_ai_loop_active;
static volatile LONG64 g_jobs, g_job_nodes, g_by_helper, g_inline, g_waited;
#define NSTRIPE 64
static volatile LONG g_ctx_active[NSTRIPE];
static SRWLOCK g_ctx_lock[NSTRIPE];
typedef char (*cget_t)(void* map, uint8_t* out, uint64_t a, uint64_t b);
typedef void (*cput_t)(void* map, uint64_t a, uint64_t b, uint8_t v);
typedef void (*king_t)(uint8_t* king, void* ctx);
static cget_t o_cget;
static cput_t o_cput;
static king_t o_king;
static int stripe(void* p) { return (int)(((uintptr_t)p >> 5) & (NSTRIPE - 1)); }
// ---- foreign minister diagnostics (stats): calls and time of its main functions within each run ----
#define VT_FM 0x1c3df20u
#define NFM 14
static const struct { uint32_t rva; uint8_t sig[8]; const char* name; } g_fmf[NFM] = {
    { 0x132c40, { 0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x4c }, "peace deals 140132c40" },
    { 0x98c0e0, { 0x48,0x89,0x5c,0x24,0x08,0x44,0x88,0x4c }, "deal builder 14098c0e0" },
    { 0x98b670, { 0x4c,0x89,0x4c,0x24,0x20,0x44,0x88,0x44 }, "best item 14098b670" },
    { 0x979a40, { 0x48,0x89,0x5c,0x24,0x08,0x44,0x88,0x4c }, "deal value 140979a40" },
    { 0x97a1c0, { 0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74 }, "can demand 14097a1c0" },
    { 0x979060, { 0x48,0x8b,0xc4,0x53,0x48,0x83,0xec,0x40 }, "deal cost 140979060" },
    { 0x975d40, { 0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c }, "war costs 140975d40" },
    { 0x99d9d0, { 0x4c,0x89,0x44,0x24,0x18,0x89,0x54,0x24 }, "war cost 14099d9d0" },
    { 0x38a530, { 0x48,0x89,0x5c,0x24,0x08,0x4c,0x89,0x4c }, "AE all 14038a530" },
    { 0x388b40, { 0x4c,0x8b,0xdc,0x55,0x56,0x41,0x54,0x48 }, "AE one 140388b40" },
    { 0x1340c0, { 0x48,0x89,0x5c,0x24,0x20,0x44,0x88,0x44 }, "wargoal 1401340c0" },
    { 0x139730, { 0x48,0x89,0x5c,0x24,0x20,0x55,0x56,0x57 }, "other 140139730" },
    { 0x388880, { 0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74 }, "AE province 140388880" },
    { 0x385d40, { 0x48,0x89,0x54,0x24,0x10,0x48,0x89,0x4c }, "AE compute 140385d40" },
};
typedef uint64_t (*fn10_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static fn10_t o_fm[NFM];
static __thread LONG64 t_fm_n[NFM], t_fm_t[NFM];  // within the foreign-minister run on this thread
static __thread int t_fm_depth[NFM];
static SRWLOCK g_fm_lock = SRWLOCK_INIT;
static LONG64 g_fm_max, g_fm_max_n[NFM], g_fm_max_t[NFM];          // the longest run
static LONG64 g_fm_big_runs, g_fm_big_t, g_fm_big_n[NFM], g_fm_big_tt[NFM];   // runs over 100 ms
#define FMW(k) static uint64_t fm_hk##k(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g, uint64_t h, uint64_t i, uint64_t j) { \
    int outer = t_fm_depth[k]++ == 0; LARGE_INTEGER t0; if (outer) QueryPerformanceCounter(&t0); \
    uint64_t r = o_fm[k](a, b, c, d, e, f, g, h, i, j); \
    if (outer) { LARGE_INTEGER t1; QueryPerformanceCounter(&t1); t_fm_t[k] += t1.QuadPart - t0.QuadPart; } \
    t_fm_n[k]++; t_fm_depth[k]--; return r; }
FMW(0) FMW(1) FMW(2) FMW(3) FMW(4) FMW(5) FMW(6) FMW(7) FMW(8) FMW(9) FMW(10) FMW(11) FMW(12) FMW(13)
static void* const g_fm_hk[NFM] = { (void*)fm_hk0, (void*)fm_hk1, (void*)fm_hk2, (void*)fm_hk3, (void*)fm_hk4, (void*)fm_hk5,
                                    (void*)fm_hk6, (void*)fm_hk7, (void*)fm_hk8, (void*)fm_hk9, (void*)fm_hk10, (void*)fm_hk11,
                                    (void*)fm_hk12, (void*)fm_hk13 };
static int g_fm_on;
static void fm_run_done(LONG64 d) {
    if (d * g_ms > 100.0 || d > g_fm_max) {
        AcquireSRWLockExclusive(&g_fm_lock);
        if (d * g_ms > 100.0) {
            g_fm_big_runs++; g_fm_big_t += d;
            for (int k = 0; k < NFM; k++) { g_fm_big_n[k] += t_fm_n[k]; g_fm_big_tt[k] += t_fm_t[k]; }
        }
        if (d > g_fm_max) { g_fm_max = d; for (int k = 0; k < NFM; k++) { g_fm_max_n[k] = t_fm_n[k]; g_fm_max_t[k] = t_fm_t[k]; } }
        ReleaseSRWLockExclusive(&g_fm_lock);
    }
}
// ---- province evaluation experiment state ----
#define RVA_PLOOP 0x118d20u       // FUN_140118d20(node, country, target, char)
#define RVA_PEVAL 0x118470u       // FUN_140118470(int out[2], country, target, province, building, buf, int* a, int* b, std::string* log)
#define RVA_PPRE  0x1174d0u       // FUN_1401174d0(node, country, target, char): only for the own country, uses the RNG
#define N_BEST    0xa50           // the loop writes +0xa50, +0xa58, +0xa60, +0xa68
#define PBUF      0x78            // the loop's local buffer (FUN_140181140) passed to every scoring call
#define PBUF_CMP  0x40            // FUN_140181140 writes only its first 8 qwords (the rest is stack garbage); scoring reads those
static const uint8_t sig_ploop[24] = { 0x44,0x88,0x4c,0x24,0x20,0x4c,0x89,0x44,0x24,0x18,0x48,0x89,0x54,0x24,0x10,0x48,0x89,0x4c,0x24,0x08,0x55,0x53,0x56,0x57 };
static const uint8_t sig_peval[24] = { 0x48,0x8b,0xc4,0x48,0x89,0x58,0x08,0x48,0x89,0x70,0x10,0x48,0x89,0x78,0x18,0x55,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57 };
static const uint8_t sig_ppre[24]  = { 0x48,0x8b,0xc4,0x48,0x89,0x50,0x10,0x48,0x89,0x48,0x08,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48 };
typedef struct { uint8_t* prov; uint8_t* bld; void* target; int a, b; int out[2]; } ecall_t;
typedef struct {
    uint8_t* country;
    uint8_t buf[PBUF];
    int have_buf, bad;
    ecall_t* calls;
    int* hash;                    // replay: index+1 by (province, building, target, a, b), open addressing
    int n, cap, hmask, chunk, nchunks;
    volatile LONG next, done, refs;
} ework_t;
typedef int* (*peval_t)(int* out, uint8_t* country, void* target, uint8_t* prov, uint8_t* bld, uint8_t* buf, int* a, int* b, void* log);
typedef void (*ploop_t)(uint8_t* node, uint8_t* country, uint8_t* target, char flag);
static peval_t o_peval;
static ploop_t o_ploop, o_ppre;
static __thread int t_pmode;      // 1 record, 2 replay
static __thread ework_t* t_work;
static int g_prov;
static volatile LONG64 g_pl_runs, g_pl_calls, g_pl_hits, g_pl_miss, g_pl_ver, g_pl_bad, g_pl_tick, g_pl_own;
static volatile LONG64 g_pl_t_serial, g_pl_t_par;
static volatile LONG64 g_ab_t[2], g_ab_n[2], g_ab_max[2], g_ab_calls[2];   // A/B: wall per big evaluation
static volatile LONG64 g_ab_hist[2][6];          // < 2, 5, 10, 20, 50, >= 50 ms
// phases of the king update on this thread (stats): before + military child, other children, armies + tail
static __thread LONG64 t_ph[3];
static volatile LONG64 g_max_ph[3];               // phases of the current loop's slowest item
static LONG64 g_slow_ph[3], g_spiky_ph[3], g_spiky_n, g_spiky_slow;   // sums over loops (main thread)
#define NWALL 4096
static float g_walls[NWALL];

static LONG64 qpc(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

static cls_t* cls_of(void* vt) {
    unsigned h = (unsigned)(((uintptr_t)vt >> 3) * 2654435761u) & (NCLS - 1);
    for (int k = 0; k < NCLS; k++, h = (h + 1) & (NCLS - 1)) {
        void* v = g_cls[h].vt;
        if (v == vt) return &g_cls[h];
        if (!v && (InterlockedCompareExchangePointer(&g_cls[h].vt, vt, NULL) == NULL || g_cls[h].vt == vt)) return &g_cls[h];
    }
    return NULL;
}

static int g_aec;                                 // [parallel] ai_ae_cache (the AE cache experiment below)
static __thread unsigned t_node_run;               // AI task runs on this thread (army experiment: answers kept per run)
static volatile LONG g_ai_loop_n;                  // AI loops so far (bumped as each starts)
// same steps as FUN_1400e4890
static void run_node(uint8_t* node, void* ctx) {
    void** vt = *(void***)node;
    if (((char (*)(void*))vt[0x50 / 8])(node) && --*(int*)(node + 0x20) == 0) {
        vt = *(void***)node;
        t_node_run++;
        if (!g_stats) { ((void (*)(void*, void*))vt[0x60 / 8])(node, ctx); goto kids; }
        LONG64 saved = t_nested; t_nested = 0;
        int fm = g_fm_on && (uintptr_t)vt == (uintptr_t)g_exe + VT_FM;
        if (fm) for (int k = 0; k < NFM; k++) t_fm_n[k] = t_fm_t[k] = 0;
        LONG64 t0 = qpc();
        ((void (*)(void*, void*))vt[0x60 / 8])(node, ctx);
        LONG64 d = qpc() - t0;
        if (fm) fm_run_done(d);
        cls_t* c = cls_of(vt);
        if (c) {
            InterlockedIncrement64(&c->runs);
            InterlockedAdd64(&c->self, d - t_nested);
            for (LONG64 m = c->max; d > m && InterlockedCompareExchange64(&c->max, d, m) != m; m = c->max) {}
            if (d > g_spike) InterlockedIncrement(&c->spikes);
        }
        t_nested = saved + d;
    }
kids:
    for (uint8_t** q = *(uint8_t***)(node + 0x40); q;) {
        uint8_t* child = q[0];
        q = (uint8_t**)q[2];
        ((void (*)(void*, void*))(*(void***)child)[0x58 / 8])(child, ctx);
    }
}

// ---- split experiment ----
static void run_job(job_t* j);
static void eval_ticket(job_t* j);
static void work_release(ework_t* w) { if (InterlockedDecrement(&w->refs) == 0) { free(w->calls); free(w->hash); free(w); } }
// a queued job that never ran: give back what it holds
static void job_discard(job_t* j) {
    if (j->run == eval_ticket && j->state == 0 && j->arg) work_release((ework_t*)j->arg);
    if (InterlockedDecrement(&j->refs) == 0) free(j);
}
static void run_job(job_t* j) {
    for (int i = 0; i < j->n; i++) ((void (*)(void*, void*))(*(void***)j->nodes[i])[0x58 / 8])(j->nodes[i], j->ctx);
}
static void job_release(job_t* j) { if (InterlockedDecrement(&j->refs) == 0) free(j); }
static void job_finish(job_t* j) {
    AcquireSRWLockExclusive(&g_done_lock);
    j->state = 2;
    ReleaseSRWLockExclusive(&g_done_lock);
    WakeAllConditionVariable(&g_done_cv);
}

// Takes one posted job and runs it; 1 if there was one. Any thread outside a king update.
static int help_one(void) {
    if (!g_qn) return 0;
    job_t* j = NULL;
    AcquireSRWLockExclusive(&g_qlock);
    if (g_qn) j = g_q[--g_qn];
    ReleaseSRWLockExclusive(&g_qlock);
    if (!j) return 0;
    int ran = 0;
    if (InterlockedCompareExchange(&j->state, 1, 0) == 0) {
        split_t saved = t_split; memset(&t_split, 0, sizeof t_split);
        ework_t* sw = t_work; int sm = t_pmode; t_work = NULL; t_pmode = 0;
        j->run(j); job_finish(j); ran = 1;
        t_work = sw; t_pmode = sm;
        t_split = saved;
        if (j->run == run_job) InterlockedIncrement64(&g_by_helper);
    }
    job_release(j);
    return ran;
}

// parallel.c: the main thread waiting for a loop's last items
int eb_ai_in_loop(void) { return g_ai_loop_active; }       // guard.c: the parallel AI loop is running (0 when it is not hooked)
static int ai_help(void) { return g_ai_loop_active && help_one(); }

// parallel.c: a worker that found no more items; in the AI loop it stays to take army jobs until the
// loop's items are all complete
static int ai_linger(uint8_t* ctx) {
    if (!g_ai_loop_active) return 0;
    int end = *(int*)(ctx + 0xa8);
    while (g_ai_loop_active && **(volatile int**)(ctx + 0x48) < end)
        if (!help_one()) WaitForSingleObject(g_qsem, 1);
    return 1;
}

static void post(split_t* s) {
    int n = 0;
    for (uint8_t** q = *(uint8_t***)(s->king + K_ARMIES); q; q = (uint8_t**)q[2]) n++;
    if (n < g_cfg.ai_split_min) return;
    job_t* j = (job_t*)malloc(sizeof(job_t) + (size_t)n * sizeof(void*));
    if (!j) return;
    j->run = run_job; j->arg = NULL;
    j->ctx = s->ctx; j->state = 0; j->refs = 2; j->n = 0;
    for (uint8_t** q = *(uint8_t***)(s->king + K_ARMIES); q && j->n < n; q = (uint8_t**)q[2]) j->nodes[j->n++] = q[0];
    if (s->ctx) InterlockedIncrement(&g_ctx_active[stripe(s->ctx)]);
    AcquireSRWLockExclusive(&g_qlock);
    int queued = g_qn < QCAP;
    if (queued) g_q[g_qn++] = j;
    ReleaseSRWLockExclusive(&g_qlock);
    if (!queued) j->refs = 1;   // the king's thread runs it when it gets there
    else ReleaseSemaphore(g_qsem, 1, NULL);
    s->job = j; s->next = 0;
    InterlockedIncrement64(&g_jobs); InterlockedAdd64(&g_job_nodes, n);
}

static void join(split_t* s) {
    job_t* j = s->job;
    if (InterlockedCompareExchange(&j->state, 1, 0) == 0) {
        split_t saved = *s; memset(&t_split, 0, sizeof t_split);
        run_job(j); job_finish(j);
        t_split = saved;
        InterlockedIncrement64(&g_inline);
    } else {
        if (j->state != 2) InterlockedIncrement64(&g_waited);
        AcquireSRWLockExclusive(&g_done_lock);
        while (j->state != 2) SleepConditionVariableSRW(&g_done_cv, &g_done_lock, INFINITE, 0);
        ReleaseSRWLockExclusive(&g_done_lock);
    }
    if (j->ctx) InterlockedDecrement(&g_ctx_active[stripe(j->ctx)]);
}

static void hk_node(uint8_t* node, void* ctx) {
    split_t* s = &t_split;
    if (s->king) {
        if (node == s->first) {                   // the military child: run it, then post the armies
            s->first = NULL;
            run_node(node, ctx);
            s->t1 = qpc();
            if (g_split) post(s);
            return;
        }
        if (!s->t2) {
            uint8_t** head = *(uint8_t***)(s->king + K_ARMIES);
            if (head && node == head[0]) s->t2 = qpc();
        }
        if (s->job && s->next < s->job->n && node == s->job->nodes[s->next]) {   // the original's army loop
            if (s->next == 0) join(s);
            s->next++;
            return;
        }
    }
    run_node(node, ctx);
}

static void hk_king(uint8_t* king, void* ctx) {
    if (*(volatile uint8_t*)((uint8_t*)g_exe + RVA_FIXED_SEED_FLAG) ||
        *(uintptr_t*)king != (uintptr_t)g_exe + VT_KING) { o_king(king, ctx); return; }
    uint8_t** kids = *(uint8_t***)(king + K_KIDS);
    split_t saved = t_split;
    t_split.king = king; t_split.ctx = ctx; t_split.job = NULL; t_split.next = 0;
    t_split.first = (kids && *(uintptr_t*)kids[0] == (uintptr_t)g_exe + VT_MILITARY) ? kids[0] : NULL;
    t_split.t1 = t_split.t2 = 0;
    t_split.t0 = g_stats ? qpc() : 0;
    o_king(king, ctx);
    job_t* j = t_split.job;
    if (j) {
        if (t_split.next == 0) join(&t_split);   // the army loop was not reached (should not happen)
        job_release(j);
    }
    if (g_stats) {
        LONG64 t3 = qpc(), t1 = t_split.t1 ? t_split.t1 : t_split.t0, t2 = t_split.t2 ? t_split.t2 : t3;
        t_ph[0] += t1 - t_split.t0; t_ph[1] += t2 - t1; t_ph[2] += t3 - t2;
    }
    t_split = saved;
}

static char hk_cget(void* map, uint8_t* out, uint64_t a, uint64_t b) {
    int s = stripe(map);
    if (!map || !g_ctx_active[s]) return o_cget(map, out, a, b);
    AcquireSRWLockShared(&g_ctx_lock[s]);
    char r = o_cget(map, out, a, b);
    ReleaseSRWLockShared(&g_ctx_lock[s]);
    return r;
}
static void hk_cput(void* map, uint64_t a, uint64_t b, uint8_t v) {
    int s = stripe(map);
    if (!map || !g_ctx_active[s]) { o_cput(map, a, b, v); return; }
    AcquireSRWLockExclusive(&g_ctx_lock[s]);
    o_cput(map, a, b, v);
    ReleaseSRWLockExclusive(&g_ctx_lock[s]);
}

// ---- province evaluation experiment ----
static void eval_chunks(ework_t* w, volatile LONG64* counter) {
    for (;;) {
        LONG i = InterlockedIncrement(&w->next) - 1;
        if (i >= w->nchunks) break;
        int lo = i * w->chunk, hi = lo + w->chunk;
        if (hi > w->n) hi = w->n;
        for (int k = lo; k < hi; k++) {
            ecall_t* c = &w->calls[k];
            int a = c->a, b = c->b;
            o_peval(c->out, w->country, c->target, c->prov, c->bld, w->buf, &a, &b, NULL);
        }
        InterlockedIncrement64(counter);
        InterlockedIncrement(&w->done);
    }
}
static void eval_ticket(job_t* j) { ework_t* w = (ework_t*)j->arg; j->arg = NULL; eval_chunks(w, &g_pl_tick); work_release(w); }

static void post_ticket(ework_t* w) {
    job_t* j = (job_t*)malloc(sizeof(job_t));
    if (!j) return;
    InterlockedIncrement(&w->refs);
    j->run = eval_ticket; j->arg = w; j->ctx = NULL; j->state = 0; j->refs = 1; j->n = 0;
    AcquireSRWLockExclusive(&g_qlock);
    int queued = g_qn < QCAP;
    if (queued) g_q[g_qn++] = j;
    ReleaseSRWLockExclusive(&g_qlock);
    if (queued) ReleaseSemaphore(g_qsem, 1, NULL);
    else { work_release(w); free(j); }
}

static unsigned ehash(uint8_t* prov, uint8_t* bld, void* target, int a, int b) {
    uint64_t h = (uintptr_t)prov * 0x9E3779B97F4A7C15ull ^ (uintptr_t)bld * 0xC2B2AE3D27D4EB4Full ^ (uintptr_t)target * 0x165667B19E3779F9ull;
    h ^= ((uint64_t)(uint32_t)a << 32 | (uint32_t)b) * 0x27D4EB2F165667C5ull;
    return (unsigned)(h ^ h >> 29);
}
static void build_hash(ework_t* w) {
    int cap = 1;
    while (cap < 2 * w->n) cap <<= 1;
    w->hash = (int*)calloc((size_t)cap, sizeof(int));
    if (!w->hash) { w->bad = 1; return; }
    w->hmask = cap - 1;
    for (int i = 0; i < w->n; i++) {
        ecall_t* c = &w->calls[i];
        unsigned h = ehash(c->prov, c->bld, c->target, c->a, c->b) & w->hmask;
        while (w->hash[h]) h = (h + 1) & w->hmask;
        w->hash[h] = i + 1;
    }
}
static ecall_t* find_call(ework_t* w, uint8_t* prov, uint8_t* bld, void* target, int a, int b) {
    for (unsigned h = ehash(prov, bld, target, a, b) & w->hmask; w->hash[h]; h = (h + 1) & w->hmask) {
        ecall_t* c = &w->calls[w->hash[h] - 1];
        if (c->prov == prov && c->bld == bld && c->target == target && c->a == a && c->b == b) return c;
    }
    return NULL;
}
static volatile LONG g_pl_misslog;

static int* hk_peval(int* out, uint8_t* country, void* target, uint8_t* prov, uint8_t* bld, uint8_t* buf, int* a, int* b, void* log) {
    ework_t* w = t_work;
    if (w && !log && country == w->country) {
        if (t_pmode == 1) {                           // record
            if (!w->have_buf) { memcpy(w->buf, buf, PBUF); w->have_buf = 1; }
            else if (memcmp(w->buf, buf, PBUF_CMP)) w->bad = 1;
            if (w->n == w->cap) {
                int cap = w->cap ? w->cap * 2 : 1024;
                ecall_t* c = (ecall_t*)realloc(w->calls, (size_t)cap * sizeof(ecall_t));
                if (!c) { w->bad = 1; out[0] = out[1] = 0; return out; }
                w->calls = c; w->cap = cap;
            }
            ecall_t* c = &w->calls[w->n++];
            c->prov = prov; c->bld = bld; c->target = target; c->a = *a; c->b = *b;
            out[0] = out[1] = 0;
            return out;
        }
        if (t_pmode == 2 && !w->bad && w->hash) {        // replay
            ecall_t* c = memcmp(w->buf, buf, PBUF_CMP) ? NULL : find_call(w, prov, bld, target, *a, *b);
            if (!c && InterlockedIncrement(&g_pl_misslog) <= 12) {
                int same_pair = 0;
                for (int i = 0; i < w->n; i++) if (w->calls[i].prov == prov && w->calls[i].bld == bld) { same_pair = i + 1; break; }
                eb_log("ai prov: replay miss: province %p building %p target %p a %d b %d buf %s; noted %d calls%s",
                       (void*)prov, (void*)bld, target, *a, *b, memcmp(w->buf, buf, PBUF_CMP) ? "DIFFERS" : "same", w->n,
                       same_pair ? " (this pair was noted with other target/a/b)" : " (pair not noted)");
                if (same_pair) {
                    ecall_t* s = &w->calls[same_pair - 1];
                    eb_log("ai prov:   noted: target %p a %d b %d", s->target, s->a, s->b);
                }
            }
            if (c) {
                InterlockedIncrement64(&g_pl_hits);
                if (g_prov == 2) {
                    o_peval(out, country, target, prov, bld, buf, a, b, log);
                    InterlockedIncrement64(&g_pl_ver);
                    if ((out[0] != c->out[0] || out[1] != c->out[1]) && InterlockedIncrement64(&g_pl_bad) <= 20)
                        eb_log("ai prov: VERIFY MISMATCH province %p building %p: game {%d,%d}, precomputed {%d,%d}",
                               (void*)prov, (void*)bld, out[0], out[1], c->out[0], c->out[1]);
                }
                out[0] = c->out[0]; out[1] = c->out[1];
                return out;
            }
        }
        if (t_pmode == 2) InterlockedIncrement64(&g_pl_miss);
    }
    return o_peval(out, country, target, prov, bld, buf, a, b, log);
}

static void hk_ppre(uint8_t* node, uint8_t* country, uint8_t* target, char flag) {
    if (t_pmode == 1) return;                         // record pass: it runs in the replay
    ework_t* sw = t_work; int sm = t_pmode; t_work = NULL; t_pmode = 0;
    o_ppre(node, country, target, flag);
    t_work = sw; t_pmode = sm;
}

static void hk_ploop(uint8_t* node, uint8_t* country, uint8_t* target, char flag) {
    if (t_work) {                                     // nested: plain
        ework_t* sw = t_work; int sm = t_pmode; t_work = NULL; t_pmode = 0;
        o_ploop(node, country, target, flag);
        t_work = sw; t_pmode = sm;
        return;
    }
    if (*(volatile uint8_t*)((uint8_t*)g_exe + RVA_FIXED_SEED_FLAG) || *(int*)(country + 0x1344) < g_cfg.ai_prov_min) {
        o_ploop(node, country, target, flag);
        return;
    }
    if (g_prov == 3 && !(g_loops & 1)) {             // A/B: the original side
        LONG64 a0 = qpc();
        o_ploop(node, country, target, flag);
        LONG64 d = qpc() - a0;
        double ms = d * g_ms;
        InterlockedAdd64(&g_ab_t[0], d); InterlockedIncrement64(&g_ab_n[0]);
        for (LONG64 m = g_ab_max[0]; d > m && InterlockedCompareExchange64(&g_ab_max[0], d, m) != m; m = g_ab_max[0]) {}
        InterlockedIncrement64(&g_ab_hist[0][ms < 2 ? 0 : ms < 5 ? 1 : ms < 10 ? 2 : ms < 20 ? 3 : ms < 50 ? 4 : 5]);
        return;
    }
    ework_t* w = (ework_t*)calloc(1, sizeof *w);
    if (!w) { o_ploop(node, country, target, flag); return; }
    w->country = country; w->refs = 1;
    LONG64 t0 = qpc();
    uint8_t best[0x20];
    memcpy(best, node + N_BEST, sizeof best);
    t_work = w; t_pmode = 1;
    o_ploop(node, country, target, flag);
    t_pmode = 0;
    memcpy(node + N_BEST, best, sizeof best);
    LONG64 t1 = qpc();
    if (!w->bad && w->n) {
        w->chunk = w->n / 48;
        if (w->chunk < 8) w->chunk = 8;
        w->nchunks = (w->n + w->chunk - 1) / w->chunk;
        if (g_ai_loop_active) for (int k = 1; k < w->nchunks && k < 12; k++) post_ticket(w);
        eval_chunks(w, &g_pl_own);
        for (int spins = 0; w->done < w->nchunks; spins++) { if (spins < 2000) YieldProcessor(); else SwitchToThread(); }
    }
    if (!w->bad && w->n) build_hash(w);
    LONG64 t2 = qpc();
    t_pmode = 2;
    o_ploop(node, country, target, flag);
    t_pmode = 0; t_work = NULL;
    LONG64 t3 = qpc();
    InterlockedAdd64(&g_pl_t_serial, (t1 - t0) + (t3 - t2));
    InterlockedAdd64(&g_pl_t_par, t2 - t1);
    if (g_prov == 3) {
        LONG64 d = t3 - t0;
        double ms = d * g_ms;
        InterlockedAdd64(&g_ab_t[1], d); InterlockedIncrement64(&g_ab_n[1]); InterlockedAdd64(&g_ab_calls[1], w->n);
        for (LONG64 m = g_ab_max[1]; d > m && InterlockedCompareExchange64(&g_ab_max[1], d, m) != m; m = g_ab_max[1]) {}
        InterlockedIncrement64(&g_ab_hist[1][ms < 2 ? 0 : ms < 5 ? 1 : ms < 10 ? 2 : ms < 20 ? 3 : ms < 50 ? 4 : 5]);
    }
    InterlockedIncrement64(&g_pl_runs); InterlockedAdd64(&g_pl_calls, w->n);
    work_release(w);
}

// ---- AE cache experiment ----
#define RVA_AEP     0x388880u     // FUN_140388880(taker, int* out, int* value, province, char flag, p6, observer tag, cache)
#define RVA_AECALC  0x385d40u     // FUN_140385d40(taker, int* out, args*) - the AE of one province
#define RVA_ETS_GET 0x0df320u     // FUN_1400df320(ets, bool* found) -> this thread's element
#define RVA_AE_ETS  0x1fd1ab8u    // the AE cache slot (TBB enumerable_thread_specific<map*>)
#define RVA_AE_UNIT 0x1c337fcu    // int 100000 (.rdata): the value the cached path computes with
static const uint8_t sig_aep[16]    = { 0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x48,0x89,0x7c,0x24,0x18,0x55 };
static const uint8_t sig_aecalc[16] = { 0x48,0x89,0x54,0x24,0x10,0x48,0x89,0x4c,0x24,0x08,0x55,0x53,0x56,0x57,0x41,0x54 };
static const uint8_t sig_etsget[16] = { 0x40,0x53,0x55,0x57,0x41,0x54,0x41,0x56,0x48,0x83,0xec,0x30,0x4c,0x8b,0xf1,0x48 };
typedef int* (*aep_t)(uint8_t* taker, int* out, int* value, uint8_t* prov, uint8_t flag, uint64_t p6, uint64_t obs, void* cache);
typedef void (*aecalc_t)(uint8_t* taker, int* out, void* args);
typedef void** (*etsget_t)(void* ets, uint8_t* found);
static aep_t o_aep;
static aecalc_t g_aecalc;
static etsget_t g_etsget;
static void* volatile g_ae_cache;                 // the current AI loop's shared AE map (NULL outside the loop)
static volatile LONG g_ae_gen = 1;                // AI loop number: the per-thread tables are valid for one
static int g_ae_unit;                             // 100000: the value the game's map entries are computed with
typedef struct { uint64_t key; int32_t v; uint32_t gen; int32_t val; } aent_t;   // val: the value v was computed with
typedef struct { aent_t* e; uint32_t mask, n, gen; void** slot; LONG64 hit, calc, neg, ver, bad; int ui; } atab_t;
static __thread atab_t t_at;
static volatile LONG64 g_ae_hit, g_ae_calc, g_ae_neg, g_ae_ver, g_ae_bad, g_ae_badlog, g_ae_nomem;
static volatile LONG g_ae_maxn;
// Outside the AI loop (the war view, the peace windows, the AI's own peace checks on the main thread) the
// game has no map for the whole loop: FUN_14098c0e0 (fill a deal) puts an unlocked map of its own in the
// slot for its run, FUN_1401bf150 (acceptance) runs without one and computes every AE again
// (FUN_140385d40 with the real value). Report 29: 2.5 - 4.3 s per war view refresh in a big war. On the
// main thread one top-level call of either is one generation of t_ut: the same AE lookups and sums in C
// as in the AI loop; without a map the entries are computed with the real value, as the game does.
static __thread atab_t t_ut;
static __thread uint32_t t_ugen;
static __thread uint32_t t_lpscope;               // the land search memo's generation: one per top-level call (main or AI)
static __thread int t_ui, t_ui_block;             // depth inside such a call; t_ui_block: the game's code (A/B test)
static __thread uintptr_t t_ui_frame;             // the top-level hook's frame: a call from above it is not inside
static inline int ui_in(void) {                   // (a call left without its hook returning, e.g. an exception)
    if (!t_ui) return 0;
    if ((uintptr_t)__builtin_frame_address(0) < t_ui_frame) return 1;
    t_ui = 0;
    return 0;
}
static volatile LONG64 g_ui_hit, g_ui_calc, g_ui_neg, g_ui_ver, g_ui_bad, g_ui_n[2], g_ui_t[2], g_ui_sums;
static LONG64 g_ui_calc_t, g_ui_sum_t;            // testing ([test] warview): time in FUN_140385d40 and in ae_sum
static volatile LONG g_ui_maxn;
#define RVA_AEALL   0x38a530u     // FUN_14038a530(taker, int64* out, int* value, province, deal items, cache): AE sum over all countries
#define RVA_AE_ZERO   0x23582d0u  // int64 the sum starts from
#define RVA_AE_DEFS   0x233ff08u  // defines object (lazy); +0x190 -> AE defines, +0x50 int factor
#define RVA_GAMESTATE 0x233fe78u  // +0x1d08 countries (ptr array), +0x1d14 count
#define RVA_COUNTRIES 0x233d8d0u  // +0x118 country by index
#define RVA_AE_LIMIT  0x1fc2338u  // int define compared with opinion*1000 + AE
#define RVA_OPIN_AE   0x383db0u   // short FUN_140383db0(country, tag, defs)
#define RVA_OPIN      0x3839b0u   // short FUN_1403839b0(country, tag, std::string* log, char)
#define RVA_ALLIED    0x2f1590u   // char FUN_1402f1590(country, tag, 0)
#define RVA_STRENGTH  0x93ca70u   // FUN_14093ca70(country+0x28c0, int64* out, char)
static const uint8_t sig_opin_ae[16] = { 0x48,0xc1,0xea,0x20,0x4d,0x8b,0xd0,0x0f,0xbf,0xc2,0x4c,0x8d,0x0c,0xc0,0x48,0x8b };
static const uint8_t sig_opin[16]    = { 0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x54,0x24,0x10,0x55,0x56,0x57,0x41,0x54,0x41 };
static const uint8_t sig_allied[16]  = { 0x40,0x53,0x48,0x83,0xec,0x20,0x49,0x8b,0xd8,0x4c,0x8b,0xd2,0x4c,0x8b,0xc9,0xe8 };
static const uint8_t sig_strength[16]= { 0x48,0x89,0x54,0x24,0x10,0x48,0x89,0x4c,0x24,0x08,0x55,0x53,0x56,0x57,0x41,0x54 };
typedef int (*opin_ae_t)(uint8_t* c, uint64_t tag, uint8_t* defs);
typedef int (*opin_t)(uint8_t* c, uint64_t tag, void* log, uint8_t p4);
typedef uint8_t (*allied_t)(uint8_t* c, uint64_t tag, uint64_t p3);
typedef void (*strength_t)(uint8_t* p, int64_t* out, uint8_t p3);
static opin_ae_t g_opin_ae;
static opin_t g_opin;
static allied_t g_allied;
static strength_t g_strength;
static const uint8_t sig_aeall[16]  = { 0x48,0x89,0x5c,0x24,0x08,0x4c,0x89,0x4c,0x24,0x20,0x4c,0x89,0x44,0x24,0x18,0x55 };
typedef int64_t* (*aeall_t)(uint8_t* taker, int64_t* out, int* value, uint8_t* prov, void** deal, uint64_t cache);
static aeall_t o_aeall;
static __thread int t_ae_orig, t_ae_in;           // the game's code with the game's map (A/B side A, verify)
static __thread unsigned t_ae_flip;
static volatile LONG64 g_ae_ab_t[2], g_ae_ab_n[2], g_ae_ab_max[2];   // A/B: time of the AE sums per side
static volatile LONG64 g_ae_sums, g_ae_sum_ver, g_ae_sum_bad, g_ae_sum_badlog;

static void ae_flush(atab_t* t) {
    if (t->ui) {
        g_ui_hit += t->hit; g_ui_calc += t->calc; g_ui_neg += t->neg; g_ui_ver += t->ver; g_ui_bad += t->bad;
        if ((LONG)t->n > g_ui_maxn) g_ui_maxn = (LONG)t->n;
        t->hit = t->calc = t->neg = t->ver = t->bad = 0;
        return;
    }
    if (t->hit) InterlockedAdd64(&g_ae_hit, t->hit);
    if (t->calc) InterlockedAdd64(&g_ae_calc, t->calc);
    if (t->neg) InterlockedAdd64(&g_ae_neg, t->neg);
    if (t->ver) InterlockedAdd64(&g_ae_ver, t->ver);
    if (t->bad) InterlockedAdd64(&g_ae_bad, t->bad);
    for (LONG m = g_ae_maxn; (LONG)t->n > m && InterlockedCompareExchange(&g_ae_maxn, (LONG)t->n, m) != m; m = g_ae_maxn) {}
    t->hit = t->calc = t->neg = t->ver = t->bad = 0;
}
// a new AI loop: entries of older loops count as empty
static int ae_reset(atab_t* t, LONG gen) {
    ae_flush(t);
    if (!t->e) {
        t->e = (aent_t*)calloc(1u << 14, sizeof(aent_t));
        if (!t->e) { InterlockedIncrement64(&g_ae_nomem); return 0; }
        t->mask = (1u << 14) - 1;
    }
    uint8_t found;
    if (!t->slot) t->slot = g_etsget((uint8_t*)g_exe + RVA_AE_ETS, &found);
    t->n = 0; t->gen = (uint32_t)gen;
    return 1;
}
static inline uint32_t ae_hash(uint64_t k, int32_t val) {
    k = (k ^ (uint64_t)(uint32_t)val << 20) * 0x9E3779B97F4A7C15ull;
    return (uint32_t)(k >> 40) ^ (uint32_t)(k >> 13);
}
static int ae_grow(atab_t* t) {
    uint32_t cap = (t->mask + 1) * 2;
    if (cap > (1u << 22)) return 0;
    aent_t* e = (aent_t*)calloc(cap, sizeof(aent_t));
    if (!e) { InterlockedIncrement64(&g_ae_nomem); return 0; }
    for (uint32_t i = 0; i <= t->mask; i++) {
        aent_t* o = &t->e[i];
        if (o->gen != t->gen) continue;
        uint32_t j = ae_hash(o->key, o->val) & (cap - 1);
        while (e[j].gen == t->gen) j = (j + 1) & (cap - 1);
        e[j] = *o;
    }
    free(t->e); t->e = e; t->mask = cap - 1;
    return 1;
}
// FUN_140385d40 as FUN_140388880 calls it (args laid out as its local at rbp-0x20): with value 100000 on
// the map path, with the real value without a map
static int ae_calc(uint8_t* taker, uint8_t* prov, uint8_t flag, uint64_t obs, int32_t val) {
    uint8_t args[0x20];
    memset(args, 0, sizeof args);
    *(uint8_t**)(args + 0x00) = prov;
    *(uint64_t*)(args + 0x08) = 0;                                  // p6 (tooltip text): none
    *(int32_t*)(args + 0x10) = val;
    memcpy(args + 0x14, &obs, 8);
    args[0x1c] = flag;
    int v = 0;
    g_aecalc(taker, &v, args);
    return v;
}
// the game's scaling of a cached value: (int)(((value * v) / 1000 as int) * 1000 / 100000)
static inline int ae_scale(int value, int v) {
    int64_t q1 = ((int64_t)value * (int64_t)v) / 1000;
    int64_t q2 = ((int64_t)(int32_t)q1 * 1000) / 100000;
    return (int)q2;
}
// the AE of one province (FUN_140385d40 with value val) from this thread's table
static inline int ae_value(atab_t* t, uint8_t* taker, uint8_t* prov, uint8_t flag, uint64_t obs, int32_t val) {
    uint64_t key = (uint64_t)*(uint16_t*)(taker + 0x24) | (uint64_t)(uint16_t)(obs >> 32) << 16 |
                   (uint64_t)(uint16_t)*(uint32_t*)(prov + 0x20) << 32 | (uint64_t)flag << 48;
    uint32_t h = ae_hash(key, val), i = h & t->mask;
    for (;; i = (i + 1) & t->mask) {
        aent_t* e = &t->e[i];
        if (e->gen != t->gen) break;
        if (e->key == key && e->val == val) { t->hit++; return e->v; }
    }
    LONG64 c0 = t->ui && g_cfg.test_warview ? qpc() : 0;
    int v = ae_calc(taker, prov, flag, obs, val);     // not there: compute
    if (c0) g_ui_calc_t += qpc() - c0;
    if (v < 0) { t->neg++; return v; }                // the game recomputes negative values every time too
    t->calc++;
    if ((t->n + 1) * 2 > t->mask + 1 && !ae_grow(t)) return v;
    for (i = ae_hash(key, val) & t->mask; t->e[i].gen == t->gen; i = (i + 1) & t->mask) {}
    t->e[i].key = key; t->e[i].v = v; t->e[i].gen = t->gen; t->e[i].val = val; t->n++;
    return v;
}
// FUN_140388880: value <= 0 gives 0; with a map the scaled value at 100000, without one (direct) the
// value computed with the real value
static inline int ae_prov(atab_t* t, uint8_t* taker, int value, uint8_t* prov, uint8_t flag, uint64_t obs, int direct) {
    if (value <= 0) return 0;
    return direct ? ae_value(t, taker, prov, flag, obs, value) : ae_scale(value, ae_value(t, taker, prov, flag, obs, g_ae_unit));
}
static __thread int t_wv;                         // inside the war view test (testing only)
static int g_wish;                                // [parallel] ai_deal_memo (the wish lists far below; rows in ae_sum)
static volatile LONG64 g_wv_aep, g_wv_aep_cache, g_wv_aep_slot, g_wv_aep_none;
static int* ui_aep(uint8_t* taker, int* out, int* value, uint8_t* prov, uint8_t flag, uint64_t p6, uint64_t obs, void* cache) {
    if (p6 || t_ae_orig) return o_aep(taker, out, value, prov, flag, p6, obs, cache);
    atab_t* t = &t_ut;
    int r = ae_prov(t, taker, *value, prov, flag, obs, !cache && !*t->slot);
    if (g_aec == 2 && *value > 0) {
        int g = 0;
        o_aep(taker, &g, value, prov, flag, p6, obs, cache);
        t->ver++;
        if (g != r) {
            t->bad++;
            if (InterlockedIncrement64(&g_ae_badlog) <= 20)
                eb_log("ai ae: VERIFY MISMATCH outside the AI loop (%s) taker %.3s province %d observer %d flag %d value %d: game %d, table %d",
                       !cache && !*t->slot ? "no map" : "map", (char*)(taker + 0x20), (int)(uint16_t)*(uint32_t*)(prov + 0x20),
                       (int)(int16_t)(obs >> 32), flag, *value, g, r);
        }
    }
    *out = r;
    return out;
}
static int* hk_aep(uint8_t* taker, int* out, int* value, uint8_t* prov, uint8_t flag, uint64_t p6, uint64_t obs, void* cache) {
    if (t_wv) {                                       // which path the game takes outside the AI loop
        g_wv_aep++;
        if (cache) g_wv_aep_cache++;
        else { uint8_t found; void** s = g_etsget((uint8_t*)g_exe + RVA_AE_ETS, &found); if (s && *s) g_wv_aep_slot++; else g_wv_aep_none++; }
    }
    if (ui_in()) return ui_aep(taker, out, value, prov, flag, p6, obs, cache);
    void* shared = g_ae_cache;
    if (p6 || !shared || t_ae_orig) return o_aep(taker, out, value, prov, flag, p6, obs, cache);
    atab_t* t = &t_at;
    LONG gen = g_ae_gen;
    if (t->gen != (uint32_t)gen && !ae_reset(t, gen)) return o_aep(taker, out, value, prov, flag, p6, obs, cache);
    if (!cache) cache = *t->slot;
    if (cache != shared) return o_aep(taker, out, value, prov, flag, p6, obs, cache);
    if (*value <= 0) { *out = 0; return out; }
    int v = ae_value(t, taker, prov, flag, obs, g_ae_unit);
    int r = ae_scale(*value, v);
    if (g_aec == 2) {
        int g = 0;
        o_aep(taker, &g, value, prov, flag, p6, obs, cache);
        t->ver++;
        if (g != r) {
            t->bad++;
            if (InterlockedIncrement64(&g_ae_badlog) <= 20)
                eb_log("ai ae: VERIFY MISMATCH taker %.3s province %d observer %d flag %d value %d: game %d, table %d (v %d)",
                       (char*)(taker + 0x20), (int)(uint16_t)*(uint32_t*)(prov + 0x20), (int)(int16_t)(obs >> 32), flag, *value, g, r, v);
        }
    }
    *out = r;
    return out;
}
// The deal's part of a sum: FUN_14099fc60 sums the AE of a candidate item over its own entries followed by
// the entries of the items already in the deal (FUN_140979340), each again for every country; FUN_14098b670
// does that for every candidate with the same deal. The common last entries (found by comparing with the
// previous call's) are summed once per country (32-bit adds as the game's) while they, the taker and the
// table generation stay the same.
typedef struct {
    atab_t* t; uint32_t gen, id; uint8_t* taker; int direct, ncap;
    size_t pnb, pcap, snb, scap; uint8_t* prev; uint8_t* suf; int32_t* d; uint32_t* stamp;
} dmemo_t;
static __thread dmemo_t t_dm;
static volatile LONG64 g_dm_new, g_dm_reuse;
static int dm_room(uint8_t** p, size_t* cap, size_t n) {
    if (n <= *cap) return 1;
    uint8_t* q = (uint8_t*)realloc(*p, n * 2);
    if (!q) return 0;
    *p = q; *cap = n * 2;
    return 1;
}
// the number of bytes at the end of e0..e0+nb that are the kept common part (0: none)
static size_t dm_begin(dmemo_t* m, atab_t* t, uint8_t* taker, int direct, const uint8_t* e0, size_t nb, int ncl) {
    if (ncl > m->ncap) {
        int n = ncl + 256;
        int32_t* d = (int32_t*)malloc((size_t)n * sizeof *d);
        uint32_t* s = (uint32_t*)calloc((size_t)n, sizeof *s);
        if (!d || !s) { free(d); free(s); m->t = NULL; return 0; }
        free(m->d); free(m->stamp); m->d = d; m->stamp = s; m->ncap = n; m->id = 0; m->snb = 0; m->t = NULL;
    }
    int same = m->t == t && m->gen == t->gen && m->taker == taker && m->direct == direct;
    size_t keep = 0;
    if (same && m->snb && nb >= m->snb && !memcmp(e0 + nb - m->snb, m->suf, m->snb)) { keep = m->snb; InterlockedIncrement64(&g_dm_reuse); }
    else {
        size_t c = 0;
        if (same) while (c + 0x30 <= nb && c + 0x30 <= m->pnb && !memcmp(e0 + nb - c - 0x30, m->prev + m->pnb - c - 0x30, 0x30)) c += 0x30;
        m->snb = 0;
        if (c && dm_room(&m->suf, &m->scap, c)) {
            memcpy(m->suf, e0 + nb - c, c); m->snb = keep = c;
            if (++m->id == 0) { memset(m->stamp, 0, (size_t)m->ncap * sizeof *m->stamp); m->id = 1; }
            InterlockedIncrement64(&g_dm_new);
        }
    }
    if (!dm_room(&m->prev, &m->pcap, nb)) { m->t = NULL; return 0; }
    memcpy(m->prev, e0, nb); m->pnb = nb;
    m->t = t; m->gen = t->gen; m->taker = taker; m->direct = direct;
    return keep;
}
// the AE of deal entries a..b for one observer (FUN_140388b40's loop)
static inline uint32_t ae_items(atab_t* t, uint8_t* taker, uint8_t** byidx, uint8_t* a, uint8_t* b, uint64_t otag, int direct) {
    uint32_t s = 0;
    for (uint8_t* e = a; e != b; e += 0x30) {
        uint8_t* pv;
        if (e[0x1f]) pv = *(uint8_t**)(byidx[(int16_t)(*(uint64_t*)(e + 0x18) >> 32)] + 0x14b0);
        else if (!e[0x27] && *(uint8_t**)(e + 8)) pv = *(uint8_t**)(e + 8);
        else continue;
        s += (uint32_t)ae_prov(t, taker, *(int*)(e + 0x28), pv, e[0x2c], otag, direct);
    }
    return s;
}
// With [parallel] ai_deal_memo: rows. A fill sums the AE of the same candidate provinces again in every round,
// each time one table lookup per country and deal entry (13 million lookups for the 16 war views of the SPAIN
// test, 18% of the time once the wish lists are kept). A row is the AE of one (taker, province, flag, value)
// for every country of the game's list, filled as the sums ask for it; rows live as long as the table's
// generation (one top-level call outside the AI loop, one AI loop inside it). AROW_NONE = not asked yet (a
// value that happens to be equal to it is simply computed every time).
#define AROW_MAX  2048
#define AROW_SLOTS 4096
#define AROW_NONE ((int32_t)0x80000000)
typedef struct { uint8_t* taker; uint8_t* prov; int32_t value; uint32_t id; uint16_t row; uint8_t flag; } arkey_t;
static __thread arkey_t* t_ak;
static __thread int32_t* t_av;
static __thread int t_av_cap, t_av_ncl, t_av_rows, t_av_direct;
static __thread uint32_t t_av_id, t_av_gen;
static __thread atab_t* t_av_t;
static volatile LONG g_av_maxrows;
static int arow_begin(atab_t* t, int direct, int ncl) {
    if (t_av_t == t && t_av_gen == t->gen && t_av_direct == direct && t_av_ncl == ncl) return 1;
    if (ncl <= 0) return 0;
    if (!t_ak && !(t_ak = (arkey_t*)calloc(AROW_SLOTS, sizeof *t_ak))) return 0;
    if (ncl > t_av_cap) {
        int32_t* v = (int32_t*)realloc(t_av, (size_t)AROW_MAX * (size_t)(ncl + 64) * sizeof *v);
        if (!v) { t_av_t = NULL; return 0; }
        t_av = v; t_av_cap = ncl + 64;
    }
    for (LONG m = g_av_maxrows; t_av_rows > m && InterlockedCompareExchange(&g_av_maxrows, t_av_rows, m) != m; m = g_av_maxrows) {}
    t_av_t = t; t_av_gen = t->gen; t_av_direct = direct; t_av_ncl = ncl; t_av_rows = 0;
    if (++t_av_id == 0) { memset(t_ak, 0, AROW_SLOTS * sizeof *t_ak); t_av_id = 1; }
    return 1;
}
// the row of (taker, province, flag, value); NULL when all rows are in use
static int32_t* arow(uint8_t* taker, uint8_t* prov, uint8_t flag, int32_t value) {
    uint64_t h = (uintptr_t)prov * 0x9E3779B97F4A7C15ull ^ (uintptr_t)taker * 0xC2B2AE3D27D4EB4Full ^ ((uint64_t)(uint32_t)value << 8 | flag) * 0x165667B19E3779F9ull;
    uint32_t i = ((uint32_t)(h >> 32) ^ (uint32_t)h) & (AROW_SLOTS - 1);
    for (; t_ak[i].id == t_av_id; i = (i + 1) & (AROW_SLOTS - 1)) {
        arkey_t* k = &t_ak[i];
        if (k->prov == prov && k->taker == taker && k->value == value && k->flag == flag) return t_av + (size_t)k->row * (size_t)t_av_cap;
    }
    if (t_av_rows == AROW_MAX) return NULL;
    int32_t* r = t_av + (size_t)t_av_rows * (size_t)t_av_cap;
    for (int c = 0; c < t_av_ncl; c++) r[c] = AROW_NONE;
    arkey_t* k = &t_ak[i];
    k->taker = taker; k->prov = prov; k->value = value; k->flag = flag; k->id = t_av_id; k->row = (uint16_t)t_av_rows++;
    return r;
}
typedef struct { int32_t* r; uint8_t* pv; int32_t v; uint8_t fl; } arent_t;
#define AROW_ENT 48              // deal entries of one sum that go through rows; a sum with more does it the old way
// FUN_14038a530 + FUN_140388b40 in C on table t (see the header); direct: no map (the real values); 0 = not applicable
static int ae_sum(atab_t* t, int direct, uint8_t* taker, int64_t* out, int value, uint8_t* prov, void** deal) {
    uint8_t* b = (uint8_t*)g_exe;
    uint8_t* dobj = *(uint8_t* volatile*)(b + RVA_AE_DEFS);
    if (!dobj) return 0;                              // not created yet: the game's code creates it
    uint8_t* defs = *(uint8_t**)(dobj + 0x190);
    uint8_t* gs = *(uint8_t**)(b + RVA_GAMESTATE);
    uint8_t** cl = *(uint8_t***)(gs + 0x1d08);
    int ncl = *(int*)(gs + 0x1d14);
    uint8_t** byidx = *(uint8_t***)(*(uint8_t**)(b + RVA_COUNTRIES) + 0x118);
    uint64_t ttag = *(uint64_t*)(taker + 0x20);
    int tidx = (int16_t)(ttag >> 32);
    uint8_t* e0 = deal ? (uint8_t*)deal[0] : NULL;
    uint8_t* e1 = deal ? (uint8_t*)deal[1] : NULL;
    dmemo_t* dm = &t_dm;
    size_t keep = e1 - e0 >= 2 * 0x30 ? dm_begin(dm, t, taker, direct, e0, (size_t)(e1 - e0), ncl) : 0;
    uint8_t* em = e1 - keep;                          // e0..em summed here, em..e1 the kept common part
    int32_t* rp = NULL;                               // the row of `prov`; er[0..ne): the rows of the entries e0..em
    arent_t er[AROW_ENT];
    int ne = -1;
    if (g_wish && arow_begin(t, direct, ncl)) {
        if (value > 0) rp = arow(taker, prov, 1, value);
        ne = 0;
        for (uint8_t* e = e0; e != em; e += 0x30) {
            uint8_t* pv;
            if (e[0x1f]) pv = *(uint8_t**)(byidx[(int16_t)(*(uint64_t*)(e + 0x18) >> 32)] + 0x14b0);
            else if (!e[0x27] && *(uint8_t**)(e + 8)) pv = *(uint8_t**)(e + 8);
            else continue;
            int32_t v = *(int*)(e + 0x28);
            if (v <= 0) continue;                     // such an entry adds 0
            int32_t* r = ne < AROW_ENT ? arow(taker, pv, e[0x2c], v) : NULL;
            if (!r) { ne = -1; break; }               // no row for it: all entries the old way
            er[ne].r = r; er[ne].pv = pv; er[ne].v = v; er[ne].fl = e[0x2c]; ne++;
        }
    }
    int64_t sum = *(int64_t*)(b + RVA_AE_ZERO);
    for (int ci = 0; ci < ncl; ci++) {
        uint8_t* c = cl[ci];
        uint64_t otag = *(uint64_t*)(c + 0x20);
        if ((int16_t)(otag >> 32) == tidx) continue;
        uint8_t* c14b0 = *(uint8_t**)(c + 0x14b0);
        if ((c14b0[0x959] & 0x10) && (uint8_t)(ttag >> 56) && (uint8_t)(ttag >> 48) != 7) {
            uint32_t w = (*(uint32_t**)(c14b0 + 0x5e0))[(uint32_t)tidx >> 5];
            if (!((w >> (tidx & 31)) & 1)) continue;
        }
        uint8_t* rel = *(uint8_t**)(c + 0x1510) + (int64_t)tidx * 0x48;
        uint8_t* r0 = *(uint8_t**)rel;
        int want = (int16_t)(*(uint64_t*)(rel + 0x10) >> 32);
        int* p = *(int**)(r0 + 0x50);
        int* pe = p + (int64_t)*(int*)(r0 + 0x5c) * 4;
        for (; p != pe && *p != want; p += 4) {}
        if (p != pe) continue;
        uint8_t half = rel[0x45] & 1;
        // FUN_140388b40(out, taker, &value, deal != 0, province, observer tag, deal, 0)
        int ae;
        if (rp) {
            ae = rp[ci];
            if (ae == AROW_NONE) { ae = ae_prov(t, taker, value, prov, 1, otag, direct); rp[ci] = ae; }
        } else ae = ae_prov(t, taker, value, prov, (uint8_t)(value > 0), otag, direct);
        uint32_t part = 0;                            // the deal entries' AE for this country (32-bit adds as the game's)
        if (keep) {
            if (dm->stamp[ci] == dm->id) part = (uint32_t)dm->d[ci];
            else { part = ae_items(t, taker, byidx, em, e1, otag, direct); dm->d[ci] = (int32_t)part; dm->stamp[ci] = dm->id; }
        }
        if (ne >= 0)
            for (int k = 0; k < ne; k++) {
                int32_t x = er[k].r[ci];
                if (x == AROW_NONE) { x = ae_prov(t, taker, er[k].v, er[k].pv, er[k].fl, otag, direct); er[k].r[ci] = x; }
                part += (uint32_t)x;
            }
        else part += ae_items(t, taker, byidx, e0, em, otag, direct);
        ae = (int)((uint32_t)ae + part);
        int64_t q = ((int64_t)(int32_t)((uint32_t)*(int*)(defs + 0x50) * 1000u) * (int64_t)ae) / 1000;
        int iq = (int)q;
        if (iq >= 0) continue;
        int s10 = (int16_t)g_opin_ae(c, ttag, defs);
        int a = (int)((uint32_t)s10 * 1000u + (uint32_t)iq);
        int16_t* ov = *(int16_t**)(c + 0x2380);
        int s11 = tidx < (int)((*(uint8_t**)(c + 0x2388) - (uint8_t*)ov) >> 1) ? ov[tidx] : (int16_t)g_opin(c, ttag, NULL, 1);
        int bsum = (int)((uint32_t)iq + (uint32_t)s11 * 1000u);
        if (g_allied(c, ttag, 0)) continue;
        if (a > (int)((uint32_t)*(int*)(b + RVA_AE_LIMIT) * 1000u)) continue;
        if (bsum >= 0) continue;
        uint64_t x = (uint64_t)(half ? 500 : 1000) << 30;
        uint64_t h = (uint64_t)(((unsigned __int128)x * 0x0624dd2f1a9fbe77ull) >> 64);
        int64_t m = (int64_t)((((x - h) >> 1) + h) >> 24);
        int64_t st = *(int64_t*)(c + 0x96c8);
        if (st < 0) g_strength(c + 0x28c0, &st, 0);
        sum = (int64_t)((uint64_t)sum + (uint64_t)((int64_t)((uint64_t)m * (uint64_t)st) >> 15));
    }
    *out = sum;
    if (t->ui) g_ui_sums++; else InterlockedIncrement64(&g_ae_sums);
    return 1;
}
static int64_t* hk_aeall(uint8_t* taker, int64_t* out, int* value, uint8_t* prov, void** deal, uint64_t cache) {
    if (t_ae_in || cache) return o_aeall(taker, out, value, prov, deal, cache);
    atab_t* t;
    if (ui_in()) t = &t_ut;
    else {
        if (!g_ae_cache) return o_aeall(taker, out, value, prov, deal, cache);
        t = &t_at;
        LONG gen = g_ae_gen;
        if ((t->gen != (uint32_t)gen && !ae_reset(t, gen)) || *t->slot != g_ae_cache) return o_aeall(taker, out, value, prov, deal, cache);
    }
    int side = g_aec == 3 ? (int)(++t_ae_flip & 1) : 1;   // A/B: 0 the game's code and map, 1 ae_sum
    LONG64 a0 = g_aec == 3 ? qpc() : 0;
    int64_t* r = out;
    LONG64 s0 = t->ui && g_cfg.test_warview ? qpc() : 0;
    int done = side && ae_sum(t, t->ui && !*t->slot, taker, out, *value, prov, deal);
    if (s0) g_ui_sum_t += qpc() - s0;
    if (!done) {
        t_ae_in = 1; t_ae_orig = !side;
        r = o_aeall(taker, out, value, prov, deal, cache);
        t_ae_in = 0; t_ae_orig = 0;
    } else if (g_aec == 2) {
        int64_t g = 0;
        t_ae_in = 1; t_ae_orig = 1;
        o_aeall(taker, &g, value, prov, deal, cache);
        t_ae_in = 0; t_ae_orig = 0;
        InterlockedIncrement64(&g_ae_sum_ver);
        if (g != *out) {
            InterlockedIncrement64(&g_ae_sum_bad);
            if (InterlockedIncrement64(&g_ae_sum_badlog) <= 20)
                eb_log("ai ae: SUM MISMATCH%s taker %.3s value %d: game %lld, ae_sum %lld", t->ui ? " outside the AI loop" : "",
                       (char*)(taker + 0x20), *value, (long long)g, (long long)*out);
        }
    }
    if (g_aec == 3) {
        LONG64 d = qpc() - a0;
        InterlockedAdd64(&g_ae_ab_t[side], d); InterlockedIncrement64(&g_ae_ab_n[side]);
        for (LONG64 m = g_ae_ab_max[side]; d > m && InterlockedCompareExchange64(&g_ae_ab_max[side], d, m) != m; m = g_ae_ab_max[side]) {}
    }
    return r;
}

// ---- calls outside the AI loop (report 29, see t_ut above) ----
// The war view FUN_141306dd0, whenever it is refreshed, builds the deal the enemy would demand for our
// surrender: offer FUN_140975d40(buf, enemy, us, 0), acceptance FUN_1401bf150, FUN_14098c0e0 fills it up
// to the war score budget (every candidate province's AE: FUN_14099fc60 -> FUN_14038a530 ->
// FUN_140388880), acceptance again, FUN_1409761c0 frees it. The peace windows call the same two.
#define RVA_OFFER_NEW  0x975d40u
#define RVA_OFFER_FILL 0x98c0e0u
#define RVA_ACCEPT     0x1bf150u
#define RVA_OFFER_FREE 0x9761c0u
static const uint8_t sig_offer_new[16]  = { 0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x48 };
static const uint8_t sig_offer_fill[16] = { 0x48,0x89,0x5c,0x24,0x08,0x44,0x88,0x4c,0x24,0x20,0x4c,0x89,0x44,0x24,0x18,0x89 };
static const uint8_t sig_accept[16]     = { 0x48,0x89,0x5c,0x24,0x08,0x4c,0x89,0x4c,0x24,0x20,0x44,0x88,0x44,0x24,0x18,0x48 };
static const uint8_t sig_offer_free[16] = { 0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8d,0x05,0xe7,0x16,0x2e };
typedef uint64_t (*offer_fill_t)(void* offer, uint64_t budget, void* list, uint64_t p4);
typedef int (*accept_t)(uint8_t* c, void* reasons, uint64_t p3, uint64_t tag, void* offer, uint64_t p6, uint64_t p7, uint64_t p8, uint64_t p9, uint64_t p10);
static offer_fill_t o_fill;
static accept_t o_accept;
static int g_ui_on;
// a top-level call on the main thread outside the AI loop starts a new generation of t_ut
static int ui_enter(uintptr_t frame) {
    if (t_ui && frame < t_ui_frame) { t_ui++; return 1; }
    t_ui = 0;
    if (t_ui_block || g_ai_loop_active || g_ae_cache || GetCurrentThreadId() != g_main_tid ||
        *(volatile uint8_t*)((uint8_t*)g_exe + RVA_FIXED_SEED_FLAG)) return 0;
    t_ut.ui = 1;
    if (++t_ugen == 0) t_ugen = 1;
    if (++t_lpscope == 0) t_lpscope = 1;
    if (!ae_reset(&t_ut, (LONG)t_ugen)) return 0;
    t_ui = 1; t_ui_frame = frame;
    return 1;
}
static void ui_leave(int k, LONG64 t0) {
    if (t_ui > 0 && --t_ui) return;
    t_ui = 0;
    g_ui_n[k]++; g_ui_t[k] += qpc() - t0;
}
// Half of a deal fill is FUN_14036eb10(country, war, province): a search over the land from the province
// (a visited set per call) that the cede-province items' value and checks ask for every candidate in
// every round. It reads only the country, the war's sides, the province and the map, none of which change
// during one top-level call, so inside t_ut's generation the answer is kept.
// EXPERIMENT [parallel] ai_land_memo=1 (2 = + verify, 3 = A/B, default off; needs ai_ae_cache): the same
// inside the AI loop. The foreign minister's peace deals (FUN_140132c40 -> FUN_14098c0e0, and its
// acceptance checks FUN_1401bf150) run on the AI threads; the multi-second AI loops measured in the late
// game were such deals (10-21: up to 2.6 s; report 81 waited 2 s for the AI loop, task unknown). While the AI loop
// runs no task changes wars, provinces or countries (the game itself reads them from all its AI threads
// at once), so one top-level call on an AI thread is one generation of the memo there too (t_lpscope).
// A/B: the top-level calls alternate on each thread between the game's searches and the memo, and their
// times are added up per side.
#define RVA_LANDPATH 0x36eb10u
static const uint8_t sig_landpath[16] = { 0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x18,0x48,0x89,0x7c,0x24,0x20,0x55 };
typedef uint64_t (*landpath_t)(uint8_t* c, uint8_t* w, uint8_t* p);
static landpath_t o_landpath;
typedef struct { uint8_t *c, *w, *p; uint32_t gen; uint8_t r; } lpent_t;
static __thread lpent_t* t_lp;
static __thread uint32_t t_lp_mask, t_lp_n, t_lp_gen;
static LONG64 g_lp_calls, g_lp_hit, g_lp_ver, g_lp_bad;
static inline uint32_t lp_hash(uint8_t* c, uint8_t* w, uint8_t* p) {
    uint64_t h = (uintptr_t)c * 0x9E3779B97F4A7C15ull ^ (uintptr_t)w * 0xC2B2AE3D27D4EB4Full ^ (uintptr_t)p * 0x165667B19E3779F9ull;
    return (uint32_t)(h >> 32) ^ (uint32_t)h;
}
static int lp_grow(void) {
    uint32_t cap = t_lp ? (t_lp_mask + 1) * 2 : 1u << 12;
    if (cap > (1u << 20)) return 0;
    lpent_t* e = (lpent_t*)calloc(cap, sizeof *e);
    if (!e) return 0;
    if (t_lp)
        for (uint32_t i = 0; i <= t_lp_mask; i++) {
            lpent_t* o = &t_lp[i];
            if (o->gen != t_lp_gen) continue;
            uint32_t j = lp_hash(o->c, o->w, o->p) & (cap - 1);
            while (e[j].gen == t_lp_gen) j = (j + 1) & (cap - 1);
            e[j] = *o;
        }
    free(t_lp); t_lp = e; t_lp_mask = cap - 1;
    return 1;
}
static int g_alm;                                 // [parallel] ai_land_memo
static __thread int t_ad, t_ad_side;              // depth inside a deal fill / acceptance check in the AI loop; A/B side
static __thread uintptr_t t_ad_frame;
static __thread unsigned t_ad_flip;
static __thread LONG64 t_alp_calls, t_alp_hit, t_alp_ver, t_alp_bad;   // this thread's counts, added up at the end of a call
static volatile LONG64 g_alp_calls, g_alp_hit, g_alp_ver, g_alp_bad, g_ad_n[2], g_ad_t[2], g_ad_max[2];
static inline int ad_in(void) {
    if (!t_ad) return 0;
    if ((uintptr_t)__builtin_frame_address(0) < t_ad_frame) return 1;
    t_ad = 0;
    return 0;
}
// a top-level deal fill or acceptance check on a thread of the AI loop starts a new generation of the memo
static int ad_enter(uintptr_t frame) {
    if (t_ad && frame < t_ad_frame) { t_ad++; return 1; }
    t_ad = 0;
    if ((!g_alm && !g_wish) || !g_ai_loop_active) return 0;
    if (++t_lpscope == 0) t_lpscope = 1;
    t_ad_side = g_alm == 3 ? (int)(++t_ad_flip & 1) : 1;
    t_ad = 1; t_ad_frame = frame;
    return 1;
}
static void ad_leave(LONG64 t0) {
    if (t_ad > 0 && --t_ad) return;
    t_ad = 0;
    LONG64 d = qpc() - t0;
    int s = t_ad_side;
    InterlockedIncrement64(&g_ad_n[s]); InterlockedAdd64(&g_ad_t[s], d);
    for (LONG64 m = g_ad_max[s]; d > m && InterlockedCompareExchange64(&g_ad_max[s], d, m) != m; m = g_ad_max[s]) {}
    if (t_alp_calls) { InterlockedAdd64(&g_alp_calls, t_alp_calls); InterlockedAdd64(&g_alp_hit, t_alp_hit); t_alp_calls = t_alp_hit = 0; }
    if (t_alp_ver) { InterlockedAdd64(&g_alp_ver, t_alp_ver); InterlockedAdd64(&g_alp_bad, t_alp_bad); t_alp_ver = t_alp_bad = 0; }
}
static uint64_t hk_landpath(uint8_t* c, uint8_t* w, uint8_t* p) {
    int ui = ui_in();
    if (!ui && !(g_alm && ad_in() && t_ad_side)) return o_landpath(c, w, p);
    if (ui) g_lp_calls++; else t_alp_calls++;
    if (t_lp_gen != t_lpscope) { t_lp_gen = t_lpscope; t_lp_n = 0; }
    if (!t_lp && !lp_grow()) return o_landpath(c, w, p);
    uint32_t i = lp_hash(c, w, p) & t_lp_mask;
    for (; t_lp[i].gen == t_lp_gen; i = (i + 1) & t_lp_mask) {
        lpent_t* e = &t_lp[i];
        if (e->c != c || e->w != w || e->p != p) continue;
        if (ui) g_lp_hit++; else t_alp_hit++;
        if (ui ? g_aec == 2 : g_alm == 2) {
            uint8_t g = (uint8_t)o_landpath(c, w, p);
            if (ui) g_lp_ver++; else t_alp_ver++;
            if (g != e->r) {
                if (ui) g_lp_bad++; else t_alp_bad++;
                if (InterlockedIncrement64(&g_ae_badlog) <= 20)
                    eb_log("ai ae: VERIFY MISMATCH land search %.3s province %d: kept %d, the game says %d",
                           (char*)(c + 0x20), (int)(uint16_t)*(uint32_t*)(p + 0x20), e->r, g);
            }
        }
        return e->r;
    }
    uint8_t r = (uint8_t)o_landpath(c, w, p);
    if ((t_lp_n + 1) * 2 > t_lp_mask + 1) {
        if (!lp_grow()) return r;
        for (i = lp_hash(c, w, p) & t_lp_mask; t_lp[i].gen == t_lp_gen; i = (i + 1) & t_lp_mask) {}
    }
    t_lp[i].c = c; t_lp[i].w = w; t_lp[i].p = p; t_lp[i].gen = t_lp_gen; t_lp[i].r = r; t_lp_n++;
    return r;
}
static uint64_t hk_fill(void* offer, uint64_t budget, void* list, uint64_t p4) {
    uintptr_t f = (uintptr_t)__builtin_frame_address(0);
    LONG64 t0;
    if (ui_enter(f)) {
        t0 = qpc();
        uint64_t r = o_fill(offer, budget, list, p4);
        ui_leave(0, t0);
        return r;
    }
    if (!ad_enter(f)) return o_fill(offer, budget, list, p4);
    t0 = qpc();
    uint64_t r = o_fill(offer, budget, list, p4);
    ad_leave(t0);
    return r;
}
static int hk_accept(uint8_t* c, void* reasons, uint64_t p3, uint64_t tag, void* offer, uint64_t p6, uint64_t p7, uint64_t p8, uint64_t p9, uint64_t p10) {
    uintptr_t f = (uintptr_t)__builtin_frame_address(0);
    LONG64 t0;
    if (ui_enter(f)) {
        t0 = qpc();
        int r = o_accept(c, reasons, p3, tag, offer, p6, p7, p8, p9, p10);
        ui_leave(1, t0);
        return r;
    }
    if (!ad_enter(f)) return o_accept(c, reasons, p3, tag, offer, p6, p7, p8, p9, p10);
    t0 = qpc();
    int r = o_accept(c, reasons, p3, tag, offer, p6, p7, p8, p9, p10);
    ad_leave(t0);
    return r;
}

// ---- the provinces a country wishes for in a deal (FUN_140190020) ----
// EXPERIMENT [parallel] ai_deal_memo=1 (2 = + verify, default off; needs ai_ae_cache and its deal hooks):
// what a deal fill asks again and again is kept for that fill. First the wish lists.
// While a deal is filled, the value of every candidate item (FUN_140979a40 -> FUN_1409a8760 for a province)
// asks FUN_1403b3900 what the receiving country gets for its part in the war, and for an AI receiver that
// builds the set of provinces it wishes for: FUN_140190020(set, deal, tag, int* limit, flag) makes a NEW empty
// offer between the deal's two sides (FUN_140975d40), values every province candidate of the deal that the
// tag does not already get in it as if handed to the tag - FUN_1409a8760(temporary item, &value, the new
// offer, 1, 0, &0), where the temporary item is the candidate's with the tag as receiver - and destroys the
// offer (FUN_1409761c0). So every candidate that is valued values every candidate province again: 31% of a
// fill of 0.7 s in the user's SPAIN campaign of 11-12 (630 provinces, at war with a country of 176).
// The inner valuation reads the temporary item, the new offer - always the same empty one for the same two
// sides and flag - and the game state, which does not change during one top-level fill or acceptance check
// (see the land searches above). So within one such call
//   1. the result of the inner valuation is kept per (the item's fields, the offer's sides and flag), for
//      calls made from FUN_140190020's own code only;
//   2. the new offer, which nothing but those valuations reads, is built only when one of them has to run.
// Verify mode runs the game's valuation on every kept answer and compares.
#define RVA_WISH     0x190020u
#define WISH_SIZE    0x4ffu       // the function's size: calls made from its own code return into it
#define RVA_PVALUE   0x9a8760u
static const uint8_t sig_wish[16]   = { 0x48,0x89,0x5c,0x24,0x18,0x4c,0x89,0x4c,0x24,0x20,0x48,0x89,0x54,0x24,0x10,0x48 };
static const uint8_t sig_pvalue[16] = { 0x4c,0x89,0x44,0x24,0x18,0x48,0x89,0x54,0x24,0x10,0x55,0x53,0x56,0x57,0x41,0x54 };
typedef int64_t* (*wish_t)(int64_t* set, uint8_t* deal, uint64_t tag, int* limit, uint64_t flag);
typedef int* (*pvalue_t)(uint8_t* item, int* out, uint8_t* deal, uint64_t p4, uint64_t p5, int* p6);
typedef void* (*offer_ctor_t)(void* buf, uint64_t a, uint64_t b, uint64_t flag);
typedef void* (*offer_dtor_t)(void* buf);
static wish_t o_wish;
static pvalue_t o_pvalue;
static offer_ctor_t o_offer_ctor;
static offer_dtor_t o_offer_dtor;
// FUN_140190020 calls that are running on this thread, innermost last
typedef struct { uintptr_t frame; void* buf; uint64_t a, b, flag; int pending; } wctx_t;
#define WISH_DEPTH 4
static __thread wctx_t t_wc[WISH_DEPTH];
static __thread int t_wn;
typedef struct { uint64_t k[9]; int32_t v; uint32_t gen; } went_t;
static __thread went_t* t_we;
static __thread uint32_t t_we_mask, t_we_n, t_we_gen;
static __thread LONG64 t_w_sets, t_w_hit, t_w_calc, t_w_lazy, t_w_built, t_w_ver, t_w_bad;
static volatile LONG64 g_w_sets, g_w_hit, g_w_calc, g_w_lazy, g_w_built, g_w_ver, g_w_bad, g_w_badlog, g_w_full;
static inline int wish_scope(void) { return ui_in() || (ad_in() && t_ad_side); }
// the innermost running call, NULL if there is none (a call left by an exception is dropped)
static inline wctx_t* wish_top(uintptr_t frame) {
    while (t_wn && t_wc[t_wn - 1].frame <= frame) t_wn--;
    return t_wn ? &t_wc[t_wn - 1] : NULL;
}
static inline int wish_from(void* ra) { return (uintptr_t)ra - ((uintptr_t)g_exe + RVA_WISH) < WISH_SIZE; }
static void wish_flush(void) {
    if (t_w_sets) { InterlockedAdd64(&g_w_sets, t_w_sets); t_w_sets = 0; }
    if (t_w_hit) { InterlockedAdd64(&g_w_hit, t_w_hit); t_w_hit = 0; }
    if (t_w_calc) { InterlockedAdd64(&g_w_calc, t_w_calc); t_w_calc = 0; }
    if (t_w_lazy) { InterlockedAdd64(&g_w_lazy, t_w_lazy); t_w_lazy = 0; }
    if (t_w_built) { InterlockedAdd64(&g_w_built, t_w_built); t_w_built = 0; }
    if (t_w_ver) { InterlockedAdd64(&g_w_ver, t_w_ver); t_w_ver = 0; }
    if (t_w_bad) { InterlockedAdd64(&g_w_bad, t_w_bad); t_w_bad = 0; }
}
static inline uint32_t wish_hash(const uint64_t* k) {
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 9; i++) h = (h ^ k[i]) * 0xC2B2AE3D27D4EB4Full, h ^= h >> 29;
    return (uint32_t)(h >> 32) ^ (uint32_t)h;
}
static int wish_grow(void) {
    uint32_t cap = t_we ? (t_we_mask + 1) * 2 : 1u << 11;
    if (cap > (1u << 17)) return 0;
    went_t* e = (went_t*)calloc(cap, sizeof *e);
    if (!e) return 0;
    if (t_we)
        for (uint32_t i = 0; i <= t_we_mask; i++) {
            went_t* o = &t_we[i];
            if (o->gen != t_we_gen) continue;
            uint32_t j = wish_hash(o->k) & (cap - 1);
            while (e[j].gen == t_we_gen) j = (j + 1) & (cap - 1);
            e[j] = *o;
        }
    free(t_we); t_we = e; t_we_mask = cap - 1;
    return 1;
}
static inline void wish_build(wctx_t* c) {          // the new offer is needed after all
    if (!c->pending) return;
    c->pending = 0;
    o_offer_ctor(c->buf, c->a, c->b, c->flag);
    t_w_built++;
}
static int64_t* hk_wish(int64_t* set, uint8_t* deal, uint64_t tag, int* limit, uint64_t flag) {
    uintptr_t f = (uintptr_t)__builtin_frame_address(0);
    wish_top(f);
    if (!wish_scope() || t_wn == WISH_DEPTH) return o_wish(set, deal, tag, limit, flag);
    wctx_t* c = &t_wc[t_wn++];
    c->frame = f; c->buf = NULL; c->pending = 0;
    t_w_sets++;
    int64_t* r = o_wish(set, deal, tag, limit, flag);
    if (t_wn && t_wc[t_wn - 1].frame == f) t_wn--;
    if (!t_wn) wish_flush();
    return r;
}
static void* hk_offer_ctor(void* buf, uint64_t a, uint64_t b, uint64_t flag) {
    wctx_t* c = t_wn ? wish_top((uintptr_t)__builtin_frame_address(0)) : NULL;
    if (!c || c->buf || !wish_from(__builtin_return_address(0))) return o_offer_ctor(buf, a, b, flag);
    c->buf = buf; c->a = a; c->b = b; c->flag = flag; c->pending = 1;    // FUN_140190020's own offer: not built yet
    t_w_lazy++;
    return buf;
}
static void* hk_offer_dtor(void* buf) {
    wctx_t* c = t_wn ? wish_top((uintptr_t)__builtin_frame_address(0)) : NULL;
    if (c && c->buf == buf) {
        int pending = c->pending;
        c->buf = NULL; c->pending = 0;
        if (pending) return buf;                      // never built: nothing to destroy
    }
    return o_offer_dtor(buf);
}
static int* hk_pvalue(uint8_t* item, int* out, uint8_t* deal, uint64_t p4, uint64_t p5, int* p6) {
    wctx_t* c = t_wn ? wish_top((uintptr_t)__builtin_frame_address(0)) : NULL;
    if (!c || !c->buf || c->buf != (void*)deal) return o_pvalue(item, out, deal, p4, p5, p6);
    if (!wish_from(__builtin_return_address(0)) || (uint8_t)p4 != 1 || p5 || !p6 || *p6) {
        wish_build(c);                                // another use of the offer: it has to exist
        return o_pvalue(item, out, deal, p4, p5, p6);
    }
    uint64_t k[9];
    k[0] = *(uint64_t*)item;
    k[1] = (uint64_t)*(uint32_t*)(item + 8) | (uint64_t)item[0x20] << 32 | (uint64_t)item[0x40] << 40 | (uint64_t)(uint8_t)c->flag << 48;
    k[2] = *(uint64_t*)(item + 0x10); k[3] = *(uint64_t*)(item + 0x18);
    memcpy(&k[4], item + 0x22, 8);
    k[5] = *(uint64_t*)(item + 0x30); k[6] = *(uint64_t*)(item + 0x38);
    k[7] = c->a; k[8] = c->b;
    if (t_we_gen != t_lpscope) { t_we_gen = t_lpscope; t_we_n = 0; }
    if (!t_we && !wish_grow()) { wish_build(c); return o_pvalue(item, out, deal, p4, p5, p6); }
    uint32_t i = wish_hash(k) & t_we_mask;
    for (; t_we[i].gen == t_we_gen; i = (i + 1) & t_we_mask) {
        went_t* e = &t_we[i];
        if (memcmp(e->k, k, sizeof k)) continue;
        t_w_hit++;
        if (g_wish == 2) {
            int g = 0x7fffffff;
            wish_build(c);
            o_pvalue(item, &g, deal, p4, p5, p6);
            t_w_ver++;
            if (g != e->v) {
                t_w_bad++;
                if (InterlockedIncrement64(&g_w_badlog) <= 20)
                    eb_log("ai deal memo: VERIFY MISMATCH province %d for %.3s: kept %d, the game says %d",
                           k[6] ? (int)*(uint32_t*)((uint8_t*)(uintptr_t)k[6] + 0x20) : -1, (char*)&k[5], e->v, g);
            }
        }
        *out = e->v;
        return out;
    }
    wish_build(c);
    int* r = o_pvalue(item, out, deal, p4, p5, p6);
    t_w_calc++;
    // the valuation may have used the table itself (a nested call): find the free slot again
    if (t_we_gen != t_lpscope) return r;              // a new top-level call began inside: not kept
    if ((t_we_n + 1) * 2 > t_we_mask + 1 && !wish_grow()) { InterlockedIncrement64(&g_w_full); return r; }
    for (i = wish_hash(k) & t_we_mask; t_we[i].gen == t_we_gen; i = (i + 1) & t_we_mask)
        if (!memcmp(t_we[i].k, k, sizeof k)) return r;                 // put there by a nested call
    memcpy(t_we[i].k, k, sizeof k); t_we[i].v = *out; t_we[i].gen = t_we_gen; t_we_n++;
    return r;
}

// The same option keeps two yes/no questions about a province and a country that a fill asks again and again:
// FUN_140a0f9f0(province, tag), which the value of every province item starts with, and
// FUN_140a0f140(province, tag), the same question for the province's neighbours, asked by FUN_14098b440 for
// every province candidate each time a candidate is valued (together 19% of a fill once the wish lists are
// kept). Both read the province, its neighbours and the countries' relations only.
#define RVA_PREACH 0xa0f9f0u
#define RVA_PNEAR  0xa0f140u
static const uint8_t sig_preach[16] = { 0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48 };
static const uint8_t sig_pnear[16]  = { 0x40,0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x83,0xec };
typedef uint64_t (*preach_t)(uint8_t* prov, uint64_t tag);
static preach_t o_preach, o_pnear;
typedef struct { uint8_t* p; uint64_t tag; uint32_t gen; uint8_t kind, r; } pent_t;
static __thread pent_t* t_pe;
static __thread uint32_t t_pe_mask, t_pe_n, t_pe_gen;
static __thread LONG64 t_p_hit, t_p_calc, t_p_ver, t_p_bad;
static volatile LONG64 g_p_hit, g_p_calc, g_p_ver, g_p_bad;
static void reach_flush(void) {
    if (t_p_hit) { InterlockedAdd64(&g_p_hit, t_p_hit); t_p_hit = 0; }
    if (t_p_calc) { InterlockedAdd64(&g_p_calc, t_p_calc); t_p_calc = 0; }
    if (t_p_ver) { InterlockedAdd64(&g_p_ver, t_p_ver); t_p_ver = 0; }
    if (t_p_bad) { InterlockedAdd64(&g_p_bad, t_p_bad); t_p_bad = 0; }
}
static inline uint32_t reach_hash(uint8_t* p, uint64_t tag) {
    uint64_t h = (uintptr_t)p * 0x9E3779B97F4A7C15ull ^ tag * 0xC2B2AE3D27D4EB4Full;
    return (uint32_t)(h >> 32) ^ (uint32_t)h;
}
static int reach_grow(void) {
    uint32_t cap = t_pe ? (t_pe_mask + 1) * 2 : 1u << 12;
    if (cap > (1u << 20)) return 0;
    pent_t* e = (pent_t*)calloc(cap, sizeof *e);
    if (!e) return 0;
    if (t_pe)
        for (uint32_t i = 0; i <= t_pe_mask; i++) {
            pent_t* o = &t_pe[i];
            if (o->gen != t_pe_gen) continue;
            uint32_t j = (reach_hash(o->p, o->tag) + o->kind) & (cap - 1);
            while (e[j].gen == t_pe_gen) j = (j + 1) & (cap - 1);
            e[j] = *o;
        }
    free(t_pe); t_pe = e; t_pe_mask = cap - 1;
    return 1;
}
// kind: 0 = FUN_140a0f9f0, 1 = FUN_140a0f140
static uint64_t reach(int kind, preach_t orig, uint8_t* prov, uint64_t tag) {
    if (!wish_scope()) return orig(prov, tag);
    if (t_pe_gen != t_lpscope) { reach_flush(); t_pe_gen = t_lpscope; t_pe_n = 0; }
    if (!t_pe && !reach_grow()) return orig(prov, tag);
    uint32_t i = (reach_hash(prov, tag) + (uint32_t)kind) & t_pe_mask;
    for (; t_pe[i].gen == t_pe_gen; i = (i + 1) & t_pe_mask) {
        pent_t* e = &t_pe[i];
        if (e->p != prov || e->tag != tag || e->kind != kind) continue;
        t_p_hit++;
        if (g_wish == 2) {
            uint8_t g = (uint8_t)orig(prov, tag);
            t_p_ver++;
            if (g != e->r) {
                t_p_bad++;
                if (InterlockedIncrement64(&g_w_badlog) <= 20)
                    eb_log("ai deal memo: VERIFY MISMATCH %s province %d for %.3s: kept %d, the game says %d",
                           kind ? "neighbours of" : "reach of", (int)*(uint32_t*)(prov + 0x20), (char*)&tag, e->r, g);
            }
        }
        return e->r;
    }
    uint8_t r = (uint8_t)orig(prov, tag);
    t_p_calc++;
    if (t_pe_gen != t_lpscope) return r;              // a new top-level call began inside: not kept
    if ((t_pe_n + 1) * 2 > t_pe_mask + 1 && !reach_grow()) return r;
    for (i = (reach_hash(prov, tag) + (uint32_t)kind) & t_pe_mask; t_pe[i].gen == t_pe_gen; i = (i + 1) & t_pe_mask)
        if (t_pe[i].p == prov && t_pe[i].tag == tag && t_pe[i].kind == kind) return r;   // put there by a nested call
    t_pe[i].p = prov; t_pe[i].tag = tag; t_pe[i].gen = t_pe_gen; t_pe[i].kind = (uint8_t)kind; t_pe[i].r = r; t_pe_n++;
    return r;
}
static uint64_t hk_preach(uint8_t* prov, uint64_t tag) { return reach(0, o_preach, prov, tag); }
static uint64_t hk_pnear(uint8_t* prov, uint64_t tag) { return reach(1, o_pnear, prov, tag); }

// The third thing kept: FUN_140992b40(deal, tag a, tag b, std::vector<province*>* out). The value of a province
// item calls it for every item already in the deal: it walks a's long province list (country +0x1378) and adds
// to `out` the provinces of b (or of b's subjects) that b or one of the deal's countries (deal +0x70) holds - 11%
// of a fill, all of it the walk. The answer depends on a, b, the deal's country list and the game state, so
// within one top-level call the provinces it added are kept per (a, b, the tags of that list - compared tag by
// tag, since a deal on the stack and its list can sit where another's sat) and added again in the same order
// (through the game's own vector growth, FUN_1400cc220, so that `out` grows exactly as before).
#define RVA_PLIST 0x992b40u
#define RVA_VPUSH 0xcc220u       // std::vector<void*>::_Emplace_reallocate(vec, where, &value)
static const uint8_t sig_plist[16] = { 0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57 };
static const uint8_t sig_vpush[16] = { 0x40,0x53,0x55,0x41,0x55,0x41,0x57,0x48,0x83,0xec,0x28,0x48,0x8b,0x41,0x08,0x4c };
typedef void (*plist_t)(uint8_t* deal, uint64_t a, uint64_t b, uint8_t*** vec);
typedef void* (*vpush_t)(void* vec, void* where, void* value);
static plist_t o_plist;
static vpush_t g_vpush;
typedef struct { uint64_t a, b; uint32_t gen, n, ln; size_t off, loff; } vent_t;   // ln tags of the deal's list at loff, n provinces at off
#define PLIST_CAP 1024           // entries per top-level call; more distinct questions are asked as before
static __thread vent_t* t_ve;
static __thread uint8_t** t_va;                   // the kept lists and provinces of all entries, one after the other
static __thread size_t t_va_n, t_va_cap;
static __thread uint32_t t_ve_n, t_ve_gen;
static __thread LONG64 t_v_hit, t_v_calc, t_v_ver, t_v_bad;
static volatile LONG64 g_v_hit, g_v_calc, g_v_ver, g_v_bad;
static void plist_flush(void) {
    if (t_v_hit) { InterlockedAdd64(&g_v_hit, t_v_hit); t_v_hit = 0; }
    if (t_v_calc) { InterlockedAdd64(&g_v_calc, t_v_calc); t_v_calc = 0; }
    if (t_v_ver) { InterlockedAdd64(&g_v_ver, t_v_ver); t_v_ver = 0; }
    if (t_v_bad) { InterlockedAdd64(&g_v_bad, t_v_bad); t_v_bad = 0; }
}
static void hk_plist(uint8_t* deal, uint64_t a, uint64_t b, uint8_t*** vec) {
    if (!wish_scope() || !vec) { o_plist(deal, a, b, vec); return; }
    if (t_ve_gen != t_lpscope) { plist_flush(); t_ve_gen = t_lpscope; t_ve_n = 0; t_va_n = 0; }
    if (!t_ve && !(t_ve = (vent_t*)calloc(PLIST_CAP, sizeof *t_ve))) { o_plist(deal, a, b, vec); return; }
    uint8_t* l0 = *(uint8_t**)(deal + 0x70); uint8_t* l1 = *(uint8_t**)(deal + 0x78);
    size_t lb = (size_t)(l1 - l0);
    if (lb > 256 * 8 || (lb & 7)) { o_plist(deal, a, b, vec); return; }
    uint32_t ln = (uint32_t)(lb >> 3);
    uint64_t h = a * 0xC2B2AE3D27D4EB4Full ^ b * 0x165667B19E3779F9ull ^ ln;
    for (uint32_t k = 0; k < ln; k++) h = (h ^ ((uint64_t*)l0)[k]) * 0x9E3779B97F4A7C15ull, h ^= h >> 31;
    uint32_t i = ((uint32_t)(h >> 32) ^ (uint32_t)h) & (PLIST_CAP - 1);
    for (; t_ve[i].gen == t_ve_gen; i = (i + 1) & (PLIST_CAP - 1)) {
        vent_t* e = &t_ve[i];
        if (e->a != a || e->b != b || e->ln != ln || memcmp(t_va + e->loff, l0, lb)) continue;
        t_v_hit++;
        if (g_wish == 2) {                            // the game's walk, compared with the kept provinces
            size_t n0 = (size_t)(vec[1] - vec[0]);
            o_plist(deal, a, b, vec);
            size_t n1 = (size_t)(vec[1] - vec[0]);
            t_v_ver++;
            if (n1 - n0 != e->n || memcmp(vec[0] + n0, t_va + e->off, (size_t)e->n * sizeof(void*))) {
                t_v_bad++;
                if (InterlockedIncrement64(&g_w_badlog) <= 20)
                    eb_log("ai deal memo: VERIFY MISMATCH province list of %.3s for %.3s: kept %u provinces, the game gives %zu",
                           (char*)&a, (char*)&b, e->n, n1 - n0);
            }
            return;
        }
        for (uint32_t k = 0; k < e->n; k++) {
            uint8_t* v = t_va[e->off + k];
            if (vec[1] == vec[2]) g_vpush(vec, vec[1], &v);
            else *vec[1]++ = v;
        }
        return;
    }
    size_t n0 = (size_t)(vec[1] - vec[0]);
    o_plist(deal, a, b, vec);
    t_v_calc++;
    size_t n = (size_t)(vec[1] - vec[0]) - n0;
    if (t_ve_gen != t_lpscope || t_ve_n >= PLIST_CAP / 2 || n > 0xffffff) return;
    if (*(uint8_t**)(deal + 0x70) != l0 || *(uint8_t**)(deal + 0x78) != l1) return;       // the list itself changed: not kept
    if (t_va_n + ln + n > t_va_cap) {
        size_t cap = (t_va_n + ln + n) * 2 + 1024;
        uint8_t** q = (uint8_t**)realloc(t_va, cap * sizeof *q);
        if (!q) return;
        t_va = q; t_va_cap = cap;
    }
    for (i = ((uint32_t)(h >> 32) ^ (uint32_t)h) & (PLIST_CAP - 1); t_ve[i].gen == t_ve_gen; i = (i + 1) & (PLIST_CAP - 1)) {}
    vent_t* e = &t_ve[i];
    e->a = a; e->b = b; e->gen = t_ve_gen; e->ln = ln; e->loff = t_va_n; e->n = (uint32_t)n; e->off = t_va_n + ln;
    memcpy(t_va + e->loff, l0, lb);
    memcpy(t_va + e->off, vec[0] + n0, n * sizeof(void*));
    t_va_n += ln + n; t_ve_n++;
}

// testing only ([test] warview=N): every N country loops the war view's calls for both leaders of every
// active war (gamestate+0x1e28: data +8, count +0x14), with the war view's budget (1 - acceptance) and
// with the largest (100), each once with the game's code (t_ui_block) and once through t_ut, in
// alternating order. Acceptance before and after and the filled list must come out the same.
typedef void (*offer_new_t)(void* buf, uint64_t a, uint64_t b, uint64_t flag);
typedef void* (*offer_free_t)(void* offer);
typedef struct { int a, a2; size_t nl; double ms, fill, calc_ms, sum_ms; LONG64 calls, slot, none, calcs; } wvres_t;
static LONG64 g_wv_views, g_wv_diff, g_wv_logs; static double g_wv_ms[2], g_wv_max[2];
static void wv_run(uint8_t* b, uint64_t me, uint64_t foe, uint8_t* cf, int force, int block, wvres_t* r) {
    static uint8_t buf[1024] __attribute__((aligned(16)));
    struct { uint8_t* b; uint8_t* e; uint8_t* c; } list = { 0, 0, 0 };
    LONG64 n0 = g_wv_aep, s0 = g_wv_aep_slot, z0 = g_wv_aep_none;
    LONG64 k0 = t_ut.calc + t_ut.neg + g_ui_calc + g_ui_neg, ct0 = g_ui_calc_t, st0 = g_ui_sum_t;
    LONG64 t0 = qpc();
    t_wv = 1; t_ui_block = block;
    memset(buf, 0, sizeof buf);
    ((offer_new_t)(b + RVA_OFFER_NEW))(buf, foe, me, 0);
    r->a = ((accept_t)(b + RVA_ACCEPT))(cf, NULL, 0, me, buf, 0x2d2d2d, 0, 0, 0, 0);
    int budget = 1 - r->a; if (budget > 100) budget = 100; if (budget < 0) budget = 0;
    if (force) budget = 100;
    LONG64 t1 = qpc();
    ((offer_fill_t)(b + RVA_OFFER_FILL))(buf, (uint64_t)budget, &list, 0);
    LONG64 t2 = qpc();
    r->a2 = ((accept_t)(b + RVA_ACCEPT))(cf, NULL, 0, me, buf, 0x2d2d2d, 0, 0, 0, 0);
    ((offer_free_t)(b + RVA_OFFER_FREE))(buf);
    t_wv = 0; t_ui_block = 0;
    r->ms = (qpc() - t0) * g_ms; r->fill = (t2 - t1) * g_ms; r->nl = (size_t)(list.e - list.b);
    r->calls = g_wv_aep - n0; r->slot = g_wv_aep_slot - s0; r->none = g_wv_aep_none - z0;
    r->calcs = t_ut.calc + t_ut.neg + g_ui_calc + g_ui_neg - k0; r->calc_ms = (g_ui_calc_t - ct0) * g_ms; r->sum_ms = (g_ui_sum_t - st0) * g_ms;
}
void eb_warview_test(uint8_t* gs) {
    static int loops;
    int every = g_cfg.test_warview < 0 ? -g_cfg.test_warview : g_cfg.test_warview;   // < 0: only through t_ut (profiling)
    if (!every || ++loops % every || !g_ui_on) return;
    uint8_t* b = (uint8_t*)g_exe;
    // (with the wish memo these two are hooked: their first bytes were checked when it was installed)
    if (!g_wish && (memcmp(b + RVA_OFFER_NEW, sig_offer_new, 16) || memcmp(b + RVA_OFFER_FREE, sig_offer_free, 16))) return;
    uint8_t** wars = *(uint8_t***)(gs + 0x1e30);
    int nw = *(int*)(gs + 0x1e3c), nv = 0;
    uint8_t** byidx = *(uint8_t***)(*(uint8_t**)(b + RVA_COUNTRIES) + 0x118);
    double tot[2] = { 0, 0 };
    for (int i = 0; i < nw; i++) {
        uint8_t* w = wars[i];
        uint8_t **a0 = *(uint8_t***)(w + 0x48), **a1 = *(uint8_t***)(w + 0x50), **d0 = *(uint8_t***)(w + 0x78), **d1 = *(uint8_t***)(w + 0x80);
        if (a1 <= a0 || d1 <= d0) continue;
        uint64_t att = *(uint64_t*)a0, def = *(uint64_t*)d0;
        uint8_t* ca = byidx[(int16_t)(att >> 32)]; uint8_t* cd = byidx[(int16_t)(def >> 32)];
        if (!ca || !cd) continue;
        for (int k = 0; k < 4; k++) {
            uint64_t me = k & 1 ? def : att, foe = k & 1 ? att : def;
            uint8_t* cm = k & 1 ? cd : ca; uint8_t* cf = k & 1 ? ca : cd;
            wvres_t r[2];
            int first = (int)(g_wv_views & 1);
            for (int s = 0; s < 2; s++) {
                int side = s ^ first;
                if (!side && g_cfg.test_warview < 0) { r[0] = (wvres_t){ 0 }; continue; }
                wv_run(b, me, foe, cf, k >= 2, !side, &r[side]);
            }
            g_wv_views++; nv++;
            int same = g_cfg.test_warview < 0 || (r[0].a == r[1].a && r[0].a2 == r[1].a2 && r[0].nl == r[1].nl);
            if (!same) g_wv_diff++;
            for (int s = 0; s < 2; s++) { g_wv_ms[s] += r[s].ms; tot[s] += r[s].ms; if (r[s].ms > g_wv_max[s]) g_wv_max[s] = r[s].ms; }
            if (!same || g_wv_logs < 24 || (r[0].ms >= 5.0 && g_wv_logs < 300)) {
                g_wv_logs++;
                eb_log("ai ae: TEST war view %d/%d (%d+%d) %.3s (%d provinces) vs %.3s, budget %s | game %.1f ms (fill %.1f), acceptance %d -> %d, list %zu, AE calls %lld (map %lld, none %lld) | t_ut %.1f ms (fill %.1f, AE sums %.1f, AE computed %lld in %.1f ms), acceptance %d -> %d, list %zu%s",
                       i + 1, nw, (int)(a1 - a0), (int)(d1 - d0), (char*)(cm + 0x20), *(int*)(cm + 0x1344), (char*)(cf + 0x20),
                       k >= 2 ? "100 (forced)" : "of the war view", r[0].ms, r[0].fill, r[0].a, r[0].a2, r[0].nl, (long long)r[0].calls,
                       (long long)r[0].slot, (long long)r[0].none, r[1].ms, r[1].fill, r[1].sum_ms, (long long)r[1].calcs, r[1].calc_ms, r[1].a, r[1].a2, r[1].nl, same ? "" : " - DIFFERENT");
            }
        }
    }
    ae_flush(&t_ut);
    if (nv)
        eb_log("ai ae: TEST war view: %d views now: game %.1f ms, t_ut %.1f ms | all %lld views: game %.1f ms (max %.1f), t_ut %.1f ms (max %.1f), different %lld | deal parts computed %lld, reused %lld | t_ut sums %lld, lookups %lld (%lld hits, %lld computed, %lld negative), land searches %lld (kept %lld) | verify: sums %lld, mismatches %lld; lookups %lld, mismatches %lld; land searches %lld, mismatches %lld",
               nv, tot[0], tot[1], (long long)g_wv_views, g_wv_ms[0], g_wv_max[0], g_wv_ms[1], g_wv_max[1], (long long)g_wv_diff,
               (long long)g_dm_new, (long long)g_dm_reuse, (long long)g_ui_sums, (long long)(g_ui_hit + g_ui_calc + g_ui_neg), (long long)g_ui_hit,
               (long long)g_ui_calc, (long long)g_ui_neg, (long long)g_lp_calls, (long long)g_lp_hit, (long long)g_ae_sum_ver, (long long)g_ae_sum_bad,
               (long long)g_ui_ver, (long long)g_ui_bad, (long long)g_lp_ver, (long long)g_lp_bad);
    if (nv && g_wish) {
        wish_flush(); reach_flush(); plist_flush();
        eb_log("ai deal memo: %lld wish lists: %lld province valuations answered from the kept ones, %lld computed (%lld not kept, table full); "
               "the list's own offer built %lld of %lld times; reach questions %lld, %lld of them kept; province lists %lld, %lld of them kept; most AE rows %ld | verify %lld, mismatches %lld",
               (long long)g_w_sets, (long long)g_w_hit, (long long)g_w_calc, (long long)g_w_full, (long long)g_w_built, (long long)g_w_lazy,
               (long long)(g_p_hit + g_p_calc), (long long)g_p_hit, (long long)(g_v_hit + g_v_calc), (long long)g_v_hit, (long)(t_av_rows > g_av_maxrows ? t_av_rows : g_av_maxrows),
                   (long long)(g_w_ver + g_p_ver + g_v_ver), (long long)(g_w_bad + g_p_bad + g_v_bad));
    }
}

// ---- army move check experiment ----
#define RVA_CANMOVE 0xd45250u     // char FUN_140d45250(unit, province from, province to, char)
static const uint8_t sig_canmove[16] = { 0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x54,0x24,0x10,0x55,0x56,0x57,0x41,0x54,0x41 };
typedef uint64_t (*canmove_t)(uint8_t* unit, uint8_t* from, uint8_t* to, uint8_t flag);
static canmove_t o_canmove;
static int g_army;
#define RVA_HASMOD  0xd40ae0u     // char FUN_140d40ae0(unit, modifier id, char, p4): does the unit have a bool modifier
static const uint8_t sig_hasmod[16] = { 0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x4c,0x89,0x4c,0x24,0x20,0x55 };
typedef uint64_t (*hasmod_t)(uint8_t* unit, uint32_t id, uint8_t p3, uint64_t p4);
static hasmod_t o_hasmod;
static __thread int t_cm_in;                      // inside FUN_140d45250 (experiment side)
static __thread uint8_t* t_hm_unit;               // the kept answer: unit, AI loop, AI task run, value
static __thread unsigned t_hm_run;
static __thread LONG t_hm_loop;
static __thread uint8_t t_hm_val;
typedef struct { LONG64 calls, skip, ver, bad, t[2], n[2], hm, hm_hit; unsigned flip; } acnt_t;
static __thread acnt_t t_ac;
static volatile LONG64 g_ac_calls, g_ac_skip, g_ac_ver, g_ac_bad, g_ac_badlog, g_ac_t[2], g_ac_n[2], g_ac_hm, g_ac_hm_hit;
static void ac_flush(acnt_t* c) {
    InterlockedAdd64(&g_ac_calls, c->calls); InterlockedAdd64(&g_ac_skip, c->skip);
    InterlockedAdd64(&g_ac_ver, c->ver); InterlockedAdd64(&g_ac_bad, c->bad);
    for (int s = 0; s < 2; s++) { InterlockedAdd64(&g_ac_t[s], c->t[s]); InterlockedAdd64(&g_ac_n[s], c->n[s]); }
    InterlockedAdd64(&g_ac_hm, c->hm); InterlockedAdd64(&g_ac_hm_hit, c->hm_hit);
    unsigned flip = c->flip;
    memset(c, 0, sizeof *c);
    c->flip = flip;
}
// the original's false after its two flag tests (NULL provinces: leave it to the original)
static inline int cm_flags_fail(uint8_t* from, uint8_t* to) {
    return from && to && (!(*(*(uint8_t**)(from + 0x28) + 0x228) & 8) || !(*(*(uint8_t**)(to + 0x28) + 0x228) & 8));
}
static uint64_t hk_hasmod(uint8_t* unit, uint32_t id, uint8_t p3, uint64_t p4) {
    if (!t_cm_in || id != 0xe0 || p3 || !g_ai_loop_active) return o_hasmod(unit, id, p3, p4);
    acnt_t* c = &t_ac;
    c->hm++;
    if (unit == t_hm_unit && t_hm_run == t_node_run && t_hm_loop == g_ai_loop_n) {
        c->hm_hit++;
        if (g_army == 2) {
            uint8_t g = (uint8_t)o_hasmod(unit, id, p3, p4);
            c->ver++;
            if (g != t_hm_val) {
                c->bad++;
                if (InterlockedIncrement64(&g_ac_badlog) <= 20)
                    eb_log("ai army: VERIFY MISMATCH kept modifier 0xe0 of unit %p: kept %d, the game says %d", (void*)unit, t_hm_val, g);
            }
        }
        return t_hm_val;
    }
    uint8_t v = (uint8_t)o_hasmod(unit, id, p3, p4);
    t_hm_unit = unit; t_hm_run = t_node_run; t_hm_loop = g_ai_loop_n; t_hm_val = v;
    return v;
}
static uint64_t hk_canmove(uint8_t* unit, uint8_t* from, uint8_t* to, uint8_t flag) {
    acnt_t* c = &t_ac;
    c->calls++;
    if (g_army == 3) {                                // A/B: alternate per call
        int side = (int)(++c->flip & 1);
        LONG64 a0 = qpc();
        uint64_t r;
        if (side && cm_flags_fail(from, to)) { c->skip++; r = 0; }
        else { t_cm_in = side; r = o_canmove(unit, from, to, flag); t_cm_in = 0; }
        c->t[side] += qpc() - a0; c->n[side]++;
        if ((c->calls & 4095) == 0) ac_flush(c);
        return r;
    }
    if (!cm_flags_fail(from, to)) {
        if ((c->calls & 4095) == 0) ac_flush(c);
        t_cm_in = 1;
        uint64_t r = o_canmove(unit, from, to, flag);
        t_cm_in = 0;
        return r;
    }
    c->skip++;
    if (g_army == 2) {
        uint8_t g = (uint8_t)o_canmove(unit, from, to, flag);
        c->ver++;
        if (g) {
            c->bad++;
            if (InterlockedIncrement64(&g_ac_badlog) <= 20)
                eb_log("ai army: VERIFY MISMATCH unit %p from %p to %p flag %d: the game says true", (void*)unit, (void*)from, (void*)to, flag);
        }
    }
    if ((c->calls & 4095) == 0) ac_flush(c);
    return 0;
}

// MSVC RTTI: vtable[-1] -> complete object locator {sig 1, ..., type descriptor RVA at +0x0c}; name at td+0x10
static const char* class_name(void* vt) {
    uint8_t* b = (uint8_t*)g_exe;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(b + ((IMAGE_DOS_HEADER*)g_exe)->e_lfanew);
    uintptr_t lo = (uintptr_t)b, hi = lo + nt->OptionalHeader.SizeOfImage;
    if ((uintptr_t)vt < lo + 8 || (uintptr_t)vt >= hi) return "?";
    uint8_t* col = ((uint8_t**)vt)[-1];
    if ((uintptr_t)col < lo || (uintptr_t)col + 0x18 > hi || *(int*)col != 1) return "?";
    uint32_t td = *(uint32_t*)(col + 0x0c);
    if (td < 0x1000 || lo + td + 0x20 > hi) return "?";
    return (const char*)(b + td + 0x10);
}

static void report_classes(void) {
    cls_t* top[15]; int nt = 0;
    for (int k = 0; k < 15; k++) {
        cls_t* best = NULL;
        for (int i = 0; i < NCLS; i++) {
            cls_t* c = &g_cls[i];
            if (!c->vt || !c->runs) continue;
            int used = 0; for (int j = 0; j < nt; j++) if (top[j] == c) used = 1;
            if (!used && (!best || c->self > best->self)) best = c;
        }
        if (!best) break;
        top[nt++] = best;
    }
    if (g_fm_on) {
        AcquireSRWLockShared(&g_fm_lock);
        eb_log("ai fm: longest foreign-minister run %.1f ms; runs over 100 ms: %lld (%.1f ms avg). calls / ms in the longest run | per run over 100 ms:",
               g_fm_max * g_ms, (long long)g_fm_big_runs, g_fm_big_runs ? g_fm_big_t * g_ms / g_fm_big_runs : 0.0);
        for (int k = 0; k < NFM; k++)
            eb_log("ai fm:   %-24s %8lld calls %8.1f ms | %9.0f calls %8.1f ms", g_fmf[k].name, (long long)g_fm_max_n[k], g_fm_max_t[k] * g_ms,
                   g_fm_big_runs ? (double)g_fm_big_n[k] / g_fm_big_runs : 0.0, g_fm_big_runs ? g_fm_big_tt[k] * g_ms / g_fm_big_runs : 0.0);
        ReleaseSRWLockShared(&g_fm_lock);
    }
    LONG64 all = 0; for (int i = 0; i < NCLS; i++) all += g_cls[i].self;
    eb_log("ai tasks: %lld loops, task self time %.2f ms per loop; top classes:", (long long)g_loops, all * g_ms / (g_loops ? g_loops : 1));
    for (int j = 0; j < nt; j++)
        eb_log("ai tasks:   %6.3f ms/loop self, %7lld runs, max %6.2f ms, %4ld runs > 5 ms  %s (vt %p)",
               top[j]->self * g_ms / g_loops, (long long)top[j]->runs, top[j]->max * g_ms, (long)top[j]->spikes, class_name(top[j]->vt), top[j]->vt);
}

// ---- TEST [test] ai_path_stats=1: how often the map path search and its edge check repeat ----
// FUN_14096c520(functor, unit, province from, province to, path out, options, p7, p8) is the map's path search
// (45 callers; 48% of the AI update of a big country at war, 11-12). For every step it asks the functor
// whether the unit can cross from one province to the next, which ends in
// FUN_14096f860(functor, unit, p3, from, to, adjacency entry, options) (21%). This counts the calls and how many
// ask something already asked - by the same search, the same AI task run, the same AI loop on the thread - for
// all countries and for the focus country ([parallel] ai_item_focus) alone. Nothing is changed.
#define RVA_PATH 0x96c520u
#define RVA_EDGE 0x96f860u
static const uint8_t sig_path[16] = { 0x4c,0x89,0x4c,0x24,0x20,0x48,0x89,0x54,0x24,0x10,0x48,0x89,0x4c,0x24,0x08,0x55 };
static const uint8_t sig_edge[16] = { 0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57 };
typedef uint64_t (*path_t)(uint8_t* functor, uint8_t* unit, uint8_t* from, uint8_t* to, void* out, uint8_t* opts, uint64_t p7, uint64_t p8);
typedef uint64_t (*edge_t)(uint8_t* functor, uint8_t* unit, uint64_t p3, uint8_t* from, uint8_t* to, uint8_t* adj, uint8_t* opts);
static path_t o_path;
static edge_t o_edge;
static __thread int t_focus;                      // inside the focus country's AI update
// ---- EXPERIMENT [parallel] ai_path_ahead=1 (2 = + verify, default off): an army list's path searches on idle threads ----
// FUN_140106320(ctx, target province, unit, ...) adds up the strength that can come to a province: for every
// army of the country (list at country +0xd18) and of its allies that is near enough but in another area it
// runs the map's path search FUN_14096c520(functor {vt 0x141c3af98}, army, the army's province, target,
// path out, options) and counts the army only if there is a path. A search that finds no path walks the whole
// reachable map: in the user's SPAIN campaign (11-12) Germany's five 48-regiment armies in Italy each asked
// for the same Spanish province every day, 9,018 edge checks and 2-4 ms each, one after the other on
// Germany's thread - 11 ms of its 15-19 ms a day, while the AI loop's other threads had long finished
// (89% of all path search time of the AI loop went to searches without a path).
// The searches of one such call do not depend on each other, and a search reads only its arguments and the
// game state (plus the country's access cache, see the split experiment's locks). So:
//   - a failed search is remembered per AI country (the unit and the AI loop);
//   - when a call of FUN_140106320 makes its first search, the units that failed in this or the previous AI
//     loop and are still in their country's unit list are searched ahead as jobs for idle threads: the same
//     functor bytes, options and target as that first search, from the unit's province now;
//   - a later search of the call whose every input equals a job's (unit, from, to, the functor's class - it has
//     no fields -, options bytes, an empty path list) takes the job's result - the returned value, the options as the search left
//     them and the path list - waiting for it if a thread is still on it, or runs the search itself if no
//     thread has started it;
//   - before FUN_140106320 returns, jobs nobody started are dropped and running ones are waited for (the game
//     state must not change under a search), and paths nobody took are freed.
// A wrong guess only costs idle time: nothing is used unless every input is the same. While jobs are out the
// country's access cache is used under the split experiment's lock (hk_cget / hk_cput).
// Verify mode runs the search again in place of every taken result and compares the returned value, the
// options and the path.
#define RVA_SUPPORT  0x106320u
#define SUPPORT_SIZE 4459u        // the function's size: its three calls of the path search return into it
#define RVA_GFREE    0x17394f0u   // operator delete(p, size)
#define VT_PATH_SUPPORT 0x1c3af98u
static const uint8_t sig_support[16] = { 0x48,0x8b,0xc4,0x4c,0x89,0x40,0x18,0x48,0x89,0x50,0x10,0x48,0x89,0x48,0x08,0x55 };
static const uint8_t sig_gfree[5] = { 0xe9,0x2b,0x45,0x02,0x00 };
typedef uint64_t (*support_t)(uint8_t* ctx, uint8_t* target, uint8_t* unit, void* p4, void* p5, uint64_t p6, uint64_t p7);
typedef void (*gfree_t)(void* p, size_t n);
static support_t o_support;
static gfree_t g_gfree;
static int g_ahead;                               // [parallel] ai_path_ahead
#define PF_MAX 12                 // jobs of one call
#define PF_OPTS 0x48
typedef struct {
    uint8_t* unit; uint8_t* from; uint8_t* to;
    uint64_t functor[2];          // a functor of the caller's class: nothing but its vtable (its destructor frees 8 bytes)
    uint8_t opts0[PF_OPTS];       // the options as the search got them
    uint8_t opts[PF_OPTS];        // ... and as it left them
    uint64_t out[3];              // the path list {head, tail, int count, char}: nodes of 0x20 bytes, next at +0x10
    uint64_t result;
    LONG64 ticks;
    int taken;
} pfdata_t;
typedef struct { uintptr_t frame; int active, posted, n, cidx, q, fails; void* map; uint8_t* target; uint8_t* unit; job_t* jobs[PF_MAX]; } pfscope_t;
static __thread pfscope_t t_pf;                   // the FUN_140106320 call running on this thread
#define PH_N 24
typedef struct { uint8_t* unit[PH_N]; LONG loop[PH_N]; int16_t owner[PH_N]; } phist_t;
static phist_t* g_ph[NTAG];                       // failed searches by AI country; only that country's thread uses its entry
static volatile LONG64 g_pf_scopes, g_pf_posted, g_pf_used[2], g_pf_own, g_pf_waited, g_pf_unused, g_pf_dropped,
                       g_pf_saved, g_pf_end_wait, g_pf_ver, g_pf_bad, g_pf_badlog, g_pf_hist_q[6], g_pf_hist_f[6], g_pf_samples;
// A/B (ai_path_ahead=3): odd AI loops send searches ahead, even ones do not; per side the time inside FUN_140106320 and the AI loop's wall time
static volatile LONG64 g_pf_ab_t[2], g_pf_ab_n[2], g_pf_ab_wall[2], g_pf_ab_loops[2], g_pf_ab_max[2];
static volatile LONG g_pf_loop_fails;             // searches without a path inside the reach sums of the running AI loop
static volatile LONG64 g_pf_ab_wall3[2], g_pf_ab_loops3[2], g_pf_ab_t3[2];      // the same for the loops with 3 or more of them
static LONG64 g_pf_loop_t;                        // time inside the reach sums of the running AI loop

static void pf_run(job_t* j) {
    pfdata_t* d = (pfdata_t*)j->arg;
    t_node_run++;                                 // answers the army experiment keeps per AI task run do not carry over
    LONG64 t0 = qpc();
    d->result = o_path((uint8_t*)d->functor, d->unit, d->from, d->to, d->out, d->opts, 0, 0);
    d->ticks = qpc() - t0;
}
static void pf_free_path(pfdata_t* d) {
    for (uint8_t* nd = (uint8_t*)(uintptr_t)d->out[0]; nd;) {
        uint8_t* nx = *(uint8_t**)(nd + 0x10);
        g_gfree(nd, 0x20);
        nd = nx;
    }
    d->out[0] = d->out[1] = d->out[2] = 0;
}
static void pf_wait(job_t* j) {
    AcquireSRWLockExclusive(&g_done_lock);
    while (j->state != 2) SleepConditionVariableSRW(&g_done_cv, &g_done_lock, INFINITE, 0);
    ReleaseSRWLockExclusive(&g_done_lock);
}
// the end of a FUN_140106320 call: no job of it may be left running
static void pf_finish(pfscope_t* s) {
    for (int i = 0; i < s->n; i++) {
        job_t* j = s->jobs[i];
        pfdata_t* d = (pfdata_t*)j->arg;
        if (InterlockedCompareExchange(&j->state, 3, 0) == 0) InterlockedIncrement64(&g_pf_dropped);   // never started
        else if (j->state != 3) {
            if (j->state != 2) { InterlockedIncrement64(&g_pf_end_wait); pf_wait(j); }
            if (!d->taken) InterlockedIncrement64(&g_pf_unused);
            pf_free_path(d);
        }
        job_release(j);
    }
    if (s->map) InterlockedDecrement(&g_ctx_active[stripe(s->map)]);
    s->n = 0; s->map = NULL; s->active = 0;
}
static void pf_note_fail(int cidx, uint8_t* unit) {
    phist_t* h = g_ph[cidx];
    if (!h) { h = (phist_t*)calloc(1, sizeof *h); if (!h) return; g_ph[cidx] = h; }
    LONG now = g_ai_loop_n;
    int at = -1;
    for (int k = 0; k < PH_N && at < 0; k++) if (h->unit[k] == unit) at = k;
    for (int k = 0; k < PH_N && at < 0; k++) if (!h->unit[k]) at = k;
    if (at < 0) { at = 0; for (int k = 1; k < PH_N; k++) if (now - h->loop[k] > now - h->loop[at]) at = k; }   // the oldest
    h->unit[at] = unit; h->loop[at] = now; h->owner[at] = (int16_t)(*(uint64_t*)(unit + 0xd0) >> 32);
}
static int pf_alive(uint8_t* unit, int owner) {     // still a unit of that country
    if (owner < 0 || owner >= NTAG) return 0;
    uint8_t* c = (*(uint8_t***)(*(uint8_t**)((uint8_t*)g_exe + RVA_COUNTRIES) + 0x118))[owner];
    if (!c) return 0;
    for (uint8_t** n = *(uint8_t***)(c + 0xd18); n; n = (uint8_t**)n[2])
        if (n[0] == unit) return (int16_t)(*(uint64_t*)(unit + 0xd0) >> 32) == owner;
    return 0;
}
// the first search of the call: the units that failed lately are searched ahead with its functor, options and target
static void pf_post(pfscope_t* s, uint8_t* functor, uint8_t* unit, uint8_t* to, uint8_t* opts) {
    s->posted = 1;
    phist_t* h = g_ph[s->cidx];
    if (!h || !g_qsem) return;
    LONG now = g_ai_loop_n;
    void* map = *(void**)(opts + 0x38);           // the country's access cache the edge checks use
    for (int k = 0; k < PH_N && s->n < PF_MAX; k++) {
        uint8_t* u = h->unit[k];
        if (!u || u == unit || (uint32_t)(now - h->loop[k]) > 1u || !pf_alive(u, h->owner[k])) continue;
        uint8_t* from = *(uint8_t**)(u + 0xe8);
        if (!from || from == to) continue;
        job_t* j = (job_t*)calloc(1, sizeof(job_t) + sizeof(pfdata_t));
        if (!j) return;
        pfdata_t* d = (pfdata_t*)(j + 1);
        j->run = pf_run; j->arg = d; j->ctx = NULL; j->state = 0; j->refs = 2; j->n = 0;
        d->unit = u; d->from = from; d->to = to;
        d->functor[0] = *(uint64_t*)functor;
        memcpy(d->opts0, opts, PF_OPTS); memcpy(d->opts, opts, PF_OPTS);
        if (!s->n && map) { s->map = map; InterlockedIncrement(&g_ctx_active[stripe(map)]); }
        AcquireSRWLockExclusive(&g_qlock);
        int queued = g_qn < QCAP;
        if (queued) g_q[g_qn++] = j;
        ReleaseSRWLockExclusive(&g_qlock);
        if (!queued) { free(j); return; }
        ReleaseSemaphore(g_qsem, 1, NULL);
        s->jobs[s->n++] = j;
        InterlockedIncrement64(&g_pf_posted);
    }
}
static int pf_same_path(uint64_t* a, uint64_t* b) {
    if (*(int*)(a + 2) != *(int*)(b + 2)) return 0;
    uint8_t* x = (uint8_t*)(uintptr_t)a[0]; uint8_t* y = (uint8_t*)(uintptr_t)b[0];
    for (; x && y; x = *(uint8_t**)(x + 0x10), y = *(uint8_t**)(y + 0x10))
        if (*(int*)x != *(int*)y) return 0;
    return !x && !y;
}
// the path search as asked by the game (ra: where the call returns)
static uint64_t path_do(void* ra, uint8_t* functor, uint8_t* unit, uint8_t* from, uint8_t* to, uint64_t* out, uint8_t* opts, uint64_t p7, uint64_t p8) {
    pfscope_t* s = &t_pf;
    if (!s->active || p8 || !out || !opts || !unit || !from || !to ||
        (uintptr_t)ra - ((uintptr_t)g_exe + RVA_SUPPORT) >= SUPPORT_SIZE ||
        (uintptr_t)__builtin_frame_address(0) >= s->frame ||
        *(uintptr_t*)functor != (uintptr_t)g_exe + VT_PATH_SUPPORT ||
        out[0] || out[1] || *(int*)(out + 2) || *((uint8_t*)out + 0x14))
        return o_path(functor, unit, from, to, out, opts, p7, p8);
    for (int i = 0; i < s->n; i++) {
        job_t* j = s->jobs[i];
        pfdata_t* d = (pfdata_t*)j->arg;
        if (d->taken || d->unit != unit || d->from != from || d->to != to || memcmp(d->opts0, opts, PF_OPTS)) continue;
        d->taken = 1;
        if (InterlockedCompareExchange(&j->state, 3, 0) == 0) {       // no thread took it: here, as usual
            InterlockedIncrement64(&g_pf_own);
            break;
        }
        if (j->state != 2) { InterlockedIncrement64(&g_pf_waited); pf_wait(j); }
        int found = (uint8_t)d->result != 0;
        InterlockedIncrement64(&g_pf_used[found]);
        InterlockedAdd64(&g_pf_saved, d->ticks);
        if (!found) { pf_note_fail(s->cidx, unit); InterlockedIncrement(&g_pf_loop_fails); }
        if (g_ahead == 2) {
            uint64_t r = o_path(functor, unit, from, to, out, opts, p7, p8);
            InterlockedIncrement64(&g_pf_ver);
            if ((uint8_t)r != (uint8_t)d->result || memcmp(opts, d->opts, PF_OPTS) || !pf_same_path(out, d->out)) {
                InterlockedIncrement64(&g_pf_bad);
                if (InterlockedIncrement64(&g_pf_badlog) <= 20)
                    eb_log("ai path ahead: VERIFY MISMATCH unit %p from %d to %d: ahead %d (%d steps), now %d (%d steps)%s", (void*)unit,
                           *(int*)(from + 0x20), *(int*)(to + 0x20), (int)(uint8_t)d->result, *(int*)(d->out + 2), (int)(uint8_t)r, *(int*)(out + 2),
                           memcmp(opts, d->opts, PF_OPTS) ? ", options differ" : "");
            }
            return r;
        }
        memcpy(opts, d->opts, PF_OPTS);
        memcpy(out, d->out, 0x15);
        d->out[0] = d->out[1] = d->out[2] = 0;        // the path is the caller's now
        return d->result;
    }
    if (!s->posted) pf_post(s, functor, unit, to, opts);
    uint64_t r = o_path(functor, unit, from, to, out, opts, p7, p8);
    s->q++;
    if (!(uint8_t)r) { pf_note_fail(s->cidx, unit); s->fails++; InterlockedIncrement(&g_pf_loop_fails); }
    return r;
}
static uint64_t hk_support(uint8_t* ctx, uint8_t* target, uint8_t* unit, void* p4, void* p5, uint64_t p6, uint64_t p7) {
    pfscope_t* s = &t_pf;
    uintptr_t f = (uintptr_t)__builtin_frame_address(0);
    if (s->active && s->frame <= f) pf_finish(s);     // a call left by an exception
    if (s->active || !g_ai_loop_active || *(volatile uint8_t*)((uint8_t*)g_exe + RVA_FIXED_SEED_FLAG))
        return o_support(ctx, target, unit, p4, p5, p6, p7);
    int cidx = (int16_t)(*(uint64_t*)(ctx + 0x28) >> 32);
    if (cidx < 0 || cidx >= NTAG) return o_support(ctx, target, unit, p4, p5, p6, p7);
    int side = g_ahead != 3 || (g_ai_loop_n & 1);
    s->active = 1; s->frame = f; s->posted = !side; s->n = 0; s->cidx = cidx; s->map = NULL; s->q = s->fails = 0; s->target = target; s->unit = unit;
    LONG64 t0 = g_ahead == 3 ? qpc() : 0;
    uint64_t r = o_support(ctx, target, unit, p4, p5, p6, p7);
    if (g_ahead == 3) { LONG64 d = qpc() - t0; InterlockedAdd64(&g_pf_ab_t[side], d); InterlockedIncrement64(&g_pf_ab_n[side]); InterlockedAdd64(&g_pf_loop_t, d); }
    InterlockedIncrement64(&g_pf_hist_q[s->q < 5 ? s->q : 5]); InterlockedIncrement64(&g_pf_hist_f[s->fails < 5 ? s->fails : 5]);
    if (t_focus && s->fails && InterlockedIncrement64(&g_pf_samples) <= 60)
        eb_log("ai path ahead: TEST sample loop %ld run %u: reach sum for unit %p (in %d) to province %d: %d searches run here, %d without a path, %d jobs",
               (long)g_ai_loop_n, t_node_run, (void*)unit, unit && *(uint8_t**)(unit + 0xe8) ? *(int*)(*(uint8_t**)(unit + 0xe8) + 0x20) : -1,
               target ? *(int*)(target + 0x20) : -1, s->q, s->fails, s->n);
    pf_finish(s);
    InterlockedIncrement64(&g_pf_scopes);
    return r;
}
static void pf_report(void) {
    eb_log("ai path ahead: %lld loops: %lld calls of the reach sum; %lld searches sent ahead: %lld taken without a path and %lld with one "
           "(%.1f ms of searching a loop not done on the country's thread; waited for %lld), %lld run by the asking thread itself, "
           "%lld never asked for, %lld never started; waited at the end of a call %lld times | verify %lld, mismatches %lld",
           (long long)g_loops, (long long)g_pf_scopes, (long long)g_pf_posted, (long long)g_pf_used[0], (long long)g_pf_used[1],
           g_pf_saved * g_ms / (g_loops ? g_loops : 1), (long long)g_pf_waited, (long long)g_pf_own, (long long)g_pf_unused,
           (long long)g_pf_dropped, (long long)g_pf_end_wait, (long long)g_pf_ver, (long long)g_pf_bad);
    if (g_ahead == 3)
        for (int k = 0; k < 2; k++)
            eb_log("ai path ahead A/B %s: %lld AI loops, wall %.2f ms each (longest %.1f ms); %lld calls of the reach sum, %.2f ms a loop inside them | "
                   "loops with 3+ searches without a path: %lld, wall %.2f ms each, %.2f ms inside the reach sums",
                   k ? "B (searches ahead)" : "A (the game)      ", (long long)g_pf_ab_loops[k], g_pf_ab_loops[k] ? g_pf_ab_wall[k] * g_ms / g_pf_ab_loops[k] : 0.0,
                   g_pf_ab_max[k] * g_ms, (long long)g_pf_ab_n[k], g_pf_ab_loops[k] ? g_pf_ab_t[k] * g_ms / g_pf_ab_loops[k] : 0.0,
                   (long long)g_pf_ab_loops3[k], g_pf_ab_loops3[k] ? g_pf_ab_wall3[k] * g_ms / g_pf_ab_loops3[k] : 0.0,
                   g_pf_ab_loops3[k] ? g_pf_ab_t3[k] * g_ms / g_pf_ab_loops3[k] : 0.0);
    eb_log("ai path ahead:   calls by searches run on the thread (0/1/2/3/4/5+): %lld/%lld/%lld/%lld/%lld/%lld; by searches without a path: %lld/%lld/%lld/%lld/%lld/%lld",
           (long long)g_pf_hist_q[0], (long long)g_pf_hist_q[1], (long long)g_pf_hist_q[2], (long long)g_pf_hist_q[3], (long long)g_pf_hist_q[4], (long long)g_pf_hist_q[5],
           (long long)g_pf_hist_f[0], (long long)g_pf_hist_f[1], (long long)g_pf_hist_f[2], (long long)g_pf_hist_f[3], (long long)g_pf_hist_f[4], (long long)g_pf_hist_f[5]);
}

#define PS_SLOTS 65536
typedef struct { uint64_t* k; uint32_t* g; uint32_t gen; } pset_t;             // a set of 64-bit keys per generation
static __thread pset_t t_ps[7];                   // 0 path/run, 1 path/loop, 2 edge/search, 3 edge/run, 4 edge/loop, 5 start/run, 6 start/loop
static __thread uint32_t t_ps_search;
static __thread LONG64 t_ps_edges;                // edge checks of the running search
static volatile LONG g_ps_samples;
static volatile LONG64 g_ps_path[2][3], g_ps_edge[2][4], g_ps_path_t[2], g_ps_found[2];   // [all, focus][calls, repeats...]
static volatile LONG64 g_ps_start[2][2], g_ps_fail_t[2], g_ps_fail_start[2][2];           // same unit, start and options searched before: run, loop
static int ps_seen(int s, uint32_t gen, uint64_t key) {          // 1 = already in set s of this generation
    pset_t* p = &t_ps[s];
    if (!p->k) { p->k = (uint64_t*)calloc(PS_SLOTS, sizeof *p->k); p->g = (uint32_t*)calloc(PS_SLOTS, sizeof *p->g); if (!p->k || !p->g) return 0; }
    uint32_t i = (uint32_t)(key >> 20) & (PS_SLOTS - 1);
    for (int n = 0; n < 64; n++, i = (i + 1) & (PS_SLOTS - 1)) {
        if (p->g[i] != gen) { p->g[i] = gen; p->k[i] = key; return 0; }
        if (p->k[i] == key) return 1;
    }
    return 0;                                         // crowded: counted as new
}
static inline uint64_t ps_mix(uint64_t h, uint64_t v) { h = (h ^ v) * 0x9E3779B97F4A7C15ull; return h ^ h >> 29; }
static uint64_t hk_path(uint8_t* functor, uint8_t* unit, uint8_t* from, uint8_t* to, void* out, uint8_t* opts, uint64_t p7, uint64_t p8) {
    void* ra = __builtin_return_address(0);
    if (!g_cfg.test_path_stats || !g_ai_loop_active) return path_do(ra, functor, unit, from, to, (uint64_t*)out, opts, p7, p8);
    uint64_t key = ps_mix(ps_mix(ps_mix(ps_mix(1, *(uint64_t*)functor), (uintptr_t)unit), (uintptr_t)from), (uintptr_t)to);
    if (opts) for (int i = 0; i < 0x48; i += 8) key = ps_mix(key, *(uint64_t*)(opts + i));
    uint32_t run = (uint32_t)t_node_run * 2654435761u ^ (uint32_t)g_ai_loop_n;
    int r_run = ps_seen(0, run | 1, key), r_loop = ps_seen(1, (uint32_t)g_ai_loop_n | 1, key);
    uint64_t ks = ps_mix(ps_mix(ps_mix(3, *(uint64_t*)functor), (uintptr_t)unit), (uintptr_t)from);    // the search without its target
    if (opts) for (int i = 0; i < 0x48; i += 8) ks = ps_mix(ks, *(uint64_t*)(opts + i));
    int s_run = ps_seen(5, run | 1, ks), s_loop = ps_seen(6, (uint32_t)g_ai_loop_n | 1, ks);
    uint32_t saved = t_ps_search;
    t_ps_search = (t_ps_search * 7 + 3 + (uint32_t)(uintptr_t)&key) | 1;        // a generation of its own for this search's edges
    LONG64 e0 = t_ps_edges; t_ps_edges = 0;
    LONG64 t0 = qpc();
    uint64_t r = path_do(ra, functor, unit, from, to, (uint64_t*)out, opts, p7, p8);
    LONG64 d = qpc() - t0;
    LONG64 edges = t_ps_edges; t_ps_edges = e0 + edges;
    t_ps_search = saved;
    if (t_focus && d * g_ms > 0.3 && InterlockedIncrement(&g_ps_samples) <= 120) {
        uint8_t* b = (uint8_t*)g_exe;
        uint8_t* fl = from ? *(uint8_t**)(from + 0x28) : NULL; uint8_t* tl = to ? *(uint8_t**)(to + 0x28) : NULL;
        eb_log("ai path: TEST sample loop %ld run %u: %s in %.2f ms, %lld edge checks | caller eu4.exe+0x%llx functor vt +0x%llx | unit %p (%d regiments, vt +0x%llx) "
               "from %d (flags %02x %02x, owner %.3s) to %d (flags %02x %02x, owner %.3s) | opts %016llx %016llx %016llx %016llx %016llx | p7 %llx p8 %llx | out count %d",
               (long)g_ai_loop_n, t_node_run, (uint8_t)r ? "FOUND" : "no path", d * g_ms, (long long)edges,
               (unsigned long long)((uint8_t*)ra - b), (unsigned long long)(*(uint8_t**)functor - b),
               (void*)unit, (int)((*(uint8_t**)(unit + 0x38) - *(uint8_t**)(unit + 0x30)) >> 3), (unsigned long long)(*(uint8_t**)unit - b),
               from ? *(int*)(from + 0x20) : -1, fl ? fl[0x228] : 0, fl ? fl[0x229] : 0, from ? (char*)(from + 0x468) : "?",
               to ? *(int*)(to + 0x20) : -1, tl ? tl[0x228] : 0, tl ? tl[0x229] : 0, to ? (char*)(to + 0x468) : "?",
               opts ? *(unsigned long long*)opts : 0ull, opts ? *(unsigned long long*)(opts + 8) : 0ull, opts ? *(unsigned long long*)(opts + 0x10) : 0ull,
               opts ? *(unsigned long long*)(opts + 0x18) : 0ull, opts ? *(unsigned long long*)(opts + 0x20) : 0ull,
               (unsigned long long)p7, (unsigned long long)p8, out ? *(int*)((uint8_t*)out + 0x10) : -1);
    }
    for (int f = 0; f <= t_focus; f++) {
        InterlockedIncrement64(&g_ps_path[f][0]);
        if (r_run) InterlockedIncrement64(&g_ps_path[f][1]);
        if (r_loop) InterlockedIncrement64(&g_ps_path[f][2]);
        InterlockedAdd64(&g_ps_path_t[f], d);
        if ((uint8_t)r) InterlockedIncrement64(&g_ps_found[f]);
        else { InterlockedAdd64(&g_ps_fail_t[f], d); if (s_run) InterlockedIncrement64(&g_ps_fail_start[f][0]); if (s_loop) InterlockedIncrement64(&g_ps_fail_start[f][1]); }
        if (s_run) InterlockedIncrement64(&g_ps_start[f][0]);
        if (s_loop) InterlockedIncrement64(&g_ps_start[f][1]);
    }
    return r;
}
static uint64_t hk_edge(uint8_t* functor, uint8_t* unit, uint64_t p3, uint8_t* from, uint8_t* to, uint8_t* adj, uint8_t* opts) {
    if (g_ai_loop_active) {
        t_ps_edges++;
        uint64_t key = ps_mix(ps_mix(ps_mix(ps_mix(ps_mix(2, (uintptr_t)unit), p3), (uintptr_t)from), (uintptr_t)to), (uintptr_t)adj);
        if (opts) key = ps_mix(ps_mix(key, *(uint64_t*)opts), *(uint64_t*)(opts + 8));
        uint32_t run = (uint32_t)t_node_run * 2654435761u ^ (uint32_t)g_ai_loop_n;
        int r_s = ps_seen(2, t_ps_search | 1, key), r_run = ps_seen(3, run | 1, key), r_loop = ps_seen(4, (uint32_t)g_ai_loop_n | 1, key);
        for (int f = 0; f <= t_focus; f++) {
            InterlockedIncrement64(&g_ps_edge[f][0]);
            if (r_s) InterlockedIncrement64(&g_ps_edge[f][1]);
            if (r_run) InterlockedIncrement64(&g_ps_edge[f][2]);
            if (r_loop) InterlockedIncrement64(&g_ps_edge[f][3]);
        }
    }
    return o_edge(functor, unit, p3, from, to, adj, opts);
}
static void ps_report(void) {
    for (int f = 0; f < 2; f++) {
        if (!g_ps_path[f][0] && !g_ps_edge[f][0]) continue;
        eb_log("ai path: TEST %s: %lld path searches (%.1f ms a loop, %.0f us each, %lld found), asked before in the same AI task run %lld (%.1f%%), "
               "in the same AI loop on the thread %lld (%.1f%%) | %lld edge checks (%.0f a search): asked before in the same search %lld (%.1f%%), "
               "the same run %lld (%.1f%%), the same loop %lld (%.1f%%)", f ? g_cfg.ai_item_focus : "all countries",
               (long long)g_ps_path[f][0], g_ps_path_t[f] * g_ms / (g_loops ? g_loops : 1), g_ps_path[f][0] ? g_ps_path_t[f] * g_ms * 1000.0 / g_ps_path[f][0] : 0.0,
               (long long)g_ps_found[f], (long long)g_ps_path[f][1], g_ps_path[f][0] ? 100.0 * g_ps_path[f][1] / g_ps_path[f][0] : 0.0,
               (long long)g_ps_path[f][2], g_ps_path[f][0] ? 100.0 * g_ps_path[f][2] / g_ps_path[f][0] : 0.0,
               (long long)g_ps_edge[f][0], g_ps_path[f][0] ? (double)g_ps_edge[f][0] / g_ps_path[f][0] : 0.0,
               (long long)g_ps_edge[f][1], g_ps_edge[f][0] ? 100.0 * g_ps_edge[f][1] / g_ps_edge[f][0] : 0.0,
               (long long)g_ps_edge[f][2], g_ps_edge[f][0] ? 100.0 * g_ps_edge[f][2] / g_ps_edge[f][0] : 0.0,
               (long long)g_ps_edge[f][3], g_ps_edge[f][0] ? 100.0 * g_ps_edge[f][3] / g_ps_edge[f][0] : 0.0);
        LONG64 nf = g_ps_path[f][0] - g_ps_found[f];
        eb_log("ai path: TEST %s: searches without a path %lld, %.1f ms a loop (%.0f us each; the found ones %.0f us each) | the same unit, start and options "
               "searched before (any target): in the same run %lld, in the same loop %lld; of the searches without a path %lld and %lld", f ? g_cfg.ai_item_focus : "all countries",
               (long long)nf, g_ps_fail_t[f] * g_ms / (g_loops ? g_loops : 1), nf ? g_ps_fail_t[f] * g_ms * 1000.0 / nf : 0.0,
               g_ps_found[f] ? (g_ps_path_t[f] - g_ps_fail_t[f]) * g_ms * 1000.0 / g_ps_found[f] : 0.0,
               (long long)g_ps_start[f][0], (long long)g_ps_start[f][1], (long long)g_ps_fail_start[f][0], (long long)g_ps_fail_start[f][1]);
    }
}

// diagnostics [parallel] ai_item_focus=TAG (with ai_item_stats): that country's AI update runs through item_focus,
// a frame of its own on the stack, so that a profile of all threads can be cut down to that one country (the
// log has the frame's address; bench tool prof_focus.py).
static volatile LONG64 g_focus_runs;
static __attribute__((noinline)) void item_focus(uint8_t* ai) {
    t_focus = 1;
    o_item(ai);
    t_focus = 0;
    InterlockedIncrement64(&g_focus_runs);            // (also keeps the call above from becoming a jump)
}
static void hk_item(uint8_t* ai) {
    t_nested = 0;
    t_ph[0] = t_ph[1] = t_ph[2] = 0;
    LONG64 t0 = qpc();
    if (g_cfg.ai_item_focus[0] && !memcmp(ai + 0x20, g_cfg.ai_item_focus, 3)) item_focus(ai);
    else o_item(ai);
    LONG64 d = qpc() - t0;
    int idx = *(int16_t*)(ai + 0x24);
    if (idx < 0 || idx >= NTAG) return;
    if (!g_tag[idx][0]) memcpy(g_tag[idx], ai + 0x20, 3);
    InterlockedAdd64(&g_total[idx], d);
    InterlockedIncrement(&g_calls[idx]);
    for (LONG64 m = g_max[idx]; d > m && InterlockedCompareExchange64(&g_max[idx], d, m) != m; m = g_max[idx]) {}
    for (LONG64 m = g_loop_max; d > m; m = g_loop_max)
        if (InterlockedCompareExchange64(&g_loop_max, d, m) == m) {
            InterlockedExchange(&g_loop_max_tag, idx);
            for (int k = 0; k < 3; k++) InterlockedExchange64(&g_max_ph[k], t_ph[k]);
            break;
        }
}

static int cmp_f(const void* a, const void* b) { float x = *(const float*)a, y = *(const float*)b; return x < y ? -1 : x > y; }

static void report(void) {
    eb_log("ai items: %lld loops: wall %.2f ms, slowest item %.2f ms (%.0f%% of the wall), %.0f items per loop",
           (long long)g_loops, g_wall * g_ms / g_loops, g_slow * g_ms / g_loops, 100.0 * g_slow / (g_wall ? g_wall : 1), (double)g_items / g_loops);
    int n = g_loops < NWALL ? (int)g_loops : NWALL;
    static float tmp[NWALL];
    memcpy(tmp, g_walls, sizeof(float) * (size_t)n);
    qsort(tmp, (size_t)n, sizeof(float), cmp_f);
    eb_log("ai items:   wall of the last %d loops: median %.2f, p90 %.2f, p99 %.2f, max %.2f ms", n, tmp[n / 2], tmp[n * 9 / 10], tmp[n * 99 / 100], tmp[n - 1]);
    eb_log("ai items:   slowest item by phase (king: before+military / other children / armies+tail): %.2f / %.2f / %.2f ms; "
           "loops over 30 ms (%lld): slowest %.2f = %.2f / %.2f / %.2f ms",
           g_slow_ph[0] * g_ms / g_loops, g_slow_ph[1] * g_ms / g_loops, g_slow_ph[2] * g_ms / g_loops, (long long)g_spiky_n,
           g_spiky_n ? g_spiky_slow * g_ms / g_spiky_n : 0.0, g_spiky_n ? g_spiky_ph[0] * g_ms / g_spiky_n : 0.0,
           g_spiky_n ? g_spiky_ph[1] * g_ms / g_spiky_n : 0.0, g_spiky_n ? g_spiky_ph[2] * g_ms / g_spiky_n : 0.0);
    int top[10]; int nt = 0;
    for (int k = 0; k < 10; k++) {
        int best = -1;
        for (int i = 0; i < NTAG; i++) {
            if (!g_calls[i]) continue;
            int used = 0; for (int j = 0; j < nt; j++) if (top[j] == i) used = 1;
            if (!used && (best < 0 || g_total[i] > g_total[best])) best = i;
        }
        if (best < 0) break;
        top[nt++] = best;
    }
    for (int j = 0; j < nt; j++) {
        int i = top[j];
        eb_log("ai items:   %-3.3s (#%d) avg %.2f ms, max %.2f ms, slowest in %ld of %ld loops",
               g_tag[i], i, g_total[i] * g_ms / g_calls[i], g_max[i] * g_ms, (long)g_slowest[i], (long)g_calls[i]);
    }
}

// After loop 50 (workers idle): the task tree of every country into eu4boost_aitree.txt. The tree
// root is the country's "king" *(ai+0x28) (FUN_1400b0fa0 calls its vt+0x58 = FUN_14014b370).
static void dump_node(FILE* f, uint8_t* n, int depth) {
    uintptr_t vt = *(uintptr_t*)n;
    int kids = 0;
    for (uint8_t** q = *(uint8_t***)(n + 0x40); q && kids < 100000; q = (uint8_t**)q[2]) kids++;
    fprintf(f, "%*s%llx cd=%d kids=%d node=%p\n", depth * 2, "", (unsigned long long)(vt - (uintptr_t)g_exe + 0x140000000ull),
            *(int*)(n + 0x20), kids, (void*)n);
    if (depth > 6) return;
    // children of one class in a row: print the first two and a count
    uintptr_t prev = 0; int run = 0;
    for (uint8_t** q = *(uint8_t***)(n + 0x40); q; q = (uint8_t**)q[2]) {
        uint8_t* c = q[0]; uintptr_t cvt = *(uintptr_t*)c;
        if (cvt == prev) { if (++run >= 2) continue; } else { prev = cvt; run = 0; }
        dump_node(f, c, depth + 1);
    }
}

static void dump_trees(uint8_t* items) {
    wchar_t path[MAX_PATH]; _snwprintf(path, MAX_PATH, L"%lseu4boost_aitree.txt", g_dir);
    FILE* f = _wfopen(path, L"w");
    if (!f) return;
    uint8_t** ai = *(uint8_t***)(items + 8);
    int n = *(int*)(items + 0x14);
    for (int i = 0; i < n; i++) {
        uint8_t* king = *(uint8_t**)(ai[i] + 0x28);
        int idx = *(int16_t*)(ai[i] + 0x24);
        fprintf(f, "== %.3s #%d ai=%p king=%p avg %.2f ms\n", (char*)(ai[i] + 0x20), idx, (void*)ai[i], (void*)king,
                (idx >= 0 && idx < NTAG && g_calls[idx]) ? g_total[idx] * g_ms / g_calls[idx] : 0.0);
        if (king) dump_node(f, king, 1);
    }
    fclose(f);
    eb_log("ai items: task trees of %d countries written to eu4boost_aitree.txt", n);
}

static void hk_loop(uint8_t* items, void* ctx) {
    InterlockedExchange64(&g_loop_max, 0);
    InterlockedExchange(&g_loop_max_tag, -1);
    for (int k = 0; k < 3; k++) g_max_ph[k] = 0;
    LONG64 t0 = qpc();
    if (g_aec && ctx && !*(volatile uint8_t*)((uint8_t*)g_exe + RVA_FIXED_SEED_FLAG)) {
        // FUN_14075b4b0 passes &map (FUN_140478280: float max load 1.0 at +0, lock flag at +0x48)
        uint8_t* m = *(uint8_t**)ctx;
        if (m && *(uint32_t*)m == 0x3f800000u && m[0x48] == 1) {
            InterlockedIncrement(&g_ae_gen);
            InterlockedExchangePointer(&g_ae_cache, m);
        }
    }
    InterlockedIncrement(&g_ai_loop_n);
    InterlockedExchange(&g_ai_loop_active, 1);
    o_loop(items, ctx);
    InterlockedExchange(&g_ai_loop_active, 0);
    InterlockedExchangePointer(&g_ae_cache, NULL);
    if ((g_split || g_prov || g_ahead) && g_qn) { // jobs left behind (done by their owners): drop
        AcquireSRWLockExclusive(&g_qlock);
        while (g_qn) job_discard(g_q[--g_qn]);
        ReleaseSRWLockExclusive(&g_qlock);
    }
    LONG64 w = qpc() - t0;
    if (g_ahead == 3) {
        int k = g_ai_loop_n & 1;
        g_pf_ab_wall[k] += w; g_pf_ab_loops[k]++; if (w > g_pf_ab_max[k]) g_pf_ab_max[k] = w;
        if (g_pf_loop_fails >= 3) { g_pf_ab_wall3[k] += w; g_pf_ab_loops3[k]++; g_pf_ab_t3[k] += g_pf_loop_t; }
        InterlockedExchange(&g_pf_loop_fails, 0); InterlockedExchange64(&g_pf_loop_t, 0);
    }
    g_wall += w;
    g_slow += g_loop_max;
    g_walls[g_loops % NWALL] = (float)(w * g_ms);
    for (int k = 0; k < 3; k++) g_slow_ph[k] += g_max_ph[k];
    if (w * g_ms > 30.0) { g_spiky_n++; g_spiky_slow += g_loop_max; for (int k = 0; k < 3; k++) g_spiky_ph[k] += g_max_ph[k]; }
    g_items += *(int*)(items + 0x14);
    if (g_loop_max_tag >= 0) g_slowest[g_loop_max_tag]++;
    ++g_loops;
    if (g_split && g_loops % 500 == 0)
        eb_log("ai split: %lld loops, %lld army jobs (%.1f nodes each): %lld run by helpers, %lld by their king (%lld waited)",
               (long long)g_loops, (long long)g_jobs, g_jobs ? (double)g_job_nodes / g_jobs : 0.0,
               (long long)g_by_helper, (long long)g_inline, (long long)g_waited);
    if (g_prov && g_loops % 500 == 0)
        eb_log("ai prov: %lld loops, %lld big evaluations (%.0f scoring calls each; record+replay %.2f ms, scoring %.2f ms per loop): "
               "%lld chunks by idle threads, %lld by the owner; replay %lld hits, %lld misses | verify %lld, mismatches %lld",
               (long long)g_loops, (long long)g_pl_runs, g_pl_runs ? (double)g_pl_calls / g_pl_runs : 0.0,
               g_pl_t_serial * g_ms / g_loops, g_pl_t_par * g_ms / g_loops, (long long)g_pl_tick, (long long)g_pl_own,
               (long long)g_pl_hits, (long long)g_pl_miss, (long long)g_pl_ver, (long long)g_pl_bad);
    if (g_prov == 3 && g_loops % 500 == 0)
        for (int s = 0; s < 2; s++)
            eb_log("ai prov A/B %s: %lld big evaluations, avg %.2f ms, max %.2f ms; by time <2/5/10/20/50/more ms: %lld/%lld/%lld/%lld/%lld/%lld",
                   s ? "B (experiment)" : "A (original)  ", (long long)g_ab_n[s], g_ab_n[s] ? g_ab_t[s] * g_ms / g_ab_n[s] : 0.0, g_ab_max[s] * g_ms,
                   (long long)g_ab_hist[s][0], (long long)g_ab_hist[s][1], (long long)g_ab_hist[s][2], (long long)g_ab_hist[s][3],
                   (long long)g_ab_hist[s][4], (long long)g_ab_hist[s][5]);
    if (g_army && g_loops % 500 == 0) {
        ac_flush(&t_ac);
        eb_log("ai army: %lld loops: %lld move checks, %lld answered by the flags first (%.1f%%); modifier 0xe0 asked %lld times, "
               "%lld from the kept answer (%.1f%%) | verify %lld, mismatches %lld",
               (long long)g_loops, (long long)g_ac_calls, (long long)g_ac_skip, g_ac_calls ? 100.0 * g_ac_skip / g_ac_calls : 0.0,
               (long long)g_ac_hm, (long long)g_ac_hm_hit, g_ac_hm ? 100.0 * g_ac_hm_hit / g_ac_hm : 0.0,
               (long long)g_ac_ver, (long long)g_ac_bad);
        if (g_army == 3)
            for (int s = 0; s < 2; s++)
                eb_log("ai army A/B %s: %lld checks, avg %.3f us, total %.1f ms", s ? "B (flags first)" : "A (original)   ",
                       (long long)g_ac_n[s], g_ac_n[s] ? g_ac_t[s] * g_ms * 1000.0 / g_ac_n[s] : 0.0, g_ac_t[s] * g_ms);
    }
    if (g_aec && g_loops % 500 == 0) {
        ae_flush(&t_at);                              // the main thread's own counts; workers flush on their next loop
        eb_log("ai ae: %lld loops: %lld AE sums in C; %lld lookups in the per-thread tables (%lld hits, %lld computed, %lld negative), "
               "largest table %ld | verify: sums %lld, mismatches %lld; lookups %lld, mismatches %lld%s", (long long)g_loops, (long long)g_ae_sums,
               (long long)(g_ae_hit + g_ae_calc + g_ae_neg), (long long)g_ae_hit, (long long)g_ae_calc, (long long)g_ae_neg, (long)g_ae_maxn,
               (long long)g_ae_sum_ver, (long long)g_ae_sum_bad, (long long)g_ae_ver, (long long)g_ae_bad,
               g_ae_nomem ? " - OUT OF MEMORY, fell back to the game's map" : "");
        if (g_ui_on) {
            ae_flush(&t_ut);
            eb_log("ai ae: outside the AI loop (main thread): %lld deal fills %.1f ms, %lld acceptance checks %.1f ms; %lld AE sums in C; "
                   "%lld lookups (%lld hits, %lld computed, %lld negative), largest table %ld; land searches %lld (kept %lld) | verify: lookups %lld, mismatches %lld; land searches %lld, mismatches %lld | deal parts computed %lld, reused %lld",
                   (long long)g_ui_n[0], g_ui_t[0] * g_ms, (long long)g_ui_n[1], g_ui_t[1] * g_ms, (long long)g_ui_sums,
                   (long long)(g_ui_hit + g_ui_calc + g_ui_neg), (long long)g_ui_hit, (long long)g_ui_calc, (long long)g_ui_neg, (long)g_ui_maxn,
                   (long long)g_lp_calls, (long long)g_lp_hit, (long long)g_ui_ver, (long long)g_ui_bad, (long long)g_lp_ver, (long long)g_lp_bad,
                   (long long)g_dm_new, (long long)g_dm_reuse);
        }
        if (g_alm) {
            eb_log("ai land: in the AI loop %lld deal fills and acceptance checks (%.1f ms, longest %.1f ms), land searches %lld (kept %lld) | verify %lld, mismatches %lld",
                   (long long)(g_ad_n[0] + g_ad_n[1]), (g_ad_t[0] + g_ad_t[1]) * g_ms, (g_ad_max[0] > g_ad_max[1] ? g_ad_max[0] : g_ad_max[1]) * g_ms,
                   (long long)g_alp_calls, (long long)g_alp_hit, (long long)g_alp_ver, (long long)g_alp_bad);
            if (g_alm == 3)
                for (int s = 0; s < 2; s++)
                    eb_log("ai land A/B %s: %lld top-level calls, avg %.3f ms, total %.1f ms, longest %.1f ms", s ? "B (memo)    " : "A (the game)",
                           (long long)g_ad_n[s], g_ad_n[s] ? g_ad_t[s] * g_ms / g_ad_n[s] : 0.0, g_ad_t[s] * g_ms, g_ad_max[s] * g_ms);
        }
        if (g_wish) {
            wish_flush(); reach_flush(); plist_flush();
            eb_log("ai deal memo: %lld wish lists: %lld province valuations answered from the kept ones, %lld computed (%lld not kept, table full); "
                   "the list's own offer built %lld of %lld times; reach questions %lld, %lld of them kept; province lists %lld, %lld of them kept; most AE rows %ld | verify %lld, mismatches %lld",
                   (long long)g_w_sets, (long long)g_w_hit, (long long)g_w_calc, (long long)g_w_full, (long long)g_w_built, (long long)g_w_lazy,
                   (long long)(g_p_hit + g_p_calc), (long long)g_p_hit, (long long)(g_v_hit + g_v_calc), (long long)g_v_hit, (long)(t_av_rows > g_av_maxrows ? t_av_rows : g_av_maxrows),
                   (long long)(g_w_ver + g_p_ver + g_v_ver), (long long)(g_w_bad + g_p_bad + g_v_bad));
        }
        if (g_aec == 3)
            for (int s = 0; s < 2; s++)
                eb_log("ai ae A/B %s: %lld AE sums, avg %.2f us, total %.1f ms, max %.3f ms",
                       s ? "B (ae_sum, tables)    " : "A (game's code and map)", (long long)g_ae_ab_n[s], g_ae_ab_n[s] ? g_ae_ab_t[s] * g_ms * 1000.0 / g_ae_ab_n[s] : 0.0,
                       g_ae_ab_t[s] * g_ms, g_ae_ab_max[s] * g_ms);
    }
    if (g_cfg.test_path_stats && g_loops % 100 == 0) ps_report();
    if (g_ahead && g_loops % (g_ahead == 1 ? 500 : 100) == 0) pf_report();
    if (!g_stats) return;
    if (g_loops == 50) dump_trees(items);
    if (g_loops % 100 == 0) report();
    if (g_loops % 500 == 0) report_classes();
}

static int ai_all_off(void) {                    // nothing of this file is installed
    g_stats = g_split = g_prov = g_aec = g_alm = g_army = g_wish = g_ahead = 0;
    g_cfg.ai_item_stats = g_cfg.ai_split = g_cfg.ai_prov_split = g_cfg.ai_ae_cache = g_cfg.ai_land_memo = g_cfg.ai_army_fast = g_cfg.ai_deal_memo = g_cfg.ai_path_ahead = 0;
    return 0;
}

int eb_aiprof_install(void) {
    g_stats = g_cfg.ai_item_stats; g_split = g_cfg.ai_split == 1; g_prov = g_cfg.ai_prov_split; g_aec = g_cfg.ai_ae_cache;
    g_alm = g_aec ? g_cfg.ai_land_memo : 0;
    g_wish = g_aec ? g_cfg.ai_deal_memo : 0;
    g_army = g_cfg.ai_army_fast;
    if (!g_stats && !g_split && !g_prov && !g_aec && !g_army && !g_cfg.test_path_stats && !g_cfg.ai_path_ahead) return 1;
    uint8_t* b = (uint8_t*)g_exe;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(b + ((IMAGE_DOS_HEADER*)g_exe)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != 0x66FE6AB9u || memcmp(b + RVA_AI_ITEM, sig_item, sizeof sig_item) ||
        memcmp(b + RVA_AI_LOOP, sig_loop, sizeof sig_loop) || memcmp(b + RVA_AI_NODE, sig_node, sizeof sig_node) ||
        (g_split && (memcmp(b + RVA_KING, sig_king, sizeof sig_king) || memcmp(b + RVA_CGET, sig_cget, sizeof sig_cget) ||
                     memcmp(b + RVA_CPUT, sig_cput, sizeof sig_cput)))) {
        eb_log("ai items: signature mismatch - not installed");
        return ai_all_off();
    }
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_ms = 1000.0 / (double)f.QuadPart; g_spike = f.QuadPart / 200;
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) { eb_log("ai items: MinHook init failed"); return ai_all_off(); }
    if (!g_qsem) g_qsem = CreateSemaphoreW(NULL, 0, 0x7fffffff, NULL);
    // the AI loop and node hooks every feature below relies on (g_ai_loop_active, the per-run state) first:
    // without them none of the features is installed. Each feature is a set of its own: all of its hooks or none.
    eb_hookset hs;
    eb_hs_init(&hs, "ai loop");
    if ((g_stats && !eb_hs_add(&hs, b + RVA_AI_ITEM, (void*)hk_item, (void**)&o_item)) ||
        !eb_hs_add(&hs, b + RVA_AI_LOOP, (void*)hk_loop, (void**)&o_loop) ||
        !eb_hs_add(&hs, b + RVA_AI_NODE, (void*)hk_node, (void**)&o_node) || !eb_hs_enable(&hs)) {
        eb_log("ai items: hook failed - no AI feature installed");
        return ai_all_off();
    }
    if (g_prov) {
        eb_hs_init(&hs, "ai prov");
        if (!g_qsem || memcmp(b + RVA_PLOOP, sig_ploop, sizeof sig_ploop) || memcmp(b + RVA_PEVAL, sig_peval, sizeof sig_peval) ||
            memcmp(b + RVA_PPRE, sig_ppre, sizeof sig_ppre) ||
            !eb_hs_add(&hs, b + RVA_PEVAL, (void*)hk_peval, (void**)&o_peval) ||
            !eb_hs_add(&hs, b + RVA_PPRE, (void*)hk_ppre, (void**)&o_ppre) ||
            !eb_hs_add(&hs, b + RVA_PLOOP, (void*)hk_ploop, (void**)&o_ploop) || !eb_hs_enable(&hs)) {
            if (!hs.failed) eb_hs_undo(&hs, "a signature differs");
            eb_log("ai prov: signature mismatch or hook failed - not installed");
            g_prov = 0;
        }
    }
    if (g_army) {
        eb_hs_init(&hs, "ai army");
        if (memcmp(b + RVA_CANMOVE, sig_canmove, sizeof sig_canmove) || memcmp(b + RVA_HASMOD, sig_hasmod, sizeof sig_hasmod) ||
            !eb_hs_add(&hs, b + RVA_HASMOD, (void*)hk_hasmod, (void**)&o_hasmod) ||
            !eb_hs_add(&hs, b + RVA_CANMOVE, (void*)hk_canmove, (void**)&o_canmove) || !eb_hs_enable(&hs)) {
            if (!hs.failed) eb_hs_undo(&hs, "a signature differs");
            eb_log("ai army: signature mismatch or hook failed - not installed");
            g_army = 0;
        } else
            eb_log("ai army: EXPERIMENT on%s - the army move check tests its province flags first and keeps modifier 0xe0 per AI task run",
                   g_army == 2 ? " with VERIFY" : g_army == 3 ? " as A/B (checks alternate between the original and flags first)" : "");
    }
    if (g_aec) {
        eb_hs_init(&hs, "ai ae");
        if (memcmp(b + RVA_AEP, sig_aep, sizeof sig_aep) || memcmp(b + RVA_AECALC, sig_aecalc, sizeof sig_aecalc) ||
            memcmp(b + RVA_ETS_GET, sig_etsget, sizeof sig_etsget) || *(int32_t*)(b + RVA_AE_UNIT) != 100000 ||
            memcmp(b + RVA_AEALL, sig_aeall, sizeof sig_aeall) || memcmp(b + RVA_OPIN_AE, sig_opin_ae, sizeof sig_opin_ae) ||
            memcmp(b + RVA_OPIN, sig_opin, sizeof sig_opin) || memcmp(b + RVA_ALLIED, sig_allied, sizeof sig_allied) ||
            memcmp(b + RVA_STRENGTH, sig_strength, sizeof sig_strength) ||
            !eb_hs_add(&hs, b + RVA_AEP, (void*)hk_aep, (void**)&o_aep) ||
            !eb_hs_add(&hs, b + RVA_AEALL, (void*)hk_aeall, (void**)&o_aeall) || !eb_hs_enable(&hs)) {
            if (!hs.failed) eb_hs_undo(&hs, "a signature differs");
            eb_log("ai ae: signature mismatch or hook failed - not installed");
            g_aec = 0; g_alm = 0; g_wish = 0;
        } else {
            g_aecalc = (aecalc_t)(b + RVA_AECALC);
            g_etsget = (etsget_t)(b + RVA_ETS_GET);
            g_ae_unit = *(int32_t*)(b + RVA_AE_UNIT);
            g_opin_ae = (opin_ae_t)(b + RVA_OPIN_AE);
            g_opin = (opin_t)(b + RVA_OPIN);
            g_allied = (allied_t)(b + RVA_ALLIED);
            g_strength = (strength_t)(b + RVA_STRENGTH);
            // outside the AI loop: deal fills and acceptance checks on the main thread (report 29)
            eb_hs_init(&hs, "ai deal");
            g_ui_on = !memcmp(b + RVA_OFFER_FILL, sig_offer_fill, 16) && !memcmp(b + RVA_ACCEPT, sig_accept, 16) &&
                      eb_hs_add(&hs, b + RVA_OFFER_FILL, (void*)hk_fill, (void**)&o_fill) &&
                      eb_hs_add(&hs, b + RVA_ACCEPT, (void*)hk_accept, (void**)&o_accept) && eb_hs_enable(&hs);
            eb_hs_init(&hs, "ai land");
            if (g_ui_on && (memcmp(b + RVA_LANDPATH, sig_landpath, 16) ||
                            !eb_hs_add(&hs, b + RVA_LANDPATH, (void*)hk_landpath, (void**)&o_landpath) || !eb_hs_enable(&hs))) {
                eb_log("ai ae: land search signature mismatch or hook failed - not kept");
                g_alm = 0;
            }
            if (!g_ui_on) { eb_log("ai ae: deal fill / acceptance signature mismatch or hook failed - only the AI loop uses the tables"); g_alm = 0; }
            eb_hs_init(&hs, "ai wish");
            if (g_wish && (!g_ui_on || memcmp(b + RVA_WISH, sig_wish, 16) || memcmp(b + RVA_PVALUE, sig_pvalue, 16) ||
                           memcmp(b + RVA_OFFER_NEW, sig_offer_new, 16) || memcmp(b + RVA_OFFER_FREE, sig_offer_free, 16) ||
                           !eb_hs_add(&hs, b + RVA_WISH, (void*)hk_wish, (void**)&o_wish) ||
                           !eb_hs_add(&hs, b + RVA_PVALUE, (void*)hk_pvalue, (void**)&o_pvalue) ||
                           !eb_hs_add(&hs, b + RVA_OFFER_NEW, (void*)hk_offer_ctor, (void**)&o_offer_ctor) ||
                           !eb_hs_add(&hs, b + RVA_OFFER_FREE, (void*)hk_offer_dtor, (void**)&o_offer_dtor) ||
                           memcmp(b + RVA_PREACH, sig_preach, 16) || memcmp(b + RVA_PNEAR, sig_pnear, 16) ||
                           !eb_hs_add(&hs, b + RVA_PREACH, (void*)hk_preach, (void**)&o_preach) ||
                           !eb_hs_add(&hs, b + RVA_PNEAR, (void*)hk_pnear, (void**)&o_pnear) ||
                           memcmp(b + RVA_PLIST, sig_plist, 16) || memcmp(b + RVA_VPUSH, sig_vpush, 16) ||
                           !eb_hs_add(&hs, b + RVA_PLIST, (void*)hk_plist, (void**)&o_plist) || !eb_hs_enable(&hs))) {
                if (!hs.failed) eb_hs_undo(&hs, "a signature differs");
                eb_log("ai deal memo: signature mismatch or hook failed - not installed");
                g_wish = 0;
            } else g_vpush = (vpush_t)(b + RVA_VPUSH);
        }
    }
    if (g_stats) {
        eb_hs_init(&hs, "ai fm");
        int ok = 1;
        for (int k = 0; k < NFM && ok; k++) {
            if (g_aec && (g_fmf[k].rva == RVA_AEP || g_fmf[k].rva == RVA_AEALL || (g_ui_on && g_fmf[k].rva == RVA_OFFER_FILL))) continue;   // hooked by the experiment
            if (g_wish && g_fmf[k].rva == RVA_OFFER_NEW) continue;
            ok = !memcmp(b + g_fmf[k].rva, g_fmf[k].sig, 8) && eb_hs_add(&hs, b + g_fmf[k].rva, g_fm_hk[k], (void**)&o_fm[k]);
        }
        if (!ok && !hs.failed) eb_hs_undo(&hs, "a signature differs");
        g_fm_on = ok && eb_hs_enable(&hs);
        eb_log(g_fm_on ? "ai fm: timing the foreign minister's functions" : "ai fm: hook failed - no foreign-minister breakdown");
    }
    if (g_stats && !g_split) {
        eb_hs_init(&hs, "ai king");
        if (memcmp(b + RVA_KING, sig_king, sizeof sig_king) || !eb_hs_add(&hs, b + RVA_KING, (void*)hk_king, (void**)&o_king) || !eb_hs_enable(&hs))
            eb_log("ai items: king hook failed - no phase times");
    }
    if (g_split) {
        for (int i = 0; i < NSTRIPE; i++) InitializeSRWLock(&g_ctx_lock[i]);
        eb_hs_init(&hs, "ai split");
        if (!g_qsem ||
            !eb_hs_add(&hs, b + RVA_CGET, (void*)hk_cget, (void**)&o_cget) ||
            !eb_hs_add(&hs, b + RVA_CPUT, (void*)hk_cput, (void**)&o_cput) ||
            !eb_hs_add(&hs, b + RVA_KING, (void*)hk_king, (void**)&o_king) || !eb_hs_enable(&hs)) {
            eb_log("ai split: hook failed - not installed");
            g_split = 0;
        }
    }
    if (g_split || g_prov) { eb_par_help = ai_help; eb_par_linger = ai_linger; }
    if (g_prov)
        eb_log("ai prov: EXPERIMENT on%s - the province evaluation of countries with %d+ provinces is scored on all cores",
               g_prov == 2 ? " with VERIFY" : g_prov == 3 ? " as A/B (even loops original, odd loops experiment)" : "", g_cfg.ai_prov_min);
    if (g_split) {
        eb_log("ai split: EXPERIMENT on - the army AI of countries with %d+ armies runs beside their other AI tasks", g_cfg.ai_split_min);
    }
    if (g_aec)
        eb_log("ai ae: EXPERIMENT on%s - AE sums in C and the AI loop's AE cache lookups in per-thread tables%s",
               g_aec == 2 ? " with VERIFY" : g_aec == 3 ? " as A/B (AE sums alternate between the game's code and ae_sum)" : "",
               g_ui_on ? "; also deal fills and acceptance checks on the main thread outside the AI loop (war view, peace windows)" : "");
    if (g_alm)
        eb_log("ai land: EXPERIMENT on%s - the land searches of a peace deal in the AI loop are kept for that deal",
               g_alm == 2 ? " with VERIFY" : g_alm == 3 ? " as A/B (deals alternate between the game's searches and the memo)" : "");
    else if (g_cfg.ai_land_memo) eb_log("ai land: off - needs [parallel] ai_ae_cache and its deal hooks");
    if (g_stats) eb_log("ai items: timing every country of the AI loop and every AI task run (reports every 100 / 500 loops)");
    g_ahead = g_cfg.ai_path_ahead;
    if (g_cfg.test_path_stats || g_ahead) {
        eb_hs_init(&hs, "ai path");
        int ok = !memcmp(b + RVA_PATH, sig_path, 16) && eb_hs_add(&hs, b + RVA_PATH, (void*)hk_path, (void**)&o_path);
        if (ok && g_cfg.test_path_stats) ok = !memcmp(b + RVA_EDGE, sig_edge, 16) && eb_hs_add(&hs, b + RVA_EDGE, (void*)hk_edge, (void**)&o_edge);
        if (ok && g_ahead)
            ok = g_qsem && !memcmp(b + RVA_SUPPORT, sig_support, 16) && !memcmp(b + RVA_GFREE, sig_gfree, 5) &&
                 eb_hs_add(&hs, b + RVA_SUPPORT, (void*)hk_support, (void**)&o_support) &&
                 (g_split || (!memcmp(b + RVA_CGET, sig_cget, sizeof sig_cget) && !memcmp(b + RVA_CPUT, sig_cput, sizeof sig_cput) &&
                              eb_hs_add(&hs, b + RVA_CGET, (void*)hk_cget, (void**)&o_cget) &&
                              eb_hs_add(&hs, b + RVA_CPUT, (void*)hk_cput, (void**)&o_cput)));
        if (!ok || !eb_hs_enable(&hs)) {
            if (!hs.failed) eb_hs_undo(&hs, "a signature differs");
            eb_log("ai path: signature mismatch or hook failed - not installed");
            g_ahead = 0; g_cfg.test_path_stats = 0;
        } else {
            g_gfree = (gfree_t)(b + RVA_GFREE);
            if (g_cfg.test_path_stats) eb_log("ai path: TEST counting the path searches and edge checks of the AI loop");
            if (g_ahead) {
                eb_par_help = ai_help; eb_par_linger = ai_linger;
                eb_log("ai path ahead: EXPERIMENT on%s - the path searches of an army list that found no path lately are run ahead on idle threads",
                       g_ahead == 2 ? " with VERIFY" : g_ahead == 3 ? " as A/B (odd AI loops ahead, even ones the game's way)" : "");
            }
        }
    }
    g_cfg.ai_path_ahead = g_ahead;
    if (g_stats && g_cfg.ai_item_focus[0]) {
        HMODULE me = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(void*)item_focus, &me);
        eb_log("ai items: focus on %.3s - its AI update runs through eu4boost.dll+0x%llx", g_cfg.ai_item_focus,
               (unsigned long long)((uintptr_t)(void*)item_focus - (uintptr_t)me));
    }
    if (g_wish)
        eb_log("ai deal memo: EXPERIMENT on%s - within one peace deal fill the provinces a country wishes for are valued once, and the reach questions and province lists are asked once",
               g_wish == 2 ? " with VERIFY" : "");
    else if (g_cfg.ai_deal_memo) eb_log("ai deal memo: off - needs [parallel] ai_ae_cache and its deal hooks");
    g_cfg.ai_split = g_split; g_cfg.ai_prov_split = g_prov; g_cfg.ai_ae_cache = g_aec; g_cfg.ai_land_memo = g_alm; g_cfg.ai_army_fast = g_army;
    g_cfg.ai_deal_memo = g_wish;
    return 1;
}
