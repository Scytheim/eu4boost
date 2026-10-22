// Parallel-for instrumentation and experiments ([parallel] section). chunked and small_loop_caps are on
// by default since 0.3.0 (fixed-seed checksum identical, normal-mode A/B -2.8% time); the rest is off.
//
// The game's parallel for (see the analysis doc):
//   dispatcher  eu4.exe+0x1798a0(self, ?, start, count, item_fn*, done_fn*)
//       FUN_14017a070(out, mode, int* ntasks, start, count) creates the shared state; *ntasks < 1
//       means "hardware_concurrency - 1" (11 on a 12-thread CPU). One job per task is pushed to
//       the pool (mutex + deque + notify_one each).
//   task body   eu4.exe+0x17a200(param): takes items one by one (lock xadd on ctx+0x44 < ctx+0xa8)
//   main wait   eu4.exe+0x179e90(self): the main thread takes items too, then spins until every
//       item is completed
//   worker loop eu4.exe+0x1712d70: when the queue is empty it calls _Cnd_wait (eu4.exe+0x1a13b48)
//       at +0x1712dc4, i.e. sleeps in the kernel until the next notify_one.
//
// stats=1        per call site: calls, items, wall time, dispatch time, main-thread wait, worker
//                start latency, tasks that arrived after all items were taken ("late")
// min_items_per_task=K  use at most count/K tasks for a loop (never more than the game would)
//                - only for loops without a completion callback, whose firing depends on the
//                  task count (it runs when the (ntasks-1)-th participant finishes)
// worker_spin_us=N      a worker whose queue is empty first spins (with pause) up to N us before
//                sleeping, so the next loop right after does not pay the kernel wake-up
// spin_workers=M        at most M workers spin at the same time (others sleep at once)
//
// None of these change which items run or what they compute; only how many threads take part
// and how they wait.
#include "common.h"
#include <string.h>
#include <intrin.h>
#include "MinHook.h"

#define RVA_DISPATCH   0x1798a0u
#define RVA_WAIT       0x179e90u
#define RVA_TASK       0x17a200u
#define RVA_STATEINIT  0x17a070u
#define RVA_CNDWAIT    0x1a13b48u
#define RVA_MTX_LOCK   0x1a139ccu
#define RVA_MTX_UNLOCK 0x1a139f0u
#define RVA_WORKER_WAIT_RET 0x1712dc9u      // return address of the _Cnd_wait call in the worker loop
#define RVA_POOL1      0x242b8e0u           // {mtx*, cv*, deque{..., size at +0x30}}
#define RVA_POOL2      0x242b950u

static const uint8_t sig_dispatch[16] = { 0x48,0x89,0x5c,0x24,0x18,0x89,0x54,0x24,0x10,0x55,0x56,0x57,0x41,0x54,0x41,0x55 };
static const uint8_t sig_wait[16]     = { 0x48,0x89,0x5c,0x24,0x20,0x57,0x48,0x83,0xec,0x40,0x48,0x8b,0xf9,0x48,0x83,0xc1 };
static const uint8_t sig_task[16]     = { 0x48,0x89,0x5c,0x24,0x20,0x48,0x89,0x4c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48 };
static const uint8_t sig_state[16]    = { 0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57 };
static const uint8_t sig_cndwait[16]  = { 0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x48 };
static const uint8_t sig_mtxlock[8]   = { 0x33,0xd2,0xe9,0xf9,0xfd,0xff,0xff,0xcc };
static const uint8_t sig_mtxunlock[8] = { 0x48,0x83,0xec,0x28,0x83,0x69,0x4c,0x01 };

typedef void  (*dispatch_t)(void*, void*, int, int, void*, void*);
typedef void  (*wait_t)(void*);
typedef void  (*task_t)(void**);
typedef void* (*state_t)(void*, int, int*, int, int);
typedef int   (*cndwait_t)(void*, void*);
typedef int   (*mtx_t)(void*);
static dispatch_t o_dispatch; static wait_t o_wait; static task_t o_task; static state_t o_state; static cndwait_t o_cndwait;
static mtx_t mtx_lock, mtx_unlock;
static uint8_t* g_base;
static double g_us;                 // microseconds per QPC tick

