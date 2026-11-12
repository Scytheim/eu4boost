// AI strategy: the objectives a country's missions add ([ai] fast_objectives, experiment 2026-11-12; [diag]
// ai_strategy=1 times it). In the user's SPAIN campaign (짭짭죽, about 2,200 provinces) the game stopped for
// 6.3-6.7 s on every 11 November since 1871 - "not responding".
//
// Where: the country's daily update calls CAIStrategy::Recalculate FUN_140182580(strategy, reason, flag)
// (ai_strategy.cpp; the country's tag is the text at strategy +0x1c) - for the player's country too. Its
// CalcObjectives FUN_140184360 calls FUN_140183420(strategy), which walks the country's mission slots (country
// +0x99f0): for every mission that is available and not completed it runs the mission's effects through their
// virtual +0x58 with the AI weight of the mission, and a trigger that is not met (FUN_140c53370, FUN_140c19880)
// adds an objective: FUN_14017e8e0(strategy, name, id, amount) -> FUN_14017f900(-, strategy +0x280, std::string*
// name, int id, int amount). The list at +0x280 is a doubly linked list {head, tail, int count +0x10, byte
// "being walked" +0x14} of nodes {entry, prev, next, byte removed} sorted by the entries' amount; an entry is
// 0x40 bytes {vtable, .., int id +0x10, std::string name +0x18, int amount +0x38}. FUN_14017f900:
//   1. walks the list from its head for an entry with the same id and name;
//   2. found: amount added, the entry's node taken out (or marked while the list is being walked);
//      not found: a new entry (operator new 0x40, FUN_140095280 copies the name, FUN_140194da0 makes the entry);
//   3. amount > 0: FUN_140191690(list, &entry) puts it in by amount, walking back from the tail; else deleted;
//   4. the name is freed.
// A mission with "has this building" conditions over all provinces adds one objective per building and province:
// SPAIN 1874, 43,361 adds, none of them in the list before - every add walks the whole list (940 million steps,
// 5.6 s) only to find nothing.
//
// What fast_objectives does: during one FUN_140183420 the adds keep a set of the (id, name) hashes that are in
// the list (made by one walk of the list at the first add; inside that function the list changes only through
// these adds, and the list's count is checked against ours at every add - any difference makes the set anew).
// A hash that is not in the set is not in the list: steps 2 (not found), 3 and 4 run with the game's own
// functions, without the walk. A hash that is in the set (the same objective again, a removed one, a collision)
// goes to the game's function as before. The list ends up the same, entry by entry.
//   2 = on + verify: every "not in the list" is checked by the walk the game would have made.
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "MinHook.h"

#define RVA_RECALC  0x182580u   // CAIStrategy::Recalculate(strategy, reason, char)
#define RVA_MISSION 0x183420u   // FUN_140183420(strategy): the objectives of the country's missions
#define RVA_ADD     0x17f900u   // FUN_14017f900(-, list, std::string* name, int id, int amount)
#define RVA_INSERT  0x191690u   // FUN_140191690(list, entry**): into the list by amount
#define RVA_ENTRY   0x194da0u   // FUN_140194da0(mem, std::string* name, int id, int amount) -> entry
#define RVA_STRCOPY 0x095280u   // FUN_140095280(std::string* dst, const std::string* src) -> dst
#define RVA_NEW     0x1a332d4u  // operator new
#define RVA_FREE    0x17394f0u  // operator delete(p, size)
#define RVA_STR_TIDY 0x095660u
static const uint8_t sig_recalc[16]  = { 0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57 };
static const uint8_t sig_mission[16] = { 0x48,0x8b,0xc4,0x48,0x89,0x48,0x08,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41 };
static const uint8_t sig_add[16]     = { 0x48,0x89,0x5c,0x24,0x10,0x4c,0x89,0x44,0x24,0x18,0x48,0x89,0x4c,0x24,0x08,0x55 };
static const uint8_t sig_insert[16]  = { 0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x48,0x89,0x7c,0x24,0x18,0x41 };
static const uint8_t sig_entry[16]   = { 0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x18,0x48,0x89,0x54,0x24,0x10,0x57 };
static const uint8_t sig_strcopy[16] = { 0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x83,0x7a,0x18,0x10,0x48,0x8b,0xd9,0x72,0x03 };
static const uint8_t sig_new[16]     = { 0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0xeb,0x0f,0x48,0x8b,0xcb,0xe8,0x95 };
static const uint8_t sig_free[5]     = { 0xe9,0x2b,0x45,0x02,0x00 };
static const uint8_t sig_tidy[16]    = { 0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x51,0x18,0x48,0x8b,0xd9,0x48,0x83,0xfa };

