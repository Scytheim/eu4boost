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
//
// Provinces ([save] parallel_provinces, experiment, 2026-11-07): the province loop of the same writer
// (0x140751657..0x14075172f, before the countries) walks gamestate+0x1ca8 (provinces inline, 0x2e10
// bytes each, count = the pointer vector +0x1c90/+0x1c98) and, for every province whose id (+0x20) is
// not 0, writes a key record { type 0xc, "%d" of -id } and a second record { type 1, the std::string at
// *0x14242b888 (+0x20 when *0x142357b28 >= 1) } with FUN_141708870(writer, record), then the province
// through its vtable +8 (FUN_140d2f410 -> FUN_1409d6e60). Profile 2026-11-07 (user's JAP campaign copy,
// 40 MB saves): 38 ms of the ~130 ms a save keeps the main thread, the biggest serial part left.
// A hook at the loop's first instruction (0x140751660) writes, at index 0, every province at once the
// same way the countries are (the same per-thread writers, streams, serializer copies and checks), then
// at every index appends that province's bytes to the real stream and jumps to the loop's increment
// (0x14075170c), skipping the two key writes and the province's own write. Shared state on the province
// writer's paths (532 functions: static calls, the profile's virtual calls and the Write of every persistent
// class the province constructor FUN_1409d5a20 makes): the same as the country writer's, plus lazily created
// objects (0x14233d8d0, already checked; 0x14242be70 FUN_140d33000; 0x14242bd98 FUN_140099b60), so a batch
// also needs those and the lexer's token table (0x142357b30, FUN_14170d250) to exist; and FUN_140451a40, a
// province member's Write that sets serializer A itself (see hk_static_m).
// Modes as for the countries: 2 = the game writes and a hook at the increment compares, 3 = serial
// precompute on the main thread + verify.
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
// provinces (see the head comment)
#define RVA_PLOOP    0x751657u   // the province loop: mov rbx, r13 ... jl back (0xd8 bytes)
#define RVA_PHEAD    0x751660u   // first instruction of an iteration (mov rax, [r14+0x1ca8])
#define RVA_PNEXT    0x75170cu   // the increment (inc edi; add rbx, 0x2e10)
#define RVA_KEYREC   0x1708870u  // FUN_141708870(writer, record*): writes a key record
#define RVA_SPRINTF  0x15c230u   // FUN_14015c230(buf, fmt, ...): the game's sprintf
#define RVA_FMT_D    0x1d64d54u  // "%d"
#define RVA_KEY2     0x242b888u  // std::string* (the second record's text)
#define RVA_KEY2SEL  0x2357b28u  // int: >= 1 -> the string at +0x20
#define RVA_LEXINIT  0x2357b30u  // char: the lexer's token table exists (FUN_14170d250)
#define LAZY_PROV    0x242be70u  // FUN_140d33000
#define LAZY_PROV2   0x242bd98u  // FUN_140099b60
#define RVA_STATIC_M 0x451a40u   // FUN_140451a40(list, writer): a province member, writes through serializer A itself
static const uint8_t sig_static_m[24] = { 0x4c,0x8b,0xdc,0x53,0x55,0x56,0x41,0x57,0x48,0x81,0xec,0x38,0x02,0x00,0x00,0x4c,0x8b,0x41,0x10,0x45,0x33,0xff,0x48,0x8b };
#define OFF_M_VALUE  0x103       // FUN_140451a40+0x103: mov [rip -> 0x142458c00], rax (the value pointer of object A)
#define GS_PARR      0x1ca8
#define GS_PVEC      0x1c90
#define PSTRIDE      0x2e10
#define REC_SIZE     0x210       // type (4), text (0x200), flag at +0x204
static const uint8_t sig_ploop[0xd8] = {
    0x49,0x8b,0xdd,0x66,0x0f,0x1f,0x44,0x00,0x00,0x49,0x8b,0x86,0xa8,0x1c,0x00,0x00,
    0x44,0x8b,0x44,0x03,0x20,0x45,0x85,0xc0,0x0f,0x84,0x97,0x00,0x00,0x00,0x41,0xf7,
    0xd8,0xc7,0x45,0xe0,0x0c,0x00,0x00,0x00,0xc6,0x85,0xe4,0x01,0x00,0x00,0x00,0x48,
    0x8d,0x15,0xc7,0x36,0x61,0x01,0x48,0x8d,0x4d,0xe4,0xe8,0x9a,0xab,0xa0,0xff,0x48,
    0x8d,0x55,0xe0,0x48,0x8b,0xce,0xe8,0xce,0x71,0xfb,0x00,0x80,0x3d,0x87,0x64,0xc0,
    0x01,0x00,0x75,0x05,0xe8,0xa0,0xbb,0xfb,0x00,0x48,0x8b,0x15,0xd1,0xa1,0xcd,0x01,
    0x83,0x3d,0x6a,0x64,0xc0,0x01,0x01,0x7c,0x04,0x48,0x83,0xc2,0x20,0xc6,0x85,0xe4,
    0x01,0x00,0x00,0x00,0xc7,0x45,0xe0,0x01,0x00,0x00,0x00,0x8b,0x42,0x10,0x48,0x83,
    0x7a,0x18,0x10,0x72,0x03,0x48,0x8b,0x12,0xff,0xc0,0x4c,0x63,0xc0,0x48,0x8d,0x4d,
    0xe4,0xe8,0x63,0x40,0x2e,0x01,0x48,0x8d,0x55,0xe0,0x48,0x8b,0xce,0xe8,0x77,0x71,
    0xfb,0x00,0x49,0x8b,0x8e,0xa8,0x1c,0x00,0x00,0x48,0x03,0xcb,0x48,0x8b,0x01,0x48,
    0x8b,0xd6,0xff,0x50,0x08,0xff,0xc7,0x48,0x81,0xc3,0x10,0x2e,0x00,0x00,0x49,0x8b,
    0x86,0x98,0x1c,0x00,0x00,0x49,0x2b,0x86,0x90,0x1c,0x00,0x00,0x48,0xc1,0xf8,0x03,
    0x3b,0xf8,0x0f,0x8c,0x31,0xff,0xff,0xff };
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
static static_l_t o_static_l, o_static_m;
static CRITICAL_SECTION g_alock;                  // provinces: FUN_140451a40 one thread at a time
static volatile LONG g_alock_on;                  // ... while a province batch runs
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

