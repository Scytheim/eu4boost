// Faster "add modifier container" (FUN_1408ea420, dst += src) - [modifiers] fast_add.
//
// A modifier container keeps {int id, int value} pairs at +0x58 (capacity +0x60, count +0x64). When
// its dense flag (+0x10) is set, ids below 0x286 sit at index id; otherwise the pairs are in insertion
// order and every add looks the id up with a linear search. FUN_1408ea420 adds all pairs of src to
// dst (id 0x286 is skipped), appending ids dst does not have yet, and finally pushes src onto the
// vector at dst+0x38. Adding a large container into a sparse one is therefore O(n^2): the top
// caller FUN_140d54600 adds a whole country modifier into an empty sparse local (profile 2026-10-15,
// modded GER save: FUN_1408ea420 is the top main-thread function, 3.6% self time).
//
// This re-implementation does exactly the same steps in the same order - same pairs, same
// appends, the same growth (max(2*cap, count+1)) through the same allocation and free calls, the
// same push at the end - except that for a sparse dst the first position of each id is kept in a
// per-thread table instead of being searched for. ids outside the table use a linear search.
//
// fast_search=1 (default with fast_add): the remaining adds - fewer than FAST_MIN source pairs (1-3
// pairs into a large dst: 16.8 million calls in 61 s, the linear search 0x1408ea4e0 is ~half of
// the function's 1.65% self time) and dense destinations - run the game's algorithm step by step,
// but the search for the first pair with the id compares 8 pairs at once (AVX2, or 4 with SSE2).
// dst == src always goes through the original code.
#include "common.h"
#include <string.h>
#include <stdlib.h>
#include <immintrin.h>
#include "MinHook.h"

#define RVA_MODADD   0x8ea420u
#define RVA_NEW      0x1a33310u      // operator new thunk called by FUN_1408ea420
#define RVA_PUSHBACK 0x8f0e0u        // vector<void*> grow-and-insert called by FUN_1408ea420
static const uint8_t sig_modadd[16] = { 0x40,0x55,0x41,0x55,0x48,0x83,0xec,0x48,0x4c,0x63,0x4a,0x64,0x45,0x33,0xc0,0x4c };
#define OFF_CALL_NEW  0x101          // call sites inside FUN_1408ea420
#define OFF_CALL_PUSH 0x1dc

#define M_DENSE 0x10
#define M_VEC   0x38
#define M_ALLOC 0x50
#define M_DATA  0x58
#define M_CAP   0x60
#define M_CNT   0x64
#define SKIP_ID 0x286
#define FAST_MIN 4                   // src pairs below this: original code (its search is cheaper there)
#define FAST_MIN_DST 16              // dst + src pairs below this: original code
#define TBL 16384                    // ids 0..TBL-1 are indexed

typedef void* (*modadd_t)(uint8_t* dst, uint8_t* src);
typedef void* (*new_t)(uint64_t size);
typedef void* (*pushback_t)(void* vec, void* where, void** val);
static modadd_t o_modadd;
static new_t game_new;
static pushback_t game_pushback;

typedef struct { uint32_t gen; uint32_t stamp[TBL]; int pos[TBL]; } idx_t;
static __thread idx_t* t_idx;
static volatile LONG64 g_fast, g_slow, g_pairs, g_search;   // [modifiers] stats=1 only: the add runs ~280,000 times a second
static int g_avx2, g_mstats;

static int* pair(uint8_t* c, int i) { return (int*)(*(uint8_t**)(c + M_DATA) + (int64_t)i * 8); }

static void append(uint8_t* dst, int id, int value) {
    int count = *(int*)(dst + M_CNT), cap = *(int*)(dst + M_CAP);
    uint8_t* data;
    if (count + 1 > cap) {
        int ncap = cap * 2;
        if (count + 1 > ncap) ncap = count + 1;
        uint64_t bytes = (uint64_t)(uint32_t)ncap * 8;      // the game computes 8 * zero-extended ncap
        data = (uint8_t*)game_new(bytes);
        uint8_t* old = *(uint8_t**)(dst + M_DATA);
        for (int i = 0; i < count; i++) memcpy(data + (int64_t)i * 8, old + (int64_t)i * 8, 8);
        void** alloc = (void**)(dst + M_ALLOC);              // frees its own pointer (dst+0x58)
        ((void (*)(void*, int64_t))(*(void***)alloc)[2])(alloc, 0);
        *(uint8_t**)(dst + M_DATA) = data;
        *(int*)(dst + M_CAP) = ncap;
    } else data = *(uint8_t**)(dst + M_DATA);
    int* p = (int*)(data + (int64_t)count * 8);
    *(int*)(dst + M_CNT) = count + 1;
    p[0] = id; p[1] = value;
}

