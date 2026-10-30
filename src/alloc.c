// Replace eu4.exe's statically linked UCRT heap functions with mimalloc.
//
// eu4.exe 1.37.5 links the CRT statically; every operator new/delete, std::string,
// container and malloc in the game funnels into these five/six functions, which in
// turn call HeapAlloc/HeapFree on the process heap. We redirect them to mimalloc,
// which has per-thread heaps (no shared lock) and is generally faster for the
// small-object churn of the Clausewitz engine.
//
// Safety:
//  * every target is verified byte-for-byte before patching (wrong game version -> no patch)
//  * pointers that mimalloc does not own (allocated before the patch, or by another DLL)
//    are handed back to the Windows heap
//  * optional guard: if another module frees a game pointer with HeapFree (both used to
//    live on the process heap), the ntdll heap entry points pass it to mimalloc instead
#include "common.h"
#include <string.h>
#include <stdio.h>
#include <wchar.h>
#include <mimalloc.h>
#include "MinHook.h"
#include <psapi.h>

typedef struct {
    const char* name;
    uint32_t rva;
    uint8_t sig[24];
    void* hook;
} crt_target_t;

// eu4.exe "EU4 v1.37.5.0 Inca", PE timestamp 0x66FE6AB9, SizeOfImage 0x25E0000
#define EU4_TIMESTAMP   0x66FE6AB9u
#define EU4_IMAGESIZE   0x25E0000u
#define RVA_ACRT_HEAP   0x233d850u      // __acrt_heap (HANDLE) used by the CRT functions below

static HANDLE g_heap;                   // process heap (foreign pointers)
static volatile LONG64 g_foreign_free, g_foreign_realloc, g_guard_hits;

static inline int ours(const void* p) { return mi_is_in_heap_region(p); }

// EU4dll (plugins\plugin64.dll, "Font buffer heap zero clear") patches the HeapAlloc call
// inside _malloc_base to pass HEAP_ZERO_MEMORY, so with EU4dll every game malloc returns
// zeroed memory and its multibyte font code depends on that. Our entry hook bypasses that
// call site, so we must zero ourselves when EU4dll is present.
// EU4dll's own source (Plugin64/font.cpp) shows why: it enlarges the bitmap-font allocation
// from 0x3D88 to 0x103D88 bytes ("Font buffer expansion") and relies on the extra glyph table
// being zero. Zeroing every allocation cost ~2% of the main thread in a 5x-speed profile, so by
// default (mode 2) only allocations of at least zero_fill_min_bytes (64 KB) are zeroed.
// Zeroed allocations of at least zero_fill_windows_kb (1 MB) come from the Windows heap with
// HEAP_ZERO_MEMORY - exactly what the game's malloc does with EU4dll. Blocks that big are fresh pages
// from the system, zero already, and only the pages the game touches are ever faulted in; mimalloc
// would reuse a freed block and write zeros over all of it. The save list reads every save's meta
// through a ~68 MB buffer (FUN_1405cd120), so 0.4.0 wrote ~68 MB of zeros per save file: 7 ms per file
// whenever a list is built (save dialog, load screen, twice after deleting; users reported all three).
// To mimalloc these blocks are foreign, and free/realloc/msize already hand those to the Windows heap.
static int g_zero_malloc;      // 0 off, 1 all, 2 large only
static size_t g_zero_min = 65536, g_zero_os;
static volatile LONG64 g_zeroed, g_zeroed_bytes, g_zeroed_os;