static inline LONGLONG qpc(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

// ---------------- stats ----------------
#define NSITES 32
typedef struct {
    uint32_t site;                  // RVA of the dispatcher's return address
    volatile LONG64 calls, items, tasks, late, capped;
    volatile LONG64 t_wall, t_disp, t_wait, t_lat, lat_n, lat_max;
} site_t;
static site_t g_sites[NSITES];
static volatile LONG g_nsites;
static volatile int g_cur = -1;           // site of the loop most recently dispatched (main thread)
static volatile LONGLONG g_cur_t0;
static __thread int t_site = -1;          // per dispatching thread
static __thread LONGLONG t_t0;
static __thread int t_req_tasks;          // task count wanted for the next FUN_14017a070 call

static int site_index(uint32_t rva) {
    LONG n = g_nsites;
    for (int i = 0; i < n; i++) if (g_sites[i].site == rva) return i;
    if (n >= NSITES) return -1;
    // only the dispatching (main) thread adds sites
    g_sites[n].site = rva; InterlockedExchange(&g_nsites, n + 1);
    return n;
}

static void* hk_state(void* out, int mode, int* ntasks, int start, int count) {
    if (t_req_tasks > 0 && ntasks && *ntasks < 1) { *ntasks = t_req_tasks; }
    t_req_tasks = 0;
    return o_state(out, mode, ntasks, start, count);
}

static int default_tasks(int mode) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors - 1; if (n < 1) n = 1;
    return mode == 1 ? n + 1 : n;
}

// ---------------- per-site task caps (small loops) ----------------
// From the 0.3.0-dev stats run (1596-1597, 12 threads): these loops are so short that handing out
// 11 jobs (20-30 us) costs as much as the work itself. RVA = return address of the dispatcher call.
static const struct { uint32_t site; int tasks; } g_caps[] = {
    { 0x169be36, 1 },   //   62 items, wall 26 us = dispatch 25 us, 84% of tasks arrive after the end
    { 0xdb2525,  2 },   //  257 items, wall 26 us, 41% late
    { 0x424462,  4 },   // 1380 items, wall 58 us (22 dispatch), 22 calls/day (per-country loop) -> 34 us
    // tried and reverted (dev2, fixed-seed A/B): 4 tasks made these slower (heavier items)
    //   0x191b42 516 items 75 -> 93 us, 0xe0ff78 516 items 73 -> 87 us, 0x77f8c1 258 items 100 -> 128 us
};
static int site_cap(uint32_t rva) {
    for (size_t i = 0; i < sizeof g_caps / sizeof g_caps[0]; i++) if (g_caps[i].site == rva) return g_caps[i].tasks;
    return 0;
}

// ---------------- chunked item claiming (re-implementation of task body and main wait) --------
// Same structure as eu4.exe+0x17a200 / +0x179e90, but a participant may claim several consecutive
// items with one lock xadd (guided: remaining / (8 * participants), 1..64) instead of one, and the
// main thread pauses while it waits for the last items. Every item is still run exactly once by
// the same std::function; only which thread runs it changes (that was already arbitrary).
// Loops with fewer than 32 items per participant keep claiming one item at a time.
typedef void (*spinlock_t)(char*);
static spinlock_t game_spinlock;               // eu4.exe+0x1581820
static void (*bad_function_call)(void);        // eu4.exe+0x1a0c164 (throws std::bad_function_call)
#define RVA_SPINLOCK 0x1581820u
#define RVA_BADCALL  0x1a0c164u
static const uint8_t sig_spinlock[8] = { 0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74 };

static void sp_release(int64_t* cb) {           // std::shared_ptr control block release
    if (!cb) return;
    if (InterlockedExchangeAdd((volatile LONG*)(cb + 1), -1) == 1) {
        void** vt = *(void***)cb;
        ((void (*)(void*))vt[0])(cb);
        if (InterlockedExchangeAdd((volatile LONG*)((uint8_t*)cb + 0xc), -1) == 1) ((void (*)(void*))vt[1])(cb);
    }
}

