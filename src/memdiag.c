// Memory growth diagnostics ([diag] mem_sites=N, off by default): which code of the game holds the
// memory that keeps growing over a long session.
//
// Every block the hooked CRT functions (alloc.c) hand out whose address falls in 1 of N hash buckets
// is a sample: its call stack (up to 6 frames outside this DLL) and size go into a table, and its
// free takes it out again. Choosing samples by address keeps the free side cheap: one multiply
// decides, and only sampled frees look into the table. Every mem_sites_s seconds a snapshot is
// appended to plugins\eu4boost_memsites.tsv: the process totals and the call sites that hold the
// most sampled live bytes (estimates = sampled x N). A site whose live bytes keep rising from one
// snapshot to the next is where the memory piles up; tools/analysis/memsites.py compares snapshots.
#include "common.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <psapi.h>
#include <mimalloc.h>

#define NSH     64                         // shards of the table of sampled live blocks
#define SH_CAP  (1u << 15)                 // blocks per shard (2M in all)
#define NSITE   (1u << 16)                 // call sites
#define NFR     6                          // frames per call site

typedef struct { uintptr_t p; uint32_t site, size; } blk_t;
typedef struct { SRWLOCK lock; blk_t* e; uint32_t n; } shard_t;
typedef struct { volatile LONG64 key; uintptr_t fr[NFR]; volatile LONG64 live, live_n, allocs, bytes; } site_t;

int eb_md_on;
static uint64_t g_thresh;                  // a block is sampled when the hash of its address is below this
static shard_t g_sh[NSH];
static site_t* g_site;
static volatile LONG64 g_full, g_nosite, g_samples;
static uintptr_t g_self_lo, g_self_hi;

static inline uint64_t addr_hash(uintptr_t a) { return (uint64_t)(a >> 4) * 0x9E3779B97F4A7C15ull; }
static inline uint32_t slot_of(uintptr_t a) { return (uint32_t)(((uint64_t)(a >> 4) * 0xff51afd7ed558ccdull) >> 40) & (SH_CAP - 1); }

static uint32_t site_of(const uintptr_t* fr, int n) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < n; i++) { h ^= fr[i]; h *= 1099511628211ull; }
    h |= 1;
    uint32_t i = (uint32_t)(h >> 40) & (NSITE - 1);
    for (uint32_t k = 0; k < NSITE; k++, i = (i + 1) & (NSITE - 1)) {
        LONG64 key = g_site[i].key;
        if (key == (LONG64)h) return i;
        if (key == 0) {
            if (InterlockedCompareExchange64(&g_site[i].key, (LONG64)h, 0) == 0) { memcpy(g_site[i].fr, fr, (size_t)n * sizeof *fr); return i; }
            if (g_site[i].key == (LONG64)h) return i;
        }
    }
    InterlockedIncrement64(&g_nosite);
    return NSITE;
}

void eb_md_alloc(void* p, size_t n) {
    uintptr_t a = (uintptr_t)p;
    if (!a || addr_hash(a) >= g_thresh || n > 0xffffffffu) return;
    void* raw[NFR + 8]; USHORT got = RtlCaptureStackBackTrace(1, NFR + 8, raw, NULL);
    uintptr_t fr[NFR]; int k = 0;
    memset(fr, 0, sizeof fr);
    for (int i = 0; i < got && k < NFR; i++) {
        uintptr_t x = (uintptr_t)raw[i];
        if (!k && x >= g_self_lo && x < g_self_hi) continue;   // our hooks
        fr[k++] = x;
    }
    uint32_t s = site_of(fr, k);
    if (s >= NSITE) return;
    shard_t* sh = &g_sh[(a >> 4) & (NSH - 1)];
    int kept = 0;
    AcquireSRWLockExclusive(&sh->lock);
    if (sh->n < SH_CAP / 4 * 3) {
        uint32_t i = slot_of(a);
        while (sh->e[i].p && sh->e[i].p != a) i = (i + 1) & (SH_CAP - 1);
        if (!sh->e[i].p) sh->n++;
        else {                                                 // a block whose free we never saw
            InterlockedAdd64(&g_site[sh->e[i].site].live, -(LONG64)sh->e[i].size);
            InterlockedDecrement64(&g_site[sh->e[i].site].live_n);
        }
        sh->e[i].p = a; sh->e[i].site = s; sh->e[i].size = (uint32_t)n;
        kept = 1;
    } else InterlockedIncrement64(&g_full);
    ReleaseSRWLockExclusive(&sh->lock);
    site_t* st = &g_site[s];
    if (kept) { InterlockedAdd64(&st->live, (LONG64)n); InterlockedIncrement64(&st->live_n); }
    InterlockedIncrement64(&st->allocs); InterlockedAdd64(&st->bytes, (LONG64)n);
    InterlockedIncrement64(&g_samples);
}