// first index j < c with p[2j] == id (p = {id, value} pairs), or -1
__attribute__((target("avx2"))) static int find_avx2(const int* p, int c, int id) {
    const __m256i v = _mm256_set1_epi32(id);
    int j = 0;
    for (; j + 8 <= c; j += 8) {                                  // 8 pairs: ids are the even lanes
        __m256i a = _mm256_loadu_si256((const __m256i*)(p + 2 * j));
        __m256i b = _mm256_loadu_si256((const __m256i*)(p + 2 * j + 8));
        unsigned ma = (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(a, v))) & 0x55u;
        unsigned mb = (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(b, v))) & 0x55u;
        if (ma | mb) return j + (ma ? (int)__builtin_ctz(ma) / 2 : 4 + (int)__builtin_ctz(mb) / 2);
    }
    for (; j < c; j++) if (p[2 * j] == id) return j;
    return -1;
}
static int find_sse2(const int* p, int c, int id) {
    const __m128i v = _mm_set1_epi32(id);
    int j = 0;
    for (; j + 4 <= c; j += 4) {
        __m128i a = _mm_loadu_si128((const __m128i*)(p + 2 * j));
        __m128i b = _mm_loadu_si128((const __m128i*)(p + 2 * j + 4));
        unsigned ma = (unsigned)_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(a, v))) & 0x5u;
        unsigned mb = (unsigned)_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(b, v))) & 0x5u;
        if (ma | mb) return j + (ma ? (int)__builtin_ctz(ma) / 2 : 2 + (int)__builtin_ctz(mb) / 2);
    }
    for (; j < c; j++) if (p[2 * j] == id) return j;
    return -1;
}
static int find_id(const int* p, int c, int id) { return g_avx2 ? find_avx2(p, c, id) : find_sse2(p, c, id); }

// the game's FUN_1408ea420 step by step (disassembly: src count read once, the flags and data
// pointers re-read for every pair, dense dst ids < 0x286 written by index), with the faster search
static void* search_add(uint8_t* dst, uint8_t* src) {
    const int n = *(int*)(src + M_CNT);
    for (int k = 0; k < n; k++) {
        int* sd = *(int**)(src + M_DATA);
        int id = sd[k * 2], value;
        if (*(char*)(src + M_DENSE) && id <= SKIP_ID) value = sd[id * 2 + 1];
        else value = k < *(int*)(src + M_CNT) ? sd[k * 2 + 1] : 0;
        if (id == SKIP_ID) continue;
        int* dd = *(int**)(dst + M_DATA);
        if (*(char*)(dst + M_DENSE) && id < SKIP_ID) { dd[(int64_t)id * 2] = id; dd[(int64_t)id * 2 + 1] += value; continue; }
        int j = find_id(dd, *(int*)(dst + M_CNT), id);
        if (j >= 0) dd[j * 2 + 1] += value; else append(dst, id, value);
    }
    void** end = *(void***)(dst + M_VEC + 8);
    if (end == *(void***)(dst + M_VEC + 16)) { void* v = src; game_pushback(dst + M_VEC, end, &v); }
    else { *end = src; *(void***)(dst + M_VEC + 8) = end + 1; }
    return dst;
}

