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
#include "common.h"
#include <stdlib.h>
#include <string.h>
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
    g_sim++; g_day_n++; eb_in_day++; uint64_t r = o_day(gs); eb_in_day--; g_sim--;
    eb_day_ms = GetTickCount64();
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

int eb_guard_install(void) {
    int want_day_time = g_cfg.online_poll_ms > 0 && g_cfg.online_running_only;
    if (!g_cfg.guard_tooltip && !g_cfg.guard_relation && !g_cfg.guard_great_project && !g_cfg.prov_text_cache && !want_day_time) return 1;
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
    if ((want_tip || want_day_time) && day && midtick &&
        hook(day, sday, sizeof sig_day, (void*)hk_day, (void**)&o_day, "daily tick") &&
        hook(midtick, smid, sizeof sig_midtick, (void*)hk_midtick, (void**)&o_midtick, "mid-tick frame"))
        eb_day_hooked = 1;
    if (want_tip && eb_day_hooked && g_rdata_lo && probe_init() &&
        hook(tip, stip, sizeof sig_tip, (void*)hk_tip, (void**)&o_tip, "tooltip")) {
        if (g_cfg.guard_tooltip) { ok++; eb_log("guard: modifier tooltips skip freed or invalid sources (outside the daily tick)"); }
        if (g_ptc_ret)
            eb_log("modifiers: EXPERIMENT province text cache on%s - the monthly province text of a country is built once a day",
                   g_cfg.prov_text_cache == 2 ? " with VERIFY" : g_cfg.prov_text_cache == 3 ? " as A/B (calls alternate)" : "");
    } else g_ptc_ret = 0;
    if (g_cfg.guard_relation && hook(rel, srel, sizeof sig_rel, (void*)hk_rel, (void**)&o_rel, "relation")) {
        ok++; eb_log("guard: a missing relation on annexation is skipped instead of crashing");
    }
    if (g_cfg.guard_great_project && g_gp_mgr && g_hc_vt && probe_init()) {
        InitializeCriticalSection(&g_gp_cs);
        // both or neither: the trigger reads are recorded only for the array moves
        if (hook(hc, shc, sizeof sig_hc_read, (void*)hk_hc_read, (void**)&o_hc_read, "has_construction read")) {
            if (hook(gpb, sgpb, sizeof sig_gp_setbuf, (void*)hk_gp_setbuf, (void**)&o_gp_setbuf, "great project array")) {
                ok++; eb_log("guard: has_construction triggers that name a great project follow the great project array when it moves");
            } else MH_DisableHook(hc);
        }
    }
    return ok;
}

void eb_guard_report(void) {
    if (g_ptc_ret) ptc_report();
    if (g_nhc || g_gp_moved)
        eb_log("guard: %d has_construction triggers name a great project, the array moved %d times, %d trigger addresses moved with it",
               g_nhc + g_hc_lost, g_gp_moves, g_gp_moved);
    if (g_bad_containers || g_bad_sources || g_null_relations)
        eb_log("guard: %ld invalid tooltip containers, %ld invalid tooltip sources, %ld missing relations",
               (long)g_bad_containers, (long)g_bad_sources, (long)g_null_relations);
}
