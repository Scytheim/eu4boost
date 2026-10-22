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
// relation ([guard] relation): when a country is annexed, FUN_140a61900 (relation.cpp) looks up the
// active relation to remove and calls FUN_140a5e130 with NULL when it is missing, right after
// FUN_1404e1570 logged "!pRelation CDiplomacy::RemActive"; FUN_140a5e130 then dereferences it (crash
// 21:34, a peace deal annexing a country in a personal union). The other call site already tests the
// relation for NULL after this call, so a NULL relation is skipped.
#include "common.h"
#include <stdlib.h>
#include <string.h>
#include "MinHook.h"

#define RVA_TIP 0x8e8ca0u    // modifier breakdown text (container, std::string* out, descriptor)
#define RVA_REL 0xa5e130u    // remove an active relation (relation)
static const uint8_t sig_tip[24] = { 0x48,0x89,0x54,0x24,0x10,0x48,0x89,0x4c,0x24,0x08,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d };
static const uint8_t sig_rel[24] = { 0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0x48,0x8b,0x0d,0x38,0x1d,0x8e,0x01,0x48,0x81,0xc1,0xb8,0x1b,0x00,0x00,0x48 };

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

static uint64_t hk_day(void* gs) { g_sim++; uint64_t r = o_day(gs); g_sim--; return r; }
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

static void* hk_tip(uint8_t* c, uint64_t* out, uint8_t* desc) {
    if (g_sim) return o_tip(c, out, desc);
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

static int hook(uint8_t* t, const uint8_t* sig, size_t n, void* fn, void** orig, const char* what) {
    if (!t) { eb_log("guard: %s target not found", what); return 0; }
    if (sig && memcmp(t, sig, n)) { eb_log("guard: %s signature mismatch - not installed", what); return 0; }
    if (MH_CreateHook(t, fn, orig) != MH_OK || MH_EnableHook(t) != MH_OK) { eb_log("guard: %s hook failed", what); return 0; }
    return 1;
}

int eb_guard_install(void) {
    if (!g_cfg.guard_tooltip && !g_cfg.guard_relation) return 1;
    uint8_t* b = (uint8_t*)g_exe;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(b + ((IMAGE_DOS_HEADER*)g_exe)->e_lfanew);
#ifdef EB_TEST
    uint8_t* tip = (uint8_t*)GetProcAddress(g_exe, "t_tip"); uint8_t* rel = (uint8_t*)GetProcAddress(g_exe, "t_rel");
    uint8_t* day = (uint8_t*)GetProcAddress(g_exe, "t_day"); uint8_t* midtick = (uint8_t*)GetProcAddress(g_exe, "t_midtick");
    const uint8_t *stip = NULL, *srel = NULL, *sday = NULL, *smid = NULL;
#else
    if (nt->FileHeader.TimeDateStamp != 0x66FE6AB9u) { eb_log("guard: unknown eu4.exe - not installed"); return 0; }
    uint8_t* tip = b + RVA_TIP; uint8_t* rel = b + RVA_REL; uint8_t* day = b + RVA_DAY; uint8_t* midtick = b + RVA_MIDTICK;
    const uint8_t *stip = sig_tip, *srel = sig_rel, *sday = sig_day, *smid = sig_midtick;
#endif
    IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!memcmp(s[i].Name, ".rdata", 7)) { g_rdata_lo = (uintptr_t)b + s[i].VirtualAddress; g_rdata_hi = g_rdata_lo + s[i].Misc.VirtualSize; }
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) { eb_log("guard: MinHook init failed"); return 0; }
    int ok = 0;
    // the tooltip guard needs the daily tick hooks: without them every province would be checked monthly
    if (g_cfg.guard_tooltip && g_rdata_lo && day && midtick && AddVectoredExceptionHandler(1, probe_veh) &&
        hook(day, sday, sizeof sig_day, (void*)hk_day, (void**)&o_day, "daily tick") &&
        hook(midtick, smid, sizeof sig_midtick, (void*)hk_midtick, (void**)&o_midtick, "mid-tick frame") &&
        hook(tip, stip, sizeof sig_tip, (void*)hk_tip, (void**)&o_tip, "tooltip")) {
        ok++; eb_log("guard: modifier tooltips skip freed or invalid sources (outside the daily tick)");
    }
    if (g_cfg.guard_relation && hook(rel, srel, sizeof sig_rel, (void*)hk_rel, (void**)&o_rel, "relation")) {
        ok++; eb_log("guard: a missing relation on annexation is skipped instead of crashing");
    }
    return ok;
}

void eb_guard_report(void) {
    if (g_bad_containers || g_bad_sources || g_null_relations)
        eb_log("guard: %ld invalid tooltip containers, %ld invalid tooltip sources, %ld missing relations",
               (long)g_bad_containers, (long)g_bad_sources, (long)g_null_relations);
}