static void* hk_modadd(uint8_t* dst, uint8_t* src) {
    int n = *(int*)(src + M_CNT);
    if (dst == src) { if (g_mstats) InterlockedIncrement64(&g_slow); return o_modadd(dst, src); }
    if (*(char*)(dst + M_DENSE) || n < FAST_MIN || *(int*)(dst + M_CNT) + n < FAST_MIN_DST) {
        if (g_cfg.mod_search) { if (g_mstats) g_search++; return search_add(dst, src); }   // (a statistic: not exact off the main thread)
        if (g_mstats) InterlockedIncrement64(&g_slow);
        return o_modadd(dst, src);
    }
    idx_t* t = t_idx;
    if (!t) {
        t = (idx_t*)calloc(1, sizeof *t);
        if (!t) { if (g_mstats) InterlockedIncrement64(&g_slow); return o_modadd(dst, src); }
        t_idx = t;
    }
    if (++t->gen == 0) { memset(t->stamp, 0, sizeof t->stamp); t->gen = 1; }
    const uint32_t gen = t->gen;
    for (int j = 0, c = *(int*)(dst + M_CNT); j < c; j++) {     // first position of each id in dst
        int id = pair(dst, j)[0];
        if ((unsigned)id < TBL && t->stamp[id] != gen) { t->stamp[id] = gen; t->pos[id] = j; }
    }
    const char src_dense = *(char*)(src + M_DENSE);
    for (int k = 0; k < n; k++) {
        int id = pair(src, k)[0];
        int value;
        if (src_dense && id <= SKIP_ID) value = pair(src, id)[1];
        else value = k < *(int*)(src + M_CNT) ? pair(src, k)[1] : 0;
        if (id == SKIP_ID) continue;
        if ((unsigned)id < TBL) {
            if (t->stamp[id] == gen) { pair(dst, t->pos[id])[1] += value; continue; }
            int at = *(int*)(dst + M_CNT);
            append(dst, id, value);
            t->stamp[id] = gen; t->pos[id] = at;
        } else {                                                 // as the game: linear search
            int j = find_id(*(int**)(dst + M_DATA), *(int*)(dst + M_CNT), id);
            if (j >= 0) pair(dst, j)[1] += value; else append(dst, id, value);
        }
    }
    void** end = *(void***)(dst + M_VEC + 8);
    if (end == *(void***)(dst + M_VEC + 16)) { void* v = src; game_pushback(dst + M_VEC, end, &v); }
    else { *end = src; *(void***)(dst + M_VEC + 8) = end + 1; }
    if (g_mstats) { InterlockedIncrement64(&g_fast); InterlockedAdd64(&g_pairs, n); }
    return dst;
}

// [modifiers] verify=1 (diagnostics): the game keeps using its own function, and for every add the
// fast path would take, the same algorithm is replayed on a copy of dst's pairs and compared with
// what the game's function produced (pairs, count, capacity, the pushed source).
static volatile LONG64 g_ver_calls, g_ver_bad;
static void* verify_modadd(uint8_t* dst, uint8_t* src) {
    int n = *(int*)(src + M_CNT);
    if (dst == src || *(char*)(dst + M_DENSE) || n < FAST_MIN || *(int*)(dst + M_CNT) + n < FAST_MIN_DST) return o_modadd(dst, src);
    idx_t* t = t_idx;
    if (!t) { t = (idx_t*)calloc(1, sizeof *t); if (!t) return o_modadd(dst, src); t_idx = t; }
    int c = *(int*)(dst + M_CNT), cap = *(int*)(dst + M_CAP);
    int* sp = (int*)malloc((size_t)(c + n + 1) * 8);
    if (!sp) return o_modadd(dst, src);
    if (c) memcpy(sp, *(uint8_t**)(dst + M_DATA), (size_t)c * 8);
    if (++t->gen == 0) { memset(t->stamp, 0, sizeof t->stamp); t->gen = 1; }
    const uint32_t gen = t->gen;
    for (int j = 0; j < c; j++) { int id = sp[j * 2]; if ((unsigned)id < TBL && t->stamp[id] != gen) { t->stamp[id] = gen; t->pos[id] = j; } }
    const char src_dense = *(char*)(src + M_DENSE);
    for (int k = 0; k < n; k++) {
        int id = pair(src, k)[0], value;
        if (src_dense && id <= SKIP_ID) value = pair(src, id)[1];
        else value = k < *(int*)(src + M_CNT) ? pair(src, k)[1] : 0;
        if (id == SKIP_ID) continue;
        int at = -1;
        if ((unsigned)id < TBL) { if (t->stamp[id] == gen) at = t->pos[id]; }
        else for (int j = 0; j < c; j++) if (sp[j * 2] == id) { at = j; break; }
        if (at >= 0) { sp[at * 2 + 1] += value; continue; }
        if (c + 1 > cap) { int ncap = cap * 2; if (c + 1 > ncap) ncap = c + 1; cap = ncap; }
        sp[c * 2] = id; sp[c * 2 + 1] = value;
        if ((unsigned)id < TBL) { t->stamp[id] = gen; t->pos[id] = c; }
        c++;
    }
    void** end0 = *(void***)(dst + M_VEC + 8); void** beg0 = *(void***)(dst + M_VEC);
    size_t vn0 = (size_t)(end0 - beg0);
    void* r = o_modadd(dst, src);                       // the game's own function decides the state
    InterlockedIncrement64(&g_ver_calls);
    void** beg1 = *(void***)(dst + M_VEC); void** end1 = *(void***)(dst + M_VEC + 8);
    int gc = *(int*)(dst + M_CNT), gcap = *(int*)(dst + M_CAP);
    int* gp = *(int**)(dst + M_DATA);
    int same = r == dst && gc == c && gcap == cap && memcmp(gp, sp, (size_t)c * 8) == 0 &&
               (size_t)(end1 - beg1) == vn0 + 1 && end1[-1] == src;
    if (!same && InterlockedIncrement64(&g_ver_bad) <= 20) {
        int d = 0; while (d < c && d < gc && gp[d * 2] == sp[d * 2] && gp[d * 2 + 1] == sp[d * 2 + 1]) d++;
        eb_log("modifiers: VERIFY MISMATCH dst=%p src=%p (src %d pairs, dense %d): game count %d cap %d, fast count %d cap %d, first difference at %d: game {%d,%d} fast {%d,%d}, vector %zu->%zu",
               (void*)dst, (void*)src, n, src_dense, gc, gcap, c, cap, d,
               d < gc ? gp[d * 2] : 0, d < gc ? gp[d * 2 + 1] : 0, d < c ? sp[d * 2] : 0, d < c ? sp[d * 2 + 1] : 0,
               vn0, (size_t)(end1 - beg1));
    }
    free(sp);
    return r;
}

