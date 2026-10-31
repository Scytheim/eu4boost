// Saves: the countries of the gamestate are serialized on all cores ([save] parallel_countries).
//
// A save writes the gamestate into an in-memory stream (FUN_1414e3f00 -> FUN_14074ff30), then
// checksums and compresses it. In the gamestate writer the country loop (0x140751760) calls, for
// every country of gamestate+0x1ce8 in turn, FUN_141707a20(writer, tag string, country): the tag
// as a key, then the country's own writer FUN_1402dc6b0 through its vtable. With the user's modded
// 1543 ironman save that loop is ~54% of the ~0.45 s every save takes (profile 2026-10-16), and the
// game stands still meanwhile (ironman saves every quarter).
//
// At the loop's first call, every country is written at once, each pool thread into its own
// memory stream of the game's class (constructor FUN_14171ebc0, vtable 0x141dc0358, bytes kept in
// a std::vector<char>) through a copy of the game's writer (0x20 bytes: vtable, depth, stream,
// binary flag, owns-stream flag). The loop's calls then append each country's bytes to the real
// stream in the game's order (stream vtable +0x50, the call every token write ends in), so the
// stream - and the save - gets the same bytes in the same order.
//
// Shared state found on the country writer's paths (static call graph + profile, 956 functions):
//   - two static serializer objects whose value pointer is set on every use (FUN_14015bf70 ->
//     0x142458bf0, also used by FUN_140451740; FUN_1402c59b0 -> 0x142458e80): on pool threads these
//     functions return/use a per-thread copy of the object instead;
//   - lazily created default objects (if null: allocate and store): the batch only runs when all of
//     them exist already, otherwise that save is written the original way;
//   - magic statics: thread-safe by themselves.
// Modes: 1 = on, 2 = on + verify (the game's own call still writes every country and its bytes
// are compared with the parallel ones), 3 = serial precompute on the main thread + verify.
// Anything unexpected (another call order, a country missing, a lazy object missing) falls back
// to the original call for the rest of the loop; nothing parallel is written before the batch is
// complete. Every country's bytes are also checked before any of them is used (balanced(): the binary
// tokens walk through and every '{' is closed); if one fails, the whole save is written the original
// way. A save that loads with its blocks out of step - a player's save on 2026-10-23 broke that way,
// though before the countries - can then never come from this code.
#include "common.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "MinHook.h"

#define RVA_KEYOBJ   0x1707a20u  // FUN_141707a20(writer, const std::string* key, object*): key = { object }
#define RET_COUNTRY  0x75179au   // return address of its call in the country loop of FUN_14074ff30
#define RVA_STATIC_A 0x15bf70u   // FUN_14015bf70(value*) -> static serializer 0x142458bf0 (value at +0x10)
#define RVA_STATIC_B 0x2c59b0u   // FUN_1402c59b0(value*) -> static serializer 0x142458e80 (value at +0x10)
#define RVA_STATIC_L 0x451740u   // FUN_140451740(list, writer): {token, value} pairs through serializer A
#define OBJ_A        0x2458bf0u
#define OBJ_B        0x2458e80u
#define RVA_TOKEN    0x1709280u  // FUN_141709280(writer, token)
#define RVA_STRCTOR  0x95240u    // FUN_140095240(std::string*, const char*)
#define RVA_STRFREE  0x95660u    // FUN_140095660(std::string*)
#define RVA_MSCTOR   0x171ebc0u  // memory stream (s, data, size, std::vector<char>*, mode, mode2, owns_data, owns_vector)
#define VT_MSTREAM   0x1dc0358u
#define RVA_NEW      0x1a33310u  // operator new (thunk)
#define RVA_GS       0x233fe78u  // DAT_14233fe78: the gamestate
#define GS_CLIST     0x1ce8      // country pointers: begin, end
#define MS_SIZE      0x58
#define MS_VEC       0x38        // std::vector<char>* (begin, end, cap)
// lazily created objects reached from the country writer (FUN_140798dc0, FUN_1407a73c0, FUN_140b354c0,
// FUN_140b6b4e0, FUN_1407b23e0/FUN_141711d40, FUN_140289600)
static const uint32_t lazy_rva[] = { 0x233fe88u, 0x242bc80u, 0x2458f30u, 0x242be50u, 0x2456bc0u, 0x233d8d0u };
static const uint8_t sig_keyobj[24]  = { 0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x81,0xec,0x30,0x02,0x00,0x00,0x49,0x8b,0xf0,0x48,0x8b,0xda };
static const uint8_t sig_static_l[24] = { 0x40,0x53,0x56,0x57,0x41,0x54,0x48,0x83,0xec,0x28,0x4c,0x8b,0x41,0x10,0x45,0x33,0xe4,0x48,0x8b,0x41,0x18,0x48,0x8b,0xfa };
static const uint8_t sig_msctor[24]  = { 0x4c,0x8b,0xdc,0x49,0x89,0x5b,0x08,0x49,0x89,0x6b,0x10,0x49,0x89,0x73,0x18,0x49,0x89,0x7b,0x20,0x41,0x56,0x48,0x83,0xec };
static const uint8_t sig_strctor[16] = { 0x40,0x53,0x48,0x83,0xec,0x20,0x33,0xc0,0x48,0xc7,0x41,0x18,0x0f,0x00,0x00,0x00 };
static const uint8_t sig_strfree[16] = { 0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x51,0x18,0x48,0x8b,0xd9,0x48,0x83,0xfa };
static const uint8_t sig_loop[24]    = { 0x8b,0xcf,0x49,0x8b,0x86,0xe8,0x1c,0x00,0x00,0x48,0x8b,0x1c,0xc8,0x48,0x8b,0x43,0x20,0x48,0x89,0x85,0xc8,0xac,0x00,0x00 };
#define RVA_LOOP     0x751760u

