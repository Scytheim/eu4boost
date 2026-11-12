// The building windows ([gui] fast_build_list, experiment 2026-11-11): with 짭짭죽's 100+ building types the macro
// builder's building tab and the province view's building list lag badly (the user's SPAIN campaign; reports with
// 2-7 s stalls at eu4.exe+0x124d7e7 on four PCs).
//
// What the game does: both windows keep one entry per building type (0x30 bytes: building, cheapest cost at +8,
// state at +0xc, reason text - a std::string - at +0x10) and fill them with FUN_14124fc80(results, entries,
// province or NULL). For every building it walks ALL provinces of the player (country +0x1348, a list of province
// ids) and calls FUN_14124d6a0(province, entry, results), which asks FUN_14032acb0(country, building, province,
// &entry.reason, 0, 0, 0) "can it be built here?":
//   no, and the building was not found buildable yet -> state 2, and the reason text is copied to results[index];
//   otherwise (yes - or no, but an earlier province said yes) -> state 3 or 4, FUN_14032a420 gives the cost and
//   the entry keeps the cheapest.
// The walk does not stop at the first province where the building can be built, and a building that cannot be
// built anywhere goes on through the provinces of the subjects (+0x1a18).
// The reason text is the trouble. FUN_14032acb0 and the building's own check FUN_14032b600 do not start from an
// empty text: a province whose general checks pass and whose building trigger fails APPENDS its lines to what
// the provinces before it left. With a mod whose buildings have long triggers the text of one building grows by
// kilobytes per province - megabytes in all - and every failing province copies the whole text to the results
// (a strlen and a copy of megabytes, hundreds of times per building). Measured without any window, the game's
// own fill for SPAIN 1704 (122 buildings, 379 provinces): 1.1 s as the macro builder asks it, 0.64 s as the
// province view does, 37-44 MB of text; the entries keep their text, so the next fill appends again: 3.3 s, then
// 5.6 s. The same questions without a text take 4 ms in all.
// Callers: FUN_1411ffda0 (macro builder: the tab is opened, a building or an option is clicked) and
// FUN_141388720 (province view: again whenever the treasury, a technology level, the province or its
// buildings changed since the last frame - with the game running, every time the treasury changes).
//
// What this does: FUN_14124d6a0 is replaced by the same steps (from its disassembly: the two early outs, the
// player's country, the states, the cheapest cost), except for the text: while an entry's text is shorter than
// TEXT_LIMIT the game's function is asked with the text exactly as before, so the text starts with the same
// lines the game would have written; after that it is asked without a text (the function then stops at the
// first failing check and writes nothing), and not at all once the building is known to be buildable (the game
// ignores the answer then). States and costs are the game's; the text of a building that cannot be built is
// the beginning of the game's megabytes - more than a tooltip shows.
//   2 = on + verify: the game's own fill runs first on copies of the entries, then states and costs are
//       compared and each text must be the beginning of the game's (slow: it is the game's fill).
//   3 = off, but every fill of the game is timed (to see how long and how often the windows fill).
//
// TEST [test] autorun_script command "scan" (autorun.c): without any window, the question for every building
// and province with and without a text, then the game's fill and this one on entries made here, compared.
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "MinHook.h"

#define RVA_FILL        0x124fc80u   // FUN_14124fc80(results, entries, province or NULL)
#define RVA_ITEM        0x124d6a0u   // FUN_14124d6a0(province id, entry, results) -> bool
#define RVA_CAN_BUILD   0x32acb0u    // FUN_14032acb0(country, building, province, std::string* reason, 0, 0, 0) -> bool
#define RVA_BUILD_COST  0x32a420u    // FUN_14032a420(country, int* out, building, province) -> out
#define RVA_BUILDING_DB 0x242ba88u   // the building database (FUN_1401e5390): building* array at +0x10 .. +0x18
#define RVA_COUNTRIES   0x233d8d0u   // country array object: country* at (+0x118)[tag]
#define RVA_GS          0x233fe78u
#define RVA_STR_ASSIGN  0x095110u    // std::string::assign(const char*, size_t)
#define RVA_STR_TIDY    0x095660u    // std::string: free the buffer, make it empty
#define GS_PROVINCES    0x1ca8       // province array, 0x2e10 bytes each
#define C_PROVINCES     0x1348       // country: list of province ids { int id; ...; next at +0x10 }
#define B_INDEX         0x14         // building: its index (0 = none)
#define E_SIZE          0x30         // an entry: building, +8 cheapest cost, +0xc state, +0x10 std::string reason
#define TEXT_LIMIT      4096         // bytes of reason text an entry gathers before the questions go without a text