static void* __cdecl hk_malloc(size_t n) {
    void* p;
    if (g_zero_malloc && (g_zero_malloc == 1 || n >= g_zero_min)) {
        if (g_zero_os && n >= g_zero_os) { g_zeroed_os++; p = HeapAlloc(g_heap, HEAP_ZERO_MEMORY, n ? n : 1); }
        else { g_zeroed++; g_zeroed_bytes += n; p = mi_zalloc(n); }
    } else p = mi_malloc(n);
    if (eb_md_on) eb_md_alloc(p, n);
    return p;
}
static void* __cdecl hk_calloc(size_t count, size_t size) {
    void* p = mi_calloc(count, size);
    if (eb_md_on && p) eb_md_alloc(p, count * size);
    return p;
}
static void __cdecl hk_free(void* p) {
    if (p == NULL) return;
    if (eb_md_on) eb_md_free(p);
    if (ours(p)) { mi_free(p); return; }
    InterlockedIncrement64(&g_foreign_free);
    HeapFree(g_heap, 0, p);
}
static void* realloc_in(void* p, size_t n);
static void* __cdecl hk_realloc(void* p, size_t n) {
    if (!eb_md_on) return realloc_in(p, n);
    if (p && n == 0) { hk_free(p); return NULL; }
    void* q = realloc_in(p, n);
    if (q) { if (p) eb_md_free(p); eb_md_alloc(q, n); }
    return q;
}
static void* realloc_in(void* p, size_t n) {
    if (p == NULL) return mi_malloc(n);
    if (n == 0) { hk_free(p); return NULL; }
    if (ours(p)) return mi_realloc(p, n);
    // block from the Windows heap: move it into mimalloc
    InterlockedIncrement64(&g_foreign_realloc);
    SIZE_T old = HeapSize(g_heap, 0, p);
    if (old == (SIZE_T)-1) return NULL;
    void* q = mi_malloc(n);
    if (q == NULL) return NULL;
    memcpy(q, p, old < n ? old : n);
    HeapFree(g_heap, 0, p);
    return q;
}
static size_t __cdecl hk_msize(void* p) {
    if (p == NULL) return (size_t)-1;
    if (ours(p)) return mi_usable_size(p);
    return HeapSize(g_heap, 0, p);
}
static void* __cdecl hk_recalloc(void* p, size_t count, size_t size) {
    if (p == NULL || ours(p)) {
        void* q = mi_recalloc(p, count, size);
        if (eb_md_on && q) { if (p) eb_md_free(p); eb_md_alloc(q, count * size); }
        return q;
    }
    // foreign block: emulate _recalloc semantics
    if (size && count > ((size_t)-1) / size) return NULL;
    size_t n = count * size;
    SIZE_T old = HeapSize(g_heap, 0, p);
    void* q = hk_realloc(p, n);
    if (q && old != (SIZE_T)-1 && n > old) memset((char*)q + old, 0, n - old);
    return q;
}

static crt_target_t g_targets[] = {
    { "_malloc_base",   0x1a6837c, { 0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0x48,0x83,0xf9,0xe0,0x77,0x3c,0x48,0x85,0xc9,0xb8,0x01,0x00,0x00,0x00,0x48 }, (void*)hk_malloc },
    { "_calloc_base",   0x1a6989c, { 0x40,0x53,0x48,0x83,0xec,0x20,0x4c,0x8b,0xc2,0x48,0x8b,0xd9,0x48,0x85,0xc9,0x74,0x0e,0x33,0xd2,0x48,0x8d,0x42,0xe0,0x48 }, (void*)hk_calloc },
    { "_free_base",     0x1a6833c, { 0x48,0x85,0xc9,0x74,0x37,0x53,0x48,0x83,0xec,0x20,0x4c,0x8b,0xc1,0x33,0xd2,0x48,0x8b,0x0d,0xfe,0x54,0x8d,0x00,0xff,0x15 }, (void*)hk_free },
    { "_realloc_base",  0x1a6dd98, { 0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xda,0x48,0x8b,0xf9,0x48,0x85,0xc9,0x75,0x0a,0x48,0x8b,0xca }, (void*)hk_realloc },
    { "_msize_base",    0x1a706f4, { 0x48,0x83,0xec,0x28,0x48,0x85,0xc9,0x75,0x19,0xe8,0xd6,0x7c,0xfe,0xff,0xc7,0x00,0x16,0x00,0x00,0x00,0xe8,0x3f,0x15,0xfd }, (void*)hk_msize },
    { "_recalloc_base", 0x1a67bf8, { 0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xec,0x20,0x49,0x8b,0xe8,0x48 }, (void*)hk_recalloc },
};
#define NTARGETS (sizeof g_targets / sizeof g_targets[0])

