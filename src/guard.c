// Crash guards for two game bugs seen in play (crash dumps of 2026-10-18). Both act only where the
// game would otherwise crash.
//
// tooltip ([guard] tooltip): the modifier breakdown FUN_1408e8ca0(container, std::string* out,
// descriptor) walks the sources a container was summed from - the pointer vector at +0x38 that the
// add FUN_1408ea420 appends to - and recurses into them (for some descriptors it first calls the
// source's virtual +0x60). The vector holds raw pointers, so a source freed before its owner's next
// recalculation stays in it. Crash 20:08: the government power bar tooltip, drawn in a mid-tick frame
// of the province monthly loop (FUN_14075d690), went country modifier -> child sum -> a freed source
// whose memory now held map vertex floats, and read its "data pointer".
// Every container is checked before the original reads it: header readable, vtable inside .rdata,
// sane counts, data and source vector readable. Invalid sources are hidden by giving the original a
// filtered copy of the source vector for the duration of the call (it only reads the vector); an
// invalid container itself gives an empty text and total, as a container without modifiers does.
// Cost: FUN_1408e8ca0 is not only a tooltip function - the province monthly update
// (FUN_1409f35c0 -> FUN_140a2c5e0, "ProvinceTaxText") runs it for every province on the owner's whole
// modifier (300+ sources). The first version checked readability with VirtualQuery and took 66% of
// the main thread at speed 5 (profile 2026-10-19: 18 -> 7 days/s). Now the daily tick FUN_140758430
// runs it unchecked (the crash came from a GUI frame; the mid-tick frames FUN_140769ea0 inside the
// tick are checked again), and readability is tested by reading one aligned qword per page through
// eb_probe8, whose access violation a vectored handler turns into "not readable" (no system call).
//
// EXPERIMENT [modifiers] prov_text_cache=1 (2 = + verify, 3 = A/B, default off): the same function
// makes the text the province monthly update stores - FUN_1409f35c0 -> FUN_140a2c5e0 picks the country
// with the largest modifier 0x98 among the province's list at +0x490 and asks FUN_1408e8ca0 for the
// condensed text of that country's modifier 0x98 (descriptor: no total, no entry list - the text is
// the only output). In the late game this is 6.5% of the main thread (profile 2026-10-22), and most
// provinces ask about the same few countries. With the experiment a call from that one call site
// (return address 0x140a2c7ea) is answered from a copy made by the first such call for the same
// container on the same day (the daily tick hook counts days); the copy goes into the caller's
// std::string through the game's own assign (FUN_140095110). Verify mode builds the text again on
// every hit and compares; A/B alternates calls between the original and the copy and times both.
//
// relation ([guard] relation): when a country is annexed, FUN_140a61900 (relation.cpp) looks up the
// active relation to remove and calls FUN_140a5e130 with NULL when it is missing, right after
// FUN_1404e1570 logged "!pRelation CDiplomacy::RemActive"; FUN_140a5e130 then dereferences it (crash
// 21:34, a peace deal annexing a country in a personal union). The other call site already tests the
// relation for NULL after this call, so a NULL relation is skipped.
//
// great_project ([guard] great_project): the has_construction trigger keeps the great project it names
// as a raw address in the great project array. Its read FUN_140cc6e40 looks the name up with
// FUN_1407db3a0 (index * 0x1d38 + array data) and stores the element's address at +0x78. The array
// (manager DAT_14242bc90 +0x40: data +0x48, capacity +0x50, count +0x54) is filled while the great
// project files are read: FUN_1407db120 adds an element - doubling the capacity through FUN_1407db530,
// which moves the elements into a new buffer and frees the old one - and then reads it, triggers and
// all. A trigger inside a great project that names an earlier project, or itself, keeps an address in a
// buffer that a later doubling frees. Anbennar does this (its tunnels: NOT = { 6830 = { ...
// has_construction = dwarovar_valley_tunnel } } inside dwarovar_valley_tunnel). The evaluation
// FUN_140cc7550 reads the type byte at +0x1d36 of that address: when the buffer was given back to the
// system it crashes (report 18: Anbennar with four monument submods, AI building evaluation, the same day
// every time); when it is still mapped it reads stale memory, and the address never equals the
// construction's (+0x80), so the trigger was always false. Vanilla's great projects name no other project.
// The guard records every has_construction trigger that holds a great project address and moves those
// addresses along when the array moves (only a trigger that still has its vtable and the recorded
// address). Both functions run while the game data is read, not during play.
//
// ironman_name ([guard] ironman_name, report 36): EU4dll keeps non-Latin text in its escapes (0x10-0x13
// followed by two bytes of the UTF-16 code unit, escape_tool.cpp), short-cuts the game's file name
// sanitizer FUN_141705180 and gives ordinary saves a UTF-8 file name (escapedStrToUtf8 at the call sites
// of "ファイル名をUTF-8に変換して保存できるようにする"). The ironman save builds its paths with
// FUN_1405ca070(std::string* out, std::string* name, dir, p4) = "<dir>/<name>.eu4" (also the .tmp, the
// backups), which EU4dll does not convert: PhysFS takes a path as UTF-8, the escape bytes are no valid
// UTF-8, Windows refuses the name, and DoLocalIronmanSave (FUN_1405c7ac0) throws "Could not open file:
// save games/<name>.tmp, error: filename is illegal or insecure" - the game crashes at its first ironman
// save of a game named in Korean. A name with escape bytes (no file can have one) is decoded the way
// escapedStrToUtf8 decodes it and passed on in UTF-8; other names (ASCII, already UTF-8) unchanged.
//
// loc_duplicates ([guard] loc_duplicates, reports 40, 41): while the localisation is read, FUN_1416fd7a0
// runs FUN_1416fb1c0 on each table; it walks the sorted keys (16-byte entries at +0x20..+0x28) and for
// every run of equal keys takes three arrays of (8k + 8) bytes from the stack (alloca) before it logs the
// duplicate ("Duplicate localization found", FUN_1414b36c0) - inside its loop, so none of it is given back
// before it returns. With tens of thousands of duplicate keys (mods whose localisation repeats another's
// under other file names, e.g. 짭짭죽 13.0.4 with 맵한글화 and 통신효율) the main thread's 4 MB stack runs
// out: the game crashes at start-up with EXCEPTION_STACK_OVERFLOW in the OutputDebugStringA of the next
// log line - without this plugin too (checked: a test mod repeating 짭짭죽's localisation crashes vanilla
// the same way). The stack the call will take is counted from the keys first; only when it does not fit
// in what is left of the thread's stack, the same call runs on a fiber of this thread with a stack that
// fits (same thread, same code; the check only reads the table and logs, so the result is the same; the
// fiber is deleted afterwards).
//
// diplomacy_reset ([guard] diplomacy_reset, reports 51, 77, 78): CDiplomacy keeps the active relations
// (alliances, marriages, subjects, ...) in one list per country at gamestate +0x1bb8 (AddActive
// FUN_1404e12f0, RemActive FUN_1404e1570): lists of 0x18 bytes from +0x10, nodes { relation, prev, next,
// removal pending }. Loading a game from inside a game destroys the old gamestate (FUN_141176020 -> ...
// -> FUN_14074bf70 -> FUN_14074a260), and FUN_1404e0d90 calls virtual +0x60 and the deleting destructor of
// every relation in the lists. In the reports one of them had been freed already: its first qword was the
// next-free pointer mimalloc writes into a freed block - a heap address (executed: return address
// eu4.exe+0x4e0e55) or 0 at the end of a free list (read of 0x60 at eu4.exe+0x4e0e52). Two PCs with
// mods, players who load often, 0.4.1 and 0.4.8. A freed relation in that list is the game's bug; a heap
// that leaves the old vtable in place only hides it (the game then calls into freed memory and frees it a
// second time). Every relation is checked before the original runs: readable, vtable inside .rdata, its
// slots 0 (deleting destructor), 9 (type) and 12 (the call before it) inside .text. One that fails is taken
// out of its node - the original skips empty nodes - so the freed block is neither called nor freed again.
#include "common.h"
#include <stdlib.h>
#include <string.h>
#include <mimalloc.h>
#include "MinHook.h"

