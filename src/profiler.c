// Low-overhead sampling profiler (all threads, call stacks).
//
// A background thread wakes every `interval_us`, suspends each busy thread for a few
// microseconds, reads its registers and walks the stack with the modules' own unwind
// tables (.pdata) through RtlVirtualUnwind. While a thread is suspended the sampler
// touches only memory it owns (no heap, no locks), so it cannot deadlock on a lock the
// suspended thread holds. Stacks are aggregated in a fixed-size hash table and written
// to plugins\eu4boost_profile_<time>.tsv when sampling stops.
//
// Toggle in game with Ctrl+Alt+P (a beep confirms start / stop).
#include "common.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include <tlhelp32.h>
#include <mmsystem.h>
#include <psapi.h>

#define MAX_THREADS   512
#define MAX_MODULES   512
#define MAX_DEPTH     64
#define TABLE_BITS    19                      // 512k distinct stacks
#define TABLE_SIZE    (1u << TABLE_BITS)
#define FRAME_POOL    (24u * 1024u * 1024u)   // frames (8 bytes each) = 192 MB virtual, committed lazily

typedef struct {
    uintptr_t base, end;
    RUNTIME_FUNCTION* pdata;
    DWORD npdata;
    wchar_t name[64];
} module_t;

typedef struct {
    DWORD tid;
    HANDLE h;
    ULONG64 last_cycles;
    ULONG64 samples;
    uintptr_t start_addr;
    uintptr_t stack_lo, stack_hi;
    FILETIME created;
    wchar_t desc[64];
    int alive;
} thread_t;

typedef struct {
    uint64_t hash;
    uint32_t frames_off;   // index into frame pool
    uint16_t depth;
    uint16_t thread;       // index into g_threads
    uint64_t count;
} entry_t;

static module_t g_mods[MAX_MODULES];
static int g_nmods;
static thread_t g_threads[MAX_THREADS];
static int g_nthreads;
static entry_t* g_table;
static uintptr_t* g_frames;
static uint32_t g_frames_used;
static uint64_t g_total_samples, g_dropped, g_ticks, g_unwind_fail;
static volatile LONG g_active;       // sampling on?
static volatile LONG g_stop;         // shut down the sampler thread
static HANDLE g_thread;
static DWORD g_self_tid;
static ULONGLONG g_started_ms;
static CRITICAL_SECTION g_dump_cs;

typedef LONG (NTAPI *NtQueryInformationThread_t)(HANDLE, int, PVOID, ULONG, PULONG);
typedef HRESULT (WINAPI *GetThreadDescription_t)(HANDLE, PWSTR*);
static NtQueryInformationThread_t pNtQIT;
static GetThreadDescription_t pGetThreadDescription;

// --------------------------------------------------------------------- modules / unwind data
static void refresh_modules(void) {
    HMODULE mods[MAX_MODULES]; DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed)) return;
    int n = (int)(needed / sizeof(HMODULE));
    if (n > MAX_MODULES) n = MAX_MODULES;
    int out = 0;
    for (int i = 0; i < n; i++) {
        uint8_t* b = (uint8_t*)mods[i];
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)b;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) continue;
        IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(b + dos->e_lfanew);
        if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) continue;
        module_t m; memset(&m, 0, sizeof m);
        m.base = (uintptr_t)b; m.end = m.base + nt->OptionalHeader.SizeOfImage;
        IMAGE_DATA_DIRECTORY* d = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (d->VirtualAddress && d->Size) { m.pdata = (RUNTIME_FUNCTION*)(b + d->VirtualAddress); m.npdata = d->Size / sizeof(RUNTIME_FUNCTION); }
        wchar_t path[MAX_PATH];
        if (K32GetModuleBaseNameW(GetCurrentProcess(), mods[i], path, MAX_PATH)) { wcsncpy(m.name, path, 63); m.name[63] = 0; }
        g_mods[out++] = m;
    }
    // sort by base (insertion sort, small n)
    for (int i = 1; i < out; i++) { module_t t = g_mods[i]; int j = i - 1; while (j >= 0 && g_mods[j].base > t.base) { g_mods[j+1] = g_mods[j]; j--; } g_mods[j+1] = t; }
    g_nmods = out;
}

static module_t* find_module(uintptr_t pc) {
    int lo = 0, hi = g_nmods - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (pc < g_mods[mid].base) hi = mid - 1;
        else if (pc >= g_mods[mid].end) lo = mid + 1;
        else return &g_mods[mid];
    }
    return NULL;
}