void eb_md_free(void* p) {
    uintptr_t a = (uintptr_t)p;
    if (!a || addr_hash(a) >= g_thresh) return;
    shard_t* sh = &g_sh[(a >> 4) & (NSH - 1)];
    uint32_t site = NSITE, size = 0;
    AcquireSRWLockExclusive(&sh->lock);
    uint32_t i = slot_of(a);
    while (sh->e[i].p && sh->e[i].p != a) i = (i + 1) & (SH_CAP - 1);
    if (sh->e[i].p) {
        site = sh->e[i].site; size = sh->e[i].size;
        for (uint32_t j = i;;) {                               // linear probing: shift the cluster back
            j = (j + 1) & (SH_CAP - 1);
            if (!sh->e[j].p) break;
            uint32_t k = slot_of(sh->e[j].p);
            if (i <= j ? (i < k && k <= j) : (i < k || k <= j)) continue;
            sh->e[i] = sh->e[j]; i = j;
        }
        sh->e[i].p = 0; sh->n--;
    }
    ReleaseSRWLockExclusive(&sh->lock);
    if (site < NSITE) { InterlockedAdd64(&g_site[site].live, -(LONG64)size); InterlockedDecrement64(&g_site[site].live_n); }
}

// ---- snapshots ----
static void fmt_frame(char* out, size_t n, uintptr_t x) {
    HMODULE h = NULL; char name[MAX_PATH] = "?";
    if (!x) { snprintf(out, n, "-"); return; }
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)x, &h) && h) {
        if (h == g_exe) { snprintf(out, n, "eu4+%llx", (unsigned long long)(x - (uintptr_t)h)); return; }
        wchar_t w[MAX_PATH]; DWORD k = GetModuleFileNameW(h, w, MAX_PATH);
        const wchar_t* s = w + k; while (s > w && s[-1] != L'\\') s--;
        WideCharToMultiByte(CP_UTF8, 0, s, -1, name, sizeof name, NULL, NULL);
        snprintf(out, n, "%s+%llx", name, (unsigned long long)(x - (uintptr_t)h));
    } else snprintf(out, n, "%llx", (unsigned long long)x);
}
static int readable(const void* p) {
    MEMORY_BASIC_INFORMATION mi;
    return p && VirtualQuery(p, &mi, sizeof mi) && mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}