#define RVA_TIP 0x8e8ca0u    // modifier breakdown text (container, std::string* out, descriptor)
#define RVA_PTC_CALL 0xa2c7e5u  // FUN_140a2c5e0's call of it (e8 rel32); the hook sees return address +5
#define RVA_STR_ASSIGN 0x095110u   // std::string::assign(const char*, size_t)
#define RVA_STR_TIDY   0x095660u   // std::string: free the buffer, make it empty
static const uint8_t sig_assign[16] = { 0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x56,0x57,0x41,0x57,0x48,0x83 };
static const uint8_t sig_tidy[16]   = { 0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x51,0x18,0x48,0x8b,0xd9,0x48,0x83,0xfa };
#define RVA_REL 0xa5e130u    // remove an active relation (relation)
static const uint8_t sig_tip[24] = { 0x48,0x89,0x54,0x24,0x10,0x48,0x89,0x4c,0x24,0x08,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d };
static const uint8_t sig_rel[24] = { 0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0x48,0x8b,0x0d,0x38,0x1d,0x8e,0x01,0x48,0x81,0xc1,0xb8,0x1b,0x00,0x00,0x48 };
#define RVA_HC_READ   0xcc6e40u    // has_construction trigger: read its value (trigger, token)
#define RVA_GP_SETBUF 0x7db530u    // great project array: move the elements into a new buffer (array, buffer, capacity)
#define RVA_GP_MGR    0x242bc90u   // great project manager (pointer)
#define RVA_HC_VT     0x1d17230u   // vtable of the has_construction trigger
static const uint8_t sig_hc_read[24] = { 0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x18,0x48,0x89,0x7c,0x24,0x20,0x55,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57 };
static const uint8_t sig_gp_setbuf[24] = { 0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x20,0x44,0x8b,0x49,0x14,0x45 };
#define GP_SIZE 0x1d38             // one great project
#define HC_GP   0x78               // trigger: address of the great project it names (or 0)

// modifier container (see modifier.c)
#define M_DENSE 0x10
#define M_VEC   0x38         // std::vector<container*> of sources: begin, end, end of storage
#define M_DATA  0x58
#define M_CAP   0x60
#define M_CNT   0x64
#define M_HDR   0x68
#define D_TOTAL 0x10         // descriptor: int* that receives the summed value (may be NULL)

#define RVA_DAY     0x758430u   // daily tick (gamestate)
#define RVA_MIDTICK 0x769ea0u   // frame rendered in the middle of a long tick (gamestate, char, file, line)
static const uint8_t sig_day[24] = { 0x48,0x8b,0xc4,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xa8,0xc8,0xfe,0xff,0xff,0x48,0x81 };
static const uint8_t sig_midtick[24] = { 0x48,0x8b,0xc4,0x48,0x89,0x58,0x10,0x4c,0x89,0x40,0x18,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d };

typedef void* (*tip_t)(uint8_t* c, uint64_t* out, uint8_t* desc);
typedef void (*rel_t)(void* rel);
typedef uint64_t (*day_t)(void* gs);
typedef uint64_t (*midtick_t)(void* gs, char a, void* b, uint32_t c);
static tip_t o_tip;
static rel_t o_rel;
static day_t o_day;
static midtick_t o_midtick;
static int g_sim;                    // main thread: inside the daily tick, outside its mid-tick frames
static uintptr_t g_rdata_lo, g_rdata_hi;
static volatile LONG g_bad_sources, g_bad_containers, g_null_relations;

static unsigned g_day_n;              // daily ticks so far (main thread)
volatile ULONGLONG eb_day_ms;         // online.c throttles only while days are processed
int eb_day_hooked;
static uint64_t hk_day(void* gs) {
    eb_day_ms = GetTickCount64();
    LARGE_INTEGER t0; if (eb_pm_on) QueryPerformanceCounter(&t0);
    g_sim++; g_day_n++; eb_in_day++; uint64_t r = o_day(gs); eb_in_day--; g_sim--;
    eb_day_ms = GetTickCount64();
    if (eb_pm_on) { LARGE_INTEGER t1; QueryPerformanceCounter(&t1); eb_pm_day(t1.QuadPart - t0.QuadPart); }
    return r;
}
static uint64_t hk_midtick(void* gs, char a, void* b, uint32_t c) {
    int s = g_sim; g_sim = 0;
    uint64_t r = o_midtick(gs, a, b, c);
    g_sim = s;
    return r;
}

// eb_probe8(p, &v): 1 and *v = the qword at p, or 0 when p is not readable.
int eb_probe8(const void* p, uint64_t* out);
extern char eb_probe_insn[], eb_probe_fail[];
__asm__(".text\n"
        ".globl eb_probe8\n"
        "eb_probe8:\n"
        ".globl eb_probe_insn\n"
        "eb_probe_insn:\n"
        "    movq (%rcx), %rax\n"
        "    movq %rax, (%rdx)\n"
        "    movl $1, %eax\n"
        "    ret\n"
        ".globl eb_probe_fail\n"
        "eb_probe_fail:\n"
        "    xorl %eax, %eax\n"
        "    ret\n");