static const uint8_t sig_fill[16]   = { 0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x54,0x24,0x10,0x55,0x56,0x57,0x41,0x54,0x41 };
static const uint8_t sig_item[16]   = { 0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x56,0x48,0x83,0xec,0x40,0x48,0x8b,0x2a,0x45 };
static const uint8_t sig_can[16]    = { 0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x54,0x24,0x10,0x48,0x89,0x4c,0x24,0x08,0x55 };
static const uint8_t sig_cost[16]   = { 0x48,0x89,0x5c,0x24,0x18,0x56,0x57,0x41,0x56,0x48,0x83,0xec,0x20,0x49,0x8b,0xf1 };
static const uint8_t sig_assign[16] = { 0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x56,0x57,0x41,0x57,0x48,0x83 };
static const uint8_t sig_tidy[16]   = { 0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x51,0x18,0x48,0x8b,0xd9,0x48,0x83,0xfa };

typedef void (*fill_t)(uint8_t** results, uint8_t** entries, uint8_t* province);
typedef char (*item_t)(int province, uint8_t* entry, uint8_t** results);
typedef char (*can_t)(uint8_t* country, uint8_t* building, uint8_t* province, uint64_t* reason, char a, char b, char c);
typedef int* (*cost_t)(uint8_t* country, int* out, uint8_t* building, uint8_t* province);
typedef void* (*assign_t)(uint64_t* s, const char* p, size_t n);
typedef void (*tidy_t)(uint64_t* s);
static fill_t o_fill; static item_t o_item;
static can_t g_can; static cost_t g_cost; static assign_t g_assign; static tidy_t g_tidy;
static uint8_t** g_gsp; static uint8_t** g_carr;
static int g_ready;                      // the game functions are known (signatures match)
static int g_mode;                       // [gui] fast_build_list as installed
static volatile LONG g_game_way;         // the next fills are the game's own (verify, the scan)
static LONG64 g_fills, g_fill_qpc, g_fill_max_qpc, g_items, g_text_asks, g_plain_asks, g_no_asks;
static LONG64 g_ver_fills, g_ver_entries, g_ver_bad, g_ver_game_qpc, g_ver_ours_qpc, g_ver_game_text, g_ver_ours_text;
static double g_ms;
static int g_slow_logged;

static const char* str_data(const uint64_t* s) { return s[3] > 15 ? (const char*)s[0] : (const char*)s; }