typedef void  (*keyobj_t)(void* writer, void* key, void* obj);
typedef void* (*static_t)(void* value);
typedef void  (*static_l_t)(void* list, void* writer);
typedef void  (*token_t)(void* writer, uint32_t token);
typedef void* (*strctor_t)(void* s, const char* c);
typedef void  (*strfree_t)(void* s);
typedef void* (*msctor_t)(void* s, void* data, int size, void* vec, int mode, int mode2, char owns_data, char owns_vec);
typedef void* (*new_t)(uint64_t n);
static keyobj_t o_keyobj;
static static_t o_static_a, o_static_b;
static static_l_t o_static_l;
static token_t game_token;
static strctor_t game_strctor;
static strfree_t game_strfree;
static msctor_t game_msctor;
static new_t game_new;
static uint8_t* g_base;
static uint8_t* g_ret_country;

#define VT(o) (*(void***)(o))
typedef void (*st_write_t)(void* self, const void* src, int64_t n);
#define ST_WRITE(s, p, n) ((st_write_t)VT(s)[0x50 / 8])((s), (p), (n))

// ---- per-thread copies of the shared serializer objects -------------------------------------
static uint8_t g_tmpl_a[0x20], g_tmpl_b[0x18];
static __thread int t_par;                        // 1 on pool threads while a batch runs
static __thread uint8_t t_obj_a[0x20] __attribute__((aligned(16)));
static __thread uint8_t t_obj_b[0x20] __attribute__((aligned(16)));

static void* hk_static_a(void* value) {
    if (!t_par) return o_static_a(value);
    memcpy(t_obj_a, g_tmpl_a, sizeof g_tmpl_a);
    *(void**)(t_obj_a + 0x10) = value; *(uint64_t*)(t_obj_a + 0x18) = 0;
    return t_obj_a;
}
static void* hk_static_b(void* value) {
    if (!t_par) return o_static_b(value);
    memcpy(t_obj_b, g_tmpl_b, sizeof g_tmpl_b);
    *(void**)(t_obj_b + 0x10) = value;
    return t_obj_b;
}
static void hk_static_l(void* list, void* writer) {           // FUN_140451740 with object A per thread
    if (!t_par) { o_static_l(list, writer); return; }
    uint8_t* l = (uint8_t*)list;
    memcpy(t_obj_a, g_tmpl_a, 0x10);                          // vtable and type, as initialized
    for (int i = 0; i < (int)((*(int64_t*)(l + 0x18) - *(int64_t*)(l + 0x10)) >> 3); i++) {
        *(uint64_t*)(t_obj_a + 0x18) = 0;
        *(void**)(t_obj_a + 0x10) = *(uint8_t**)(l + 0x10) + (int64_t)i * 8 + 4;
        game_token(writer, *(uint32_t*)(*(uint8_t**)(l + 0x10) + (int64_t)i * 8));
        ((void (*)(void*, void*))VT(t_obj_a)[1])(t_obj_a, writer);
    }
}