// [modifiers] verify=2 (diagnostics): the fast path decides the state, then the game's quadratic
// search is replayed on a copy (so the add takes about as long as the original) and compared.
static void* fast_then_slow(uint8_t* dst, uint8_t* src) {
    int n = *(int*)(src + M_CNT);
    if (dst == src || *(char*)(dst + M_DENSE) || n < FAST_MIN || *(int*)(dst + M_CNT) + n < FAST_MIN_DST) return o_modadd(dst, src);
    int c = *(int*)(dst + M_CNT);
    int* sp = (int*)malloc((size_t)(c + n + 1) * 8);
    if (!sp) return hk_modadd(dst, src);
    if (c) memcpy(sp, *(uint8_t**)(dst + M_DATA), (size_t)c * 8);
    void* r = hk_modadd(dst, src);
    const char src_dense = *(char*)(src + M_DENSE);
    for (int k = 0; k < n; k++) {                      // the game's algorithm, linear search
        int id = pair(src, k)[0], value;
        if (src_dense && id <= SKIP_ID) value = pair(src, id)[1];
        else value = k < *(int*)(src + M_CNT) ? pair(src, k)[1] : 0;
        if (id == SKIP_ID) continue;
        int j = 0;
        for (; j < c; j++) if (((volatile int*)sp)[j * 2] == id) break;
        if (j < c) { sp[j * 2 + 1] += value; continue; }
        sp[c * 2] = id; sp[c * 2 + 1] = value; c++;
    }
    InterlockedIncrement64(&g_ver_calls);
    if ((c != *(int*)(dst + M_CNT) || memcmp(sp, *(uint8_t**)(dst + M_DATA), (size_t)c * 8) != 0) &&
        InterlockedIncrement64(&g_ver_bad) <= 20)
        eb_log("modifiers: VERIFY(2) MISMATCH dst=%p src=%p: slow count %d, fast count %d", (void*)dst, (void*)src, c, *(int*)(dst + M_CNT));
    free(sp);
    return r;
}

// [modifiers] verify=3 (diagnostics): adds that take search_add run it, then the game's algorithm
// with a plain linear search is replayed on a copy of dst's pairs and compared
static void* search_then_slow(uint8_t* dst, uint8_t* src) {
    int n = *(int*)(src + M_CNT);
    if (dst == src || !(*(char*)(dst + M_DENSE) || n < FAST_MIN || *(int*)(dst + M_CNT) + n < FAST_MIN_DST)) return hk_modadd(dst, src);
    int c = *(int*)(dst + M_CNT), cap = *(int*)(dst + M_CAP), dense = *(char*)(dst + M_DENSE);
    int span = c > cap ? c : cap;
    int* sp = (int*)malloc((size_t)(span + n + 1) * 8);
    if (!sp) return search_add(dst, src);
    if (span) memcpy(sp, *(uint8_t**)(dst + M_DATA), (size_t)span * 8);
    void* r = search_add(dst, src);
    for (int k = 0; k < n; k++) {
        int* sd = *(int**)(src + M_DATA);
        int id = sd[k * 2], value;
        if (*(char*)(src + M_DENSE) && id <= SKIP_ID) value = sd[id * 2 + 1];
        else value = k < *(int*)(src + M_CNT) ? sd[k * 2 + 1] : 0;
        if (id == SKIP_ID) continue;
        if (dense && id < SKIP_ID) { if (id >= 0 && id < span) { sp[id * 2] = id; sp[id * 2 + 1] += value; } continue; }
        int j = 0;
        for (; j < c; j++) if (((volatile int*)sp)[j * 2] == id) break;
        if (j < c) { sp[j * 2 + 1] += value; continue; }
        sp[c * 2] = id; sp[c * 2 + 1] = value; c++;
        if (c > span) span = c;
    }
    InterlockedIncrement64(&g_ver_calls);
    int cmp = c; if (dense && cmp < SKIP_ID && span >= SKIP_ID) cmp = SKIP_ID;
    if ((c != *(int*)(dst + M_CNT) || memcmp(sp, *(uint8_t**)(dst + M_DATA), (size_t)cmp * 8) != 0) &&
        InterlockedIncrement64(&g_ver_bad) <= 20)
        eb_log("modifiers: VERIFY(3) MISMATCH dst=%p src=%p (dense %d, src %d pairs): slow count %d, fast count %d", (void*)dst, (void*)src, dense, n, c, *(int*)(dst + M_CNT));
    free(sp);
    return r;
}

