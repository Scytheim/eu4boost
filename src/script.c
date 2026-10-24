// Script variables ([script] fast_variables).
//
// Reading a variable (check_variable, multiply_variable with which = ..., change_variable ...) goes
// through FUN_140d7ce90(int* out, scope, std::string* name) (variables.cpp). It builds the list of stores
// to search: the local variable stores of the scope chain (FUN_140d7ccc0) when the thread's local
// variable depth (tbb enumerable_thread_specific at 0x141fd5fa8, read with FUN_1400df320) is above 0,
// then the scope's own store - a country's at +0x330 (country table DAT_14233d8d0 +0x118, the tag in the
// scope at +0x2c, or +0x44 when the byte at +0x4b is set, its top byte 0 = no country), a province's at
// +0x238 (province array DAT_14233fe78 +0x1ca8, 0x2e10 bytes each, the id at scope +0x28 when >= 1).
// Each store keeps an std::unordered_map<std::string, value> at +0x10 (end node at +0x18), searched
// with the 32-bit FNV-1 hash of the name through FUN_1401fd1a0; the value is the int at node +0x30
// (fixed point), 0 when no store has the name. Even with no local variables the list is built in two
// std::vectors - two allocations and two frees for every read.
// MEIOU and Taxes (report 19: stalls of 10 s on monthly scripts) reads variables millions of times:
// profile 2026-10-24 (1357, speed 5): this function was 13.6% of the daily tick, its vectors ~6%, and
// the allocator's work for them more.
// With depth 0 the replacement looks in the scope's own store directly - the same store, the same hash,
// the same map search, the same value, no allocation. With depth above 0 the game's function runs.
// Mode 2 (verify) also runs the game's function on every read and compares the two values.
#include "common.h"
#include <string.h>
#include "MinHook.h"

#define RVA_READVAR   0xd7ce90u    // FUN_140d7ce90(int* out, scope, std::string* name) -> out
#define RVA_ETS_LOCAL 0x0df320u    // FUN_1400df320(ets, bool* exists) -> T* (tbb enumerable_thread_specific::local)
#define RVA_DEPTH_ETS 0x1fd5fa8u   // the ets of the local variable depth (int)
#define RVA_MAP_FIND  0x1fd1a0u    // FUN_1401fd1a0(map, out pair, std::string* key, hash) (unordered_map<string,...>::find)
#define RVA_COUNTRIES 0x233d8d0u   // country table holder: +0x118 = country pointers by tag index
#define RVA_PROVINCES 0x233fe78u   // province holder: +0x1ca8 = provinces, 0x2e10 bytes each
static const uint8_t sig_readvar[24] = { 0x48,0x89,0x5c,0x24,0x08,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8b,0xec,0x48,0x81,0xec,0x80,0x00 };
static const uint8_t sig_ets_local[24] = { 0x40,0x53,0x55,0x57,0x41,0x54,0x41,0x56,0x48,0x83,0xec,0x30,0x4c,0x8b,0xf1,0x48,0x8b,0xda,0x48,0x8d,0x4c,0x24,0x60,0xff };
static const uint8_t sig_map_find[16] = { 0x48,0x89,0x5c,0x24,0x18,0x56,0x57,0x41,0x56,0x48,0x83,0xec,0x20,0x4c,0x8b,0x71 };

typedef int32_t* (*readvar_t)(int32_t* out, uint8_t* scope, const void* name);
typedef int* (*ets_local_t)(void* ets, uint8_t* exists);
typedef uint64_t* (*map_find_t)(uint8_t* map, uint64_t* out, const void* key, uint64_t hash);
static readvar_t o_readvar;
static ets_local_t ets_local;
static map_find_t map_find;
static void* g_depth_ets;
static uint8_t** g_countries;        // where the game keeps its country table holder
static uint8_t** g_provinces;        // ... and its province holder
static int g_mode;
static volatile LONG64 g_reads, g_local, g_bad;

static const char* str_c(const void* s) { return ((const uint64_t*)s)[3] > 15 ? *(const char* const*)s : (const char*)s; }