// ---- the batch ---------------------------------------------------------------------------------
#define MAXW 15
typedef struct { uint8_t* c; int thr, ok; int64_t beg, end; } sjob_t;
static sjob_t* g_jobs;
static int g_jobcap;
static volatile LONG g_njobs, g_next, g_done, g_pending;
static HANDLE g_wake[MAXW];
static int g_nw;
static void* g_stream[MAXW + 1];                    // per thread, [0] = main
static uint8_t g_wtmpl[0x20];                       // the game's writer as the loop uses it
static int g_mode_ms, g_mode2_ms;                   // the real stream's mode fields
static int g_active, g_k, g_n;                      // batch state: next index, countries
static LONG64 g_saves, g_batches, g_fallback, g_emitted, g_ver, g_ver_bad, g_bytes, g_checked, g_check_bad;
static char g_status[160];                          // what the latest save's country loop did (savezip.c's save journal)
static double g_pre_ms, g_pre_max, g_qpf_ms;
static int g_logged;

static inline LONGLONG qpc(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static int64_t vsize(void* s) { int64_t* v = *(int64_t**)((uint8_t*)s + MS_VEC); return v ? v[1] - v[0] : 0; }
static uint8_t* vdata(void* s) { int64_t* v = *(int64_t**)((uint8_t*)s + MS_VEC); return v ? (uint8_t*)v[0] : NULL; }
// a memory stream's logical bytes: the fixed buffer (+0x40, +0x50 bytes) and then its vector; the write
// position is +0x4c (FUN_14171f290). Compares [pos, pos + len) with p; returns the first difference or -1.
static int64_t scmp(void* s, int64_t pos, const uint8_t* p, int64_t len) {
    uint8_t* fixed = *(uint8_t**)((uint8_t*)s + 0x40);
    int64_t nfix = fixed ? *(int*)((uint8_t*)s + 0x50) : 0;
    uint8_t* v = vdata(s);
    int64_t nv = vsize(s);
    for (int64_t i = 0; i < len; i++) {
        int64_t q = pos + i;
        uint8_t b = q < nfix ? fixed[q] : (v && q - nfix < nv ? v[q - nfix] : (uint8_t)~p[i]);
        if (b != p[i]) return i;
    }
    return -1;
}

// One country in the binary save format: u16 tokens; '{' 0x0003, '}' 0x0004, '=' 0x0001; tokens with a
// payload: 0x000c i32, 0x000d f32, 0x0014 u32 (4 bytes), 0x000e bool (1), 0x0167 fixed-point and 0x029c
// u64 (8), 0x000f and 0x0017 strings (u16 length, then the bytes); every other token is a name. The bytes
// must end exactly at a token's end, with every block that was opened closed again.
static int balanced(const uint8_t* p, int64_t n) {
    int64_t i = 0; int depth = 0, opened = 0;
    if (!p || n <= 0) return 0;
    while (i + 2 <= n) {
        unsigned t = (unsigned)p[i] | (unsigned)p[i + 1] << 8;
        i += 2;
        switch (t) {
        case 0x0003: depth++; opened = 1; break;
        case 0x0004: if (--depth < 0) return 0; break;
        case 0x000c: case 0x000d: case 0x0014: i += 4; break;
        case 0x000e: i += 1; break;
        case 0x0167: case 0x029c: i += 8; break;
        case 0x000f: case 0x0017:
            if (i + 2 > n) return 0;
            i += 2 + ((int64_t)p[i] | (int64_t)p[i + 1] << 8);
            break;
        default: break;
        }
    }
    return i == n && depth == 0 && opened;
}

static void* new_stream(void) {
    void* s = game_new(MS_SIZE);
    uint64_t* vec = (uint64_t*)game_new(0x18);
    if (!s || !vec) return NULL;
    vec[0] = vec[1] = vec[2] = 0;
    game_msctor(s, NULL, 0, vec, g_mode_ms, g_mode2_ms, 0, 1);      // as eu4savegamehelper builds its stream
    *(void**)s = g_base + VT_MSTREAM;
    return s;
}
static void free_streams(void) {
    for (int i = 0; i <= MAXW; i++)
        if (g_stream[i]) { ((void (*)(void*, int))VT(g_stream[i])[0])(g_stream[i], 1); g_stream[i] = NULL; }
}

static void run_jobs(int thr) {
    uint8_t w[0x20] __attribute__((aligned(16)));
    memcpy(w, g_wtmpl, sizeof w);
    *(void**)(w + 0x10) = g_stream[thr];
    w[0x19] = 0;                                    // does not own the stream
    for (;;) {
        LONG i = InterlockedIncrement(&g_next) - 1;
        if (i >= g_njobs) break;
        sjob_t* j = &g_jobs[i];
        char tag[16] = { 0 };
        memcpy(tag, j->c + 0x20, 8);                // the loop copies these 8 bytes and makes a string of them
        uint8_t key[0x20] __attribute__((aligned(16)));
        j->beg = vsize(g_stream[thr]);
        game_strctor(key, tag);
        o_keyobj(w, key, j->c);
        game_strfree(key);
        j->end = vsize(g_stream[thr]);
        j->ok = balanced(vdata(g_stream[thr]) + j->beg, j->end - j->beg);   // right after writing, while the bytes are in cache
        j->thr = thr;
        InterlockedIncrement(&g_done);
    }
}

static DWORD WINAPI worker(LPVOID arg) {
    int thr = (int)(intptr_t)arg;
    for (;;) {
        WaitForSingleObject(g_wake[thr - 1], INFINITE);
        t_par = 1;
        run_jobs(thr);
        t_par = 0;
        InterlockedDecrement(&g_pending);
    }
    return 0;
}

static const char* start_batch(uint8_t* writer, uint8_t** list, int n) {
    if (!writer[0x18]) return "text save";
    for (size_t i = 0; i < sizeof lazy_rva / sizeof lazy_rva[0]; i++)
        if (!*(void**)(g_base + lazy_rva[i])) return "a lazily created object does not exist yet";
    void* rs = *(void**)(writer + 0x10);
    if (!rs) return "no stream";
    if (!*(void**)((uint8_t*)rs + MS_VEC)) return "the stream has no vector";   // (a full fixed buffer drops whole writes)
    if (!g_logged++) eb_log("save: stream: fixed buffer %p (%d bytes), %d bytes in it, position %d, vector %lld bytes",
                            *(void**)((uint8_t*)rs + 0x40), *(int*)((uint8_t*)rs + 0x50), *(int*)((uint8_t*)rs + 0x48),
                            *(int*)((uint8_t*)rs + 0x4c), (long long)vsize(rs));
    if (n > g_jobcap) {
        sjob_t* p = (sjob_t*)realloc(g_jobs, (size_t)n * sizeof *p);
        if (!p) return "out of memory";
        g_jobs = p; g_jobcap = n;
    }
    if (!g_nw && g_cfg.save_countries != 3) {
        SYSTEM_INFO si; GetSystemInfo(&si);
        int nw = (int)si.dwNumberOfProcessors - 1;
        if (nw > MAXW) nw = MAXW;
        for (int i = 0; i < nw; i++) {
            g_wake[i] = CreateEventW(NULL, FALSE, FALSE, NULL);
            HANDLE h = g_wake[i] ? CreateThread(NULL, 4u << 20, worker, (LPVOID)(intptr_t)(i + 1), STACK_SIZE_PARAM_IS_A_RESERVATION, NULL) : NULL;
            if (!h) break;
            CloseHandle(h); g_nw++;
        }
    }
    o_static_a(NULL); o_static_b(NULL);              // make sure both statics are initialized, then copy them
    memcpy(g_tmpl_a, g_base + OBJ_A, sizeof g_tmpl_a);
    memcpy(g_tmpl_b, g_base + OBJ_B, sizeof g_tmpl_b);
    memcpy(g_wtmpl, writer, sizeof g_wtmpl);
    g_mode_ms = *(int*)((uint8_t*)rs + 0x10); g_mode2_ms = *(int*)((uint8_t*)rs + 0x14);
    int threads = g_cfg.save_countries == 3 ? 1 : 1 + g_nw;
    if (threads > n) threads = n;
    for (int i = 0; i < threads; i++) if (!(g_stream[i] = new_stream())) { free_streams(); return "out of memory"; }
    for (int i = 0; i < n; i++) { g_jobs[i].c = list[i]; g_jobs[i].thr = -1; g_jobs[i].ok = 0; g_jobs[i].beg = g_jobs[i].end = 0; }
    LONGLONG t0 = qpc();
    while (g_pending) YieldProcessor();
    g_next = 0; g_done = 0;
    MemoryBarrier();
    g_njobs = n;
    int wake = threads - 1;
    InterlockedExchange(&g_pending, wake);
    for (int i = 0; i < wake; i++) SetEvent(g_wake[i]);
    run_jobs(0);                                     // the main thread uses the game's own statics
    while (g_done < n) YieldProcessor();
    while (g_pending) YieldProcessor();
    double ms = (double)(qpc() - t0) * g_qpf_ms;
    g_pre_ms += ms; if (ms > g_pre_max) g_pre_max = ms;
    int64_t bytes = 0;
    for (int i = 0; i < n; i++) { if (g_jobs[i].thr < 0) { free_streams(); return "a country was not written"; } bytes += g_jobs[i].end - g_jobs[i].beg; }
    int bad = 0, first = -1;
    for (int i = 0; i < n; i++) if (!g_jobs[i].ok) { if (first < 0) first = i; bad++; }
    g_checked += n;
    if (bad) {
        g_check_bad += bad;
        static int logged;
        if (logged++ < 5)
            eb_log("save: CHECK FAILED - %d of %d countries' bytes do not close (first: %d, %.3s, %lld bytes) - this save is written the original way",
                   bad, n, first, (const char*)(g_jobs[first].c + 0x20), (long long)(g_jobs[first].end - g_jobs[first].beg));
        free_streams();
        return "the check of a country's bytes failed";
    }
    g_bytes += bytes;
    if (g_cfg.save_stats) eb_log("save: %d countries serialized in %.1f ms on %d threads (%.1f MB)", n, ms, threads, (double)bytes / 1048576.0);
    return NULL;
}

static void end_batch(void) { free_streams(); g_active = 0; }

static void hk_keyobj(void* writer, void* key, void* obj) {
    if ((uint8_t*)__builtin_return_address(0) != g_ret_country || GetCurrentThreadId() != g_main_tid) { o_keyobj(writer, key, obj); return; }
    uint8_t* gs = *(uint8_t**)(g_base + RVA_GS);
    uint8_t** list = gs ? *(uint8_t***)(gs + GS_CLIST) : NULL;
    int n = 0;                                       // the loop's count: gamestate vtable +0x40, within the list
    if (list) {
        int cap = (int)((*(uint8_t***)(gs + GS_CLIST + 8)) - list);
        n = ((int (*)(void*))VT(gs)[0x40 / 8])(gs);
        if (n > cap) n = cap;
    }
    if (list && n > 0 && obj == list[0]) {           // the loop starts: a new batch
        if (g_active) end_batch();
        g_saves++;
        const char* why = start_batch((uint8_t*)writer, list, n);
        if (why) {
            g_fallback++;
            snprintf(g_status, sizeof g_status, "countries original (%s)", why);
            if (g_logged++ < 10) eb_log("save: countries written the original way (%s)", why);
        } else { g_active = 1; g_k = 0; g_n = n; g_batches++; snprintf(g_status, sizeof g_status, "countries parallel (%d, checked)", n); }
    }
    if (!g_active || g_k >= g_n || obj != g_jobs[g_k].c) {
        if (g_active) {
            end_batch(); g_fallback++;
            size_t l = strlen(g_status); snprintf(g_status + l, sizeof g_status - l, "; original from %d on", g_k);
            if (g_logged++ < 10) eb_log("save: country loop order differs at %d - rest written the original way", g_k);
        }
        o_keyobj(writer, key, obj);
        return;
    }
    sjob_t* j = &g_jobs[g_k];
    const uint8_t* p = vdata(g_stream[j->thr]) + j->beg;
    int64_t len = j->end - j->beg;
    if (g_cfg.save_countries >= 2) {                       // verify: the game writes, the parallel bytes are compared
        void* rs = *(void**)((uint8_t*)writer + 0x10);
        int64_t before = *(int*)((uint8_t*)rs + 0x4c);
        o_keyobj(writer, key, obj);
        int64_t after = *(int*)((uint8_t*)rs + 0x4c);
        g_ver++;
        int64_t d = after - before == len ? scmp(rs, before, p, len) : 0;
        if (d >= 0) {
            g_ver_bad++;
            if (g_ver_bad <= 20)
                eb_log("save: VERIFY MISMATCH country %d (%.3s): game %lld bytes, parallel %lld, first difference at %lld",
                       g_k, (const char*)((uint8_t*)obj + 0x20), (long long)(after - before), (long long)len, (long long)d);
        }
    } else {
        ST_WRITE(*(void**)((uint8_t*)writer + 0x10), p, len);
        g_emitted++;
    }
    if (++g_k == g_n) end_batch();
}

int eb_savepar_install(void) {
    if (!g_cfg.save_countries) return 0;
#ifdef EB_TEST
    eb_log("save: parallel countries are not available in the test build");
    return 0;
#else
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpf_ms = 1000.0 / (double)f.QuadPart;
    uint8_t* b = (uint8_t*)g_exe;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != 0x66FE6AB9u) { eb_log("save: unexpected eu4.exe - parallel countries off"); return 0; }
    uint8_t* call = b + RET_COUNTRY - 5;
    if (memcmp(b + RVA_KEYOBJ, sig_keyobj, sizeof sig_keyobj) || memcmp(b + RVA_STATIC_L, sig_static_l, sizeof sig_static_l) ||
        memcmp(b + RVA_MSCTOR, sig_msctor, sizeof sig_msctor) || memcmp(b + RVA_STRCTOR, sig_strctor, sizeof sig_strctor) ||
        memcmp(b + RVA_STRFREE, sig_strfree, sizeof sig_strfree) || memcmp(b + RVA_LOOP, sig_loop, sizeof sig_loop) ||
        call[0] != 0xe8 || call + 5 + *(int32_t*)(call + 1) != b + RVA_KEYOBJ ||
        *(void**)(b + VT_MSTREAM + 0x50) != (void*)(b + 0x171f290u)) {
        eb_log("save: signature mismatch - parallel countries off");
        return 0;
    }
    // the two static-object getters: the same code, distinguished by the objects they reference
    for (int k = 0; k < 2; k++) {
        uint8_t* fn = b + (k ? RVA_STATIC_B : RVA_STATIC_A);
        uint32_t obj = k ? OBJ_B : OBJ_A;
        int found = 0;
        for (int i = 0; i < 0x80 && !found; i++)              // lea rax, [rip + obj] somewhere in the getter
            if (fn[i] == 0x48 && fn[i + 1] == 0x8d && (fn[i + 2] & 0xc7) == 0x05 && fn + i + 7 + *(int32_t*)(fn + i + 3) == b + obj) found = 1;
        if (!found) { eb_log("save: static serializer %d not found - parallel countries off", k); return 0; }
    }
    g_base = b;
    g_ret_country = b + RET_COUNTRY;
    game_token = (token_t)(void*)(b + RVA_TOKEN);
    game_strctor = (strctor_t)(void*)(b + RVA_STRCTOR);
    game_strfree = (strfree_t)(void*)(b + RVA_STRFREE);
    game_msctor = (msctor_t)(void*)(b + RVA_MSCTOR);
    game_new = (new_t)(void*)(b + RVA_NEW);
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) { eb_log("save: MinHook init failed"); return 0; }
    struct { uint32_t rva; void* hook; void** orig; } t[] = {
        { RVA_STATIC_A, (void*)hk_static_a, (void**)&o_static_a },
        { RVA_STATIC_B, (void*)hk_static_b, (void**)&o_static_b },
        { RVA_STATIC_L, (void*)hk_static_l, (void**)&o_static_l },
        { RVA_KEYOBJ,   (void*)hk_keyobj,   (void**)&o_keyobj },
    };
    eb_hookset hs; eb_hs_init(&hs, "save countries");        // the serializer copies work only together
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!eb_hs_add(&hs, b + t[i].rva, t[i].hook, t[i].orig)) break;
    if (!eb_hs_enable(&hs)) {
        eb_log("save: hooks failed - parallel countries off");
        g_cfg.save_countries = 0;
        return 0;
    }
    eb_log("save: countries serialized in parallel (mode %d: %s)", g_cfg.save_countries,
           g_cfg.save_countries == 1 ? "on" : g_cfg.save_countries == 2 ? "on + verify" : "serial precompute + verify");
    return 1;