// ---------------- longest-first order for uneven loops ----------------
// The AI unit loop (dispatcher call returning to eu4+0x77d9b5, ~254 items once a day, 4.6 ms) has a
// few items that take milliseconds while most take microseconds. Items are taken in index order,
// so a heavy item near the end starts late and the main thread then waits for it alone (profile
// 2026-10-15, modded GER save: main thread idle in that wait 6.2% of its time). With
// [parallel] longest_first=1 the participants take the items of the listed loops in descending
// order of the time each index took the last time. Every item still runs exactly once with its
// own index; only the order and the thread differ, and the game fixes neither. A loop is
// recognised by the type of its item std::function (vtable learned at its call site). Off while
// the game's fixed-seed flag is set. [parallel] stats=1 measures the items of these loops either way.
#define ORD_MAXN 8192
typedef struct {
    uint32_t site; void* vt; void* self; LONGLONG t0; int start, n; volatile int active; int reorder;
    int order[ORD_MAXN]; uint64_t cost[ORD_MAXN];          // rdtsc cycles of each index, last call
    LONG64 calls; double wall_us, sum_cyc, max_cyc;        // stats
} ord_t;
static const uint32_t g_ord_sites[] = { 0x77d9b5 };
#define NORD ((int)(sizeof g_ord_sites / sizeof g_ord_sites[0]))
static ord_t g_ord[NORD];                                  // zero-initialised (.bss); sites set at install
static volatile LONG g_ord_any;                            // number of active ord_t
static __thread ord_t* t_ord;                              // dispatched by this thread, not yet waited for
static LONGLONG g_qpc0; static uint64_t g_tsc0;
#define RVA_FIXED_SEED_FLAG 0x23581c0u

static ord_t* ord_site(uint32_t rva) { for (int i = 0; i < NORD; i++) if (g_ord[i].site == rva) return &g_ord[i]; return NULL; }
static ord_t* ord_ctx(uint8_t* ctx, int start, int end) {
    void* fn = *(void**)(ctx + 0xa0);
    if (!fn) return NULL;
    void* vt = *(void**)fn;
    for (int i = 0; i < NORD; i++)
        if (g_ord[i].active && g_ord[i].vt == vt && g_ord[i].start == start && g_ord[i].n == end - start) return &g_ord[i];
    return NULL;
}
static const uint64_t* g_sort_cost;
static int cmp_cost(const void* a, const void* b) {
    int x = *(const int*)a, y = *(const int*)b;
    if (g_sort_cost[x] != g_sort_cost[y]) return g_sort_cost[x] > g_sort_cost[y] ? -1 : 1;
    return x - y;
}

static void participate(uint8_t* ctx) {
    const int start = *(int*)(ctx + 0x40), end = *(int*)(ctx + 0xa8);
    const int parts = *(int*)(ctx + 0xb8) + 1;
    ord_t* o = g_ord_any ? ord_ctx(ctx, start, end) : NULL;
    const int chunky = !o && (end - start) >= 32 * parts;
    for (;;) {
        int chunk = 1;
        if (chunky) {
            int rem = end - *(volatile int*)(ctx + 0x44);
            chunk = rem / (8 * parts);
            if (chunk < 1) chunk = 1; else if (chunk > 64) chunk = 64;
        }
        int idx = (int)InterlockedExchangeAdd((volatile LONG*)(ctx + 0x44), chunk);
        if (idx < *(int*)(ctx + 0x40)) break;
        if (idx >= end) { *(volatile char*)(ctx + 0x50) = 1; break; }
        int hi = idx + chunk; if (hi > end || hi < idx) hi = end;
        int done = 0, cancelled = 0;
        for (int i = idx; i < hi; i++) {
            void* fn = *(void**)(ctx + 0xa0);
            if (!fn) bad_function_call();
            int arg = o ? start + o->order[i - start] : i;
            uint64_t c0 = o ? __rdtsc() : 0;
            ((void (*)(void*, int*))(*(void***)fn)[2])(fn, &arg);
            if (o) o->cost[o->order[i - start]] = __rdtsc() - c0;
            done++;
            if (**(volatile int**)(ctx + 0x60) != 0) { cancelled = 1; break; }
        }
        InterlockedExchangeAdd(*(volatile LONG**)(ctx + 0x48), done);
        if (cancelled) break;
    }
    if (*(void**)(ctx + 0x38)) {                 // completion callback, exactly as the game does it
        LONG old = InterlockedExchangeAdd(*(volatile LONG**)(ctx + 0xb0), 1);
        if (old + 1 == *(int*)(ctx + 0xb8) - 1) {
            char c = **(volatile int**)(ctx + 0x60) != 0;
            void* cb = *(void**)(ctx + 0x38);
            ((void (*)(void*, char*))(*(void***)cb)[2])(cb, &c);
        }
        **(volatile int**)(ctx + 0x58) = 1;
    }
}