// the scope's own variable store, NULL if it has none
static uint8_t* own_store(const uint8_t* scope) {
    int prov = *(const int*)(scope + 0x28);
    if (prov >= 1) return *(uint8_t**)(*g_provinces + 0x1ca8) + 0x238 + (int64_t)prov * 0x2e10;
    uint64_t tag;
    memcpy(&tag, scope + (scope[0x4b] ? 0x44 : 0x2c), 8);
    if (!(uint8_t)(tag >> 56)) return NULL;
    return *(uint8_t**)(*(uint8_t**)(*g_countries + 0x118) + (int64_t)(int16_t)(tag >> 32) * 8) + 0x330;
}

static int32_t fast_read(uint8_t* scope, const void* name) {
    uint8_t* store = own_store(scope);
    if (!store) return 0;
    uint32_t h = 0x811c9dc5u;
    for (const char* p = str_c(name); *p; p++) h = h * 0x1000193u ^ (uint32_t)(int32_t)*p;
    uint64_t res[2];
    map_find(store + 0x10, res, name, h);
    uint8_t* node = (uint8_t*)res[1];
    if (!node) node = *(uint8_t**)(store + 0x18);
    return node != *(uint8_t**)(store + 0x18) ? *(int32_t*)(node + 0x30) : 0;
}

static int32_t* hk_readvar(int32_t* out, uint8_t* scope, const void* name) {
    uint8_t exists = 0;
    int* depth = ets_local(g_depth_ets, &exists);
    if (*depth > 0) {
        if (g_mode == 2) InterlockedIncrement64(&g_local);
        return o_readvar(out, scope, name);
    }
    int32_t v = fast_read(scope, name);
    if (g_mode == 2) {
        int32_t w = 0;
        o_readvar(&w, scope, name);
        InterlockedIncrement64(&g_reads);
        if (w != v && InterlockedIncrement64(&g_bad) <= 10)
            eb_log("script: VERIFY mismatch reading variable '%s': %d, the game %d", str_c(name), v, w);
        v = w;
    }
    *out = v;
    return out;
}

int eb_script_install(void) {
    g_mode = g_cfg.script_fast_vars;
    if (!g_mode) return 0;
#ifdef EB_TEST
    return 0;
#else
    uint8_t* b = (uint8_t*)g_exe;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != 0x66FE6AB9u) { eb_log("script: unknown eu4.exe - fast variables off"); return 0; }
    if (memcmp(b + RVA_READVAR, sig_readvar, sizeof sig_readvar) || memcmp(b + RVA_ETS_LOCAL, sig_ets_local, sizeof sig_ets_local) ||
        memcmp(b + RVA_MAP_FIND, sig_map_find, sizeof sig_map_find)) {
        eb_log("script: signature mismatch - fast variables off"); return 0;
    }
    ets_local = (ets_local_t)(b + RVA_ETS_LOCAL);
    map_find = (map_find_t)(b + RVA_MAP_FIND);
    g_depth_ets = b + RVA_DEPTH_ETS;
    g_countries = (uint8_t**)(b + RVA_COUNTRIES);
    g_provinces = (uint8_t**)(b + RVA_PROVINCES);
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) { eb_log("script: MinHook init failed"); return 0; }
    if (MH_CreateHook(b + RVA_READVAR, (void*)hk_readvar, (void**)&o_readvar) != MH_OK || MH_EnableHook(b + RVA_READVAR) != MH_OK) {
        eb_log("script: hook failed - fast variables off"); return 0;
    }
    eb_log("script: variables are read without building store lists%s", g_mode == 2 ? " (VERIFY: the game's read runs too and is compared)" : "");
    return 1;
#endif
}

void eb_script_report(void) {
    if (g_mode == 2 && o_readvar)
        eb_log("script: verify: %lld variable reads compared, %lld mismatches, %lld with local variables (the game's read)",
               (long long)g_reads, (long long)g_bad, (long long)g_local);
}