typedef void (*recalc_t)(uint8_t* strategy, void* reason, char flag);
typedef void (*mission_t)(uint8_t* strategy);
typedef void (*add_t)(void* self, uint8_t* list, uint64_t* name, int id, int amount);
typedef void (*insert_t)(uint8_t* list, uint8_t** entry);
typedef uint8_t* (*entry_t)(uint8_t* mem, uint64_t* name, int id, int amount);
typedef uint64_t* (*strcopy_t)(uint64_t* dst, const uint64_t* src);
typedef void* (*new_t)(size_t n);
typedef void (*free_t)(void* p, size_t n);
typedef void (*tidy_t)(uint64_t* s);
static recalc_t o_recalc; static mission_t o_mission; static add_t o_add;
static insert_t g_insert; static entry_t g_entry; static strcopy_t g_strcopy; static new_t g_new; static free_t g_free; static tidy_t g_tidy;
static int g_fast, g_diag;
static double g_ms;
static LONG64 g_calls, g_qpc, g_adds_all, g_long;
static LONG64 g_bursts, g_fast_adds, g_game_adds, g_scans, g_scanned, g_ver, g_ver_bad;

static LONGLONG qpc(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static const char* str_data(const uint64_t* s) { return s[3] > 15 ? (const char*)s[0] : (const char*)s; }

// ---- the set of (id, name) hashes of the list, for one FUN_140183420 on this thread ----
static __thread int tl_burst;
static __thread uint8_t* tl_list;
static __thread int tl_count;                  // the list's count as we left it
static __thread uint64_t* tl_set; static __thread size_t tl_cap, tl_n;
static uint64_t key_hash(int id, const char* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)p[i]) * 1099511628211ull;
    h ^= (uint64_t)(uint32_t)id * 0x9E3779B97F4A7C15ull;
    h ^= h >> 29;
    return h ? h : 1;
}
static int set_has(uint64_t h) {
    if (!tl_n) return 0;
    for (size_t i = (size_t)h & (tl_cap - 1); tl_set[i]; i = (i + 1) & (tl_cap - 1)) if (tl_set[i] == h) return 1;
    return 0;
}
static int set_put(uint64_t h) {               // 0 = no memory (the caller then lets the game do it)
    if ((tl_n + 1) * 2 > tl_cap) {
        size_t cap = tl_cap ? tl_cap * 2 : 1 << 12;
        uint64_t* t = (uint64_t*)calloc(cap, sizeof *t);
        if (!t) return 0;
        for (size_t i = 0; i < tl_cap; i++) if (tl_set[i]) { size_t k = (size_t)tl_set[i] & (cap - 1); while (t[k]) k = (k + 1) & (cap - 1); t[k] = tl_set[i]; }
        free(tl_set); tl_set = t; tl_cap = cap;
    }
    size_t k = (size_t)h & (tl_cap - 1);
    while (tl_set[k]) { if (tl_set[k] == h) return 1; k = (k + 1) & (tl_cap - 1); }
    tl_set[k] = h; tl_n++;
    return 1;
}
static void set_clear(void) { if (tl_n) memset(tl_set, 0, tl_cap * sizeof *tl_set); tl_n = 0; tl_list = NULL; }
static int set_from_list(uint8_t* list) {      // one walk: every entry's hash
    set_clear();
    int ok = 1; LONG64 n = 0;
    for (uint8_t* node = *(uint8_t**)list; node && ok; node = *(uint8_t**)(node + 0x10), n++) {
        uint8_t* e = *(uint8_t**)node;
        const uint64_t* nm = (const uint64_t*)(e + 0x18);
        ok = set_put(key_hash(*(int*)(e + 0x10), str_data(nm), (size_t)nm[2]));
    }
    InterlockedIncrement64(&g_scans); InterlockedAdd64(&g_scanned, n);
    if (!ok) { set_clear(); return 0; }
    tl_list = list; tl_count = *(int*)(list + 0x10);
    return 1;
}
static int in_list(uint8_t* list, const uint64_t* name, int id) {     // the game's own search (verify)
    for (uint8_t* node = *(uint8_t**)list; node; node = *(uint8_t**)(node + 0x10)) {
        uint8_t* e = *(uint8_t**)node;
        const uint64_t* nm = (const uint64_t*)(e + 0x18);
        if (*(int*)(e + 0x10) == id && nm[2] == name[2] && !memcmp(str_data(nm), str_data(name), (size_t)nm[2])) return 1;
    }
    return 0;
}