// aiprof.c (ai_split): idle threads take posted AI army jobs
int (*eb_par_help)(void);
int (*eb_par_linger)(uint8_t* ctx);

static void my_task(void** param) {
    uint8_t* ctx = (uint8_t*)param[0];
    if (*(volatile char*)(ctx + 0x50) != 1) participate(ctx);
    if (eb_par_linger) eb_par_linger(ctx);   // holds its reference to ctx while it stays
    sp_release((int64_t*)param[1]);
}

static void my_wait(uint8_t* self) {
    char* lockp = *(char**)(self + 8);
    game_spinlock(lockp);
    for (;;) {
        uint8_t* ctx = *(uint8_t**)(self + 0x10);
        if ((*(void**)(ctx + 0x38) == NULL || **(volatile int**)(ctx + 0x58) != 0) &&
            **(volatile int**)(ctx + 0x48) >= *(int*)(ctx + 0xa8)) {
            *(volatile char*)lockp = 0;
            return;
        }
        if (*(volatile char*)(ctx + 0x50) != 1) participate(ctx);
        else if (!(eb_par_help && eb_par_help())) _mm_pause();
    }
}

static void hk_dispatch(void* self, void* p2, int start, int count, void* item_fn, void* done_fn) {
    uint32_t rva = (uint32_t)((uint8_t*)__builtin_return_address(0) - g_base);
    // statistics only for loops started by the main thread (single writer for the site table)
    int si = (g_cfg.par_stats && GetCurrentThreadId() == g_main_tid) ? site_index(rva) : -1;
    LONGLONG t0 = qpc();
    if (g_cfg.par_caps && count > 0) {
        int cap = site_cap(rva);
        void* done_impl = done_fn ? *(void**)((uint8_t*)done_fn + 0x38) : NULL;
        if (cap > 0 && !done_impl && cap < default_tasks(*(int*)((uint8_t*)self + 0x20))) {
            t_req_tasks = cap; if (si >= 0) InterlockedIncrement64(&g_sites[si].capped);
        }
    } else if (g_cfg.par_min_items > 0 && count > 0) {
        // done_fn is a std::function (64 bytes, implementation pointer last): leave loops that
        // have a completion callback alone.
        void* done_impl = done_fn ? *(void**)((uint8_t*)done_fn + 0x38) : NULL;
        if (!done_impl) {
            int mode = *(int*)((uint8_t*)self + 0x20);
            int def = default_tasks(mode);
            int want = count / g_cfg.par_min_items; if (want < 1) want = 1;
            if (want < def) { t_req_tasks = want; if (si >= 0) InterlockedIncrement64(&g_sites[si].capped); }
        }
    }
    t_site = si; t_t0 = t0;
    if (si >= 0) { g_cur_t0 = t0; g_cur = si; }
    ord_t* o = NULL;
    if ((g_cfg.par_lpt || g_cfg.par_stats) && g_cfg.par_chunked && count > 1 && count <= ORD_MAXN &&
        GetCurrentThreadId() == g_main_tid && !*(volatile uint8_t*)(g_base + RVA_FIXED_SEED_FLAG) && (o = ord_site(rva))) {
        void* impl = item_fn ? *(void**)((uint8_t*)item_fn + 0x38) : NULL;
        o->vt = impl ? *(void**)impl : NULL;
        if (!o->vt) o = NULL;
    }
    if (o) {
        if (o->start != start || o->n != count) memset(o->cost, 0, sizeof o->cost);   // new shape: no history
        for (int i = 0; i < count; i++) o->order[i] = i;
        o->reorder = g_cfg.par_lpt;
        if (o->reorder) { g_sort_cost = o->cost; qsort(o->order, (size_t)count, sizeof(int), cmp_cost); }
        o->start = start; o->n = count; o->self = self; o->t0 = t0;
        o->active = 1; InterlockedIncrement(&g_ord_any);
        t_ord = o;
    }
    o_dispatch(self, p2, start, count, item_fn, done_fn);
    t_req_tasks = 0;
    if (si >= 0) {
        site_t* s = &g_sites[si];
        s->calls++; s->items += count; s->t_disp += qpc() - t0;
    }
}