static LONG CALLBACK probe_veh(EXCEPTION_POINTERS* e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_GUARD_PAGE) &&
        e->ContextRecord->Rip == (DWORD64)(uintptr_t)eb_probe_insn) {
        e->ContextRecord->Rip = (DWORD64)(uintptr_t)eb_probe_fail;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// every page of [a, a+n) has a readable qword
static int readable(uintptr_t a, size_t n) {
    if (!n) n = 1;
    if (a < 0x10000 || a + n < a || a + n > 0x7FFFFFFF0000ull) return 0;
    uint64_t v;
    for (uintptr_t q = a & ~(uintptr_t)7, end = a + n; q < end; q = (q | 0xfff) + 1)
        if (!eb_probe8((const void*)q, &v)) return 0;
    return 1;
}

static int container_ok(const uint8_t* c) {
    uintptr_t a = (uintptr_t)c;
    if ((a & 7) || !readable(a, M_HDR)) return 0;
    uintptr_t vt = *(const uintptr_t*)c;
    if ((vt & 7) || vt < g_rdata_lo || vt + 0x68 > g_rdata_hi) return 0;
    if (c[M_DENSE] > 1) return 0;
    int cnt = *(const int*)(c + M_CNT), cap = *(const int*)(c + M_CAP);
    if (cnt < 0 || cap < cnt || cap > (1 << 20)) return 0;
    if (cap && !readable(*(const uintptr_t*)(c + M_DATA), (size_t)cap * 8)) return 0;
    uintptr_t b = *(const uintptr_t*)(c + M_VEC), e = *(const uintptr_t*)(c + M_VEC + 8), z = *(const uintptr_t*)(c + M_VEC + 16);
    if (b > e || e > z || ((e - b) & 7) || e - b > ((size_t)8 << 20)) return 0;
    return e == b || readable(b, e - b);
}

static void* empty_text(uint64_t* out, uint8_t* desc) {
    int* total = *(int**)(desc + D_TOTAL);
    if (total) *total = 0;
    out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 15;   // empty std::string (SSO)
    return out;
}

// ---- province text cache (main thread only: the province monthly update runs there) ----
typedef void* (*str_assign_t)(uint64_t* s, const char* p, size_t n);
typedef void (*str_tidy_t)(uint64_t* s);
static str_assign_t g_str_assign;
static str_tidy_t g_str_tidy;
static uintptr_t g_ptc_ret;          // absolute return address of the cached call site
#define NPTC 4096
typedef struct { uint8_t* c; char* data; size_t len; } ptc_t;
static ptc_t g_ptc[NPTC];
static unsigned g_ptc_day = ~0u;
static LONG64 g_ptc_calls, g_ptc_hits, g_ptc_ver, g_ptc_bad, g_ptc_t[2], g_ptc_n[2], g_ptc_months;
static unsigned g_ptc_flip, g_ptc_logday = ~0u;
static double g_ptc_ms;

static const char* str_data(const uint64_t* s) { return s[3] > 15 ? (const char*)s[0] : (const char*)s; }
static void ptc_clear(void) {
    for (int i = 0; i < NPTC; i++) { free(g_ptc[i].data); g_ptc[i].c = NULL; g_ptc[i].data = NULL; }
}
static void ptc_report(void) {
    eb_log("modifiers: province text cache: %lld months, %lld texts asked, %lld copied (%.1f%%) | verify %lld, mismatches %lld",
           (long long)g_ptc_months, (long long)g_ptc_calls, (long long)g_ptc_hits, g_ptc_calls ? 100.0 * g_ptc_hits / g_ptc_calls : 0.0,
           (long long)g_ptc_ver, (long long)g_ptc_bad);
    if (g_cfg.prov_text_cache == 3)
        for (int s = 0; s < 2; s++)
            eb_log("modifiers: province text A/B %s: %lld texts, avg %.2f us, total %.1f ms", s ? "B (cache)   " : "A (original)",
                   (long long)g_ptc_n[s], g_ptc_n[s] ? g_ptc_t[s] * g_ptc_ms * 1000.0 / g_ptc_n[s] : 0.0, g_ptc_t[s] * g_ptc_ms);
}
static void* ptc_text(uint8_t* c, uint64_t* out, uint8_t* desc) {
    if (g_day_n != g_ptc_day) {                       // a new day: texts of earlier days are stale
        if (g_ptc_calls && g_ptc_logday != g_ptc_day) {
            g_ptc_months++; g_ptc_logday = g_ptc_day;
            if (g_ptc_months % 12 == 0) ptc_report();
        }
        ptc_clear(); g_ptc_day = g_day_n;
    }
    g_ptc_calls++;
    int side = 1;
    LARGE_INTEGER t0;
    if (g_cfg.prov_text_cache == 3) { side = (int)(++g_ptc_flip & 1); QueryPerformanceCounter(&t0); }
    void* r = out;
    unsigned h = (unsigned)(((uintptr_t)c >> 4) * 2654435761u) & (NPTC - 1);
    ptc_t* e = NULL;
    for (int k = 0; k < NPTC; k++, h = (h + 1) & (NPTC - 1))
        if (!g_ptc[h].c || g_ptc[h].c == c) { e = &g_ptc[h]; break; }
    if (side && e && e->c == c) {
        g_ptc_hits++;
        out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 15;   // empty std::string, then the game's assign
        g_str_assign(out, e->data, e->len);
        if (g_cfg.prov_text_cache == 2) {
            uint64_t tmp[4];
            o_tip(c, tmp, desc);
            g_ptc_ver++;
            if (tmp[2] != e->len || memcmp(str_data(tmp), e->data, e->len)) {
                g_ptc_bad++;
                if (g_ptc_bad <= 10) eb_log("modifiers: VERIFY MISMATCH province text of container %p: kept %zu bytes, the game made %zu", (void*)c, e->len, (size_t)tmp[2]);
            }
            g_str_tidy(tmp);
        }
    } else {
        r = o_tip(c, out, desc);
        if (side && e && !e->c) {
            char* d = (char*)malloc(out[2] + 1);
            if (d) { memcpy(d, str_data(out), out[2]); d[out[2]] = 0; e->c = c; e->data = d; e->len = out[2]; }
        }
    }
    if (g_cfg.prov_text_cache == 3) {
        LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
        g_ptc_t[side] += t1.QuadPart - t0.QuadPart; g_ptc_n[side]++;
    }
    return r;
}

static void* hk_tip(uint8_t* c, uint64_t* out, uint8_t* desc) {
    if (g_ptc_ret && g_sim && (uintptr_t)__builtin_return_address(0) == g_ptc_ret && GetCurrentThreadId() == g_main_tid)
        return ptc_text(c, out, desc);
    if (g_sim || !g_cfg.guard_tooltip) return o_tip(c, out, desc);
    if (!container_ok(c)) {
        if (InterlockedIncrement(&g_bad_containers) <= 20) eb_log("guard: tooltip skipped an invalid modifier container %p", (void*)c);
        return empty_text(out, desc);
    }
    void** b = *(void***)(c + M_VEC);
    size_t n = (size_t)(*(void***)(c + M_VEC + 8) - b), bad = 0;
    for (size_t i = 0; i < n; i++) if (b[i] && !container_ok((uint8_t*)b[i])) bad++;
    void* r;
    if (!bad) {
        r = o_tip(c, out, desc);
    } else {
        void** tmp = (void**)malloc((n - bad + 1) * sizeof(void*));
        if (!tmp) return empty_text(out, desc);
        size_t k = 0;
        for (size_t i = 0; i < n; i++) if (!b[i] || container_ok((uint8_t*)b[i])) tmp[k++] = b[i];
        if (InterlockedAdd(&g_bad_sources, (LONG)bad) <= 20)
            eb_log("guard: tooltip hid %zu invalid modifier source(s) of %p", bad, (void*)c);
        void** saved[3] = { b, *(void***)(c + M_VEC + 8), *(void***)(c + M_VEC + 16) };
        *(void***)(c + M_VEC) = tmp; *(void***)(c + M_VEC + 8) = tmp + k; *(void***)(c + M_VEC + 16) = tmp + k;
        r = o_tip(c, out, desc);
        *(void***)(c + M_VEC) = saved[0]; *(void***)(c + M_VEC + 8) = saved[1]; *(void***)(c + M_VEC + 16) = saved[2];
        free(tmp);
    }
    return r;
}

static void hk_rel(void* rel) {
    if (!rel) {
        if (InterlockedIncrement(&g_null_relations) <= 20)
            eb_log("guard: missing relation skipped (the game logged \"!pRelation CDiplomacy::RemActive\" and would have crashed)");
        return;
    }
    o_rel(rel);
}

typedef uint64_t (*hc_read_t)(uint8_t* t, void* tok);
typedef void (*gp_setbuf_t)(uint8_t* arr, uint8_t* buf, uint32_t cap);
static hc_read_t o_hc_read;
static gp_setbuf_t o_gp_setbuf;
static uint8_t** g_gp_mgr;            // where the game keeps the manager's address
static void* g_hc_vt;
typedef struct { uint8_t* t; uint8_t* gp; } hcref_t;
#define NHC 8192
static hcref_t g_hc[NHC];
static int g_nhc, g_hc_lost;
static CRITICAL_SECTION g_gp_cs;
static int g_gp_moves, g_gp_moved;

static uint64_t hk_hc_read(uint8_t* t, void* tok) {
    EnterCriticalSection(&g_gp_cs);   // the array cannot move between the lookup and the record
    uint64_t r = o_hc_read(t, tok);
    uint8_t* gp = *(uint8_t**)(t + HC_GP);
    if (gp) {
        if (g_nhc < NHC) { g_hc[g_nhc].t = t; g_hc[g_nhc].gp = gp; g_nhc++; }
        else if (g_hc_lost++ == 0) eb_log("guard: more than %d has_construction triggers name a great project - the rest are not tracked", NHC);
    }
    LeaveCriticalSection(&g_gp_cs);
    return r;
}

static void hk_gp_setbuf(uint8_t* arr, uint8_t* buf, uint32_t cap) {
    EnterCriticalSection(&g_gp_cs);
    uint8_t* old = *(uint8_t**)(arr + 8);
    int n = *(int*)(arr + 0x14);
    o_gp_setbuf(arr, buf, cap);
    uint8_t* mgr = *g_gp_mgr;
    if (old && buf && old != buf && n > 0 && mgr && arr == mgr + 0x40) {
        uintptr_t lo = (uintptr_t)old, hi = lo + (uintptr_t)n * GP_SIZE;
        int moved = 0;
        for (int i = 0; i < g_nhc; i++) {
            hcref_t* h = &g_hc[i];
            uint64_t vt, gp;
            if ((uintptr_t)h->gp < lo || (uintptr_t)h->gp >= hi) continue;
            // the trigger may have been freed since: it must still look like the one recorded
            if (!eb_probe8(h->t, &vt) || vt != (uintptr_t)g_hc_vt || !eb_probe8(h->t + HC_GP, &gp) || gp != (uintptr_t)h->gp) continue;
            h->gp = buf + ((uintptr_t)h->gp - lo);
            *(uint8_t**)(h->t + HC_GP) = h->gp;
            moved++;
        }
        g_gp_moves++;
        g_gp_moved += moved;
        if (moved) eb_log("guard: great projects moved to a new array (%d of capacity %u) - %d has_construction triggers moved with them", n, cap, moved);
    }
    LeaveCriticalSection(&g_gp_cs);
}

static int g_probe_on;
static int probe_init(void) {
    if (!g_probe_on && AddVectoredExceptionHandler(1, probe_veh)) g_probe_on = 1;
    return g_probe_on;
}

static int hook(uint8_t* t, const uint8_t* sig, size_t n, void* fn, void** orig, const char* what) {
    if (!t) { eb_log("guard: %s target not found", what); return 0; }
    if (sig && memcmp(t, sig, n)) { eb_log("guard: %s signature mismatch - not installed", what); return 0; }
    if (MH_CreateHook(t, fn, orig) != MH_OK || MH_EnableHook(t) != MH_OK) { eb_log("guard: %s hook failed", what); return 0; }
    return 1;
}

// ---- ironman save names in EU4dll's escapes ----
#define RVA_SAVE_PATH 0x5ca070u
static const uint8_t sig_save_path[16] = { 0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48,0x89,0x4c,0x24,0x08,0x55 };
typedef void* (*save_path_t)(void* out, void* name, void* dir, void* p4);
static save_path_t o_save_path;
static volatile LONG g_iname_fixed;
// EU4dll's convertEscapedTextToWideText + convertWideTextToUtf8 (escape_tool.cpp), quirks included: a byte
// outside an escape goes through CP1252ToUCS2(char), whose switch never matches a signed char, so it is the
// sign-extended byte. 0 = the text has no escape byte (or does not fit).
static size_t iname_utf8(const uint8_t* s, size_t n, char* out, size_t cap) {
    size_t i = 0;
    while (i < n && (s[i] < 0x10 || s[i] > 0x13)) i++;
    if (i == n || n > 1024) return 0;
    wchar_t w[1024]; size_t k = 0;
    for (i = 0; i < n;) {
        uint8_t c = s[i++];
        uint32_t sp;
        if (c >= 0x10 && c <= 0x13) {
            uint8_t lo = i < n ? s[i++] : 0, hi = i < n ? s[i++] : 0;    // EU4dll reads the terminating NUL here
            sp = (uint32_t)hi << 8 | lo;
            if (c == 0x11) sp -= 0xE; else if (c == 0x12) sp += 0x900; else if (c == 0x13) sp += 0x8F2;
        } else sp = (uint16_t)(int16_t)(int8_t)c;
        if (sp > 0xFFFF || (sp < 0x98F && sp > 0x100)) sp = 0x2026;
        w[k++] = (wchar_t)sp;
    }
    int m = WideCharToMultiByte(CP_UTF8, 0, w, (int)k, out, (int)cap - 1, NULL, NULL);
    if (m <= 0) return 0;
    out[m] = 0;
    return (size_t)m;
}
static void* hk_save_path(void* out, void* name, void* dir, void* p4) {
    const uint64_t* q = (const uint64_t*)name;                         // MSVC std::string: text or pointer, size +0x10, capacity +0x18
    const uint8_t* s = q[3] > 15 ? (const uint8_t*)q[0] : (const uint8_t*)name;
    size_t sn = (size_t)q[2];
    // testing only: the save of the autorun copy gets an ironman name typed in Korean ("eb_시험" as EU4dll writes it)
    static const uint8_t test_name[] = { 'e', 'b', '_', 0x10, 0xDC, 0xC2, 0x10, 0xD8, 0xD5, 0 };
    uint64_t tq[4] = { 0, 0, 9, 15 };
    if (g_cfg.test_ironman_name && sn == 7 && !memcmp(s, "eb_auto", 7)) {
        memcpy(tq, test_name, sizeof test_name);
        name = tq; s = test_name; sn = 9;
        if (!g_cfg.guard_ironman_name) return o_save_path(out, name, dir, p4);
    }
    char u[3200];
    size_t m = iname_utf8(s, sn, u, sizeof u);
    if (!m) return o_save_path(out, name, dir, p4);
    uint64_t fake[4] = { (uint64_t)(uintptr_t)u, 0, m, m > 15 ? m : 16 };   // read only by the callee
    if (InterlockedIncrement(&g_iname_fixed) == 1) eb_log("guard: an ironman save name in EU4dll's escapes is saved under its UTF-8 name (%zu bytes)", m);
    return o_save_path(out, fake, dir, p4);
}

// ---- the duplicate localisation check on a stack that fits ----
#define RVA_LOC_DUPS 0x16fb1c0u
static const uint8_t sig_loc_dups[16] = { 0x48,0x89,0x54,0x24,0x10,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41 };
typedef uint64_t (*loc_dups_t)(void* table, void* p2);
static loc_dups_t o_loc_dups;
static volatile LONG g_loc_fibers;
// the stack FUN_1416fb1c0 takes with alloca: per run of k > 1 equal keys three arrays of
// (8k + 8 + 15) & ~15 bytes (its loop: lea rcx,[k*8+8]; lea rax,[rcx+0xf]; and rax,-16; x3)
static size_t loc_dups_need(const uint8_t* table) {
    const uint64_t* b = *(const uint64_t* const*)(table + 0x20);
    const uint64_t* e = *(const uint64_t* const*)(table + 0x28);
    if (!b || e <= b) return 0;
    size_t n = (size_t)(e - b) / 2, need = 0;
    for (size_t i = 0; i + 1 < n;) {
        size_t j = i + 1;
        while (j < n && b[2 * j] == b[2 * i]) j++;
        if (j - i > 1) need += 3 * ((8 * (j - i) + 8 + 15) & ~(size_t)15);
        i = j;
    }
    return need;
}
typedef struct { void* table; void* p2; uint64_t r; LPVOID back; } locf_t;
static void CALLBACK loc_dups_fiber(LPVOID p) {
    locf_t* a = (locf_t*)p;
    a->r = o_loc_dups(a->table, a->p2);
    SwitchToFiber(a->back);                                     // a fiber's function must not return
}
static uint64_t loc_dups_logged(void* table, uint64_t r, int fiber) {
    if (g_cfg.test_loc_fiber)
        eb_log("guard: TEST duplicate localisation check of a table of %lld keys -> %d%s",
               (long long)((*(uint8_t**)((uint8_t*)table + 0x28) - *(uint8_t**)((uint8_t*)table + 0x20)) / 16), (int)(r & 0xff),
               fiber ? " (larger stack)" : "");
    return r;
}
static uint64_t hk_loc_dups(void* table, void* p2) {
    size_t need = loc_dups_need((const uint8_t*)table);
    const NT_TIB* tib = (const NT_TIB*)NtCurrentTeb();
    MEMORY_BASIC_INFORMATION mi;
    uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
    uintptr_t lo = VirtualQuery((const uint8_t*)tib->StackBase - 1, &mi, sizeof mi) ? (uintptr_t)mi.AllocationBase : (uintptr_t)tib->StackLimit;
    size_t left = sp > lo ? sp - lo : 0;
    size_t room = (size_t)1 << 20;                              // the log line, its exception dispatch, guard pages
    if (g_cfg.test_loc_fiber != 1 && need + room < left) return loc_dups_logged(table, o_loc_dups(table, p2), 0);
    int was = IsThreadAFiber();
    LPVOID me = was ? GetCurrentFiber() : ConvertThreadToFiber(NULL);
    if (!me) return loc_dups_logged(table, o_loc_dups(table, p2), 0);
    locf_t a = { table, p2, 0, me };
    SIZE_T reserve = (need + room + (16u << 20)) & ~(SIZE_T)0xffff;
    LPVOID f = CreateFiberEx(64 << 10, reserve, FIBER_FLAG_FLOAT_SWITCH, loc_dups_fiber, &a);
    if (!f) { if (!was) ConvertFiberToThread(); return loc_dups_logged(table, o_loc_dups(table, p2), 0); }
    if (InterlockedIncrement(&g_loc_fibers) <= 5)
        eb_log("guard: duplicate localisation check needs %zu KB of stack, %zu KB left - run on a %zu MB stack", need >> 10, left >> 10, (size_t)(reserve >> 20));
    SwitchToFiber(f);
    DeleteFiber(f);
    if (!was) ConvertFiberToThread();
    return loc_dups_logged(table, a.r, 1);
}

// ---- the diplomacy reset skips relations that were freed already ----
// FUN_1404e0d90(d) (CDiplomacy, gamestate+0x1bb8; ResetGamestate on every load) walks the active relation
// lists d+0x10..d+0x18 (a header of 0x18 bytes per country: head node, tail node, count +0x10, "being walked"
// byte +0x14; a node of 0x20: relation, prev, next, removal pending +0x18), and for every relation calls its
// virtual +0x60 and its deleting destructor, reading the next node first. Then it frees the lists and makes
// them again. Reports 78, 77, 51 (0.4.1, 0.4.8): a relation already freed before the reset -> 0.4.9 checks
// every relation before the game's walk. Reports 123 (0.4.9), 125 (x2) and 132 (0.4.10) crashed in the same
// walk (eu4.exe+0x4e0e55 executing a heap address, +0x4e0e52 reading 0x60) with that check on: something
// is freed *during* the walk. RemActive (FUN_1404e1570) unlinks and frees a relation's node at once unless its
// list's +0x14 byte is set, then it only marks the node pending; the reset never sets it. So when the +0x60 or
// destructor of one relation removes another one (RemActive, and its remover deletes it), the node the walk
// has already read as the next one is freed memory: its first qword (mimalloc's next free block) is taken for
// a relation, and its "vtable" for a function table.
// Since 0.4.11 the hook does the walk itself, the way the game's other walks are safe: every list's +0x14 byte
// set first (RemActive then only marks), a node marked pending is skipped (its remover owns the relation, as
// when the game unlinks it), every relation is checked right before its calls; each relation's pointer is set
// to 0 after its two calls; the game's function then does the rest (its own walk finds only zeros, frees the
// lists including the pending nodes, makes them again). The order of the calls and the debug-log bit the game
// turns off during its walk (bit 0 of FUN_1414d4c80() + 0x38) are as in the game.
#define RVA_DIPLO_CLEAR 0x4e0d90u
#define RVA_REM_ACTIVE  0x4e1570u      // CDiplomacy::RemActive(d, relation) (testing only: the removal of report 125)
#define RVA_LOGSET      0x14d4c80u     // FUN_1414d4c80: the log settings, flags at +0x38
#define RVA_LOGFLUSH    0x14d4180u
static const uint8_t sig_diplo_clear[16] = { 0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57 };
static const uint8_t sig_rem_active[16]  = { 0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x20,0x44 };
static const uint8_t sig_logset[16]      = { 0x48,0x83,0xec,0x28,0x65,0x48,0x8b,0x04,0x25,0x58,0x00,0x00,0x00,0xba,0x08,0x00 };
static const uint8_t sig_logflush[16]    = { 0x48,0x89,0x4c,0x24,0x08,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41 };
typedef void (*diplo_clear_t)(uint8_t* d);
typedef char (*rem_active_t)(uint8_t* d, void* rel);
typedef uint8_t* (*logset_t)(void);
typedef void (*logflush_t)(uint8_t* s);
static diplo_clear_t o_diplo_clear;
static rem_active_t g_rem_active;
static logset_t g_logset;
static logflush_t g_logflush;
static uintptr_t g_text_lo, g_text_hi;
static volatile LONG g_freed_relations;
static LONG64 g_dr_resets, g_dr_calls, g_dr_pending;
static int g_diplo_tested;
static int in_text(uintptr_t p) { return p >= g_text_lo && p < g_text_hi; }
static uintptr_t g_fake_vt;                     // testing ([test] diplo_removed): the copy of a vtable put on relation A
static int relation_ok(uintptr_t o) {
    if ((o & 7) || !readable(o, 0x40)) return 0;
    uintptr_t vt = *(const uintptr_t*)o;
    if (vt && vt == g_fake_vt) return 1;
    if ((vt & 7) || vt < g_rdata_lo || vt + 13 * 8 > g_rdata_hi) return 0;
    const uintptr_t* v = (const uintptr_t*)vt;
    return in_text(v[0]) && in_text(v[9]) && in_text(v[12]);
}

// testing ([test] diplo_removed=1): what reports 123/125/132 point at. The first relation A whose next node in
// its list holds a relation B gets a vtable copy whose +0x60 puts A's own vtable back, runs A's +0x60, then
// removes B with RemActive and deletes it - as a relation that ends another one when it ends.
static uintptr_t g_fvt[32];
static uint8_t* g_t_d; static uintptr_t g_t_a, g_t_b, g_t_vt;
static void test_a_60(void* a) {
    *(uintptr_t*)a = g_t_vt;                    // A is itself again for the rest of the walk
    ((void (*)(void*))((uintptr_t*)g_t_vt)[12])(a);
    char r = g_rem_active(g_t_d, (void*)g_t_b);
    eb_log("guard: TEST relation %p (the next in the list of %p) removed (RemActive %d) and deleted during the reset", (void*)g_t_b, a, r);
    ((void (*)(void*, int))(*(uintptr_t**)g_t_b)[0])((void*)g_t_b, 1);
    g_fake_vt = 0;
}
static void test_removal(uint8_t* d, size_t lists) {
    uint8_t* b = *(uint8_t**)(d + 0x10);
    for (size_t i = 0; i < lists; i++) {
        uint8_t* nd = *(uint8_t**)(b + i * 0x18);
        uint8_t* nx = nd ? *(uint8_t**)(nd + 0x10) : NULL;
        if (!nd || !nx || !relation_ok(*(uintptr_t*)nd) || !relation_ok(*(uintptr_t*)nx)) continue;
        g_t_d = d; g_t_a = *(uintptr_t*)nd; g_t_b = *(uintptr_t*)nx; g_t_vt = *(uintptr_t*)g_t_a;
        memcpy(g_fvt, (const void*)g_t_vt, sizeof g_fvt);
        g_fvt[12] = (uintptr_t)test_a_60;
        g_fake_vt = (uintptr_t)g_fvt;
        *(uintptr_t*)g_t_a = g_fake_vt;
        eb_log("guard: TEST relation %p of list %zu will remove and delete the next one %p%s", (void*)g_t_a, i, (void*)g_t_b,
               g_cfg.guard_diplo_reset ? "" : " (without the fix)");
        return;
    }
    eb_log("guard: TEST no list with two relations - nothing removed");
}

static void hk_diplo_clear(uint8_t* d) {
    uint8_t* b = *(uint8_t**)(d + 0x10);
    uint8_t* e = *(uint8_t**)(d + 0x18);
    size_t lists = b && e > b ? (size_t)(e - b) / 0x18 : 0, n = 0, bad = 0, pend = 0;
    int ok_lists = lists && lists < (1u << 20) && readable((uintptr_t)b, (size_t)(e - b));
    if (ok_lists && g_cfg.test_diplo_freed == 1 && !g_diplo_tested) {      // what reports 78/77/51 found: a freed relation
        for (size_t i = 0; i < lists && !g_diplo_tested; i++) {
            uint8_t* nd = *(uint8_t**)(b + i * 0x18);
            if (!nd || !*(uintptr_t*)nd) continue;
            void* f = mi_malloc(0x60);
            mi_free(f);
            eb_log("guard: TEST relation %p in the list of country %zu replaced by a freed block %p (first qword %p)",
                   *(void**)nd, i, f, *(void**)f);
            *(void**)nd = f; g_diplo_tested = 1;
        }
    }
    if (ok_lists && g_cfg.test_diplo_removed == 1 && !g_diplo_tested && g_rem_active) { test_removal(d, lists); g_diplo_tested = 1; }
    if (!g_cfg.guard_diplo_reset || !ok_lists || !g_logset) {
        o_diplo_clear(d);
        return;
    }
    g_dr_resets++;
    uint8_t* ls = g_logset();                   // the game's start: the debug-log bit 0 off for the walk
    uint32_t saved = *(uint32_t*)(ls + 0x38), off = saved & ~1u;
    if ((*(uint32_t*)(ls + 0x38) & 2) && !(off & 2)) g_logflush(ls);
    *(uint32_t*)(ls + 0x38) = off;
    for (size_t i = 0; i < lists; i++) *(uint8_t*)(*(uint8_t**)(d + 0x10) + i * 0x18 + 0x14) = 1;
    for (size_t i = 0; i < lists; i++) {
        uint8_t* L = *(uint8_t**)(d + 0x10) + i * 0x18;      // the array as it is now (a relation added for a new tag could move it)
        L[0x14] = 1;
        for (uint8_t* nd = *(uint8_t**)L; nd && n < (1u << 24); ) {
            if (!readable((uintptr_t)nd, 0x20)) break;
            uint8_t* next = *(uint8_t**)(nd + 0x10);   // read first, as the game: nodes are not freed while +0x14 is set
            uintptr_t o = *(uintptr_t*)nd;
            if (o) {
                if (nd[0x18]) { *(uintptr_t*)nd = 0; pend++; g_dr_pending++; }   // removed during this reset: its remover owns it, the game's walk must not see it
                else if (!relation_ok(o)) {
                    uint64_t q = 0; int rd = eb_probe8((const void*)(o & ~(uintptr_t)7), &q);
                    if (InterlockedIncrement(&g_freed_relations) <= 10)
                        eb_log("guard: diplomacy reset skipped a freed relation %p in the list of country %zu (first qword %s%p)",
                               (void*)o, i, rd ? "" : "unreadable ", (void*)(uintptr_t)q);
                    *(uintptr_t*)nd = 0;
                    bad++;
                } else {
                    ((void (*)(void*))(*(uintptr_t**)o)[12])((void*)o);
                    ((void (*)(void*, int))(*(uintptr_t**)o)[0])((void*)o, 1);
                    *(uintptr_t*)nd = 0;
                    n++; g_dr_calls++;
                }
            }
            nd = next;
        }
    }
    uint8_t* b2 = *(uint8_t**)(d + 0x10); uint8_t* e2 = *(uint8_t**)(d + 0x18);
    for (size_t i = 0, m = b2 && e2 > b2 ? (size_t)(e2 - b2) / 0x18 : 0; i < m; i++) b2[i * 0x18 + 0x14] = 0;
    if (!(*(uint32_t*)(ls + 0x38) & 2) || (saved & 2)) *(uint32_t*)(ls + 0x38) = saved;   // the game's end of it
    else { g_logflush(ls); *(uint32_t*)(ls + 0x38) = saved; }
    if (bad || pend || g_cfg.test_diplo_freed || g_cfg.test_diplo_removed)
        eb_log("guard: diplomacy reset: %zu relations ended in %zu lists, %zu removed during the reset skipped, %zu freed ones skipped", n, lists, pend, bad);
    o_diplo_clear(d);                           // the rest as the game: its walk finds only zeros, the lists are made again
}

int eb_guard_install(void) {
    int want_day_time = g_cfg.online_poll_ms > 0 && g_cfg.online_running_only;
    if (!g_cfg.guard_tooltip && !g_cfg.guard_relation && !g_cfg.guard_great_project && !g_cfg.guard_ironman_name && !g_cfg.test_ironman_name &&
        !g_cfg.guard_loc_duplicates && !g_cfg.guard_diplo_reset && !g_cfg.test_diplo_freed && !g_cfg.test_diplo_removed && !g_cfg.prov_text_cache && !want_day_time) return 1;
    uint8_t* b = (uint8_t*)g_exe;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(b + ((IMAGE_DOS_HEADER*)g_exe)->e_lfanew);
#ifdef EB_TEST
    uint8_t* tip = (uint8_t*)GetProcAddress(g_exe, "t_tip"); uint8_t* rel = (uint8_t*)GetProcAddress(g_exe, "t_rel");
    uint8_t* day = (uint8_t*)GetProcAddress(g_exe, "t_day"); uint8_t* midtick = (uint8_t*)GetProcAddress(g_exe, "t_midtick");
    uint8_t* hc = (uint8_t*)GetProcAddress(g_exe, "t_hc_read"); uint8_t* gpb = (uint8_t*)GetProcAddress(g_exe, "t_gp_setbuf");
    g_gp_mgr = (uint8_t**)GetProcAddress(g_exe, "t_gp_mgr"); g_hc_vt = (void*)GetProcAddress(g_exe, "t_hc_vt");
    const uint8_t *stip = NULL, *srel = NULL, *sday = NULL, *smid = NULL, *shc = NULL, *sgpb = NULL;
#else
    if (nt->FileHeader.TimeDateStamp != 0x66FE6AB9u) { eb_log("guard: unknown eu4.exe - not installed"); return 0; }
    uint8_t* tip = b + RVA_TIP; uint8_t* rel = b + RVA_REL; uint8_t* day = b + RVA_DAY; uint8_t* midtick = b + RVA_MIDTICK;
    uint8_t* hc = b + RVA_HC_READ; uint8_t* gpb = b + RVA_GP_SETBUF;
    g_gp_mgr = (uint8_t**)(b + RVA_GP_MGR); g_hc_vt = b + RVA_HC_VT;
    const uint8_t *stip = sig_tip, *srel = sig_rel, *sday = sig_day, *smid = sig_midtick, *shc = sig_hc_read, *sgpb = sig_gp_setbuf;
#endif
    IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!memcmp(s[i].Name, ".rdata", 7)) { g_rdata_lo = (uintptr_t)b + s[i].VirtualAddress; g_rdata_hi = g_rdata_lo + s[i].Misc.VirtualSize; }
        else if (!memcmp(s[i].Name, ".text", 6)) { g_text_lo = (uintptr_t)b + s[i].VirtualAddress; g_text_hi = g_text_lo + s[i].Misc.VirtualSize; }
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) { eb_log("guard: MinHook init failed"); return 0; }
    int ok = 0;