// FUN_14124d6a0 with the text kept short (see the head of the file)
static char hk_item(int prov_id, uint8_t* entry, uint8_t** results) {
    if (g_game_way || (g_mode != 1 && g_mode != 2)) return o_item(prov_id, entry, results);
    uint8_t* bld = *(uint8_t**)entry;
    uint8_t* gs = *g_gsp;
    uint8_t* prov = *(uint8_t**)(gs + GS_PROVINCES) + (intptr_t)prov_id * 0x2e10;
    if (bld) {                                           // the province has this building already
        uint32_t idx = *(uint32_t*)(bld + B_INDEX);
        if (idx) {
            uint8_t* b0 = *(uint8_t**)(prov + 0x3b0);
            int n = (int)((*(uint8_t**)(prov + 0x3b8) - b0) >> 4);
            if ((int)idx < n && *(int*)(*(uint8_t**)(b0 + (size_t)idx * 16) + 0x14) != 0) return 0;
        }
    }
    if (*(int*)(prov + 0x3f8) > 0 && !(prov[0x958] & 0x10)) return 0;
    if (!bld) return o_item(prov_id, entry, results);    // (the game would fault here; never seen)
    InterlockedIncrement64(&g_items);
    uint32_t r15 = *(uint32_t*)(bld + B_INDEX);
    uint64_t tq = (gs[0x1e66] == 7 && gs[0x1e6f] != 0) ? *(uint64_t*)(gs + 0x1e68) : *(uint64_t*)(gs + 0x1e60);
    uint8_t* country = *(uint8_t**)(*(uint8_t**)(*g_carr + 0x118) + (intptr_t)(short)(tq >> 32) * 8);
    uint64_t* reason = (uint64_t*)(entry + 0x10);
    int found = (unsigned)(*(int*)(entry + 0xc) - 3) <= 1;      // state 3 or 4: buildable in an earlier province
    int text = reason[2] < TEXT_LIMIT;
    char ok;
    if (text) { ok = g_can(country, bld, prov, reason, 0, 0, 0); g_text_asks++; }
    else if (found) { ok = 1; g_no_asks++; }                    // the game ignores the answer here
    else { ok = g_can(country, bld, prov, NULL, 0, 0, 0); g_plain_asks++; }
    uint8_t* res = *results + (size_t)r15 * E_SIZE;
    if (!ok && !found) {
        *(int*)(entry + 0xc) = 2; *(int*)(res + 0xc) = 2;
        if (text) { const char* p = str_data(reason); g_assign((uint64_t*)(res + 0x10), p, strlen(p)); }
        return 0;
    }
    uint8_t* x = *(uint8_t**)(bld + 0x18);
    int st = (x && *(char*)(*(uint8_t**)(*(uint8_t**)(country + 0x2440) + 0x100) + (intptr_t)*(int*)(x + 0x14)) != 0) ? 4 : 3;
    *(int*)(entry + 0xc) = st; *(int*)(res + 0xc) = st;
    int c = 0;
    g_cost(country, &c, bld, prov);
    if (!(*(int*)(entry + 8) < c)) *(int*)(entry + 8) = c;
    res = *results + (size_t)r15 * E_SIZE;
    if (!(*(int*)(res + 8) < c)) *(int*)(res + 8) = c;
    return 1;
}

// a copy of a vector of entries (the strings through the game's assign), and its end
static uint8_t* copy_entries(uint8_t** v, size_t* n) {
    *n = (size_t)(v[1] - v[0]) / E_SIZE;
    uint8_t* c = (uint8_t*)calloc(*n + 1, E_SIZE);
    if (!c) return NULL;
    for (size_t i = 0; i < *n; i++) {
        uint8_t* s = v[0] + i * E_SIZE; uint8_t* d = c + i * E_SIZE;
        memcpy(d, s, 0x10);
        uint64_t* ss = (uint64_t*)(s + 0x10); uint64_t* ds = (uint64_t*)(d + 0x10);
        ds[3] = 15;
        if (ss[2]) g_assign(ds, str_data(ss), (size_t)ss[2]);
    }
    return c;
}
static void free_entries(uint8_t* c, size_t n) { if (!c) return; for (size_t i = 0; i < n; i++) g_tidy((uint64_t*)(c + i * E_SIZE + 0x10)); free(c); }
// ours against the game's: the same states and costs; our text is the game's or its beginning
static int compare_entries(const uint8_t* ours, const uint8_t* game, size_t n, const char* what, LONG64* ours_text, LONG64* game_text) {
    int bad = 0;
    for (size_t i = 0; i < n; i++) {
        const uint8_t* a = ours + i * E_SIZE; const uint8_t* g = game + i * E_SIZE;
        const uint64_t* as = (const uint64_t*)(a + 0x10); const uint64_t* gs = (const uint64_t*)(g + 0x10);
        *ours_text += (LONG64)as[2]; *game_text += (LONG64)gs[2];
        int same = *(uint8_t* const*)a == *(uint8_t* const*)g && *(const int*)(a + 8) == *(const int*)(g + 8) && *(const int*)(a + 0xc) == *(const int*)(g + 0xc);
        int pre = as[2] <= gs[2] && !memcmp(str_data(as), str_data(gs), (size_t)as[2]);
        if (same && pre) continue;
        bad++;
        if (InterlockedIncrement64(&g_ver_bad) <= 10)
            eb_log("buildview: VERIFY MISMATCH %s %zu: cost %d / the game %d, state %d / %d, text %llu bytes / %llu%s", what, i,
                   *(const int*)(a + 8), *(const int*)(g + 8), *(const int*)(a + 0xc), *(const int*)(g + 0xc),
                   (unsigned long long)as[2], (unsigned long long)gs[2], pre ? "" : " (not its beginning)");
    }
    return bad;
}