static void hk_wait(void* self) {
    LONGLONG a = qpc();
    if (g_cfg.par_chunked) my_wait((uint8_t*)self); else o_wait(self);
    ord_t* o = t_ord;
    if (o && o->self == self) {                    // the loop is complete: its costs are this call's
        t_ord = NULL;
        o->active = 0; InterlockedDecrement(&g_ord_any);
        uint64_t sum = 0, mx = 0;
        for (int i = 0; i < o->n; i++) { sum += o->cost[i]; if (o->cost[i] > mx) mx = o->cost[i]; }
        o->calls++; o->sum_cyc += (double)sum; o->max_cyc += (double)mx; o->wall_us += (qpc() - o->t0) * g_us;
    }
    int si = t_site;
    if (si >= 0) {
        LONGLONG b = qpc();
        g_sites[si].t_wait += b - a;
        g_sites[si].t_wall += b - t_t0;
        t_site = -1;
    }
}

static void hk_task(void** param) {
    int si = g_cur;
    if (si >= 0) {
        uint8_t* ctx = (uint8_t*)param[0];
        site_t* s = &g_sites[si];
        LONGLONG lat = qpc() - g_cur_t0;
        InterlockedIncrement64(&s->tasks);
        if (*(volatile char*)(ctx + 0x50) == 1 || *(volatile int*)(ctx + 0x44) >= *(volatile int*)(ctx + 0xa8))
            InterlockedIncrement64(&s->late);
        if (lat > 0 && lat < (LONGLONG)(0.1 / g_us * 1e6)) {          // ignore tasks of an older loop
            InterlockedAdd64(&s->t_lat, lat); InterlockedIncrement64(&s->lat_n);
            LONG64 m = s->lat_max; while (lat > m && InterlockedCompareExchange64(&s->lat_max, lat, m) != m) m = s->lat_max;
        }
    }
    if (g_cfg.par_chunked) my_task(param); else o_task(param);
}

// ---------------- worker spin before sleep ----------------
static volatile LONG g_spinners;
static volatile LONG64 g_spin_hits, g_spin_miss, g_sleeps;

// After an unsuccessful spin we return once without waiting, so the worker loop re-checks its
// shutdown flag and the queue under the mutex (a notify_all sent while we were spinning is not
// lost); the next call then really waits.
static __thread int t_spun;

static int hk_cndwait(void* cv, void* mtx) {
    if (g_cfg.par_spin_us > 0 && (uint8_t*)__builtin_return_address(0) == g_base + RVA_WORKER_WAIT_RET) {
        if (t_spun) { t_spun = 0; g_sleeps++; return o_cndwait(cv, mtx); }
        uint8_t* pool = NULL;
        if (mtx == *(void**)(g_base + RVA_POOL1)) pool = g_base + RVA_POOL1;
        else if (mtx == *(void**)(g_base + RVA_POOL2)) pool = g_base + RVA_POOL2;
        if (pool) {
            if (InterlockedIncrement(&g_spinners) <= g_cfg.par_spin_workers) {
                volatile size_t* qsize = (volatile size_t*)(pool + 0x30);
                LONGLONG end = qpc() + (LONGLONG)(g_cfg.par_spin_us / g_us);
                mtx_unlock(mtx);
                int got = 0;
                for (int i = 0;; i++) {
                    if (*qsize) { got = 1; break; }
                    _mm_pause();
                    if ((i & 63) == 63 && qpc() > end) break;
                }
                mtx_lock(mtx);
                InterlockedDecrement(&g_spinners);
                if (got || *qsize) { g_spin_hits++; return 0; }   // the loop re-checks the queue
                g_spin_miss++;
                t_spun = 1;
                return 0;                                       // re-check shutdown flag, then wait
            }
            InterlockedDecrement(&g_spinners);
            g_sleeps++;
        }
    }
    return o_cndwait(cv, mtx);
}