static RUNTIME_FUNCTION* find_function(module_t* m, uintptr_t pc) {
    if (!m->pdata) return NULL;
    DWORD rva = (DWORD)(pc - m->base);
    int lo = 0, hi = (int)m->npdata - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        RUNTIME_FUNCTION* f = &m->pdata[mid];
        if (rva < f->BeginAddress) hi = mid - 1;
        else if (rva >= f->EndAddress) lo = mid + 1;
        else return f;
    }
    return NULL;
}

// Walk the stack of a suspended thread. Only reads memory inside [lo, hi) (its stack).
static int walk(CONTEXT* ctx, uintptr_t lo, uintptr_t hi, uintptr_t* out, int max) {
    int n = 0;
    out[n++] = ctx->Rip;
    while (n < max) {
        module_t* m = find_module(ctx->Rip);
        if (!m) break;
        if (ctx->Rsp < lo || ctx->Rsp + 8 > hi) break;
        RUNTIME_FUNCTION* f = find_function(m, ctx->Rip);
        if (!f) {
            // leaf function: return address is at [rsp]
            ctx->Rip = *(DWORD64*)ctx->Rsp;
            ctx->Rsp += 8;
        } else {
            PVOID handler_data; DWORD64 establisher;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, m->base, ctx->Rip, f, ctx, &handler_data, &establisher, NULL);
        }
        if (ctx->Rip == 0 || ctx->Rsp < lo || ctx->Rsp > hi) break;
        out[n++] = ctx->Rip;
    }
    return n;
}

// --------------------------------------------------------------------- threads
static void query_thread_info(thread_t* t) {
    if (pNtQIT) {
        PVOID start = NULL;
        if (pNtQIT(t->h, 9 /*ThreadQuerySetWin32StartAddress*/, &start, sizeof start, NULL) == 0) t->start_addr = (uintptr_t)start;
        struct { LONG ExitStatus; PVOID TebBaseAddress; PVOID UniqueProcess; PVOID UniqueThread; ULONG_PTR AffinityMask; LONG Priority; LONG BasePriority; } tbi;
        if (pNtQIT(t->h, 0 /*ThreadBasicInformation*/, &tbi, sizeof tbi, NULL) == 0 && tbi.TebBaseAddress) {
            NT_TIB* tib = (NT_TIB*)tbi.TebBaseAddress;
            t->stack_hi = (uintptr_t)tib->StackBase;
            // StackLimit is the committed bottom; the reserved region can be lower. Use the allocation base.
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery((void*)((uintptr_t)tib->StackBase - 1), &mbi, sizeof mbi)) t->stack_lo = (uintptr_t)mbi.AllocationBase;
            else t->stack_lo = (uintptr_t)tib->StackLimit;
        }
    }
    FILETIME a, b, c; GetThreadTimes(t->h, &t->created, &a, &b, &c);
    if (pGetThreadDescription) {
        PWSTR d = NULL;
        if (SUCCEEDED(pGetThreadDescription(t->h, &d)) && d) { wcsncpy(t->desc, d, 63); t->desc[63] = 0; LocalFree(d); }
    }
}

static void refresh_threads(void) {
    for (int i = 0; i < g_nthreads; i++) g_threads[i].alive = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof te;
    DWORD pid = GetCurrentProcessId();
    if (Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == g_self_tid) continue;
        int found = -1;
        for (int i = 0; i < g_nthreads; i++) if (g_threads[i].tid == te.th32ThreadID && g_threads[i].h) { found = i; break; }
        if (found >= 0) { g_threads[found].alive = 1; continue; }
        if (g_nthreads >= MAX_THREADS) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        thread_t* t = &g_threads[g_nthreads++];
        memset(t, 0, sizeof *t);
        t->tid = te.th32ThreadID; t->h = h; t->alive = 1;
        query_thread_info(t);
        QueryThreadCycleTime(h, &t->last_cycles);
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
    // close handles of dead threads (keep the slot for the report)
    for (int i = 0; i < g_nthreads; i++)
        if (!g_threads[i].alive && g_threads[i].h) { CloseHandle(g_threads[i].h); g_threads[i].h = NULL; }
}

// --------------------------------------------------------------------- aggregation
static uint64_t hash_stack(const uintptr_t* f, int n, int thread) {
    uint64_t h = 1469598103934665603ull ^ (uint64_t)thread;
    for (int i = 0; i < n; i++) { h ^= f[i]; h *= 1099511628211ull; h ^= h >> 29; }
    return h | 1;
}