static void hk_fill(uint8_t** results, uint8_t** entries, uint8_t* province) {
    LARGE_INTEGER t0, t1, t2;
    QueryPerformanceCounter(&t0);
    if (g_mode == 2 && !g_game_way) {
        size_t nr = 0, ne = 0;
        int alias = results == entries || results[0] == entries[0];
        uint8_t* rc = copy_entries(results, &nr);
        uint8_t* ec = alias ? rc : copy_entries(entries, &ne);
        if (alias) ne = nr;
        if (rc && ec) {
            uint8_t* rv[3] = { rc, rc + nr * E_SIZE, rc + nr * E_SIZE };
            uint8_t* ev[3] = { ec, ec + ne * E_SIZE, ec + ne * E_SIZE };
            InterlockedIncrement(&g_game_way);
            o_fill(rv, alias ? rv : ev, province);               // the game's own way, on the copies
            InterlockedDecrement(&g_game_way);
            QueryPerformanceCounter(&t1);
            o_fill(results, entries, province);                  // ours
            QueryPerformanceCounter(&t2);
            compare_entries(results[0], rc, nr, "result", &g_ver_ours_text, &g_ver_game_text);
            if (!alias) compare_entries(entries[0], ec, ne, "entry", &g_ver_ours_text, &g_ver_game_text);
            g_ver_fills++; g_ver_entries += (LONG64)(alias ? nr : nr + ne);
            g_ver_game_qpc += t1.QuadPart - t0.QuadPart; g_ver_ours_qpc += t2.QuadPart - t1.QuadPart;
            t0 = t1;
        } else o_fill(results, entries, province);
        if (!alias) free_entries(ec, ne);
        free_entries(rc, nr);
    } else o_fill(results, entries, province);
    QueryPerformanceCounter(&t1);
    LONG64 d = t1.QuadPart - t0.QuadPart;
    g_fills++; g_fill_qpc += d;
    if (d > g_fill_max_qpc) g_fill_max_qpc = d;
    if ((double)d * g_ms >= 100.0 && g_slow_logged < 20) {
        g_slow_logged++;
        eb_log("buildview: a fill of the building list took %.0f ms (%s, %s)", (double)d * g_ms, province ? "province view" : "macro builder",
               (g_mode == 1 || g_mode == 2) && !g_game_way ? "the short-text way" : "the game's way");
    }
}

static int known(void) {
    if (g_ready) return 1;
    uint8_t* b = (uint8_t*)g_exe;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != 0x66FE6AB9u || memcmp(b + RVA_FILL, sig_fill, 16) || memcmp(b + RVA_ITEM, sig_item, 16) ||
        memcmp(b + RVA_CAN_BUILD, sig_can, 16) || memcmp(b + RVA_BUILD_COST, sig_cost, 16) || memcmp(b + RVA_STR_ASSIGN, sig_assign, 16) ||
        memcmp(b + RVA_STR_TIDY, sig_tidy, 16)) return 0;
    g_can = (can_t)(b + RVA_CAN_BUILD); g_cost = (cost_t)(b + RVA_BUILD_COST);
    g_assign = (assign_t)(b + RVA_STR_ASSIGN); g_tidy = (tidy_t)(b + RVA_STR_TIDY);
    g_gsp = (uint8_t**)(b + RVA_GS); g_carr = (uint8_t**)(b + RVA_COUNTRIES);
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_ms = 1000.0 / (double)f.QuadPart;
    g_ready = 1;
    return 1;
}