// FUN_140451a40 (the Write of a member that only provinces have: a list of 12-byte entries, each written as
// key, "=", then its date at +8 through serializer A, then a closing record) sets object A itself instead
// of through FUN_14015bf70, so per-thread copies cannot cover it. While a province batch runs, every thread
// (the main thread included) uses per-thread copies through the getters, and this function runs under a
// lock: object A is then used by one thread at a time, inside this function only. Found 2026-11-07: three
// province bytes of 30,750 differed in mode 2 (a date of this list), none in mode 3.
static void hk_static_m(void* list, void* writer) {
    if (!g_alock_on) { o_static_m(list, writer); return; }
    EnterCriticalSection(&g_alock);
    o_static_m(list, writer);
    LeaveCriticalSection(&g_alock);
}

// ---- the batch ---------------------------------------------------------------------------------
#define MAXW 15
typedef struct { uint8_t* c; int thr, ok; int64_t beg, end; int idx, id; } sjob_t;   // idx, id: provinces
static sjob_t* g_jobs;
static volatile int g_kind;                         // what the jobs are: 0 countries, 1 provinces
typedef void (*keyrec_t)(void* writer, void* rec);
typedef int (*gsprintf_t)(char* buf, const char* fmt, ...);
static keyrec_t game_keyrec;
static gsprintf_t game_sprintf;
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
// provinces: batch state, the second record's text as the batch read it, statistics
static int g_pact, g_pk, g_pn, g_pv_job = -1;
static int64_t g_pv_before;
static char g_key2[0x200]; static int g_key2_len;
static const void* g_key2_src;
static LONG64 g_psaves, g_pbatches, g_pfallback, g_pemitted, g_pver, g_pver_bad, g_pbytes, g_pchecked, g_pcheck_bad;
static char g_pstatus[160];
static double g_ppre_ms, g_ppre_max;
static int g_plogged;

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
        if (g_kind == 1) {                          // a province, as one iteration of the loop writes it
            uint8_t rec[REC_SIZE] __attribute__((aligned(16)));
            memset(rec, 0, sizeof rec);
            j->beg = vsize(g_stream[thr]);
            *(int*)rec = 0xc;
            game_sprintf((char*)rec + 4, (const char*)(g_base + RVA_FMT_D), -j->id);
            game_keyrec(w, rec);
            rec[0x204] = 0; *(int*)rec = 1;
            memcpy(rec + 4, g_key2, (size_t)g_key2_len + 1);
            game_keyrec(w, rec);
            ((void (*)(void*, void*))VT(j->c)[1])(j->c, w);
            j->end = vsize(g_stream[thr]);
            j->ok = balanced(vdata(g_stream[thr]) + j->beg, j->end - j->beg);
            j->thr = thr;
            InterlockedIncrement(&g_done);
            continue;
        }
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