static void record(int thread, const uintptr_t* f, int n) {
    uint64_t h = hash_stack(f, n, thread);
    uint32_t idx = (uint32_t)(h >> (64 - TABLE_BITS));
    for (uint32_t probe = 0; probe < 64; probe++, idx = (idx + 1) & (TABLE_SIZE - 1)) {
        entry_t* e = &g_table[idx];
        if (e->hash == 0) {
            if (g_frames_used + (uint32_t)n > FRAME_POOL) { g_dropped++; return; }
            e->hash = h; e->depth = (uint16_t)n; e->thread = (uint16_t)thread; e->count = 1;
            e->frames_off = g_frames_used;
            memcpy(&g_frames[g_frames_used], f, (size_t)n * sizeof *f);
            g_frames_used += (uint32_t)n;
            return;
        }
        if (e->hash == h && e->depth == n && e->thread == thread &&
            memcmp(&g_frames[e->frames_off], f, (size_t)n * sizeof *f) == 0) { e->count++; return; }
    }
    g_dropped++;
}

static void sample_once(void) {
    uintptr_t frames[MAX_DEPTH];
    for (int i = 0; i < g_nthreads; i++) {
        thread_t* t = &g_threads[i];
        if (!t->h) continue;
        ULONG64 cyc = 0;
        int busy = 1;   // if cycle counters are unavailable, sample everything
        // Suspending a thread makes it run a kernel APC, which itself adds a few thousand cycles,
        // so "any cycles since last time" would mark every sampled thread busy forever.
        // A thread counts as busy only if it ran more than ~50 us worth of cycles.
        if (QueryThreadCycleTime(t->h, &cyc)) { busy = cyc - t->last_cycles > 250000; t->last_cycles = cyc; }
        if (!busy && !(g_cfg.prof_all_threads_wall || t->tid == g_main_tid)) continue;
        if (SuspendThread(t->h) == (DWORD)-1) continue;
        CONTEXT ctx; memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        int n = 0;
        if (GetThreadContext(t->h, &ctx)) n = walk(&ctx, t->stack_lo, t->stack_hi, frames, g_cfg.prof_max_depth);
        ResumeThread(t->h);
        if (n == 0) { g_unwind_fail++; continue; }
        record(i, frames, n);
        t->samples++;
        g_total_samples++;
    }
    g_ticks++;
}

// --------------------------------------------------------------------- output
static void wfmt(HANDLE f, const char* fmt, ...) {
    char buf[2048]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (n < 0) return; if (n > (int)sizeof buf) n = (int)sizeof buf;
    DWORD w; WriteFile(f, buf, (DWORD)n, &w, NULL);
}

static void dump(void) {
    EnterCriticalSection(&g_dump_cs);
    if (g_total_samples == 0) { LeaveCriticalSection(&g_dump_cs); return; }
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%lseu4boost_profile_%04d%02d%02d_%02d%02d%02d.tsv", g_dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { LeaveCriticalSection(&g_dump_cs); return; }
    ULONGLONG secs = (GetTickCount64() - g_started_ms) / 1000;
    wfmt(f, "#eu4boost_profile\t1\n#interval_us\t%d\n#seconds\t%llu\n#ticks\t%llu\n#samples\t%llu\n#dropped\t%llu\n#unwind_fail\t%llu\n#main_tid\t%lu\n",
         g_cfg.prof_interval_us, (unsigned long long)secs, (unsigned long long)g_ticks, (unsigned long long)g_total_samples,
         (unsigned long long)g_dropped, (unsigned long long)g_unwind_fail, (unsigned long)g_main_tid);
    for (int i = 0; i < g_nmods; i++)
        wfmt(f, "M\t%d\t%llx\t%llx\t%ls\n", i, (unsigned long long)g_mods[i].base, (unsigned long long)(g_mods[i].end - g_mods[i].base), g_mods[i].name);
    for (int i = 0; i < g_nthreads; i++) {
        thread_t* t = &g_threads[i];
        module_t* m = find_module(t->start_addr);
        wfmt(f, "T\t%d\t%lu\t%llu\t%ls+%llx\t%ls\n", i, (unsigned long)t->tid, (unsigned long long)t->samples,
             m ? m->name : L"?", (unsigned long long)(m ? t->start_addr - m->base : t->start_addr), t->desc);
    }
    for (uint32_t i = 0; i < TABLE_SIZE; i++) {
        entry_t* e = &g_table[i];
        if (!e->hash) continue;
        char line[64 * 24 + 64]; int p = snprintf(line, sizeof line, "S\t%u\t%llu\t", e->thread, (unsigned long long)e->count);
        for (int k = 0; k < e->depth && p < (int)sizeof line - 32; k++) {
            uintptr_t pc = g_frames[e->frames_off + k];
            module_t* m = find_module(pc);
            if (m) p += snprintf(line + p, sizeof line - p, "%s%d:%llx", k ? ";" : "", (int)(m - g_mods), (unsigned long long)(pc - m->base));
            else   p += snprintf(line + p, sizeof line - p, "%s?:%llx", k ? ";" : "", (unsigned long long)pc);
        }
        line[p++] = '\n';
        DWORD w; WriteFile(f, line, (DWORD)p, &w, NULL);
    }
    CloseHandle(f);
    eb_log("profiler: wrote %ls (%llu samples, %llu s, %llu dropped)", path, (unsigned long long)g_total_samples, (unsigned long long)secs, (unsigned long long)g_dropped);
    LeaveCriticalSection(&g_dump_cs);
}