#endif
}

#ifdef EB_TEST
// self-test of balanced() on hand-made token streams; returns the number of wrong answers
__declspec(dllexport) int eb_test_balanced(void) {
    static const uint8_t ok1[] = { 0x17,0x00, 3,0, 'S','W','E', 0x01,0x00, 0x03,0x00,            // SWE = {
                                   0x34,0x12, 0x01,0x00, 0x0c,0x00, 1,2,3,4,                     //   name = i32
                                   0x35,0x12, 0x01,0x00, 0x0f,0x00, 4,0, 0x03,0x00,0x04,0x04,    //   name = "\3\0\4\4" (tokens inside a string)
                                   0x36,0x12, 0x01,0x00, 0x03,0x00, 0x0e,0x00, 1, 0x67,0x01, 1,2,3,4,5,6,7,8, 0x04,0x00,   //   name = { bool f64 }
                                   0x04,0x00 };                                                  // }
    static const uint8_t open1[] = { 0x17,0x00, 1,0, 'X', 0x01,0x00, 0x03,0x00, 0x03,0x00, 0x04,0x00 };   // one block left open
    static const uint8_t close1[] = { 0x03,0x00, 0x04,0x00, 0x04,0x00 };                       // one '}' too many
    static const uint8_t cut1[] = { 0x03,0x00, 0x0c,0x00, 1,2, 0x04,0x00 };                    // i32 cut short: ends inside a value
    static const uint8_t str1[] = { 0x03,0x00, 0x0f,0x00, 9,0, 'a', 0x04,0x00 };               // string longer than the bytes
    int bad = 0;
    bad += balanced(ok1, sizeof ok1) != 1;
    bad += balanced(open1, sizeof open1) != 0;
    bad += balanced(close1, sizeof close1) != 0;
    bad += balanced(cut1, sizeof cut1) != 0;
    bad += balanced(str1, sizeof str1) != 0;
    bad += balanced(ok1, sizeof ok1 - 2) != 0;                  // the last '}' missing
    bad += balanced(NULL, 0) != 0;
    eb_log("TEST balanced: %d wrong answers", bad);
    return bad;
}
#endif

// savezip.c: before a save (clear) and after it (what the country loop did)
void eb_savepar_mark(void) { g_status[0] = 0; }
const char* eb_savepar_status(void) { return g_status[0] ? g_status : o_keyobj ? "countries not reached" : "parallel countries off"; }

void eb_savepar_report(void) {
    if (!o_keyobj) return;
    eb_log("save: parallel countries: %lld saves, %lld batches (%.1f ms avg, %.1f max, %.1f MB avg), %lld fallbacks, %lld countries appended | checked %lld, failed %lld | verify %lld, mismatches %lld",
           (long long)g_saves, (long long)g_batches, g_batches ? g_pre_ms / (double)g_batches : 0.0, g_pre_max,
           g_batches ? (double)g_bytes / 1048576.0 / (double)g_batches : 0.0, (long long)g_fallback, (long long)g_emitted,
           (long long)g_checked, (long long)g_check_bad, (long long)g_ver, (long long)g_ver_bad);
}