// ---- ntdll guard -------------------------------------------------------------------------
typedef BOOLEAN (NTAPI *RtlFreeHeap_t)(PVOID, ULONG, PVOID);
typedef SIZE_T  (NTAPI *RtlSizeHeap_t)(PVOID, ULONG, PVOID);
typedef PVOID   (NTAPI *RtlReAllocateHeap_t)(PVOID, ULONG, PVOID, SIZE_T);
static RtlFreeHeap_t o_RtlFreeHeap;
static RtlSizeHeap_t o_RtlSizeHeap;
static RtlReAllocateHeap_t o_RtlReAllocateHeap;

static BOOLEAN NTAPI g_RtlFreeHeap(PVOID heap, ULONG flags, PVOID p) {
    if (p != NULL && ours(p)) { InterlockedIncrement64(&g_guard_hits); if (eb_md_on) eb_md_free(p); mi_free(p); return TRUE; }
    return o_RtlFreeHeap(heap, flags, p);
}
static SIZE_T NTAPI g_RtlSizeHeap(PVOID heap, ULONG flags, PVOID p) {
    if (p != NULL && ours(p)) { InterlockedIncrement64(&g_guard_hits); return mi_usable_size(p); }
    return o_RtlSizeHeap(heap, flags, p);
}
static PVOID NTAPI g_RtlReAllocateHeap(PVOID heap, ULONG flags, PVOID p, SIZE_T n) {
    if (p != NULL && ours(p)) {
        InterlockedIncrement64(&g_guard_hits);
        size_t old = mi_usable_size(p);
        void* q;
        if (flags & HEAP_REALLOC_IN_PLACE_ONLY) q = mi_expand(p, n);
        else q = mi_realloc(p, n ? n : 1);
        if (q && (flags & HEAP_ZERO_MEMORY) && n > old) memset((char*)q + old, 0, n - old);
        if (q && eb_md_on) { eb_md_free(p); eb_md_alloc(q, n); }
        return q;
    }
    return o_RtlReAllocateHeap(heap, flags, p, n);
}

static void mi_out(const char* msg, void* arg) {
    (void)arg;
    char line[512]; size_t n = strlen(msg);
    if (n >= sizeof line) n = sizeof line - 1;
    memcpy(line, msg, n); line[n] = 0;
    while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
    if (n) eb_log("mimalloc: %s", line);
}

static int verify_exe(uint8_t* base) {
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != EU4_TIMESTAMP || nt->OptionalHeader.SizeOfImage != EU4_IMAGESIZE) {
        eb_log("allocator: unexpected eu4.exe (timestamp %08lx, size %08lx); expected EU4 1.37.5.0 (66fe6ab9 / 025e0000)",
               (unsigned long)nt->FileHeader.TimeDateStamp, (unsigned long)nt->OptionalHeader.SizeOfImage);
        return 0;
    }
    for (size_t i = 0; i < NTARGETS; i++) {
        if (memcmp(base + g_targets[i].rva, g_targets[i].sig, sizeof g_targets[i].sig) != 0) {
            eb_log("allocator: signature mismatch at %s (rva %07x) - already patched by something else?", g_targets[i].name, g_targets[i].rva);
            return 0;
        }
    }
    return 1;
}