int eb_buildview_install(void) {
    if (!g_cfg.gui_fast_build_list) return 0;
#ifdef EB_TEST
    return 0;
#else
    uint8_t* b = (uint8_t*)g_exe;
    if (!known()) { eb_log("buildview: signature mismatch - off"); g_cfg.gui_fast_build_list = 0; return 0; }
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) { eb_log("buildview: MinHook init failed"); g_cfg.gui_fast_build_list = 0; return 0; }
    eb_hookset hs; eb_hs_init(&hs, "building list");
    if (!eb_hs_add(&hs, b + RVA_FILL, (void*)hk_fill, (void**)&o_fill) || !eb_hs_add(&hs, b + RVA_ITEM, (void*)hk_item, (void**)&o_item) ||
        !eb_hs_enable(&hs)) { eb_log("buildview: hooks failed - off"); g_cfg.gui_fast_build_list = 0; return 0; }
    g_mode = g_cfg.gui_fast_build_list;
    if (g_mode == 3) eb_log("buildview: DIAGNOSTIC the fills of the building lists are timed (nothing is changed)");
    else eb_log("buildview: EXPERIMENT the building lists (macro builder, province view) are filled without megabytes of reason text%s",
                g_mode == 2 ? " - with VERIFY against the game's own fill (slow)" : "");
    return 1;
#endif
}

void eb_buildview_report(void) {
    if (!g_fills) return;
    eb_log("buildview: %lld fills of the building lists, %.0f ms in all (%.1f ms each, the longest %.0f ms); %lld province questions: "
           "%lld with the text, %lld without, %lld not needed",
           (long long)g_fills, (double)g_fill_qpc * g_ms, (double)g_fill_qpc * g_ms / (double)g_fills, (double)g_fill_max_qpc * g_ms,
           (long long)g_items, (long long)g_text_asks, (long long)g_plain_asks, (long long)g_no_asks);
    if (g_ver_fills)
        eb_log("buildview: verify: %lld fills, %lld entries compared, mismatches %lld | the game's way %.0f ms (%.1f MB of text), ours %.0f ms (%.2f MB)",
               (long long)g_ver_fills, (long long)g_ver_entries, (long long)g_ver_bad, (double)g_ver_game_qpc * g_ms,
               (double)g_ver_game_text / 1048576.0, (double)g_ver_ours_qpc * g_ms, (double)g_ver_ours_text / 1048576.0);
}

// ---- TEST: without a window ----
#ifndef EB_TEST
static int make_lists(uint8_t** bb, uint8_t** be, uint8_t** res, uint8_t** ent) {    // as FUN_14124ffb0 makes them; returns the entries
    int ndb = (int)(be - bb), ne = 0;
    *res = (uint8_t*)calloc((size_t)ndb + 1, E_SIZE); *ent = (uint8_t*)calloc((size_t)ndb + 1, E_SIZE);
    if (!*res || !*ent) return -1;
    for (int i = 0; i < ndb; i++) { *(int*)(*res + (size_t)i * E_SIZE + 8) = 10000000; *(uint64_t*)(*res + (size_t)i * E_SIZE + 0x28) = 15; }
    for (uint8_t** q = bb; q < be; q++) {
        if (!*q || !*(int*)(*q + B_INDEX) || (*q)[0x68]) continue;
        int idx = *(int*)(*q + B_INDEX);
        if (idx >= ndb) continue;
        *(uint8_t**)(*res + (size_t)idx * E_SIZE) = *q;
        uint8_t* e = *ent + (size_t)ne * E_SIZE; ne++;
        *(uint8_t**)e = *q; *(int*)(e + 8) = 10000000; *(uint64_t*)(e + 0x28) = 15;
    }
    return ne;
}
#endif