static int game_date(char* out, size_t n) {                     // gamestate date as y.m.d (speed.c)
    uint8_t* const* slot = (uint8_t* const*)((uint8_t*)g_exe + 0x233fe78u);
    uint8_t* gs = readable(slot) ? *(uint8_t* volatile const*)slot : NULL;
    if (!gs || !readable(gs + 0x1dd0)) return snprintf(out, n, "-");
    int hours = *(volatile int*)(gs + 0x1dd0) - 43800000, days = hours / 24, y = days / 365, d = days - y * 365, m = 0;
    static const int ml[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    while (m < 11 && d >= ml[m]) d -= ml[m++];
    return snprintf(out, n, "%d.%02d.%02d", y, m + 1, d + 1);
}
// where the committed private memory is: mimalloc's own regions, the Windows heaps, and the rest by
// allocation (the biggest blocks are listed so that a growing or a new one can be followed)
typedef struct { uintptr_t base; uint64_t commit, reserve; } ab_t;
static ab_t g_ab[8192];
static int by_commit(const void* a, const void* b) {
    uint64_t x = ((const ab_t*)a)->commit, y = ((const ab_t*)b)->commit;
    return x < y ? 1 : x > y ? -1 : 0;
}
typedef struct { SIZE_T cb, cbAllocated, cbCommitted, cbReserved, cbMaxReserve; } heap_summary_t;
typedef BOOL (WINAPI *heap_summary_fn)(HANDLE, DWORD, heap_summary_t*);
static void regions(FILE* f) {
    uint64_t mi = 0, other = 0, mapped = 0, image = 0; int nab = 0;
    for (uintptr_t a = 0x10000; a < 0x7FFFFFFF0000ull; ) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery((void*)a, &m, sizeof m)) break;
        uintptr_t next = (uintptr_t)m.BaseAddress + m.RegionSize;
        if (next <= a) break;
        if (m.State != MEM_FREE && m.Type == MEM_PRIVATE) {
            int ismi = mi_is_in_heap_region((const char*)m.BaseAddress + 1);   // mimalloc maps p - 1 to a segment
            uint64_t c = m.State == MEM_COMMIT ? m.RegionSize : 0;
            if (ismi) mi += c;
            else {
                other += c;
                uintptr_t ab = (uintptr_t)m.AllocationBase;
                if (nab && g_ab[nab - 1].base == ab) { g_ab[nab - 1].commit += c; g_ab[nab - 1].reserve += m.RegionSize; }
                else if (nab < (int)(sizeof g_ab / sizeof g_ab[0])) { g_ab[nab].base = ab; g_ab[nab].commit = c; g_ab[nab].reserve = m.RegionSize; nab++; }
            }
        } else if (m.State == MEM_COMMIT) { if (m.Type == MEM_MAPPED) mapped += m.RegionSize; else image += m.RegionSize; }
        a = next;
    }
    uint64_t hc = 0, ha = 0; int nh = 0;
    static heap_summary_fn hsum;
    if (!hsum) hsum = (heap_summary_fn)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "HeapSummary");
    HANDLE hs[128]; DWORD n = GetProcessHeaps(128, hs);
    for (DWORD i = 0; i < n && i < 128 && hsum; i++) {
        heap_summary_t s; memset(&s, 0, sizeof s); s.cb = sizeof s;
        if (hsum(hs[i], 0, &s)) { hc += s.cbCommitted; ha += s.cbAllocated; nh++; }
    }
    qsort(g_ab, (size_t)nab, sizeof g_ab[0], by_commit);
    fprintf(f, "#mem\tmimalloc_mb\t%llu\tother_private_mb\t%llu\theaps\t%d\theaps_commit_mb\t%llu\theaps_alloc_mb\t%llu\tmapped_mb\t%llu\timage_mb\t%llu\tblocks\t%d\r\n",
            (unsigned long long)(mi >> 20), (unsigned long long)(other >> 20), nh, (unsigned long long)(hc >> 20), (unsigned long long)(ha >> 20),
            (unsigned long long)(mapped >> 20), (unsigned long long)(image >> 20), nab);
    for (int i = 0; i < nab && i < 25 && g_ab[i].commit >= (1u << 20); i++)
        fprintf(f, "#block\t%llx\t%.1f\t%.1f\r\n", (unsigned long long)g_ab[i].base, g_ab[i].commit / 1048576.0, g_ab[i].reserve / 1048576.0);
}
static const site_t* g_sort;
static int by_live(const void* a, const void* b) {
    LONG64 x = g_sort[*(const uint32_t*)a].live, y = g_sort[*(const uint32_t*)b].live;
    return x < y ? 1 : x > y ? -1 : 0;
}
static void snapshot(int snap) {
    static uint32_t idx[NSITE];
    int ns = 0; LONG64 live = 0, live_n = 0;
    for (uint32_t i = 0; i < NSITE; i++) if (g_site[i].key) { live += g_site[i].live; live_n += g_site[i].live_n; if (g_site[i].live > 0) idx[ns++] = i; }
    g_sort = g_site; qsort(idx, (size_t)ns, sizeof idx[0], by_live);
    PROCESS_MEMORY_COUNTERS_EX pm; pm.cb = sizeof pm; GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm);
    double rate = (double)(UINT64_MAX / g_thresh);
    wchar_t p[MAX_PATH]; _snwprintf(p, MAX_PATH, L"%lseu4boost_memsites.tsv", g_dir);
    FILE* f = _wfopen(p, L"ab");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    char date[24]; game_date(date, sizeof date);
    fprintf(f, "#snap\t%d\t%02d:%02d:%02d\t%s\tprivate_mb\t%llu\tws_mb\t%llu\tlive_mb_est\t%.0f\tlive_blocks_est\t%.0f\tsites\t%d\tsamples\t%lld\tfull\t%lld\tnosite\t%lld\r\n",
            snap, st.wHour, st.wMinute, st.wSecond, date, (unsigned long long)(pm.PrivateUsage >> 20), (unsigned long long)(pm.WorkingSetSize >> 20),
            live * rate / 1048576.0, live_n * rate, ns, (long long)g_samples, (long long)g_full, (long long)g_nosite);
    regions(f);
    for (int r = 0; r < ns && r < 150; r++) {
        const site_t* s = &g_site[idx[r]];
        fprintf(f, "%u\t%.2f\t%.0f\t%.0f\t%.1f", idx[r], s->live * rate / 1048576.0, s->live_n * rate, s->allocs * rate, s->bytes * rate / 1048576.0);
        for (int k = 0; k < NFR; k++) { char t[96]; fmt_frame(t, sizeof t, s->fr[k]); fprintf(f, "\t%s", t); }
        fprintf(f, "\r\n");
    }
    fclose(f);
}
static DWORD WINAPI dumper(LPVOID u) {
    (void)u;
    for (int snap = 1;; snap++) { Sleep((DWORD)g_cfg.mem_sites_s * 1000u); snapshot(snap); }
    return 0;
}