#ifndef EB_TEST
    // the province text cache: the one call site must still be a call of the text function
    if (g_cfg.prov_text_cache) {
        uint8_t* cs = b + RVA_PTC_CALL;
        if (cs[0] == 0xe8 && cs + 5 + *(int32_t*)(cs + 1) == b + RVA_TIP && !memcmp(b + RVA_STR_ASSIGN, sig_assign, sizeof sig_assign) &&
            !memcmp(b + RVA_STR_TIDY, sig_tidy, sizeof sig_tidy)) {
            g_ptc_ret = (uintptr_t)(cs + 5);
            g_str_assign = (str_assign_t)(b + RVA_STR_ASSIGN);
            g_str_tidy = (str_tidy_t)(b + RVA_STR_TIDY);
            LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_ptc_ms = 1000.0 / (double)f.QuadPart;
        } else eb_log("modifiers: province text cache: call site not found - off");
    }
#endif
    // the daily tick hooks: the tooltip guard needs them (without them every province would be checked
    // monthly), so does the province text cache; the online throttle reads the time of the latest tick
    int want_tip = g_cfg.guard_tooltip || g_ptc_ret;
    if ((want_tip || want_day_time) && day && midtick) {
        eb_hookset hs; eb_hs_init(&hs, "guard daily tick");
        if ((!sday || !memcmp(day, sday, sizeof sig_day)) && (!smid || !memcmp(midtick, smid, sizeof sig_midtick)) &&   // the test build has no signatures
            eb_hs_add(&hs, day, (void*)hk_day, (void**)&o_day) && eb_hs_add(&hs, midtick, (void*)hk_midtick, (void**)&o_midtick) &&
            eb_hs_enable(&hs))
            eb_day_hooked = 1;
        else eb_log("guard: daily tick / mid-tick frame signature mismatch or hook failed - off");
    }
    if (want_tip && eb_day_hooked && g_rdata_lo && probe_init() &&
        hook(tip, stip, sizeof sig_tip, (void*)hk_tip, (void**)&o_tip, "tooltip")) {
        if (g_cfg.guard_tooltip) { ok++; eb_log("guard: modifier tooltips skip freed or invalid sources (outside the daily tick)"); }
        if (g_ptc_ret)
            eb_log("modifiers: EXPERIMENT province text cache on%s - the monthly province text of a country is built once a day",
                   g_cfg.prov_text_cache == 2 ? " with VERIFY" : g_cfg.prov_text_cache == 3 ? " as A/B (calls alternate)" : "");
    } else { g_ptc_ret = 0; g_cfg.guard_tooltip = 0; }
    if (!g_ptc_ret) g_cfg.prov_text_cache = 0;
    if (g_cfg.guard_relation && hook(rel, srel, sizeof sig_rel, (void*)hk_rel, (void**)&o_rel, "relation")) {
        ok++; eb_log("guard: a missing relation on annexation is skipped instead of crashing");
    } else g_cfg.guard_relation = 0;