int eb_alloc_install(void) {
    uint8_t* base = (uint8_t*)g_exe;
#ifdef EB_TEST
    // test build: hook the test harness' exported stand-ins instead of eu4.exe's CRT
    static const char* tnames[] = { "t_malloc", "t_calloc", "t_free", "t_realloc", "t_msize", "t_recalloc" };
    for (size_t i = 0; i < NTARGETS; i++) {
        uint8_t* p = (uint8_t*)GetProcAddress(g_exe, tnames[i]);
        if (!p) { eb_log("test: missing %s", tnames[i]); return 0; }
        g_targets[i].rva = (uint32_t)(p - base);
    }
#else
    if (!verify_exe(base)) return 0;
#endif

    g_heap = GetProcessHeap();
#ifdef EB_TEST
    HANDLE crt_heap = NULL;
#else
    HANDLE crt_heap = *(HANDLE*)(base + RVA_ACRT_HEAP);
#endif
    eb_log("allocator: eu4 CRT heap %s at patch time", crt_heap ? "ALREADY initialized (late load; foreign blocks will be handled)" : "not yet initialized (early load, ideal)");

    // zero-fill mode: config 1 = always, 0 = never, -1 (auto) = when EU4dll's plugin64.dll is installed
    g_zero_malloc = g_cfg.alloc_zero_fill;
    if (g_zero_malloc < 0) {
        wchar_t p64[MAX_PATH];
        _snwprintf(p64, MAX_PATH, L"%lsplugin64.dll", g_dir);
        int eu4dll = GetFileAttributesW(p64) != INVALID_FILE_ATTRIBUTES || GetModuleHandleW(L"plugin64.dll") != NULL;
        g_zero_malloc = eu4dll ? 2 : 0;
    }
    if (g_cfg.zero_min_bytes > 0) g_zero_min = (size_t)g_cfg.zero_min_bytes;
    if (g_zero_malloc == 2 && g_zero_min > 0x103D88) g_zero_min = 0x103D88;   // never miss EU4dll's font buffer
    if (g_zero_malloc && g_cfg.zero_os_kb > 0) g_zero_os = (size_t)g_cfg.zero_os_kb << 10;
    if (g_zero_malloc == 1) eb_log("allocator: zero-filled malloc ON for every allocation");
    else if (g_zero_malloc == 2) eb_log("allocator: zero-filled malloc ON for allocations >= %zu bytes (EU4dll font buffer)", g_zero_min);
    if (g_zero_os) eb_log("allocator: zero-filled blocks >= %zu KB come from the Windows heap (fresh zero pages, as in the game)", g_zero_os >> 10);
    else eb_log("allocator: zero-filled malloc off");

    mi_register_output(mi_out, NULL);
    mi_option_set(mi_option_show_errors, 1);
    mi_option_set(mi_option_purge_delay, g_cfg.alloc_purge_delay_ms);
    if (g_cfg.alloc_large_pages) {
        mi_option_set(mi_option_allow_large_os_pages, 1);
        eb_log("allocator: large OS pages requested");
    }
    // mimalloc reads allow_large_os_pages once, in its process init (normally lazy, on the first
    // allocation). Run it now, after the options are set and before the reservation below.
    mi_process_init();
    if (g_cfg.alloc_reserve_mb > 0) {
        size_t bytes = (size_t)g_cfg.alloc_reserve_mb << 20;
        int err = mi_reserve_os_memory(bytes, g_cfg.alloc_large_pages ? true : false, g_cfg.alloc_large_pages ? true : false);
        eb_log("allocator: reserve %d MB%s -> %s", g_cfg.alloc_reserve_mb, g_cfg.alloc_large_pages ? " (large pages)" : "", err == 0 ? "ok" : "FAILED (falling back to normal pages)");
        if (err != 0 && g_cfg.alloc_large_pages)
            eb_log("allocator: large pages need the 'Lock pages in memory' user right (secpol.msc) and a sign-out/sign-in");
    }

    if (g_cfg.alloc_large_pages) {
        // Ask Windows whether a freshly allocated block really sits on a large page.
        unsigned char* probe = (unsigned char*)mi_malloc(256 << 10);
        if (probe) {
            probe[0] = 1;
            PSAPI_WORKING_SET_EX_INFORMATION wi; memset(&wi, 0, sizeof wi); wi.VirtualAddress = probe;
            if (QueryWorkingSetEx(GetCurrentProcess(), &wi, sizeof wi) && wi.VirtualAttributes.Valid)
                eb_log("allocator: large pages %s", wi.VirtualAttributes.LargePage ? "IN USE (2 MB pages)"
                       : "NOT in use - grant 'Lock pages in memory' to your account (secpol.msc) and sign out/in");
            mi_free(probe);
        }
    }

    eb_md_init();                                // memory growth diagnostics, before the first hooked allocation
    if (MH_Initialize() != MH_OK) { eb_log("allocator: MinHook init failed"); return 0; }

    // guard first, so nothing can slip through between the two steps
    if (g_cfg.alloc_guard_foreign) {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        struct { const char* n; void* hook; void** orig; } g[] = {
            { "RtlFreeHeap",       (void*)g_RtlFreeHeap,       (void**)&o_RtlFreeHeap },
            { "RtlSizeHeap",       (void*)g_RtlSizeHeap,       (void**)&o_RtlSizeHeap },
            { "RtlReAllocateHeap", (void*)g_RtlReAllocateHeap, (void**)&o_RtlReAllocateHeap },
        };
        int ok = 1;
        for (int i = 0; i < 3; i++) {
            void* t = (void*)GetProcAddress(ntdll, g[i].n);
            if (!t || MH_CreateHook(t, g[i].hook, g[i].orig) != MH_OK) { ok = 0; eb_log("allocator: guard hook %s failed", g[i].n); break; }
        }
        if (!ok) { MH_RemoveHook(MH_ALL_HOOKS); MH_Uninitialize(); return 0; }
    }
    for (size_t i = 0; i < NTARGETS; i++) {
        void* tramp = NULL;
        MH_STATUS s = MH_CreateHook(base + g_targets[i].rva, g_targets[i].hook, &tramp);
        if (s != MH_OK) {
            eb_log("allocator: hook %s failed (%s)", g_targets[i].name, MH_StatusToString(s));
            MH_RemoveHook(MH_ALL_HOOKS); MH_Uninitialize();
            return 0;
        }
    }
    MH_STATUS s = MH_EnableHook(MH_ALL_HOOKS);   // MinHook suspends other threads while patching
    if (s != MH_OK) { eb_log("allocator: enable failed (%s)", MH_StatusToString(s)); return 0; }
    eb_log("allocator: %u CRT entry points redirected%s", (unsigned)NTARGETS, g_cfg.alloc_guard_foreign ? ", heap guard on" : "");
    eb_md_start();
    return 1;
}