static void reset_data(void) {
    memset(g_table, 0, sizeof(entry_t) * TABLE_SIZE);
    g_frames_used = 0; g_total_samples = g_dropped = g_ticks = g_unwind_fail = 0;
    for (int i = 0; i < g_nthreads; i++) g_threads[i].samples = 0;
}

static void set_active(int on) {
    if (on) {
        refresh_modules(); refresh_threads(); reset_data();
        g_started_ms = GetTickCount64();
        InterlockedExchange(&g_active, 1);
        eb_log("profiler: started (%d threads, %d modules)", g_nthreads, g_nmods);
        Beep(1200, 120);
    } else {
        InterlockedExchange(&g_active, 0);
        eb_log("profiler: stopped after %llu ticks, %llu samples, %llu unwind failures",
               (unsigned long long)g_ticks, (unsigned long long)g_total_samples, (unsigned long long)g_unwind_fail);
        dump();
        Beep(600, 200);
    }
}

static DWORD WINAPI sampler_main(LPVOID arg) {
    (void)arg;
    g_self_tid = GetCurrentThreadId();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    if (!timer) { timeBeginPeriod(1); timer = CreateWaitableTimerW(NULL, FALSE, NULL); }
    if (g_cfg.prof_autostart) { Sleep(2000); set_active(1); }
    ULONGLONG last_refresh = GetTickCount64(), last_key = 0;
    int key_was_down = 0;
    while (!g_stop) {
        LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(g_active ? g_cfg.prof_interval_us : 50000) * 10;
        SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
        WaitForSingleObject(timer, 100);
        ULONGLONG now = GetTickCount64();
        if (now - last_key >= 50) {
            last_key = now;
            int down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000) && (GetAsyncKeyState('P') & 0x8000);
#ifdef EB_TEST
            {   // test build: a file named toggle.flag next to the DLL acts as one hotkey press
                wchar_t f[MAX_PATH]; _snwprintf(f, MAX_PATH, L"%lstoggle.flag", g_dir);
                if (GetFileAttributesW(f) != INVALID_FILE_ATTRIBUTES) { DeleteFileW(f); down = 1; key_was_down = 0; }
            }
#endif
            if (down && !key_was_down) set_active(!g_active);
            key_was_down = down;
            // `now` was read before a possible start above, so it can be older than g_started_ms:
            // compare against a fresh clock to avoid an unsigned underflow (0.1.1 stopped right after starting)
            ULONGLONG t = GetTickCount64();
            if (g_active && g_cfg.prof_max_seconds > 0 && t > g_started_ms &&
                t - g_started_ms > (ULONGLONG)g_cfg.prof_max_seconds * 1000) set_active(0);
        }
        if (!g_active) continue;
        if (now - last_refresh > 1000) { refresh_threads(); last_refresh = now; }
        sample_once();
    }
    return 0;
}

int eb_prof_start_thread(void) {
    InitializeCriticalSection(&g_dump_cs);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    pNtQIT = (NtQueryInformationThread_t)(void*)GetProcAddress(ntdll, "NtQueryInformationThread");
    pGetThreadDescription = (GetThreadDescription_t)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
    g_table = (entry_t*)VirtualAlloc(NULL, sizeof(entry_t) * TABLE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_frames = (uintptr_t*)VirtualAlloc(NULL, sizeof(uintptr_t) * (size_t)FRAME_POOL, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_table || !g_frames) { eb_log("profiler: out of memory"); return 0; }
    g_thread = CreateThread(NULL, 256 * 1024, sampler_main, NULL, 0, NULL);
    return g_thread != NULL;
}

void eb_prof_shutdown(void) {
    // Called from DLL_PROCESS_DETACH; on process exit the sampler thread is already gone.
    if (g_active) { g_active = 0; dump(); }
    g_stop = 1;
}