// ---- the diagnostic's counts for one Recalculate on this thread ----
#define NNAMES 256
typedef struct { char name[48]; LONG64 n, qpc; } nm_t;
static __thread int tl_in;
static __thread LONG64 tl_adds, tl_add_qpc, tl_steps;
static __thread int tl_len_max;
static __thread nm_t* tl_names;

static void add_one(void* self, uint8_t* list, uint64_t* name, int id, int amount) {
    if (!g_fast || !tl_burst || !amount || !list) { o_add(self, list, name, id, amount); return; }
    if (tl_list != list || tl_count != *(int*)(list + 0x10)) {
        if (!set_from_list(list)) { o_add(self, list, name, id, amount); return; }
    }
    uint64_t h = key_hash(id, str_data(name), (size_t)name[2]);
    int absent = !set_has(h);
    if (absent && g_fast == 2) {
        InterlockedIncrement64(&g_ver);
        if (in_list(list, name, id)) {
            absent = 0;
            if (InterlockedIncrement64(&g_ver_bad) <= 10) eb_log("aistrat: VERIFY MISMATCH an objective (id %d) the set did not have is in the list", id);
        }
    }
    if (!absent || !set_put(h)) {                  // maybe in the list (or no memory): the game's function, search and all
        o_add(self, list, name, id, amount);
        InterlockedIncrement64(&g_game_adds);
        tl_count = *(int*)(list + 0x10);
        return;
    }
    // not in the list: the game's "not found" steps
    uint8_t* e = (uint8_t*)g_new(0x40);
    uint64_t tmp[4];
    e = g_entry(e, g_strcopy(tmp, name), id, amount);
    if (*(int*)(e + 0x38) > 0) g_insert(list, &e);
    else { g_tidy((uint64_t*)(e + 0x18)); g_free(e, 0x40); }
    g_tidy(name);
    tl_count = *(int*)(list + 0x10);
    InterlockedIncrement64(&g_fast_adds);
}

static void hk_add(void* self, uint8_t* list, uint64_t* name, int id, int amount) {
    if (!g_diag || !tl_in) { add_one(self, list, name, id, amount); return; }
    int len = list ? *(int*)(list + 0x10) : 0;
    char nm[48]; size_t n = name ? (size_t)name[2] : 0; if (n > 47) n = 47;
    memcpy(nm, name ? str_data(name) : "", n); nm[n] = 0;
    LONGLONG t0 = qpc();
    add_one(self, list, name, id, amount);              // frees the name
    LONGLONG d = qpc() - t0;
    tl_adds++; tl_add_qpc += d; tl_steps += len;
    if (len > tl_len_max) tl_len_max = len;
    if (tl_names) {
        unsigned hh = 2166136261u; for (const char* p = nm; *p; p++) hh = (hh ^ (unsigned char)*p) * 16777619u;
        for (int k = 0; k < NNAMES; k++) {
            nm_t* x = &tl_names[(hh + (unsigned)k) & (NNAMES - 1)];
            if (!x->name[0]) { strcpy(x->name, nm[0] ? nm : "(no name)"); x->n = 1; x->qpc = d; break; }
            if (!strcmp(x->name, nm[0] ? nm : "(no name)")) { x->n++; x->qpc += d; break; }
        }
    }
}

static void hk_mission(uint8_t* strategy) {
    if (!g_fast || tl_burst) { o_mission(strategy); return; }
    tl_burst = 1; set_clear();
    InterlockedIncrement64(&g_bursts);
    o_mission(strategy);
    tl_burst = 0; set_clear();
}