void eb_buildview_scan(void) {
#ifndef EB_TEST
    uint8_t* b = (uint8_t*)g_exe;
    if (!known()) { eb_log("buildview: TEST scan: signature mismatch"); return; }
    uint8_t* gs = *g_gsp;
    uint8_t* db = *(uint8_t**)(b + RVA_BUILDING_DB);
    if (!gs || !db || !*g_carr) { eb_log("buildview: TEST scan: no game"); return; }
    int tag = (int)(short)(*(uint64_t*)(gs + 0x1e60) >> 32);
    uint8_t* country = *(uint8_t**)(*(uint8_t**)(*g_carr + 0x118) + (intptr_t)tag * 8);
    uint8_t* provs = *(uint8_t**)(gs + GS_PROVINCES);
    uint8_t** bb = *(uint8_t***)(db + 0x10); uint8_t** be = *(uint8_t***)(db + 0x18);
    int np = 0, cap = 4096; int* ids = (int*)malloc(sizeof(int) * (size_t)cap);
    for (int* n = *(int**)(country + C_PROVINCES); n && ids && np < cap; n = *(int**)((uint8_t*)n + 0x10)) ids[np++] = *n;
    int nb = 0; for (uint8_t** q = bb; q < be; q++) if (*q && *(int*)(*q + B_INDEX)) nb++;
    size_t pairs = (size_t)nb * (size_t)np;
    uint8_t* r1 = (uint8_t*)calloc(pairs + 1, 1); uint8_t* r2 = (uint8_t*)calloc(pairs + 1, 1);
    LARGE_INTEGER t0, t1, t2;
    if (!ids || !r1 || !r2 || !np || !nb) { eb_log("buildview: TEST scan: %d buildings, %d provinces - nothing to do", nb, np); free(ids); free(r1); free(r2); return; }
    long long yes = 0, textlen = 0; size_t k = 0;
    QueryPerformanceCounter(&t0);                           // every question with a fresh text
    for (uint8_t** q = bb; q < be; q++) {
        if (!*q || !*(int*)(*q + B_INDEX)) continue;
        for (int i = 0; i < np; i++) {
            uint64_t s[4] = { 0, 0, 0, 15 };
            r1[k] = (uint8_t)(g_can(country, *q, provs + (size_t)ids[i] * 0x2e10, s, 0, 0, 0) != 0);
            yes += r1[k++]; textlen += (long long)s[2];
            g_tidy(s);
        }
    }
    QueryPerformanceCounter(&t1);                           // and without one
    k = 0;
    for (uint8_t** q = bb; q < be; q++) {
        if (!*q || !*(int*)(*q + B_INDEX)) continue;
        for (int i = 0; i < np; i++) r2[k++] = (uint8_t)(g_can(country, *q, provs + (size_t)ids[i] * 0x2e10, NULL, 0, 0, 0) != 0);
    }
    QueryPerformanceCounter(&t2);
    long long diff = 0; for (k = 0; k < pairs; k++) if (r1[k] != r2[k]) diff++;
    eb_log("buildview: TEST scan: %d buildings x %d provinces of the player = %zu questions (%lld can be built): with a fresh reason text each "
           "%.1f ms (%.2f us each, %lld bytes of text), without a text %.1f ms (%.2f us each), answers that differ %lld",
           nb, np, pairs, yes, (double)(t1.QuadPart - t0.QuadPart) * g_ms, (double)(t1.QuadPart - t0.QuadPart) * g_ms * 1000.0 / (double)pairs, textlen,
           (double)(t2.QuadPart - t1.QuadPart) * g_ms, (double)(t2.QuadPart - t1.QuadPart) * g_ms * 1000.0 / (double)pairs, diff);
    int prov0 = ids[0];
    free(ids); free(r1); free(r2);
    // the game's fill and the short-text one on lists made here: the macro builder's way (no province, entries apart
    // from the results) and the province view's (a province, one list); three fills in a row, as a window that
    // stays open fills again and again
    fill_t fill = o_fill ? (fill_t)(b + RVA_FILL) : (fill_t)(b + RVA_FILL);     // through the hooks when they are installed
    int ndb = (int)(be - bb);
    for (int mode = 0; mode < 2; mode++) {
        uint8_t* prov = mode ? provs + (size_t)prov0 * 0x2e10 : NULL;
        uint8_t* keep_res = NULL; int keep_ne = 0; uint8_t* keep_ent = NULL;
        for (int way = 0; way < 2; way++) {                  // 0 = the game's, 1 = ours (only when installed as 1 or 2)
            if (way && !(o_item && (g_mode == 1 || g_mode == 2))) break;
            uint8_t *res, *ent; int ne = make_lists(bb, be, &res, &ent);
            if (ne < 0) { free(res); free(ent); break; }
            uint8_t* rv[3] = { res, res + (size_t)ndb * E_SIZE, res + (size_t)ndb * E_SIZE };
            uint8_t* ev[3] = { ent, ent + (size_t)ne * E_SIZE, ent + (size_t)ne * E_SIZE };
            double tms[3];
            int save_mode = g_mode;
            if (way && g_mode == 2) g_mode = 1;              // the scan compares by itself
            if (!way) InterlockedIncrement(&g_game_way);
            for (int rep = 0; rep < 3; rep++) {
                LARGE_INTEGER a, z; QueryPerformanceCounter(&a);
                if (mode) fill(rv, rv, prov); else fill(rv, ev, prov);
                QueryPerformanceCounter(&z);
                tms[rep] = (double)(z.QuadPart - a.QuadPart) * g_ms;
            }
            if (!way) InterlockedDecrement(&g_game_way);
            g_mode = save_mode;
            long long tr = 0, te = 0, longest = 0; int st[5] = { 0 };
            for (int i = 0; i < ndb; i++) {
                uint64_t* s = (uint64_t*)(res + (size_t)i * E_SIZE + 0x10); tr += (long long)s[2]; if ((long long)s[2] > longest) longest = (long long)s[2];
                int v = *(int*)(res + (size_t)i * E_SIZE + 0xc); if (*(uint8_t**)(res + (size_t)i * E_SIZE) && v >= 0 && v < 5) st[v]++;
            }
            for (int i = 0; i < ne; i++) { uint64_t* s = (uint64_t*)(ent + (size_t)i * E_SIZE + 0x10); te += (long long)s[2]; if ((long long)s[2] > longest) longest = (long long)s[2]; }
            eb_log("buildview: TEST scan: %s fill %s, %d entries: %.1f / %.1f / %.1f ms (three in a row); states 0:%d 1:%d 2:%d 3:%d 4:%d; "
                   "reason text afterwards %lld bytes in the results, %lld in the entries (the longest %lld)",
                   way ? "the short-text" : "the game's", mode ? "with a province (province view)" : "without a province (macro builder)",
                   mode ? ndb : ne, tms[0], tms[1], tms[2], st[0], st[1], st[2], st[3], st[4], tr, te, longest);
            if (!way) { keep_res = res; keep_ent = ent; keep_ne = ne; }
            else {
                LONG64 ot = 0, gt = 0; LONG64 bad0 = g_ver_bad;
                int bad = compare_entries(res, keep_res, (size_t)ndb, "scan result", &ot, &gt);
                if (!mode) bad += compare_entries(ent, keep_ent, (size_t)ne, "scan entry", &ot, &gt);
                (void)bad0;
                eb_log("buildview: TEST scan: compared with the game's fill: %d of %d entries differ in state, cost or the beginning of the text",
                       bad, mode ? ndb : ndb + ne);
                free_entries(res, (size_t)ndb); free_entries(ent, (size_t)ne);
            }
        }
        free_entries(keep_res, (size_t)ndb); free_entries(keep_ent, (size_t)keep_ne);
    }
#endif
}