static void ensure_workers(void) {
    if (g_nw) return;
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

// all jobs g_jobs[0..n) on `threads` threads (the main thread is one of them); returns the milliseconds
static double run_all(int n, int threads, int kind) {
    LONGLONG t0 = qpc();
    while (g_pending) YieldProcessor();
    g_next = 0; g_done = 0; g_kind = kind;
    MemoryBarrier();
    g_njobs = n;
    int wake = threads - 1;
    InterlockedExchange(&g_pending, wake);
    for (int i = 0; i < wake; i++) SetEvent(g_wake[i]);
    if (kind == 1) { g_alock_on = 1; t_par = 1; }   // provinces: the main thread too uses per-thread copies
    run_jobs(0);                                     // (countries: the main thread uses the game's own statics)
    while (g_done < n) YieldProcessor();
    while (g_pending) YieldProcessor();
    if (kind == 1) { t_par = 0; g_alock_on = 0; }
    return (double)(qpc() - t0) * g_qpf_ms;
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
    if (g_cfg.save_countries != 3) ensure_workers();
    o_static_a(NULL); o_static_b(NULL);              // make sure both statics are initialized, then copy them
    memcpy(g_tmpl_a, g_base + OBJ_A, sizeof g_tmpl_a);
    memcpy(g_tmpl_b, g_base + OBJ_B, sizeof g_tmpl_b);
    memcpy(g_wtmpl, writer, sizeof g_wtmpl);
    g_mode_ms = *(int*)((uint8_t*)rs + 0x10); g_mode2_ms = *(int*)((uint8_t*)rs + 0x14);
    int threads = g_cfg.save_countries == 3 ? 1 : 1 + g_nw;
    if (threads > n) threads = n;
    for (int i = 0; i < threads; i++) if (!(g_stream[i] = new_stream())) { free_streams(); return "out of memory"; }
    for (int i = 0; i < n; i++) { g_jobs[i].c = list[i]; g_jobs[i].thr = -1; g_jobs[i].ok = 0; g_jobs[i].beg = g_jobs[i].end = 0; }
    double ms = run_all(n, threads, 0);
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
static void end_pbatch(void) { free_streams(); g_pact = 0; g_pv_job = -1; }

// ---- provinces ----------------------------------------------------------------------------------
// the second record's text exactly as an iteration of the loop reads it
static const uint8_t* key2_src(int* len) {
    uint8_t* s = *(uint8_t**)(g_base + RVA_KEY2);
    if (!s) return NULL;
    if (*(int*)(g_base + RVA_KEY2SEL) >= 1) s += 0x20;
    *len = *(int*)(s + 0x10);
    return *(uint64_t*)(s + 0x18) >= 0x10 ? *(const uint8_t**)s : s;
}

static const char* start_pbatch(uint8_t* gs, uint8_t* writer, int n) {
    if (!writer[0x18]) return "text save";
    for (size_t i = 0; i < sizeof lazy_rva / sizeof lazy_rva[0]; i++)
        if (!*(void**)(g_base + lazy_rva[i])) return "a lazily created object does not exist yet";
    if (!*(void**)(g_base + LAZY_PROV) || !*(void**)(g_base + LAZY_PROV2)) return "a lazily created object does not exist yet";
    if (!*(volatile char*)(g_base + RVA_LEXINIT)) return "the token table is not made yet";
    void* rs = *(void**)(writer + 0x10);
    if (!rs) return "no stream";
    if (!*(void**)((uint8_t*)rs + MS_VEC)) return "the stream has no vector";
    int len = 0;
    const uint8_t* k2 = key2_src(&len);
    if (!k2 || len < 0 || len + 1 > (int)sizeof g_key2 || len + 1 > REC_SIZE - 4 - 8) return "unexpected second key";
    memcpy(g_key2, k2, (size_t)len + 1); g_key2_len = len; g_key2_src = k2;
    uint8_t* arr = *(uint8_t**)(gs + GS_PARR);
    if (!arr) return "no provinces";
    int m = 0;
    for (int i = 0; i < n; i++) if (*(int*)(arr + (int64_t)i * PSTRIDE + 0x20)) m++;
    if (!m) return "no provinces";
    if (m > g_jobcap) {
        sjob_t* p = (sjob_t*)realloc(g_jobs, (size_t)m * sizeof *p);
        if (!p) return "out of memory";
        g_jobs = p; g_jobcap = m;
    }
    if (g_cfg.save_provinces != 3) ensure_workers();
    o_static_a(NULL); o_static_b(NULL);
    memcpy(g_tmpl_a, g_base + OBJ_A, sizeof g_tmpl_a);
    memcpy(g_tmpl_b, g_base + OBJ_B, sizeof g_tmpl_b);
    memcpy(g_wtmpl, writer, sizeof g_wtmpl);
    g_mode_ms = *(int*)((uint8_t*)rs + 0x10); g_mode2_ms = *(int*)((uint8_t*)rs + 0x14);
    int threads = g_cfg.save_provinces == 3 ? 1 : 1 + g_nw;
    if (threads > m) threads = m;
    for (int i = 0; i < threads; i++) if (!(g_stream[i] = new_stream())) { free_streams(); return "out of memory"; }
    for (int i = 0, k = 0; i < n; i++) {
        uint8_t* p = arr + (int64_t)i * PSTRIDE;
        int id = *(int*)(p + 0x20);
        if (!id) continue;
        sjob_t* j = &g_jobs[k++];
        j->c = p; j->idx = i; j->id = id; j->thr = -1; j->ok = 0; j->beg = j->end = 0;
    }
    double ms = run_all(m, threads, 1);
    g_ppre_ms += ms; if (ms > g_ppre_max) g_ppre_max = ms;
    int64_t bytes = 0; int bad = 0, first = -1;
    for (int i = 0; i < m; i++) {
        if (g_jobs[i].thr < 0) { free_streams(); return "a province was not written"; }
        bytes += g_jobs[i].end - g_jobs[i].beg;
        if (!g_jobs[i].ok) { if (first < 0) first = i; bad++; }
    }
    g_pchecked += m;
    if (bad) {
        g_pcheck_bad += bad;
        static int logged;
        if (logged++ < 5)
            eb_log("save: CHECK FAILED - %d of %d provinces' bytes do not close (first: id %d, %lld bytes) - provinces written the original way",
                   bad, m, g_jobs[first].id, (long long)(g_jobs[first].end - g_jobs[first].beg));
        free_streams();
        return "the check of a province's bytes failed";
    }
    g_pbytes += bytes; g_pn = m; g_pk = 0;
    if (g_cfg.save_stats) eb_log("save: %d provinces serialized in %.1f ms on %d threads (%.1f MB)", m, ms, threads, (double)bytes / 1048576.0);
    return NULL;
}

static void pfallback(const char* what) {
    end_pbatch(); g_pfallback++;
    size_t l = strlen(g_pstatus); snprintf(g_pstatus + l, sizeof g_pstatus - l, "; original from %d on (%s)", g_pk, what);
    if (g_plogged++ < 10) eb_log("save: province loop: %s at %d - the rest written the original way", what, g_pk);
}

// called by the stub at 0x140751660 with the loop's registers: r14 gamestate, rsi writer, edi index,
// rbx index * 0x2e10. 1 = this province's bytes were appended: go on at the increment.
int eb_sp_head(uint8_t* gs, uint8_t* writer, int i, int64_t off) {
    if (GetCurrentThreadId() != g_main_tid) return 0;
    if (g_pv_job >= 0) { pfallback("no compare after the last province"); return 0; }
    int n = (int)((*(int64_t*)(gs + GS_PVEC + 8) - *(int64_t*)(gs + GS_PVEC)) >> 3);
    if (i == 0) {                                    // the loop starts: a new batch
        if (g_pact) end_pbatch();
        if (g_active) end_batch();
        g_psaves++;
        const char* why = off != 0 ? "unexpected registers" : start_pbatch(gs, writer, n);
        if (why) {
            g_pfallback++;
            snprintf(g_pstatus, sizeof g_pstatus, "provinces original (%s)", why);
            if (g_plogged++ < 10) eb_log("save: provinces written the original way (%s)", why);
            return 0;
        }
        g_pact = 1; g_pbatches++;
        snprintf(g_pstatus, sizeof g_pstatus, "provinces parallel (%d, checked)", g_pn);
    }
    if (!g_pact) return 0;
    uint8_t* p = *(uint8_t**)(gs + GS_PARR) + off;
    if (!*(int*)(p + 0x20)) return 0;                // the loop skips it itself
    sjob_t* j = g_pk < g_pn ? &g_jobs[g_pk] : NULL;
    if (!j || j->idx != i || j->c != p || off != (int64_t)i * PSTRIDE) { pfallback("order differs"); return 0; }
    int len2 = 0;
    if (key2_src(&len2) != g_key2_src || len2 != g_key2_len) { pfallback("second key changed"); return 0; }
    void* rs = *(void**)(writer + 0x10);
    if (g_cfg.save_provinces >= 2) {                 // verify: the game writes it, eb_sp_end compares
        g_pv_job = g_pk++; g_pv_before = *(int*)((uint8_t*)rs + 0x4c);
        return 0;
    }
    ST_WRITE(rs, vdata(g_stream[j->thr]) + j->beg, j->end - j->beg);
    g_pemitted++;
    if (++g_pk == g_pn) end_pbatch();
    return 1;
}

// verify modes: called by the stub at 0x14075170c (the increment) after every iteration
void eb_sp_end(uint8_t* gs, uint8_t* writer, int i) {
    (void)gs;
    if (g_pv_job < 0 || GetCurrentThreadId() != g_main_tid) return;
    sjob_t* j = &g_jobs[g_pv_job];
    void* rs = *(void**)(writer + 0x10);
    int64_t after = *(int*)((uint8_t*)rs + 0x4c), len = j->end - j->beg;
    g_pver++;
    int64_t d = after - g_pv_before == len ? scmp(rs, g_pv_before, vdata(g_stream[j->thr]) + j->beg, len) : 0;
    if (d >= 0) {
        g_pver_bad++;
        if (g_pver_bad <= 20) {
            eb_log("save: VERIFY MISMATCH province %d (index %d, id %d, thread %d): game %lld bytes, parallel %lld, first difference at %lld",
                   g_pv_job, i, j->id, j->thr, (long long)(after - g_pv_before), (long long)len, (long long)d);
            // the bytes around it, game then parallel (diagnostics: which token's value differs)
            const uint8_t* pp = vdata(g_stream[j->thr]) + j->beg;
            uint8_t* fixed = *(uint8_t**)((uint8_t*)rs + 0x40);
            int64_t nfix = fixed ? *(int*)((uint8_t*)rs + 0x50) : 0;
            uint8_t* v = vdata(rs); int64_t nv = vsize(rs);
            int64_t a = d > 48 ? d - 48 : 0, e = d + 40 < len ? d + 40 : len;
            char hg[300], hp[300]; int og = 0, op = 0;
            for (int64_t q = a; q < e && og < 290; q++) {
                int64_t r = g_pv_before + q;
                uint8_t bg = r < nfix ? fixed[r] : (v && r - nfix < nv ? v[r - nfix] : 0);
                og += snprintf(hg + og, sizeof hg - og, q == d ? "[%02x" : "%02x", bg);
                op += snprintf(hp + op, sizeof hp - op, q == d ? "[%02x" : "%02x", pp[q]);
            }
            eb_log("save:   game     +%lld: %s", (long long)a, hg);
            eb_log("save:   parallel +%lld: %s", (long long)a, hp);
        }
    }
    g_pv_job = -1;
    if (g_pk == g_pn) end_pbatch();
}

void* eb_sp_head_tramp;                     // MinHook trampoline: the moved first instruction, then 0x140751667
void* eb_sp_end_tramp;                      // the moved increment, then 0x140751715
void* eb_sp_skip;                           // 0x14075170c
void eb_sp_head_stub(void);
void eb_sp_end_stub(void);
// rsp is 16-byte aligned at both places (the loop calls functions from the same rsp) and no volatile
// register is live there; [rsp, rsp+0x20) is the function's own outgoing home area.
__asm__(".text\n"
        ".globl eb_sp_head_stub\n"
        "eb_sp_head_stub:\n"
        "    movq %r14, %rcx\n"
        "    movq %rsi, %rdx\n"
        "    movl %edi, %r8d\n"
        "    movq %rbx, %r9\n"
        "    callq eb_sp_head\n"
        "    testl %eax, %eax\n"
        "    jz 1f\n"
        "    jmpq *eb_sp_skip(%rip)\n"
        "1:  jmpq *eb_sp_head_tramp(%rip)\n"
        ".globl eb_sp_end_stub\n"
        "eb_sp_end_stub:\n"
        "    movq %r14, %rcx\n"
        "    movq %rsi, %rdx\n"
        "    movl %edi, %r8d\n"
        "    callq eb_sp_end\n"
        "    jmpq *eb_sp_end_tramp(%rip)\n");

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
        if (g_pact) end_pbatch();                    // (the province loop always ends before)
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
    if (!g_cfg.save_countries && !g_cfg.save_provinces) return 0;
#ifdef EB_TEST
    eb_log("save: parallel countries and provinces are not available in the test build");
    return 0;
#else
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpf_ms = 1000.0 / (double)f.QuadPart;
    uint8_t* b = (uint8_t*)g_exe;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != 0x66FE6AB9u) {
        eb_log("save: unexpected eu4.exe - parallel countries and provinces off");
        g_cfg.save_countries = g_cfg.save_provinces = 0;
        return 0;
    }
    uint8_t* call = b + RET_COUNTRY - 5;
    if (memcmp(b + RVA_KEYOBJ, sig_keyobj, sizeof sig_keyobj) || memcmp(b + RVA_STATIC_L, sig_static_l, sizeof sig_static_l) ||
        memcmp(b + RVA_MSCTOR, sig_msctor, sizeof sig_msctor) || memcmp(b + RVA_STRCTOR, sig_strctor, sizeof sig_strctor) ||
        memcmp(b + RVA_STRFREE, sig_strfree, sizeof sig_strfree) || memcmp(b + RVA_LOOP, sig_loop, sizeof sig_loop) ||
        call[0] != 0xe8 || call + 5 + *(int32_t*)(call + 1) != b + RVA_KEYOBJ ||
        *(void**)(b + VT_MSTREAM + 0x50) != (void*)(b + 0x171f290u)) {
        eb_log("save: signature mismatch - parallel countries and provinces off");
        g_cfg.save_countries = g_cfg.save_provinces = 0;
        return 0;
    }
    // the two static-object getters: the same code, distinguished by the objects they reference
    for (int k = 0; k < 2; k++) {
        uint8_t* fn = b + (k ? RVA_STATIC_B : RVA_STATIC_A);
        uint32_t obj = k ? OBJ_B : OBJ_A;
        int found = 0;
        for (int i = 0; i < 0x80 && !found; i++)              // lea rax, [rip + obj] somewhere in the getter
            if (fn[i] == 0x48 && fn[i + 1] == 0x8d && (fn[i + 2] & 0xc7) == 0x05 && fn + i + 7 + *(int32_t*)(fn + i + 3) == b + obj) found = 1;
        if (!found) {
            eb_log("save: static serializer %d not found - parallel countries and provinces off", k);
            g_cfg.save_countries = g_cfg.save_provinces = 0;
            return 0;
        }
    }
    if (g_cfg.save_provinces) {                                // the province loop, byte for byte, and what it calls
        uint8_t* L = b + RVA_PLOOP;
#define CALLTGT(va) ((va) + 5 + *(int32_t*)((va) + 1))
#define LEATGT(va)  ((va) + 7 + *(int32_t*)((va) + 3))
        if (memcmp(L, sig_ploop, sizeof sig_ploop) ||
            CALLTGT(b + 0x751691u) != b + RVA_SPRINTF || CALLTGT(b + 0x75169du) != b + RVA_KEYREC ||
            CALLTGT(b + 0x7516f4u) != b + RVA_KEYREC || LEATGT(b + 0x751686u) != b + RVA_FMT_D ||
            memcmp(b + RVA_STATIC_M, sig_static_m, sizeof sig_static_m) ||
            b[RVA_STATIC_M + OFF_M_VALUE] != 0x48 || b[RVA_STATIC_M + OFF_M_VALUE + 1] != 0x89 || b[RVA_STATIC_M + OFF_M_VALUE + 2] != 0x05 ||
            b + RVA_STATIC_M + OFF_M_VALUE + 7 + *(int32_t*)(b + RVA_STATIC_M + OFF_M_VALUE + 3) != b + OBJ_A + 0x10 ||
            memcmp(b + RVA_FMT_D, "%d", 3)) {
            eb_log("save: province loop not as expected - parallel provinces off");
            g_cfg.save_provinces = 0;
        }
#undef CALLTGT
#undef LEATGT
    }
    if (!g_cfg.save_countries && !g_cfg.save_provinces) return 0;
    g_base = b;
    g_ret_country = b + RET_COUNTRY;
    game_token = (token_t)(void*)(b + RVA_TOKEN);
    game_strctor = (strctor_t)(void*)(b + RVA_STRCTOR);
    game_strfree = (strfree_t)(void*)(b + RVA_STRFREE);
    game_msctor = (msctor_t)(void*)(b + RVA_MSCTOR);
    game_new = (new_t)(void*)(b + RVA_NEW);
    game_keyrec = (keyrec_t)(void*)(b + RVA_KEYREC);
    game_sprintf = (gsprintf_t)(void*)(b + RVA_SPRINTF);
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) { eb_log("save: MinHook init failed"); g_cfg.save_countries = g_cfg.save_provinces = 0; return 0; }
    struct { uint32_t rva; void* hook; void** orig; } t[] = {
        { RVA_STATIC_A, (void*)hk_static_a, (void**)&o_static_a },
        { RVA_STATIC_B, (void*)hk_static_b, (void**)&o_static_b },
        { RVA_STATIC_L, (void*)hk_static_l, (void**)&o_static_l },
        { RVA_KEYOBJ,   (void*)hk_keyobj,   (void**)&o_keyobj },
    };
    // the serializer copies work only together; the country hook only when the countries are on
    eb_hookset hs; eb_hs_init(&hs, g_cfg.save_countries ? "save countries" : "save serializer copies");
    size_t nt_ = g_cfg.save_countries ? 4 : 3;
    for (size_t i = 0; i < nt_; i++)
        if (!eb_hs_add(&hs, b + t[i].rva, t[i].hook, t[i].orig)) break;
    if (!eb_hs_enable(&hs)) {
        eb_log("save: hooks failed - parallel countries and provinces off");
        g_cfg.save_countries = g_cfg.save_provinces = 0;
        return 0;
    }
    if (g_cfg.save_countries)
        eb_log("save: countries serialized in parallel (mode %d: %s)", g_cfg.save_countries,
               g_cfg.save_countries == 1 ? "on" : g_cfg.save_countries == 2 ? "on + verify" : "serial precompute + verify");
    if (g_cfg.save_provinces) {
        eb_sp_skip = b + RVA_PNEXT;
        eb_hookset ps; eb_hs_init(&ps, "save provinces");
        InitializeCriticalSection(&g_alock);
        int ok = eb_hs_add(&ps, b + RVA_STATIC_M, (void*)hk_static_m, (void**)&o_static_m);
        ok = ok && eb_hs_add(&ps, b + RVA_PHEAD, (void*)eb_sp_head_stub, &eb_sp_head_tramp);
        if (ok && g_cfg.save_provinces >= 2) ok = eb_hs_add(&ps, b + RVA_PNEXT, (void*)eb_sp_end_stub, &eb_sp_end_tramp);
        if (!ok || !eb_hs_enable(&ps)) {
            eb_log("save: province hooks failed - parallel provinces off");
            g_cfg.save_provinces = 0;
        } else
            eb_log("save: provinces serialized in parallel (EXPERIMENT, mode %d: %s)", g_cfg.save_provinces,
                   g_cfg.save_provinces == 1 ? "on" : g_cfg.save_provinces == 2 ? "on + verify" : "serial precompute + verify");
    }
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