static void hk_recalc(uint8_t* strategy, void* reason, char flag) {
    if (tl_in) { o_recalc(strategy, reason, flag); return; }
    if (!tl_names) tl_names = (nm_t*)calloc(NNAMES, sizeof(nm_t));
    else memset(tl_names, 0, NNAMES * sizeof(nm_t));
    tl_in = 1; tl_adds = tl_add_qpc = tl_steps = 0; tl_len_max = 0;
    LONGLONG t0 = qpc();
    o_recalc(strategy, reason, flag);
    LONGLONG d = qpc() - t0;
    tl_in = 0;
    InterlockedIncrement64(&g_calls); InterlockedAdd64(&g_qpc, d); InterlockedAdd64(&g_adds_all, tl_adds);
    if ((double)d * g_ms >= 50.0 && InterlockedIncrement64(&g_long) <= 60) {
        char tag[4]; memcpy(tag, strategy + 0x1c, 3); tag[3] = 0;
        char top[600]; int k = 0; top[0] = 0;
        if (tl_names)
            for (int r = 0; r < 6; r++) {                 // the six names with the most time
                nm_t* best = NULL;
                for (int i = 0; i < NNAMES; i++) if (tl_names[i].name[0] && tl_names[i].n > 0 && (!best || tl_names[i].qpc > best->qpc)) best = &tl_names[i];
                if (!best) break;
                k += snprintf(top + k, sizeof top - (size_t)k, "%s%s x%lld %.0f ms", k ? ", " : "", best->name, (long long)best->n, (double)best->qpc * g_ms);
                best->n = 0;
            }
        eb_log("aistrat: Recalculate of %s took %.0f ms (%s thread, flag %d): %lld objective adds %.0f ms, the list up to %d long, %lld list steps the game's way; by name: %s",
               tag, (double)d * g_ms, GetCurrentThreadId() == g_main_tid ? "main" : "worker", (int)flag, (long long)tl_adds,
               (double)tl_add_qpc * g_ms, tl_len_max, (long long)tl_steps, top[0] ? top : "-");
    }
}

int eb_aistrat_install(void) {
    if (!g_cfg.diag_ai_strategy && !g_cfg.ai_fast_objectives) return 0;
#ifdef EB_TEST
    return 0;
#else
    uint8_t* b = (uint8_t*)g_exe;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != 0x66FE6AB9u || memcmp(b + RVA_RECALC, sig_recalc, 16) || memcmp(b + RVA_ADD, sig_add, 16) ||
        memcmp(b + RVA_MISSION, sig_mission, 16) || memcmp(b + RVA_INSERT, sig_insert, 16) || memcmp(b + RVA_ENTRY, sig_entry, 16) ||
        memcmp(b + RVA_STRCOPY, sig_strcopy, 16) || memcmp(b + RVA_NEW, sig_new, 16) || memcmp(b + RVA_FREE, sig_free, 5) ||
        memcmp(b + RVA_STR_TIDY, sig_tidy, 16)) {
        eb_log("aistrat: signature mismatch - off"); g_cfg.ai_fast_objectives = 0; return 0;
    }
    g_insert = (insert_t)(b + RVA_INSERT); g_entry = (entry_t)(b + RVA_ENTRY); g_strcopy = (strcopy_t)(b + RVA_STRCOPY);
    g_new = (new_t)(b + RVA_NEW); g_free = (free_t)(b + RVA_FREE); g_tidy = (tidy_t)(b + RVA_STR_TIDY);
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) { g_cfg.ai_fast_objectives = 0; return 0; }
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_ms = 1000.0 / (double)f.QuadPart;
    eb_hookset hs; eb_hs_init(&hs, "ai strategy objectives");
    int ok = eb_hs_add(&hs, b + RVA_ADD, (void*)hk_add, (void**)&o_add);
    if (ok && g_cfg.ai_fast_objectives) ok = eb_hs_add(&hs, b + RVA_MISSION, (void*)hk_mission, (void**)&o_mission);
    if (ok && g_cfg.diag_ai_strategy) ok = eb_hs_add(&hs, b + RVA_RECALC, (void*)hk_recalc, (void**)&o_recalc);
    if (!ok || !eb_hs_enable(&hs)) { eb_log("aistrat: hooks failed - off"); g_cfg.ai_fast_objectives = 0; return 0; }
    g_fast = g_cfg.ai_fast_objectives; g_diag = g_cfg.diag_ai_strategy;
    if (g_fast) eb_log("aistrat: EXPERIMENT the objectives a country's missions add to its AI strategy skip the search of the list when they are new%s",
                       g_fast == 2 ? " - with VERIFY (the game's search checks each)" : "");
    if (g_diag) eb_log("aistrat: DIAGNOSTIC every AI strategy recalculation is timed, its objective adds counted");
    return 1;
#endif
}

void eb_aistrat_report(void) {
    if (g_fast && g_bursts)
        eb_log("aistrat: mission objectives: %lld rounds, %lld objectives added without the search, %lld by the game's function (maybe in the list); "
               "the list read %lld times (%lld entries) | verify %lld, mismatches %lld",
               (long long)g_bursts, (long long)g_fast_adds, (long long)g_game_adds, (long long)g_scans, (long long)g_scanned,
               (long long)g_ver, (long long)g_ver_bad);
    if (g_calls)
        eb_log("aistrat: %lld recalculations, %.0f ms in all, %lld objective adds; %lld took 50 ms or more",
               (long long)g_calls, (double)g_qpc * g_ms, (long long)g_adds_all, (long long)g_long);
}