// before the allocator hooks are enabled (alloc.c)
int eb_md_init(void) {
    if (g_cfg.mem_sites <= 0) return 0;
    for (int i = 0; i < NSH; i++) {
        InitializeSRWLock(&g_sh[i].lock);
        g_sh[i].e = (blk_t*)VirtualAlloc(NULL, SH_CAP * sizeof(blk_t), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!g_sh[i].e) return 0;
    }
    g_site = (site_t*)VirtualAlloc(NULL, NSITE * sizeof(site_t), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_site) return 0;
    MODULEINFO mi;
    if (GetModuleInformation(GetCurrentProcess(), g_self, &mi, sizeof mi)) { g_self_lo = (uintptr_t)mi.lpBaseOfDll; g_self_hi = g_self_lo + mi.SizeOfImage; }
    g_thresh = UINT64_MAX / (uint64_t)g_cfg.mem_sites;
    if (g_cfg.mem_sites_s < 5) g_cfg.mem_sites_s = 5;
    eb_md_on = 1;
    eb_log("memdiag: DIAGNOSTICS - call sites of 1 in %d allocations are kept; a snapshot every %d s goes to eu4boost_memsites.tsv",
           g_cfg.mem_sites, g_cfg.mem_sites_s);
    return 1;
}
void eb_md_start(void) {
    if (!eb_md_on) return;
    HANDLE h = CreateThread(NULL, 256 * 1024, dumper, NULL, 0, NULL);
    if (h) CloseHandle(h);
}