// savezip.c: before a save (clear) and after it (what the province and country loops did)
void eb_savepar_mark(void) { g_status[0] = 0; g_pstatus[0] = 0; }
const char* eb_savepar_status(void) {
    static char both[340];
    const char* c = g_status[0] ? g_status : o_keyobj ? "countries not reached" : "parallel countries off";
    if (!g_cfg.save_provinces) return c;
    snprintf(both, sizeof both, "%s; %s", g_pstatus[0] ? g_pstatus : "provinces not reached", c);
    return both;
}

void eb_savepar_report(void) {
    if (o_keyobj)
        eb_log("save: parallel countries: %lld saves, %lld batches (%.1f ms avg, %.1f max, %.1f MB avg), %lld fallbacks, %lld countries appended | checked %lld, failed %lld | verify %lld, mismatches %lld",
               (long long)g_saves, (long long)g_batches, g_batches ? g_pre_ms / (double)g_batches : 0.0, g_pre_max,
               g_batches ? (double)g_bytes / 1048576.0 / (double)g_batches : 0.0, (long long)g_fallback, (long long)g_emitted,
               (long long)g_checked, (long long)g_check_bad, (long long)g_ver, (long long)g_ver_bad);
    if (g_cfg.save_provinces)
        eb_log("save: parallel provinces: %lld saves, %lld batches (%.1f ms avg, %.1f max, %.1f MB avg), %lld fallbacks, %lld provinces appended | checked %lld, failed %lld | verify %lld, mismatches %lld",
               (long long)g_psaves, (long long)g_pbatches, g_pbatches ? g_ppre_ms / (double)g_pbatches : 0.0, g_ppre_max,
               g_pbatches ? (double)g_pbytes / 1048576.0 / (double)g_pbatches : 0.0, (long long)g_pfallback, (long long)g_pemitted,
               (long long)g_pchecked, (long long)g_pcheck_bad, (long long)g_pver, (long long)g_pver_bad);
}