#ifndef EB_TEST
    // EU4dll's plugin64.dll loads after this DLL; without it no name has escape bytes and the hook passes all on
    if ((g_cfg.guard_ironman_name || g_cfg.test_ironman_name) &&
        hook(b + RVA_SAVE_PATH, sig_save_path, sizeof sig_save_path, (void*)hk_save_path, (void**)&o_save_path, "ironman save name")) {
        if (g_cfg.guard_ironman_name) { ok++; eb_log("guard: ironman save names in EU4dll's escapes are saved in UTF-8 like ordinary saves"); }
        if (g_cfg.test_ironman_name) eb_log("guard: TEST the ironman save of eb_auto is named \"eb_\" + Korean in EU4dll's escapes%s",
                                            g_cfg.guard_ironman_name ? "" : " (without the fix)");
    } else g_cfg.guard_ironman_name = 0;
#endif
#ifndef EB_TEST
    if ((g_cfg.guard_loc_duplicates || g_cfg.test_loc_fiber) &&
        hook(b + RVA_LOC_DUPS, sig_loc_dups, sizeof sig_loc_dups, (void*)hk_loc_dups, (void**)&o_loc_dups, "duplicate localisation check")) {
        ok++; eb_log("guard: the duplicate localisation check runs on a larger stack when the main thread's would overflow%s",
                     g_cfg.test_loc_fiber == 1 ? " (TEST: always)" : "");
    } else g_cfg.guard_loc_duplicates = 0;
    if ((g_cfg.guard_diplo_reset || g_cfg.test_diplo_freed || g_cfg.test_diplo_removed) && g_rdata_lo && g_text_lo && probe_init() &&
        !memcmp(b + RVA_LOGSET, sig_logset, sizeof sig_logset) && !memcmp(b + RVA_LOGFLUSH, sig_logflush, sizeof sig_logflush) &&
        !memcmp(b + RVA_REM_ACTIVE, sig_rem_active, sizeof sig_rem_active) &&
        hook(b + RVA_DIPLO_CLEAR, sig_diplo_clear, sizeof sig_diplo_clear, (void*)hk_diplo_clear, (void**)&o_diplo_clear, "diplomacy reset")) {
        g_logset = (logset_t)(b + RVA_LOGSET); g_logflush = (logflush_t)(b + RVA_LOGFLUSH); g_rem_active = (rem_active_t)(b + RVA_REM_ACTIVE);
        if (g_cfg.guard_diplo_reset) { ok++; eb_log("guard: the diplomacy reset walks its lists with removals deferred and skips relations freed before or during it"); }
        if (g_cfg.test_diplo_freed) eb_log("guard: TEST the next diplomacy reset finds one relation freed%s", g_cfg.guard_diplo_reset ? "" : " (without the fix)");
        if (g_cfg.test_diplo_removed) eb_log("guard: TEST in the next diplomacy reset one relation removes and deletes the next one%s", g_cfg.guard_diplo_reset ? "" : " (without the fix)");
    } else g_cfg.guard_diplo_reset = 0;