static int hook(uint32_t rva, const uint8_t* sig, size_t n, void* fn, void** orig, const char* name) {
    uint8_t* t = g_base + rva;
    if (memcmp(t, sig, n) != 0) { eb_log("parallel: %s signature mismatch - off", name); return 0; }
    if (MH_CreateHook(t, fn, orig) != MH_OK || MH_EnableHook(t) != MH_OK) { eb_log("parallel: %s hook failed", name); return 0; }
    return 1;
}

int eb_parallel_install(void) {
    if (!g_cfg.par_stats && g_cfg.par_min_items <= 0 && g_cfg.par_spin_us <= 0 && !g_cfg.par_caps && !g_cfg.par_chunked) return 0;
    if (!g_cfg.par_chunked) g_cfg.par_lpt = 0;     // the reordering lives in the chunked task body
#ifdef EB_TEST
    return 0;
#else
    g_base = (uint8_t*)g_exe;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(g_base + ((IMAGE_DOS_HEADER*)g_base)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != 0x66FE6AB9u) { eb_log("parallel: unknown eu4.exe - off"); return 0; }
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_us = 1e6 / (double)f.QuadPart;
    g_qpc0 = qpc(); g_tsc0 = __rdtsc();
    for (int i = 0; i < NORD; i++) g_ord[i].site = g_ord_sites[i];
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return 0;
    int ok = 1;
    if (g_cfg.par_chunked) {
        if (memcmp(g_base + RVA_SPINLOCK, sig_spinlock, 8) == 0) {
            game_spinlock = (spinlock_t)(g_base + RVA_SPINLOCK);
            bad_function_call = (void (*)(void))(g_base + RVA_BADCALL);
        } else { eb_log("parallel: spinlock signature mismatch - chunked off"); g_cfg.par_chunked = 0; }
    }
    if (g_cfg.par_stats || g_cfg.par_min_items > 0 || g_cfg.par_caps || g_cfg.par_chunked) {
        ok &= hook(RVA_DISPATCH, sig_dispatch, 16, (void*)hk_dispatch, (void**)&o_dispatch, "dispatcher");
        ok &= hook(RVA_WAIT, sig_wait, 16, (void*)hk_wait, (void**)&o_wait, "wait");
    }
    if (g_cfg.par_stats || g_cfg.par_chunked) ok &= hook(RVA_TASK, sig_task, 16, (void*)hk_task, (void**)&o_task, "task");
    if (g_cfg.par_min_items > 0 || g_cfg.par_caps) ok &= hook(RVA_STATEINIT, sig_state, 16, (void*)hk_state, (void**)&o_state, "state");
    if (g_cfg.par_spin_us > 0) {
        if (memcmp(g_base + RVA_MTX_LOCK, sig_mtxlock, 8) == 0 && memcmp(g_base + RVA_MTX_UNLOCK, sig_mtxunlock, 8) == 0) {
            mtx_lock = (mtx_t)(g_base + RVA_MTX_LOCK); mtx_unlock = (mtx_t)(g_base + RVA_MTX_UNLOCK);
            ok &= hook(RVA_CNDWAIT, sig_cndwait, 16, (void*)hk_cndwait, (void**)&o_cndwait, "cnd_wait");
        } else { eb_log("parallel: mutex signature mismatch - spin off"); ok = 0; }
    }
    eb_log("parallel: stats=%d chunked=%d small_loop_caps=%d longest_first=%d min_items_per_task=%d worker_spin_us=%d spin_workers=%d -> %s",
           g_cfg.par_stats, g_cfg.par_chunked, g_cfg.par_caps, g_cfg.par_lpt, g_cfg.par_min_items, g_cfg.par_spin_us, g_cfg.par_spin_workers, ok ? "installed" : "partly installed");
    return ok;
#endif
}