// uitime.c: mallocs zero-filled by mimalloc so far and their bytes (statistics, not exact across threads)
void eb_alloc_zero_stats(LONG64* n, LONG64* bytes) { *n = g_zeroed; *bytes = g_zeroed_bytes; }

// mimalloc's own thread-exit hook for a static library is MSVC-only (TLS callbacks placed with #pragma const_seg,
// which clang ignores), so a thread that ended kept its heap: its pages stayed committed, emptied or not, and no
// other thread could use them (~0.85 MB per thread). savezip.c starts its compression workers for every saved
// entry: ~15 MB per save, +1.9 GB in 1645 -> 1676 with quarterly autosaves (2026-10-29). DllMain calls this on
// DLL_THREAD_DETACH; mimalloc returns at once for a thread that never allocated.
static volatile LONG64 g_threads_done;
void eb_alloc_thread_done(void) { InterlockedIncrement64(&g_threads_done); mi_thread_done(); }

void eb_alloc_report(void) {
    eb_log("allocator: foreign frees %lld, foreign reallocs %lld, guard hits %lld, zero-filled mallocs %lld (+ %lld from the Windows heap), ended threads cleaned up %lld",
           (long long)g_foreign_free, (long long)g_foreign_realloc, (long long)g_guard_hits, (long long)g_zeroed, (long long)g_zeroed_os,
           (long long)g_threads_done);
    size_t elapsed, user, sys, rss, peak_rss, commit, peak_commit, faults;
    mi_process_info(&elapsed, &user, &sys, &rss, &peak_rss, &commit, &peak_commit, &faults);
    eb_log("process: peak working set %zu MB, peak commit %zu MB, cpu user %zu s, sys %zu s",
           peak_rss >> 20, peak_commit >> 20, user / 1000, sys / 1000);
}