#ifdef EB_TEST
__declspec(dllexport) void* eb_test_modadd(uint8_t* dst, uint8_t* src) { return hk_modadd(dst, src); }
#endif

int eb_modifier_install(void) {
    if (!g_cfg.mod_fast && !g_cfg.mod_verify) return 0;
    uint8_t* t;
#ifdef EB_TEST
    t = (uint8_t*)GetProcAddress(g_exe, "t_modadd");
    game_new = (new_t)(void*)GetProcAddress(g_exe, "t_new");
    game_pushback = (pushback_t)(void*)GetProcAddress(g_exe, "t_pushback");
    if (!t || !game_new || !game_pushback) { eb_log("modifiers: test targets not found"); return 0; }
#else
    uint8_t* b = (uint8_t*)g_exe;
    t = b + RVA_MODADD;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != 0x66FE6AB9u || memcmp(t, sig_modadd, sizeof sig_modadd) != 0 ||
        t[OFF_CALL_NEW] != 0xe8 || t + OFF_CALL_NEW + 5 + *(int32_t*)(t + OFF_CALL_NEW + 1) != b + RVA_NEW ||
        t[OFF_CALL_PUSH] != 0xe8 || t + OFF_CALL_PUSH + 5 + *(int32_t*)(t + OFF_CALL_PUSH + 1) != b + RVA_PUSHBACK) {
        eb_log("modifiers: signature mismatch - fast_add off");
        return 0;
    }
    game_new = (new_t)(void*)(b + RVA_NEW);
    game_pushback = (pushback_t)(void*)(b + RVA_PUSHBACK);
#endif
    g_mstats = g_cfg.mod_stats;
    __builtin_cpu_init();
    g_avx2 = __builtin_cpu_supports("avx2") && g_cfg.mod_search != 2;   // fast_search=2: SSE2 (tests)
    MH_STATUS s = MH_Initialize();
    void* repl = g_cfg.mod_verify == 3 ? (void*)search_then_slow : g_cfg.mod_verify == 2 ? (void*)fast_then_slow : g_cfg.mod_verify ? (void*)verify_modadd : (void*)hk_modadd;
    if ((s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) ||
        MH_CreateHook(t, repl, (void**)&o_modadd) != MH_OK || MH_EnableHook(t) != MH_OK) {
        eb_log("modifiers: hook failed - fast_add off");
        return 0;
    }
    if (g_cfg.mod_search) eb_log("modifiers: fast search for small adds and dense containers on (%s)", g_avx2 ? "AVX2" : "SSE2");
    eb_log(g_cfg.mod_verify == 3 ? "modifiers: VERIFY(3) mode - small/dense adds run the fast search, then the plain search is replayed and compared" :
           g_cfg.mod_verify == 2 ? "modifiers: VERIFY(2) mode - the fast path runs, then the slow search is replayed and compared" :
           g_cfg.mod_verify ? "modifiers: VERIFY mode - the game's add runs, the fast path is replayed and compared"
                            : "modifiers: fast add for sparse containers on");
    return 1;
}

void eb_modifier_report(void) {
    if (!o_modadd) return;
    if (g_cfg.mod_verify) eb_log("modifiers: verify: %lld adds compared, %lld mismatches", (long long)g_ver_calls, (long long)g_ver_bad);
    if (g_mstats) eb_log("modifiers: %lld fast adds (%.1f pairs each), %lld with the fast search, %lld through the original code",
           (long long)g_fast, g_fast ? (double)g_pairs / (double)g_fast : 0.0, (long long)g_search, (long long)g_slow);
}