#endif
    if (g_cfg.guard_great_project && g_gp_mgr && g_hc_vt && probe_init()) {
        InitializeCriticalSection(&g_gp_cs);
        // both or neither: the trigger reads are recorded only for the array moves
        if (hook(hc, shc, sizeof sig_hc_read, (void*)hk_hc_read, (void**)&o_hc_read, "has_construction read")) {
            if (hook(gpb, sgpb, sizeof sig_gp_setbuf, (void*)hk_gp_setbuf, (void**)&o_gp_setbuf, "great project array")) {
                ok++; eb_log("guard: has_construction triggers that name a great project follow the great project array when it moves");
            } else { MH_DisableHook(hc); g_cfg.guard_great_project = 0; }
        } else g_cfg.guard_great_project = 0;
    } else g_cfg.guard_great_project = 0;
    return ok;
}

#ifdef EB_TEST
#include <stdio.h>
// the diplomacy reset with the harness's CDiplomacy, its RemActive, its log settings and its FUN_1404e0d90
__declspec(dllexport) void eb_test_diplo_reset(uint8_t* d, void* orig, void* rem, void* logset, void* flush, int guard) {
    o_diplo_clear = (diplo_clear_t)orig; g_rem_active = (rem_active_t)rem;
    g_logset = (logset_t)logset; g_logflush = (logflush_t)flush;
    int saved = g_cfg.guard_diplo_reset; g_cfg.guard_diplo_reset = guard;
    if (!probe_init()) printf("diplo: no probe\n");
    hk_diplo_clear(d);
    g_cfg.guard_diplo_reset = saved;
}
// EU4dll's convertWideTextToEscapedText (escape_tool.cpp) for one UTF-16 code unit
static size_t eu4dll_escape(wchar_t cp, uint8_t* o) {
    static const uint8_t sp[] = { 0xA4,0xA3,0xA7,0x24,0x5B,0x00,0x5C,0x20,0x0D,0x0A,0x22,0x7B,0x7D,0x40,0x80,0x7E,0x2F,0x5F,0xBD,0x3B,0x5D,
                                  0x3D,0x23,0x3F,0x3A,0x3C,0x3E,0x2A,0x7C };
    if (cp > 0x100 && cp < 0xA00) cp = (wchar_t)(cp + 0xE000);
    uint8_t hi = (uint8_t)(cp >> 8), lo = (uint8_t)cp, e = 0x10;
    if (!hi) { o[0] = lo; return 1; }
    if (memchr(sp, hi, sizeof sp)) e += 2;
    if (memchr(sp, lo, sizeof sp)) e++;
    if (e == 0x11 || e == 0x13) lo = (uint8_t)(lo + 14);
    if (e == 0x12 || e == 0x13) hi = (uint8_t)(hi - 9);
    o[0] = e; o[1] = lo; o[2] = hi;
    return 3;
}
__declspec(dllexport) int eb_test_ironman_name(void) {
    int bad = 0;
    uint8_t e[64]; char u[256], want[256];
    for (wchar_t cp = 0xAC00; cp <= 0xD7A3; cp++) {                      // every Hangul syllable, between ASCII
        size_t n = 0;
        e[n++] = 'a'; n += eu4dll_escape(cp, e + n); e[n++] = 'b';
        wchar_t w[3] = { L'a', cp, L'b' };
        int wn = WideCharToMultiByte(CP_UTF8, 0, w, 3, want, sizeof want, NULL, NULL);
        size_t m = iname_utf8(e, n, u, sizeof u);
        if (m != (size_t)wn || memcmp(u, want, m)) { if (bad < 5) printf("ironman name: U+%04X wrong\n", (unsigned)cp); bad++; }
    }
    const wchar_t* names[] = { L"\uC624\uC2A4\uB9CC \uC81C\uAD6D 1444", L"\u5927\u548C", L"\u3042\u3044\u3046" };   // 오스만 제국 1444, 大和, あいう
    for (size_t k = 0; k < sizeof names / sizeof names[0]; k++) {
        size_t n = 0;
        for (const wchar_t* p = names[k]; *p; p++) n += eu4dll_escape(*p, e + n);
        int wn = WideCharToMultiByte(CP_UTF8, 0, names[k], -1, want, sizeof want, NULL, NULL) - 1;
        size_t m = iname_utf8(e, n, u, sizeof u);
        if (m != (size_t)wn || memcmp(u, want, m)) { printf("ironman name %d wrong\n", (int)k); bad++; }
    }
    // report 36's name (the bytes of its error.log line) and names that stay as they are
    const uint8_t r36[] = { 0x11, '2', 0xC6, 0x11, 0xB2, 0xC2, 0x10, 0xCC, 0xB9 };
    wchar_t w36[3] = { 0xC624, 0xC2A4, 0xB9CC };                         // 오스만
    int wn = WideCharToMultiByte(CP_UTF8, 0, w36, 3, want, sizeof want, NULL, NULL);
    size_t m = iname_utf8(r36, sizeof r36, u, sizeof u);
    if (m != (size_t)wn || memcmp(u, want, m)) { printf("ironman name: report 36 wrong\n"); bad++; }
    bad += iname_utf8((const uint8_t*)"Ottomans 1444", 13, u, sizeof u) != 0;
    bad += iname_utf8((const uint8_t*)"\xEC\x98\xA4\xEC\x8A\xA4", 6, u, sizeof u) != 0;          // UTF-8 already
    // the stack the duplicate localisation check takes: keys 1 1 2 3 3 3 5 -> runs of 2 and 3
    static uint64_t keys[7][2] = { {1,0}, {1,0}, {2,0}, {3,0}, {3,0}, {3,0}, {5,0} };
    uint8_t table[0x30] = {0};
    *(uint64_t**)(table + 0x20) = &keys[0][0]; *(uint64_t**)(table + 0x28) = &keys[7][0];
    size_t want_need = 3 * ((8 * 2 + 8 + 15) & ~15) + 3 * ((8 * 3 + 8 + 15) & ~15);
    if (loc_dups_need(table) != want_need) { printf("loc dups need %zu, want %zu\n", loc_dups_need(table), want_need); bad++; }
    *(uint64_t**)(table + 0x28) = &keys[0][0]; bad += loc_dups_need(table) != 0;
    printf("ironman name %s (%d failures)\n", bad ? "FAILED" : "ALL TESTS PASSED", bad);
    return bad;
}
#endif

void eb_guard_report(void) {
    if (g_ptc_ret) ptc_report();
    if (g_nhc || g_gp_moved)
        eb_log("guard: %d has_construction triggers name a great project, the array moved %d times, %d trigger addresses moved with it",
               g_nhc + g_hc_lost, g_gp_moves, g_gp_moved);
    if (g_dr_resets)
        eb_log("guard: diplomacy resets: %lld, relations ended %lld, removed during a reset and skipped %lld, freed ones skipped %ld",
               (long long)g_dr_resets, (long long)g_dr_calls, (long long)g_dr_pending, (long)g_freed_relations);
    if (g_bad_containers || g_bad_sources || g_null_relations || g_freed_relations)
        eb_log("guard: %ld invalid tooltip containers, %ld invalid tooltip sources, %ld missing relations, %ld freed relations skipped",
               (long)g_bad_containers, (long)g_bad_sources, (long)g_null_relations, (long)g_freed_relations);
}