void eb_parallel_report(void) {
    double cyc_per_us = 0;
    { LONGLONG q = qpc(); uint64_t t = __rdtsc(); if (q > g_qpc0) cyc_per_us = (double)(t - g_tsc0) / ((q - g_qpc0) * g_us); }
    for (int i = 0; i < NORD; i++) {
        ord_t* o = &g_ord[i];
        if (!o->calls || cyc_per_us <= 0) continue;
        double c = (double)o->calls;
        eb_log("parallel: %s site eu4+%06x: %lld calls, %d items | per call: wall %.0f us, all items %.0f us, longest item %.0f us",
               g_cfg.par_lpt ? "longest-first" : "measured (in index order)", o->site, (long long)o->calls, o->n,
               o->wall_us / c, o->sum_cyc / cyc_per_us / c, o->max_cyc / cyc_per_us / c);
    }
    if (g_cfg.par_spin_us > 0)
        eb_log("parallel: worker spin caught work %lld times, spun in vain %lld, slept %lld",
               (long long)g_spin_hits, (long long)g_spin_miss, (long long)g_sleeps);
    if (!g_cfg.par_stats || !g_nsites) return;
    // sort by total wall time
    int idx[NSITES]; int n = g_nsites;
    for (int i = 0; i < n; i++) idx[i] = i;
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++)
        if (g_sites[idx[j]].t_wall > g_sites[idx[i]].t_wall) { int k = idx[i]; idx[i] = idx[j]; idx[j] = k; }
    double tot = 0; for (int i = 0; i < n; i++) tot += g_sites[i].t_wall * g_us;
    eb_log("parallel: %d loop sites, %.0f ms total main-thread time in parallel loops", n, tot / 1000);
    for (int k = 0; k < n; k++) {
        site_t* s = &g_sites[idx[k]];
        if (!s->calls) continue;
        double c = (double)s->calls;
        eb_log("parallel: site eu4+%06x calls %lld items/call %.0f | per call: wall %.0f us, dispatch %.0f us, main wait %.0f us | tasks/call %.1f late %.0f%% | worker start latency avg %.0f us max %.0f us | capped %lld",
               s->site, (long long)s->calls, s->items / c, s->t_wall * g_us / c, s->t_disp * g_us / c, s->t_wait * g_us / c,
               s->tasks / c, s->tasks ? 100.0 * s->late / s->tasks : 0.0,
               s->lat_n ? s->t_lat * g_us / s->lat_n : 0.0, s->lat_max * g_us, (long long)s->capped);
    }
}

#ifdef EB_TEST
// ---- self-test of the chunked participate/wait/task code on a mock of the game's structures ----
static void test_spinlock(char* p) { while (__sync_val_compare_and_swap(p, (char)0, (char)1) != 0) _mm_pause(); }
static void test_badcall(void) { eb_log("TEST: bad_function_call"); ExitProcess(3); }
static volatile LONG* t_hits;
static void item_call(void* self, int* idx) { (void)self; InterlockedIncrement(&t_hits[*idx]); for (volatile int k = 0; k < (*idx % 7) * 50; k++); }
static void* item_vt[3] = { NULL, NULL, (void*)item_call };
static volatile LONG t_cb_calls;
static void cb_call(void* self, char* c) { (void)self; (void)c; InterlockedIncrement(&t_cb_calls); }
static void* cb_vt[3] = { NULL, NULL, (void*)cb_call };
static volatile LONG t_cb_destroyed;
static void cb_destroy(void* p) { (void)p; InterlockedIncrement(&t_cb_destroyed); }
static void cb_delete(void* p) { (void)p; }
static void* ctrl_vt[2] = { (void*)cb_destroy, (void*)cb_delete };

typedef struct { uint8_t* ctx; int64_t* ctrl; } tparam_t;
static DWORD WINAPI test_worker(LPVOID a) { tparam_t* tp = (tparam_t*)a; void* p[2] = { tp->ctx, tp->ctrl }; my_task(p); return 0; }

static int t_lpt;
__declspec(dllexport) void eb_test_set_lpt(int on) { t_lpt = on; }
__declspec(dllexport) int eb_test_parallel(int start, int count, int ntasks, int with_cb) {
    game_spinlock = test_spinlock; bad_function_call = test_badcall;
    static uint8_t ctx[0xd0]; memset(ctx, 0, sizeof ctx);
    static int c48, c58, c60, cb0; c48 = c58 = c60 = cb0 = 0;
    static void* itemobj[1]; itemobj[0] = item_vt;
    static void* cbobj[1]; cbobj[0] = cb_vt;
    *(int*)(ctx + 0x40) = start; *(int*)(ctx + 0x44) = start; *(int*)(ctx + 0xa8) = start + count;
    *(int**)(ctx + 0x48) = &c48; c48 = start;           // the game starts the completed counter at start
    *(int**)(ctx + 0x58) = &c58; *(int**)(ctx + 0x60) = &c60; *(int**)(ctx + 0xb0) = &cb0;
    *(int*)(ctx + 0xb8) = ntasks;
    *(void**)(ctx + 0xa0) = itemobj;
    *(void**)(ctx + 0x38) = with_cb ? cbobj : NULL;
    t_hits = (volatile LONG*)calloc(start + count + 64, sizeof(LONG));
    t_cb_calls = 0; t_cb_destroyed = 0;
    static int64_t ctrl[2]; ctrl[0] = (int64_t)(intptr_t)ctrl_vt; ((volatile LONG*)&ctrl[1])[0] = ntasks; ((volatile LONG*)&ctrl[1])[1] = 1;
    static char lockb; lockb = 0;
    static uint8_t self[0x40]; memset(self, 0, sizeof self);
    *(char**)(self + 8) = &lockb; *(uint8_t**)(self + 0x10) = ctx;
    tparam_t tp = { ctx, ctrl };
    ord_t* o = &g_ord[0];
    if (t_lpt && count > 1 && count <= ORD_MAXN) {      // longest-first on the mock loop: costs from a hash of the index
        o->vt = item_vt; o->start = start; o->n = count; o->self = self;
        for (int i = 0; i < count; i++) { o->cost[i] = (uint64_t)((i * 2654435761u) >> 8); o->order[i] = i; }
        g_sort_cost = o->cost; qsort(o->order, (size_t)count, sizeof(int), cmp_cost);
        for (int i = 1; i < count; i++) if (o->cost[o->order[i - 1]] < o->cost[o->order[i]]) { eb_log("TEST lpt: order not descending"); return 0; }
        memset(o->cost, 0, sizeof o->cost);
        o->active = 1; InterlockedIncrement(&g_ord_any);
    } else o = NULL;
    HANDLE th[64]; int nt = ntasks > 64 ? 64 : ntasks;
    for (int i = 0; i < nt; i++) th[i] = CreateThread(NULL, 0, test_worker, &tp, 0, NULL);
    my_wait(self);
    WaitForMultipleObjects(nt, th, TRUE, INFINITE);
    for (int i = 0; i < nt; i++) CloseHandle(th[i]);
    int bad = 0;
    for (int i = 0; i < start + count; i++) { LONG want = i >= start ? 1 : 0; if (t_hits[i] != want) bad++; }
    if (o) {                                            // every index was timed once, in the new order
        for (int i = 0; i < count; i++) if (!o->cost[i]) bad++;
        o->active = 0; InterlockedDecrement(&g_ord_any);
    }
    int ok = bad == 0 && c48 == start + count && lockb == 0 && t_cb_destroyed == 1 && (!with_cb || (t_cb_calls <= 1 && c58 == 1));
    eb_log("TEST parallel start=%d count=%d tasks=%d cb=%d lpt=%d: wrong items %d, completed %d/%d, cb calls %ld, ctrl destroyed %ld -> %s",
           start, count, ntasks, with_cb, o != NULL, bad, c48 - start, count, (long)t_cb_calls, (long)t_cb_destroyed, ok ? "OK" : "FAIL");
    free((void*)t_hits);
    return ok;
}
#endif
