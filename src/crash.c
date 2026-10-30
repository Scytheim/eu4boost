// Crash, hang and abnormal-exit reports ([report] capture=1, on by default). This file only writes
// local files under plugins\crash_reports\; report.c sends them at the next launch if the user agreed.
//
// crash: the game installs its own unhandled-exception filter (FUN_141654730: exception.txt, a ~36 MB
//   minidump with memory contents, CrashReporter.exe). Ours runs first and chains to it. It writes a
//   few KB of text on a fresh thread (a stack overflow leaves little stack): the exception, the
//   crashing thread's call stack as module+offset (RtlVirtualUnwind; the game's exception.txt has
//   no offsets), the return addresses found on the stack, registers, memory status, game date,
//   loaded module names (no paths) and, if another thread crashed, the main thread's stack.
//   SetUnhandledExceptionFilter is hooked so that a filter installed later goes behind ours.
// abnormal exit: a crash through __fastfail (heap corruption, abort), TerminateProcess or a killed
//   process never reaches a filter. crash_reports\session.txt says "running" until a normal exit
//   (DLL_PROCESS_DETACH) writes "exit"; at the next launch a session that did not end with "exit"
//   gets a report, and the game's logs of that session are copied before the game rewrites them.
//   What ended it (reports 7, 15, 31, 32: sessions that just stopped): TerminateProcess on itself is hooked
//   and names its caller in session.txt (state=terminated, by=module+offset); otherwise the next launch
//   looks for Windows' own record of the crash (Application log, Application Error event 1000: faulting
//   module, exception code - 0xc0000409 = __fastfail - and offset, matched by the process id) on a
//   thread of its own after DllMain and puts it into the report and its signature. No record and no
//   TerminateProcess: the process was closed from outside (Task Manager, Steam), or the PC shut down.
//   The game restarts itself (back from the single player screen to the main menu, seen 2026-10-24):
//   the new eu4.exe starts before the old one has ended. session.txt names the process (pid=), so a new
//   session whose predecessor is the same program and still running waits up to 3 s for it and makes no
//   report; an ending session writes "exit" only if session.txt is still its own.
// hang: a watchdog thread counts frames (one POPS_RunCallbacks call with the first handle seen, as
//   in uitime.c). With no frame for hang_seconds (45) the main thread's stack is written; the report
//   is dropped if frames resume or the game exits normally, and sent if the session ends abnormally.
// slow ([report] slow=1): uitime.c times the save lists (the save dialog, the load screen, deleting)
//   and loads; one over its limit becomes an event. The watchdog also catches stalls - no frame for
//   slow_stall_ms (2 s) outside a load - and samples the main thread's stack at 1, 2, 4 and 8 s of the
//   stall; during a load it samples every 5 s. Events go to pending\<session>_slow\, which is sent
//   once the session is over (at most one slow report a day, report.c).
// call stacks: another thread stays suspended for the whole walk (see "call stacks" below).
// saves: savezip.c keeps a journal of the session's saves (crash_reports\saves.txt). Crash and hang
//   reports carry this session's and the previous one's (a save that fails to load was usually written
//   in an earlier session); an abnormal-exit report carries the ended session's.
// heartbeat: every minute the watchdog writes the session's state (memory and commit left, game date,
//   frames) to crash_reports\heartbeat.txt; an abnormal-exit report carries the last one, so a game that
//   ran out of memory (its abort cannot be caught) can be told from a killed one.
// mods: at load the enabled mods of this session (dlc_load.json, in load order) are listed with the
//   name, version and Workshop id from their .mod files into crash_reports\mods.txt; every report of
//   the session gets that list (a report made at the next launch gets the old session's file).
// test_keys=1 (testing only): Ctrl+Alt+Shift+F9 crashes the main thread, +F10 hangs it for 70 s,
//   +F11 stalls it for 3 s, +F8 samples the main thread's stack 2,000 times.
#include "common.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include <psapi.h>
#include "MinHook.h"

wchar_t eb_rep_dir[MAX_PATH];             // <plugins>\crash_reports\ (trailing backslash)
static wchar_t g_session[MAX_PATH], g_start[20];
static HANDLE g_main_thread;
static volatile LONG64 g_frames;
static volatile ULONGLONG g_frame_ms;     // unbiased interrupt time of the latest frame (ms)
static volatile LONG g_crashed;
static LONGLONG g_t0;                     // QPC at load
static double g_ms;
volatile int eb_in_day;                   // guard.c: inside the daily tick (main thread)
volatile int eb_loading;                  // uitime.c: a save is being loaded or a new game set up
static volatile ULONGLONG g_max_gap;      // longest time between two frames since the watchdog last looked (ms)
static CRITICAL_SECTION g_slow_cs;        // the slow events and the load samples
static wchar_t g_slow_dir[MAX_PATH];      // pending\<session>_slow\ once this session has an event
static int g_slow_n;
static double g_slow_ratio;               // the worst event so far: its time / its limit
static char g_lsamp[4096];                // main thread samples of the current load
static int g_lsamp_n;
static char g_mods[16 * 1024];            // this session's mods for the reports (built at load)
static char g_prev_mods[16 * 1024];       // the previous session's (mods.txt), for reports made at this launch
static char g_prev_saves[8 * 1024];       // the save journal of the latest session that saved (saves.txt, savezip.c)
static char g_prev_beat[2048];            // the previous session's last heartbeat (heartbeat.txt)
static const char g_nolog[] = "The plugin log of that session is not here (log=0 in eu4boost.ini, or a later launch replaced it).\r\n";
static volatile LONG g_test_burst;        // test_keys: F8 asked for a burst of stack samples

// ---- text building without the heap (it also runs in a crashed process) ----
typedef struct { char* p; int n, cap; } sb_t;
static void s_put(sb_t* b, const char* s) { while (*s && b->n < b->cap - 1) b->p[b->n++] = *s++; b->p[b->n] = 0; }
static void s_hex(sb_t* b, uint64_t v, int w) {
    char t[20]; int k = 0;
    do { t[k++] = "0123456789abcdef"[v & 15]; v >>= 4; } while ((v || k < w) && k < 16);
    s_put(b, "0x"); char o[20]; int j = 0; while (k) o[j++] = t[--k]; o[j] = 0; s_put(b, o);
}
static void s_dec(sb_t* b, int64_t v) {
    char t[24]; int k = 0; int neg = v < 0; uint64_t u = neg ? (uint64_t)-v : (uint64_t)v;
    do { t[k++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) t[k++] = '-';
    char o[24]; int j = 0; while (k) o[j++] = t[--k]; o[j] = 0; s_put(b, o);
}
static void s_w(sb_t* b, const wchar_t* w) {
    char t[MAX_PATH * 3]; if (WideCharToMultiByte(CP_UTF8, 0, w, -1, t, sizeof t, NULL, NULL) > 0) s_put(b, t);
}
static void s_stamp(wchar_t* out, size_t n) {                // YYYYMMDD_HHMMSS (local time)
    SYSTEMTIME st; GetLocalTime(&st);
    _snwprintf(out, n, L"%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}
static void s_date(sb_t* b, int v) {                         // the game's CDate (hours) as y.m.d
    int hours = v - 43800000;
    int days = hours >= 0 ? hours / 24 : -((-hours + 23) / 24);
    int year = days >= 0 ? days / 365 : -((-days + 364) / 365);
    int doy = days - year * 365;
    static const int ml[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int m = 0; while (m < 11 && doy >= ml[m]) { doy -= ml[m]; m++; }
    s_dec(b, year); s_put(b, "."); s_dec(b, m + 1); s_put(b, "."); s_dec(b, doy + 1);
}
static ULONGLONG unbiased_ms(void) { ULONGLONG t; QueryUnbiasedInterruptTime(&t); return t / 10000; }

static int readable(uint64_t a, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    for (uint64_t end = a + n; a < end; a = (uint64_t)mi.BaseAddress + mi.RegionSize) {
        if (a < 0x10000 || !VirtualQuery((void*)a, &mi, sizeof mi) || mi.State != MEM_COMMIT) return 0;
        if ((mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(mi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return 0;
    }
    return 1;
}
static int executable(uint64_t a) {
    MEMORY_BASIC_INFORMATION mi;
    return a >= 0x10000 && VirtualQuery((void*)a, &mi, sizeof mi) && mi.State == MEM_COMMIT &&
           (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) && !(mi.Protect & PAGE_GUARD);
}
// module name (no path) and base of an address
static int mod_of(uint64_t a, char* name, int n, uint64_t* base) {
    HMODULE h;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(uintptr_t)a, &h)) return 0;
    wchar_t w[MAX_PATH]; DWORD k = GetModuleFileNameW(h, w, MAX_PATH);
    const wchar_t* s = w + k; while (s > w && s[-1] != L'\\' && s[-1] != L'/') s--;
    if (WideCharToMultiByte(CP_UTF8, 0, s, -1, name, n, NULL, NULL) <= 0) name[0] = 0;
    *base = (uint64_t)(uintptr_t)h;
    return 1;
}
static void s_addr(sb_t* b, uint64_t a) {
    char name[MAX_PATH]; uint64_t base;
    if (mod_of(a, name, sizeof name, &base)) { s_put(b, name); s_put(b, "+"); s_hex(b, a - base, 1); }
    else s_hex(b, a, 16);
}
// the first frame inside eu4.exe, this plugin or EU4dll: where the game's own code was
static int interesting(uint64_t a) {
    char name[MAX_PATH]; uint64_t base;
    if (!mod_of(a, name, sizeof name, &base)) return 0;
    return !_stricmp(name, "eu4.exe") || !_stricmp(name, "eu4boost.dll") || !_stricmp(name, "plugin64.dll");
}
// the main thread waiting for a parallel loop (parallel.c my_wait): its own frames say nothing - the
// signature is the game's frame that waits, and the thread at its items the longest is sampled too
static int our_frame(uint64_t a) {
    char name[MAX_PATH]; uint64_t base;
    return mod_of(a, name, sizeof name, &base) && !_stricmp(name, "eu4boost.dll");
}

// ---- call stacks ----
// A stack is walked with the modules' own unwind data (.pdata, RtlVirtualUnwind) from a table of the
// loaded modules built just before, reading only inside that thread's stack. Another thread stays
// suspended for the whole walk, and the walk takes no lock (no loader or heap call), so it cannot wait
// on a lock the suspended thread holds - as in profiler.c. 0.4.1 resumed the thread first and then
// walked a stack that kept changing: a frame register restored from it sent RtlVirtualUnwind to
// address 0x40 and crashed a player's game (crash report of 2026-10-23). The walk also runs behind a
// fault guard, so a broken frame ends the walk (and the thread is resumed) instead of the game.
typedef struct { uint64_t base, end; const RUNTIME_FUNCTION* pd; DWORD npd; } mod_t;
typedef struct { mod_t m[512]; int n; } mods_t;
static mods_t g_wd_mods, g_cr_mods;       // the watchdog's and the crash writer's (both may walk at once)

static int mods_build(void* p) {
    mods_t* t = (mods_t*)p;
    HMODULE h[512]; DWORD need = 0; t->n = 0;
    if (!EnumProcessModules(GetCurrentProcess(), h, sizeof h, &need)) return 0;
    int n = (int)(need / sizeof(HMODULE)); if (n > 512) n = 512;
    for (int i = 0; i < n; i++) {
        const uint8_t* b = (const uint8_t*)h[i];
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)b;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) continue;
        const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(b + dos->e_lfanew);
        if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) continue;
        mod_t m = { (uint64_t)(uintptr_t)b, (uint64_t)(uintptr_t)b + nt->OptionalHeader.SizeOfImage, NULL, 0 };
        const IMAGE_DATA_DIRECTORY* d = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (d->VirtualAddress && d->Size && (uint64_t)d->VirtualAddress + d->Size <= nt->OptionalHeader.SizeOfImage) {
            m.pd = (const RUNTIME_FUNCTION*)(b + d->VirtualAddress); m.npd = d->Size / sizeof(RUNTIME_FUNCTION);
        }
        int j = t->n++;                                       // sorted by base
        while (j > 0 && t->m[j - 1].base > m.base) { t->m[j] = t->m[j - 1]; j--; }
        t->m[j] = m;
    }
    return t->n;
}
static const mod_t* mods_find(const mods_t* t, uint64_t pc) {
    int lo = 0, hi = t->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (pc < t->m[mid].base) hi = mid - 1; else if (pc >= t->m[mid].end) lo = mid + 1; else return &t->m[mid];
    }
    return NULL;
}
static const RUNTIME_FUNCTION* fn_find(const mod_t* m, uint64_t pc) {
    DWORD rva = (DWORD)(pc - m->base); int lo = 0, hi = (int)m->npd - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1; const RUNTIME_FUNCTION* f = &m->pd[mid];
        if (rva < f->BeginAddress) hi = mid - 1; else if (rva >= f->EndAddress) lo = mid + 1; else return f;
    }
    return NULL;
}
// once its prologue has set it, the frame register (rbp...) that f's unwind data starts from lies in the stack
static int frame_ok(const mod_t* m, const RUNTIME_FUNCTION* f, uint64_t pc, const CONTEXT* c, uint64_t lo, uint64_t hi) {
    if (f->UnwindData & 1) return 1;                          // an indirect entry: RtlVirtualUnwind follows it
    if ((uint64_t)f->UnwindData + 4 > m->end - m->base) return 0;
    const uint8_t* u = (const uint8_t*)(uintptr_t)(m->base + f->UnwindData);   // UNWIND_INFO: [1] prologue size, [3] frame register
    int reg = u[3] & 15;
    if (!reg || pc - m->base - f->BeginAddress < u[1]) return 1;
    uint64_t v = (&c->Rax)[reg];                              // CONTEXT keeps rax..r15 in the unwind codes' order
    return v >= lo && v <= hi;
}
typedef struct { CONTEXT c; uint64_t lo, hi; const mods_t* mods; uint64_t* pcs; uint64_t* sps; int max, n; } walk_t;
static int walk_body(void* p) {
    walk_t* w = (walk_t*)p; CONTEXT* c = &w->c;
    while (w->n < w->max) {
        uint64_t pc = c->Rip, sp = c->Rsp;
        if (!pc) break;
        if (w->sps) w->sps[w->n] = sp;
        w->pcs[w->n++] = pc;
        if (sp < w->lo || sp + 8 > w->hi || (sp & 7)) break;
        const mod_t* m = mods_find(w->mods, pc);
        if (!m && w->n > 1) break;                            // only the first frame may be outside a module (a bad call target)
        const RUNTIME_FUNCTION* f = m && m->pd ? fn_find(m, pc) : NULL;
        if (!f) { c->Rip = *(const DWORD64*)(uintptr_t)sp; c->Rsp = sp + 8; }    // leaf function: return address on top
        else {
            if (!frame_ok(m, f, pc, c, w->lo, w->hi)) break;
            PVOID hd; DWORD64 ef;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, m->base, pc, (PRUNTIME_FUNCTION)f, c, &hd, &ef, NULL);
        }
        if (c->Rsp <= sp || c->Rsp > w->hi) break;            // unwinding only moves towards the stack base
    }
    return w->n;
}

// fault guard: eb_guard_call(fn, arg, &rsp) returns fn(arg), or -1 when fn faults - the vectored
// handler moves the thread to eb_guard_fail with the stack pointer saved here
int eb_guard_call(int (*fn)(void*), void* arg, volatile uint64_t* rsp);
extern char eb_guard_fail[];
__asm__(".text\n"
        ".globl eb_guard_call\n"
        "eb_guard_call:\n"
        "    pushq %rbx\n    pushq %rbp\n    pushq %rdi\n    pushq %rsi\n"
        "    pushq %r12\n    pushq %r13\n    pushq %r14\n    pushq %r15\n"
        "    subq $0xa8, %rsp\n"
        "    movdqu %xmm6, 0x00(%rsp)\n    movdqu %xmm7, 0x10(%rsp)\n    movdqu %xmm8, 0x20(%rsp)\n    movdqu %xmm9, 0x30(%rsp)\n"
        "    movdqu %xmm10, 0x40(%rsp)\n    movdqu %xmm11, 0x50(%rsp)\n    movdqu %xmm12, 0x60(%rsp)\n    movdqu %xmm13, 0x70(%rsp)\n"
        "    movdqu %xmm14, 0x80(%rsp)\n    movdqu %xmm15, 0x90(%rsp)\n"
        "    movq %rsp, (%r8)\n"
        "    movq %rcx, %rax\n    movq %rdx, %rcx\n"
        "    subq $0x20, %rsp\n"
        "    callq *%rax\n"
        "    addq $0x20, %rsp\n"
        ".Leb_guard_out:\n"
        "    movdqu 0x00(%rsp), %xmm6\n    movdqu 0x10(%rsp), %xmm7\n    movdqu 0x20(%rsp), %xmm8\n    movdqu 0x30(%rsp), %xmm9\n"
        "    movdqu 0x40(%rsp), %xmm10\n    movdqu 0x50(%rsp), %xmm11\n    movdqu 0x60(%rsp), %xmm12\n    movdqu 0x70(%rsp), %xmm13\n"
        "    movdqu 0x80(%rsp), %xmm14\n    movdqu 0x90(%rsp), %xmm15\n"
        "    addq $0xa8, %rsp\n"
        "    popq %r15\n    popq %r14\n    popq %r13\n    popq %r12\n"
        "    popq %rsi\n    popq %rdi\n    popq %rbp\n    popq %rbx\n"
        "    ret\n"
        ".globl eb_guard_fail\n"
        "eb_guard_fail:\n"
        "    movl $-1, %eax\n"
        "    jmp .Leb_guard_out\n");
typedef struct { volatile LONG tid; volatile uint64_t rsp; } gslot_t;
static gslot_t g_gslot[4];
static PVOID g_guard_veh;
static volatile LONG g_guard_faults;
static LONG CALLBACK guard_veh(EXCEPTION_POINTERS* e) {
    if ((e->ExceptionRecord->ExceptionCode & 0xF0000000u) != 0xC0000000u) return EXCEPTION_CONTINUE_SEARCH;   // errors only
    LONG me = (LONG)GetCurrentThreadId();
    for (int i = 0; i < 4; i++)
        if (g_gslot[i].tid == me && g_gslot[i].rsp) {
            e->ContextRecord->Rsp = g_gslot[i].rsp; e->ContextRecord->Rip = (DWORD64)(uintptr_t)eb_guard_fail;
            InterlockedIncrement(&g_guard_faults);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void guard_init(void) { if (!g_guard_veh) g_guard_veh = AddVectoredExceptionHandler(1, guard_veh); }
static int guarded(int (*fn)(void*), void* arg) {
    LONG me = (LONG)GetCurrentThreadId(); int s = -1;
    for (int i = 0; i < 4 && s < 0; i++) if (InterlockedCompareExchange(&g_gslot[i].tid, me, 0) == 0) s = i;
    if (s < 0) return -1;
    g_gslot[s].rsp = 0;
    int r = eb_guard_call(fn, arg, &g_gslot[s].rsp);
    g_gslot[s].rsp = 0; g_gslot[s].tid = 0;
    return r;
}
int eb_guarded(int (*fn)(void*), void* arg) { guard_init(); return guarded(fn, arg); }

// the stack of a thread: [allocation base of the stack, stack base)
typedef LONG (NTAPI *ntqit_t)(HANDLE, int, PVOID, ULONG, PULONG);
static int stack_of(const NT_TIB* tib, uint64_t* lo, uint64_t* hi) {
    MEMORY_BASIC_INFORMATION mi;
    *hi = (uint64_t)(uintptr_t)tib->StackBase;
    *lo = VirtualQuery((const uint8_t*)tib->StackBase - 1, &mi, sizeof mi) ? (uint64_t)(uintptr_t)mi.AllocationBase : (uint64_t)(uintptr_t)tib->StackLimit;
    return *hi > *lo;
}
static int thread_stack(HANDLE th, uint64_t* lo, uint64_t* hi) {
    ntqit_t q = (ntqit_t)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    struct { LONG ExitStatus; PVOID Teb; PVOID UniqueProcess; PVOID UniqueThread; ULONG_PTR AffinityMask; LONG Priority; LONG BasePriority; } tbi;
    *lo = *hi = 0;
    return q && q(th, 0 /* ThreadBasicInformation */, &tbi, sizeof tbi, NULL) == 0 && tbi.Teb && stack_of((const NT_TIB*)tbi.Teb, lo, hi);
}
// the call stack from a context whose stack does not change meanwhile (the crashing thread waits for us)
static int walk_ctx(const CONTEXT* c, uint64_t lo, uint64_t hi, mods_t* mods, uint64_t* pcs, uint64_t* sps, int max) {
    if (!g_guard_veh || max < 1) return 0;
    walk_t w; memset(&w, 0, sizeof w);
    w.c = *c; w.lo = lo; w.hi = hi; w.mods = mods; w.pcs = pcs; w.sps = sps; w.max = max;
    if (guarded(mods_build, mods) < 0) mods->n = 0;
    guarded(walk_body, &w);
    if (!w.n) pcs[w.n++] = c->Rip;
    return w.n;
}
// registers and call stack of another thread, which stays suspended for the walk (tens of us)
static LONG64 g_samples, g_sample_t, g_sample_max;   // samples: count, QPC ticks the thread was suspended (sum, longest)
static uint64_t g_main_lo, g_main_hi;
static int sample_thread(HANDLE th, uint64_t lo, uint64_t hi, mods_t* mods, CONTEXT* out, uint64_t* pcs, int max) {
    memset(out, 0, sizeof *out);
    if (!g_guard_veh || !hi || max < 1 || guarded(mods_build, mods) <= 0) return 0;
    walk_t w; memset(&w, 0, sizeof w);
    w.c.ContextFlags = CONTEXT_FULL; w.lo = lo; w.hi = hi; w.mods = mods; w.pcs = pcs; w.max = max;
    HANDLE self = GetCurrentThread(); int prio = GetThreadPriority(self);
    SetThreadPriority(self, THREAD_PRIORITY_TIME_CRITICAL);    // not put aside by the scheduler while the other thread waits
    LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
    if (SuspendThread(th) == (DWORD)-1) { SetThreadPriority(self, prio); return 0; }
    int ok = GetThreadContext(th, &w.c);
    if (ok) { *out = w.c; guarded(walk_body, &w); }
    ResumeThread(th);
    QueryPerformanceCounter(&t1);
    SetThreadPriority(self, prio);
    LONG64 d = t1.QuadPart - t0.QuadPart;
    InterlockedIncrement64(&g_samples); InterlockedAdd64(&g_sample_t, d);
    if (d > g_sample_max) g_sample_max = d;
    return ok ? w.n : 0;
}
// values on the stack that point just behind a call instruction in a module (return addresses)
static int after_call(uint64_t v) {
    if (!executable(v) || !readable(v - 7, 7)) return 0;
    char name[8]; uint64_t base; if (!mod_of(v, name, sizeof name, &base)) return 0;
    const uint8_t* q = (const uint8_t*)(uintptr_t)v;
    if (q[-5] == 0xe8) return 1;                                            // call rel32
    if (q[-6] == 0xff && (q[-5] & 0x38) == 0x10) return 1;                  // call [rip+disp32] / [reg+disp32]
    if (q[-7] == 0xff && (q[-6] & 0x38) == 0x10) return 1;                  // ... with SIB
    if (q[-3] == 0xff && (q[-2] & 0x38) == 0x10 && (q[-2] & 0xc0) == 0x40) return 1;   // call [reg+disp8]
    if (q[-2] == 0xff && (q[-1] & 0x38) == 0x10) return 1;                  // call reg / [reg]
    return 0;
}
static void s_scan(sb_t* b, uint64_t rsp, int max) {
    int n = 0;
    uint64_t p = rsp & ~7ull, end = p + 24 * 1024, ok_until = 0;
    for (; p < end && n < max; p += 8) {
        if (p >= ok_until) { if (!readable(p, 8)) break; ok_until = (p | 0xfff) + 1; }
        uint64_t v = *(volatile uint64_t*)(uintptr_t)p;
        if (v < 0x10000 || !after_call(v)) continue;
        s_put(b, "  rsp+"); s_hex(b, p - rsp, 1); s_put(b, "  "); s_addr(b, v); s_put(b, "\r\n"); n++;
    }
}
typedef struct { sb_t* b; uint64_t rsp; } scan_t;
static int scan_body(void* p) { scan_t* s = (scan_t*)p; s_scan(s->b, s->rsp, 48); return 0; }
// sps (or NULL): each frame's stack pointer, shown as its distance from the first one
static void s_frames(sb_t* b, const uint64_t* pcs, const uint64_t* sps, int n, uint64_t rsp) {
    s_put(b, "stack (unwound):\r\n");
    for (int i = 0; i < n; i++) {
        s_put(b, "  "); s_dec(b, i); s_put(b, "  "); s_addr(b, pcs[i]);
        if (sps && sps[i] >= sps[0]) { s_put(b, "  (rsp+"); s_hex(b, sps[i] - sps[0], 1); s_put(b, ")"); }
        s_put(b, "\r\n");
    }
    s_put(b, "return addresses on the stack:\r\n");
    scan_t s = { b, rsp };
    if (g_guard_veh) guarded(scan_body, &s);                  // another thread's stack may change while it is read
}
static void s_regs(sb_t* b, const CONTEXT* c) {
    const char* nm[] = { "rax","rbx","rcx","rdx","rsi","rdi","rbp","rsp","r8","r9","r10","r11","r12","r13","r14","r15" };
    const DWORD64 v[] = { c->Rax,c->Rbx,c->Rcx,c->Rdx,c->Rsi,c->Rdi,c->Rbp,c->Rsp,c->R8,c->R9,c->R10,c->R11,c->R12,c->R13,c->R14,c->R15 };
    s_put(b, "registers:");
    for (int i = 0; i < 16; i++) { s_put(b, i % 4 ? " " : "\r\n  "); s_put(b, nm[i]); s_put(b, "="); s_hex(b, v[i], 16); }
    s_put(b, "\r\n  rip="); s_addr(b, c->Rip); s_put(b, " eflags="); s_hex(b, c->EFlags, 1); s_put(b, "\r\n");
}
static void s_state(sb_t* b) {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    s_put(b, "plugin: " EU4BOOST_VERSION "\r\nuptime_s: "); s_dec(b, (int64_t)((t.QuadPart - g_t0) * g_ms / 1000.0));
    uint8_t* gs = *(uint8_t* volatile*)((uint8_t*)g_exe + 0x233fe78u);      // gamestate, date at +0x1dd0 (speed.c)
    if (gs && readable((uint64_t)(uintptr_t)gs + 0x1dd0, 4)) { s_put(b, "\r\ngame_date: "); s_date(b, *(int*)(gs + 0x1dd0)); }
    ULONGLONG now = unbiased_ms();
    s_put(b, "\r\nframes: "); s_dec(b, g_frames);
    if (g_frame_ms) { s_put(b, ", last one "); s_dec(b, (int64_t)(now - g_frame_ms)); s_put(b, " ms ago"); }
    if (eb_day_ms) { s_put(b, "\r\nlast daily tick: "); s_dec(b, (int64_t)(GetTickCount64() - eb_day_ms)); s_put(b, " ms ago"); }
    s_put(b, "\r\nin daily tick: "); s_dec(b, eb_in_day);
    MEMORYSTATUSEX ms; ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) {
        s_put(b, "\r\nmemory: load "); s_dec(b, ms.dwMemoryLoad); s_put(b, "%, RAM "); s_dec(b, (int64_t)(ms.ullAvailPhys >> 20));
        s_put(b, " of "); s_dec(b, (int64_t)(ms.ullTotalPhys >> 20)); s_put(b, " MB free, commit "); s_dec(b, (int64_t)(ms.ullAvailPageFile >> 20));
        s_put(b, " of "); s_dec(b, (int64_t)(ms.ullTotalPageFile >> 20)); s_put(b, " MB free");
    }
    PROCESS_MEMORY_COUNTERS_EX pm; pm.cb = sizeof pm;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm)) {
        s_put(b, "\r\ngame process: private "); s_dec(b, (int64_t)(pm.PrivateUsage >> 20)); s_put(b, " MB, working set ");
        s_dec(b, (int64_t)(pm.WorkingSetSize >> 20)); s_put(b, " MB (peak "); s_dec(b, (int64_t)(pm.PeakWorkingSetSize >> 20)); s_put(b, " MB)");
    }
    s_put(b, "\r\n");
}
static void s_modules(sb_t* b) {
    HMODULE m[512]; DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), m, sizeof m, &need)) return;
    int n = (int)(need / sizeof(HMODULE)); if (n > 512) n = 512;
    s_put(b, "modules (name, base, size, PE timestamp):\r\n");
    for (int i = 0; i < n; i++) {
        wchar_t w[MAX_PATH]; DWORD k = GetModuleFileNameW(m[i], w, MAX_PATH);
        const wchar_t* s = w + k; while (s > w && s[-1] != L'\\' && s[-1] != L'/') s--;
        MODULEINFO mi; if (!GetModuleInformation(GetCurrentProcess(), m[i], &mi, sizeof mi)) continue;
        IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)((uint8_t*)m[i] + ((IMAGE_DOS_HEADER*)m[i])->e_lfanew);
        s_put(b, "  "); s_w(b, s); s_put(b, " "); s_hex(b, (uint64_t)(uintptr_t)m[i], 1); s_put(b, " "); s_hex(b, mi.SizeOfImage, 1);
        s_put(b, " "); s_hex(b, nt->FileHeader.TimeDateStamp, 8); s_put(b, "\r\n");
    }
}
static const char* exc_name(DWORD c) {
    switch (c) {
    case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_BREAKPOINT: return "EXCEPTION_BREAKPOINT";
    case 0xE06D7363: return "CPP_EXCEPTION";
    case 0xC0000374: return "STATUS_HEAP_CORRUPTION";
    case 0xC0000409: return "STATUS_STACK_BUFFER_OVERRUN";
    default: return "EXCEPTION";
    }
}

// ---- report folders ----
static int write_file(const wchar_t* dir, const wchar_t* name, const char* s, int n) {
    wchar_t p[MAX_PATH]; _snwprintf(p, MAX_PATH, L"%ls%ls", dir, name);
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD w; BOOL ok = WriteFile(h, s, (DWORD)n, &w, NULL); CloseHandle(h);
    return ok;
}
static void new_dir(wchar_t* out, const wchar_t* stamp, const wchar_t* kind) {   // pending\<stamp>_<kind>\ .
    _snwprintf(out, MAX_PATH, L"%lspending\\%ls_%ls\\", eb_rep_dir, stamp, kind);
    CreateDirectoryW(out, NULL);
}
int eb_pending_report(const wchar_t* kind, const char* text) {
    if (!g_cfg.report_capture) return 0;
    wchar_t stamp[20], dir[MAX_PATH]; s_stamp(stamp, 20);
    new_dir(dir, stamp, kind);
    return write_file(dir, L"report.txt", text, (int)strlen(text));
}
char eb_prev_version[40];
static char g_session_extra[160];          // one more line for session.txt (how the process ended)
static void set_session(const char* state) {
    char t[320]; sb_t b = { t, 0, sizeof t };
    char pid[16]; snprintf(pid, sizeof pid, "%lu", (unsigned long)GetCurrentProcessId());
    s_put(&b, "state="); s_put(&b, state); s_put(&b, "\r\nstart="); s_w(&b, g_start);
    s_put(&b, "\r\nversion=" EU4BOOST_VERSION "\r\npid="); s_put(&b, pid); s_put(&b, "\r\n");
    if (g_session_extra[0]) { s_put(&b, g_session_extra); s_put(&b, "\r\n"); }
    HANDLE h = CreateFileW(g_session, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, t, (DWORD)b.n, &w, NULL); FlushFileBuffers(h); CloseHandle(h); }
}
static void copy_log(const wchar_t* dir, const wchar_t* src) {
    wchar_t s[MAX_PATH], d[MAX_PATH];
    _snwprintf(s, MAX_PATH, L"%ls%ls", g_dir, src); _snwprintf(d, MAX_PATH, L"%lseu4boost.log", dir);
    CopyFileW(s, d, FALSE);
}

// ---- crash ----
static char g_cbuf[96 * 1024];            // the report text (static: no heap, little stack)
static EXCEPTION_POINTERS* g_cep;
static DWORD g_ctid;
static uint64_t g_c_lo, g_c_hi, g_c_limit;   // the crashing thread's stack: reserved [lo, hi), committed from limit
static DWORD WINAPI crash_writer(LPVOID unused) {
    (void)unused;
    EXCEPTION_RECORD* er = g_cep->ExceptionRecord; CONTEXT* c = g_cep->ContextRecord;
    sb_t b = { g_cbuf, 0, sizeof g_cbuf };
    s_put(&b, "eu4boost crash report\r\nkind: crash\r\nsignature: "); s_put(&b, exc_name(er->ExceptionCode)); s_put(&b, " ");
    uint64_t pcs[64], sps[64]; int n = walk_ctx(c, g_c_lo, g_c_hi, &g_cr_mods, pcs, sps, 64), top = 0;
    while (top < n && !interesting(pcs[top])) top++;
    s_addr(&b, top < n ? pcs[top] : (uint64_t)(uintptr_t)er->ExceptionAddress);
    s_put(&b, "\r\nexception: "); s_hex(&b, er->ExceptionCode, 8); s_put(&b, " "); s_put(&b, exc_name(er->ExceptionCode));
    s_put(&b, " at "); s_addr(&b, (uint64_t)(uintptr_t)er->ExceptionAddress);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        ULONG_PTR k = er->ExceptionInformation[0];
        s_put(&b, "\r\naccess: "); s_put(&b, k == 0 ? "read" : k == 1 ? "write" : k == 8 ? "execute" : "?"); s_put(&b, " "); s_hex(&b, er->ExceptionInformation[1], 16);
    }
    s_put(&b, "\r\nthread: "); s_put(&b, g_ctid == g_main_tid ? "main" : "other"); s_put(&b, "\r\n");
    if (g_c_hi > g_c_lo && c->Rsp > g_c_lo && c->Rsp <= g_c_hi) {   // report 40: how full the stack was (a stack overflow)
        s_put(&b, "stack: "); s_dec(&b, (long long)((g_c_hi - c->Rsp) >> 10)); s_put(&b, " KB used of ");
        s_dec(&b, (long long)((g_c_hi - g_c_lo) >> 10)); s_put(&b, " KB, committed to "); s_dec(&b, (long long)((g_c_hi - g_c_limit) >> 10));
        s_put(&b, " KB (base "); s_hex(&b, g_c_hi, 16); s_put(&b, ")\r\n");
    }
    s_state(&b);
    s_regs(&b, c);
    s_frames(&b, pcs, sps, n, c->Rsp);
    if (g_ctid != g_main_tid && g_main_thread) {
        CONTEXT mc; int mn = sample_thread(g_main_thread, g_main_lo, g_main_hi, &g_cr_mods, &mc, pcs, 64);
        if (mc.Rip) { s_put(&b, "main thread:\r\n  rip="); s_addr(&b, mc.Rip); s_put(&b, "\r\n"); s_frames(&b, pcs, NULL, mn, mc.Rsp); }
    }
    s_put(&b, g_mods);
    s_put(&b, "this session: "); s_put(&b, eb_save_journal()[0] ? eb_save_journal() : "no saves yet\r\n");
    s_put(&b, "before this session: "); s_put(&b, g_prev_saves[0] ? g_prev_saves : "no saves.txt\r\n");
    s_modules(&b);
    wchar_t stamp[20], dir[MAX_PATH]; s_stamp(stamp, 20); new_dir(dir, stamp, L"crash");
    write_file(dir, L"report.txt", g_cbuf, b.n);
    copy_log(dir, L"eu4boost.log");
    set_session("crashed");
    return 0;
}
static LPTOP_LEVEL_EXCEPTION_FILTER g_next_filter;          // the game's filter (installed after ours)
static LONG WINAPI eb_filter(EXCEPTION_POINTERS* ep) {
    if (InterlockedExchange(&g_crashed, 1) == 0) {
        g_cep = ep; g_ctid = GetCurrentThreadId();
        if (!stack_of((const NT_TIB*)NtCurrentTeb(), &g_c_lo, &g_c_hi)) g_c_lo = g_c_hi = 0;
        g_c_limit = (uint64_t)(uintptr_t)((const NT_TIB*)NtCurrentTeb())->StackLimit;
        HANDLE h = CreateThread(NULL, 1 << 20, crash_writer, NULL, 0, NULL);
        if (h) { WaitForSingleObject(h, 20000); CloseHandle(h); }
        else crash_writer(NULL);
    }
    return g_next_filter ? g_next_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}
typedef LPTOP_LEVEL_EXCEPTION_FILTER (WINAPI *set_uef_t)(LPTOP_LEVEL_EXCEPTION_FILTER);
static set_uef_t o_set_uef;
static LPTOP_LEVEL_EXCEPTION_FILTER WINAPI hk_set_uef(LPTOP_LEVEL_EXCEPTION_FILTER f) {
    LPTOP_LEVEL_EXCEPTION_FILTER prev = g_next_filter;          // ours stays first, the new one runs after it
    g_next_filter = f == eb_filter ? NULL : f;
    return prev;
}

// ---- hang watchdog ----
static int sample_busy(char* out, int cap);
static void write_hang(int seconds) {
    static char t[64 * 1024], w[1024];
    sb_t b = { t, 0, sizeof t };
    CONTEXT mc; uint64_t pcs[64];
    int waiting = eb_par_main_waiting;
    int n = sample_thread(g_main_thread, g_main_lo, g_main_hi, &g_wd_mods, &mc, pcs, 64), top = 0;
    while (top < n && (!interesting(pcs[top]) || (waiting && our_frame(pcs[top])))) top++;
    s_put(&b, "eu4boost crash report\r\nkind: hang\r\nsignature: HANG ");
    if (top < n) s_addr(&b, pcs[top]); else s_put(&b, "unknown");
    s_put(&b, "\r\nno frame for: "); s_dec(&b, seconds); s_put(&b, " s\r\n");
    s_state(&b);
    if (mc.Rip) { s_put(&b, "main thread:\r\n"); s_regs(&b, &mc); s_frames(&b, pcs, NULL, n, mc.Rsp); }
    if (sample_busy(w, sizeof w)) { s_put(&b, "the main thread waits for a parallel loop; longest at its items: "); s_put(&b, w); s_put(&b, "\r\n"); }
    s_put(&b, g_mods);
    s_put(&b, "this session: "); s_put(&b, eb_save_journal()[0] ? eb_save_journal() : "no saves yet\r\n");
    s_put(&b, "before this session: "); s_put(&b, g_prev_saves[0] ? g_prev_saves : "no saves.txt\r\n");
    s_modules(&b);
    wchar_t stamp[20], dir[MAX_PATH]; s_stamp(stamp, 20); new_dir(dir, stamp, L"hang");
    write_file(dir, L"report.txt", t, b.n);
    write_file(dir, L"unconfirmed", "", 0);                  // sent only if the session then ends abnormally
    copy_log(dir, L"eu4boost.log");
}
static void drop_unconfirmed(const wchar_t* since) {        // hang reports of this session: the game recovered
    wchar_t pat[MAX_PATH]; _snwprintf(pat, MAX_PATH, L"%lspending\\*_hang", eb_rep_dir);
    WIN32_FIND_DATAW fd; HANDLE f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || wcsncmp(fd.cFileName, since, 15) < 0) continue;
        wchar_t d[MAX_PATH], u[MAX_PATH]; _snwprintf(d, MAX_PATH, L"%lspending\\%ls\\", eb_rep_dir, fd.cFileName);
        _snwprintf(u, MAX_PATH, L"%lsunconfirmed", d);
        if (GetFileAttributesW(u) == INVALID_FILE_ATTRIBUTES) continue;
        eb_rm_dir(d);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}
// ---- slow events ----
static void append_file(const wchar_t* dir, const wchar_t* name, const char* s, int n) {
    wchar_t p[MAX_PATH]; _snwprintf(p, MAX_PATH, L"%ls%ls", dir, name);
    HANDLE h = CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w; WriteFile(h, s, (DWORD)n, &w, NULL); CloseHandle(h);
}
void eb_slow_event(const char* what, double ms, double limit, const char* detail) {
    if (!g_session[0] || !g_cfg.report_slow || g_crashed) return;
    EnterCriticalSection(&g_slow_cs);
    if (g_slow_n < 30) {
        static char t[12 * 1024];
        if (!g_slow_dir[0]) { new_dir(g_slow_dir, g_start, L"slow"); write_file(g_slow_dir, L"unconfirmed", "", 0); }
        LARGE_INTEGER q; QueryPerformanceCounter(&q);
        SYSTEMTIME st; GetLocalTime(&st);
        int k = snprintf(t, sizeof t, "event: %s - %.0f ms (limit %.0f ms) at %02d:%02d:%02d, %.0f s after launch\r\n%s\r\n\r\n",
                         what, ms, limit, st.wHour, st.wMinute, st.wSecond, (q.QuadPart - g_t0) * g_ms / 1000.0, detail);
        if (k > (int)sizeof t - 1) k = (int)sizeof t - 1;
        append_file(g_slow_dir, L"events.txt", t, k);
        g_slow_n++;
        if (ms / limit > g_slow_ratio) { g_slow_ratio = ms / limit; write_file(g_slow_dir, L"top.txt", what, (int)strlen(what)); }
        eb_log("report: slow %s: %.0f ms (limit %.0f) - kept for the slow report", what, ms, limit);
        eb_pm_event("slow report: %s - %.0f ms", what, ms);
    }
    LeaveCriticalSection(&g_slow_cs);
}
// the session is over: events.txt (and the session's mods) becomes report.txt, which report.c sends
static void finalize_slow(const wchar_t* dir, const wchar_t* log, const char* mods) {
    wchar_t u[MAX_PATH]; _snwprintf(u, MAX_PATH, L"%lsunconfirmed", dir);
    if (GetFileAttributesW(u) == INVALID_FILE_ATTRIBUTES) return;
    char top[200] = "", head[400];
    wchar_t p[MAX_PATH]; _snwprintf(p, MAX_PATH, L"%lstop.txt", dir);
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { DWORD rd = 0; ReadFile(h, top, sizeof top - 1, &rd, NULL); top[rd] = 0; CloseHandle(h); }
    int k = snprintf(head, sizeof head, "eu4boost crash report\r\nkind: slow\r\nsignature: SLOW %s\r\nplugin: " EU4BOOST_VERSION "\r\n\r\n", top[0] ? top : "unknown");
    write_file(dir, L"report.txt", head, k);
    _snwprintf(p, MAX_PATH, L"%lsevents.txt", dir);
    h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        static char buf[64 * 1024]; DWORD rd = 0;
        ReadFile(h, buf, sizeof buf, &rd, NULL); CloseHandle(h);
        append_file(dir, L"report.txt", buf, (int)rd);
    }
    append_file(dir, L"report.txt", mods, (int)strlen(mods));
    if (log) copy_log(dir, log);
    else write_file(dir, L"eu4boost.log", g_nolog, (int)strlen(g_nolog));
    DeleteFileW(u);
}
// playmon.c's frame hitch samples: the main thread's return addresses (its own module list: the watchdog may walk at once)
static mods_t g_pm_mods;
int eb_sample_main_pcs(uint64_t* pcs, int max) {
    if (!g_main_thread || !g_main_hi) return 0;
    CONTEXT mc;
    return sample_thread(g_main_thread, g_main_lo, g_main_hi, &g_pm_mods, &mc, pcs, max);
}
// one sample of the main thread's stack (the thread is suspended while it is walked)
static int sample_main(char* out, int cap, char* top, int tcap) {
    CONTEXT mc; uint64_t pcs[32];
    int n = sample_thread(g_main_thread, g_main_lo, g_main_hi, &g_wd_mods, &mc, pcs, 32), first = -1;
    out[0] = 0; if (top) top[0] = 0;
    if (!n) return 0;
    sb_t b = { out, 0, cap };
    for (int i = 0; i < n && i < 16; i++) { if (i) s_put(&b, " < "); s_addr(&b, pcs[i]); }
    int waiting = eb_par_main_waiting;
    for (int i = 0; i < n && first < 0; i++) if (interesting(pcs[i]) && !(waiting && our_frame(pcs[i]))) first = i;
    if (top) { sb_t t = { top, 0, tcap }; if (first >= 0) s_addr(&t, pcs[first]); else s_put(&t, "unknown"); }
    return b.n;
}
// while the main thread waits for a parallel loop: the thread that has been at its chunk the longest
static int sample_busy(char* out, int cap) {
    out[0] = 0;
    if (!eb_par_main_waiting) return 0;
    LONGLONG oldest = 0; int k = -1;
    LONG n = eb_nbusy; if (n > EB_NBUSY) n = EB_NBUSY;
    for (int i = 0; i < n; i++) {
        LONGLONG s = eb_busy[i].since;
        if (eb_busy[i].tid && eb_busy[i].tid != g_main_tid && eb_busy[i].h && s && (k < 0 || s < oldest)) { oldest = s; k = i; }
    }
    if (k < 0) return 0;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    uint64_t lo, hi; CONTEXT c; uint64_t pcs[32];
    if (!thread_stack(eb_busy[k].h, &lo, &hi)) return 0;
    int item = eb_busy[k].item;
    int m = sample_thread(eb_busy[k].h, lo, hi, &g_wd_mods, &c, pcs, 32);
    if (!m) return 0;
    sb_t b = { out, 0, cap };
    s_put(&b, "thread "); s_dec(&b, (long long)eb_busy[k].tid); s_put(&b, " at item "); s_dec(&b, item);
    s_put(&b, " for "); s_dec(&b, (long long)((now.QuadPart - oldest) * g_ms)); s_put(&b, " ms: ");
    for (int i = 0; i < m && i < 16; i++) { if (i) s_put(&b, " < "); s_addr(&b, pcs[i]); }
    return b.n;
}
void eb_load_samples_reset(void) { EnterCriticalSection(&g_slow_cs); g_lsamp_n = 0; g_lsamp[0] = 0; LeaveCriticalSection(&g_slow_cs); }
int eb_load_samples_take(char* out, size_t n) {
    EnterCriticalSection(&g_slow_cs);
    int k = snprintf(out, n, "%s", g_lsamp_n ? g_lsamp : "  (none)\r\n");
    g_lsamp_n = 0; g_lsamp[0] = 0;
    LeaveCriticalSection(&g_slow_cs);
    return k;
}

// every minute the session's state (memory, game date, frames) to crash_reports\heartbeat.txt: a session
// that ends without a catchable crash - the game's out-of-memory abort, a killed process - leaves its
// last state for the abnormal-exit report of the next launch
static void heartbeat(void) {
    static char t[2048];
    sb_t b = { t, 0, sizeof t };
    SYSTEMTIME st; GetLocalTime(&st);
    char hm[16]; snprintf(hm, sizeof hm, "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    s_put(&b, "last state of that session (written every minute, the latest at "); s_put(&b, hm); s_put(&b, "):\r\n");
    s_state(&b);
    write_file(eb_rep_dir, L"heartbeat.txt", t, b.n);
}
static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    LONG64 seen = -1; ULONGLONG since = 0, load_t0 = 0, load_next = 0, beat = 0; int reported = 0, ns = 0, in_load = 0;
    static char samp[4][1024], tops[4][160], tmp[1024], wsamp[4][1024];
    int samp_s[4];
    for (;;) {
        Sleep(250);
        if (unbiased_ms() - beat >= 60000) { beat = unbiased_ms(); heartbeat(); }
        if (g_test_burst == 1) {                                // test_keys: 2,000 samples of the running main thread
            LONG64 s0 = g_samples, t0 = g_sample_t; LONG f0 = g_guard_faults; int got = 0;
            for (int i = 0; i < 2000; i++) { if (sample_main(tmp, sizeof tmp, NULL, 0)) got++; Sleep(1); }
            LONG64 k = g_samples - s0;
            eb_log("report: TEST %d of 2000 main thread samples walked, suspended %.1f us on average (longest so far %.1f us), fault guard %ld | last: %.300s",
                   got, k ? (g_sample_t - t0) * g_ms * 1000.0 / k : 0.0, g_sample_max * g_ms * 1000.0, g_guard_faults - f0, tmp);
            g_test_burst = 0;
        }
        LONG64 f = g_frames; ULONGLONG now = unbiased_ms();
        // a load: the main thread every 5 s (uitime.c takes them when the load is over)
        if (eb_loading && g_cfg.report_slow) {
            if (!load_t0) { load_t0 = now; load_next = now + 5000; }
            else if (now >= load_next && g_main_thread) {
                load_next += 5000;
                if (sample_main(tmp, sizeof tmp, NULL, 0)) {
                    EnterCriticalSection(&g_slow_cs);
                    if (g_lsamp_n < (int)sizeof g_lsamp - 64)
                        g_lsamp_n += snprintf(g_lsamp + g_lsamp_n, sizeof g_lsamp - g_lsamp_n, "  +%ds: %.900s\r\n", (int)((now - load_t0) / 1000), tmp);
                    LeaveCriticalSection(&g_slow_cs);
                }
            }
        } else load_t0 = 0;
        if (f != seen) {
            if (reported) { drop_unconfirmed(g_start); eb_log("report: the main thread is running again"); }
            ULONGLONG gap = g_max_gap; g_max_gap = 0;
            if (ns && !in_load && f > 300 && gap >= (ULONGLONG)g_cfg.slow_stall_ms && !g_crashed) {
                static char d[10 * 1024]; int k = snprintf(d, sizeof d, "no frame for %llu ms; main thread at", (unsigned long long)gap);
                for (int i = 0; i < ns && k < (int)sizeof d - 2200; i++) {      // a line of each at most ~1 KB
                    k += snprintf(d + k, sizeof d - k, "\r\n  %d s: %s", samp_s[i], samp[i]);
                    if (wsamp[i][0]) k += snprintf(d + k, sizeof d - k, "\r\n    waiting for %s", wsamp[i]);
                }
                char what[200]; snprintf(what, sizeof what, "stall %s", tops[0]);
                eb_slow_event(what, (double)gap, g_cfg.slow_stall_ms, d);
            }
            seen = f; since = now; reported = 0; ns = 0; in_load = 0; continue;
        }
        if (f == 0 || g_crashed) continue;                     // not started yet / already written
        ULONGLONG gap = now - g_frame_ms;
        if (eb_loading) in_load = 1;
        if (g_cfg.report_slow && !in_load && ns < 4 && gap >= (1000ull << ns) && g_main_thread) {
            sample_main(samp[ns], sizeof samp[0], tops[ns], sizeof tops[0]); samp_s[ns] = (int)(gap / 1000);
            sample_busy(wsamp[ns], sizeof wsamp[0]);
            ns++;
        }
        if (g_cfg.report_hang_s > 0 && !reported && now - since >= (ULONGLONG)g_cfg.report_hang_s * 1000) {
            reported = 1;
            eb_log("report: no frame for %d s - main thread stack written (dropped if the game recovers)", (int)((now - since) / 1000));
            eb_pm_event("hang report: no frame for %d s", (int)((now - since) / 1000));
            write_hang((int)((now - since) / 1000));
        }
    }
    return 0;
}

// frame counter: POPS_RunCallbacks with the first handle seen (main thread, once per frame)
typedef uintptr_t (*pops_rcb_t)(void*, void*);
static pops_rcb_t o_rcb;
static void* g_frame_key;
static uintptr_t hk_rcb(void* a, void* b) {
    if (GetCurrentThreadId() == g_main_tid) {
        if (!g_frame_key) g_frame_key = a;
        if (a == g_frame_key) {
            ULONGLONG now = unbiased_ms();
            if (g_frame_ms && now - g_frame_ms > g_max_gap) g_max_gap = now - g_frame_ms;
            if (eb_pm_on && g_frame_ms) eb_pm_frame(now - g_frame_ms);
            g_frames++; g_frame_ms = now;
            if (g_cfg.report_test_keys && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000) &&
                (GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
                if (GetAsyncKeyState(VK_F9) & 0x8000) { eb_log("report: TEST crash (Ctrl+Alt+Shift+F9)"); *(volatile int*)(uintptr_t)8 = 1; }
                if (GetAsyncKeyState(VK_F10) & 0x8000) { eb_log("report: TEST hang for 70 s (Ctrl+Alt+Shift+F10)"); Sleep(70000); }
                if (GetAsyncKeyState(VK_F11) & 0x8000) { eb_log("report: TEST stall for 3 s (Ctrl+Alt+Shift+F11)"); Sleep(3000); }
                if ((GetAsyncKeyState(VK_F8) & 0x8000) && !g_test_burst) { eb_log("report: TEST stack sample burst (Ctrl+Alt+Shift+F8)"); g_test_burst = 1; }
            }
        }
    }
    return o_rcb(a, b);
}

// ---- previous session ----
// tail of a file into dir\name (the game rewrites its logs a few seconds after this DLL loads)
static void copy_tail(const wchar_t* src, const wchar_t* dir, const wchar_t* name, DWORD max) {
    HANDLE h = CreateFileW(src, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    DWORD n = sz.QuadPart > max ? max : (DWORD)sz.QuadPart;
    LARGE_INTEGER off; off.QuadPart = sz.QuadPart - n; SetFilePointerEx(h, off, NULL, FILE_BEGIN);
    char* buf = (char*)HeapAlloc(GetProcessHeap(), 0, n + 1);
    DWORD rd = 0;
    if (buf && ReadFile(h, buf, n, &rd, NULL)) write_file(dir, name, buf, (int)rd);
    if (buf) HeapFree(GetProcessHeap(), 0, buf);
    CloseHandle(h);
}
int eb_docs_dir(wchar_t* out, size_t n) {                     // Documents\Paradox Interactive\Europa Universalis IV\ (trailing \)
    wchar_t raw[MAX_PATH]; DWORD sz = sizeof raw, type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders", L"Personal",
                     RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, &type, raw, &sz) != ERROR_SUCCESS) return 0;
    wchar_t docs[MAX_PATH]; if (!ExpandEnvironmentStringsW(raw, docs, MAX_PATH)) return 0;
    _snwprintf(out, n, L"%ls\\Paradox Interactive\\Europa Universalis IV\\", docs);
    return 1;
}
static int read_small(const wchar_t* p, char* buf, int cap) {    // a whole small file, 0-terminated
    DWORD rd = 0;
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { if (!ReadFile(h, buf, (DWORD)cap - 1, &rd, NULL)) rd = 0; CloseHandle(h); }
    buf[rd] = 0;
    return (int)rd;
}
// the value of key="..." (or key=...) at the start of a line of a .mod descriptor
static void mod_key(const char* t, const char* key, char* out, int n) {
    size_t kl = strlen(key); out[0] = 0;
    for (const char* p = t; (p = strstr(p, key)); p += kl) {
        if (p != t && p[-1] != '\n' && p[-1] != '\r' && p[-1] != ' ' && p[-1] != '\t') continue;
        const char* q = p + kl; while (*q == ' ' || *q == '\t') q++;
        if (*q != '=') continue;
        q++; while (*q == ' ' || *q == '\t') q++;
        int quoted = *q == '"', k = 0; if (quoted) q++;
        while (*q && *q != '\r' && *q != '\n' && (quoted ? *q != '"' : *q != ' ' && *q != '\t') && k < n - 1) out[k++] = *q++;
        out[k] = 0;
        return;
    }
}
// this session's mods in load order (the launcher wrote dlc_load.json before starting the game), each
// with the name, version, game version and Workshop id from its .mod file - the folder path is left out
static void mods_list(void) {
    sb_t b = { g_mods, 0, sizeof g_mods };
    wchar_t d[MAX_PATH], p[MAX_PATH];
    static char js[64 * 1024], t[16 * 1024];
    s_put(&b, "mods (load order, dlc_load.json and the .mod files):\r\n");
    if (!eb_docs_dir(d, MAX_PATH)) { s_put(&b, "  (no documents folder)\r\n"); return; }
    _snwprintf(p, MAX_PATH, L"%lsdlc_load.json", d);
    if (!read_small(p, js, sizeof js)) { s_put(&b, "  (no dlc_load.json)\r\n"); return; }
    const char* e = strstr(js, "\"enabled_mods\""), *end = e ? strchr(e, ']') : NULL, *q = e ? strchr(e, '[') : NULL;
    int i = 0;
    while (q && end && (q = strchr(q + 1, '"')) && q < end && b.n < b.cap - 400) {
        const char* z = strchr(q + 1, '"'); if (!z || z > end) break;
        char file[MAX_PATH]; int fl = (int)(z - q - 1); if (fl >= MAX_PATH) fl = MAX_PATH - 1;
        memcpy(file, q + 1, (size_t)fl); file[fl] = 0; q = z;
        wchar_t wf[MAX_PATH]; if (MultiByteToWideChar(CP_UTF8, 0, file, -1, wf, MAX_PATH) <= 0) wf[0] = 0;
        _snwprintf(p, MAX_PATH, L"%ls%ls", d, wf);
        char name[160], ver[48], sup[48], rid[24];
        if (read_small(p, t, sizeof t)) { mod_key(t, "name", name, sizeof name); mod_key(t, "version", ver, sizeof ver);
                                          mod_key(t, "supported_version", sup, sizeof sup); mod_key(t, "remote_file_id", rid, sizeof rid); }
        else { name[0] = ver[0] = sup[0] = rid[0] = 0; }
        if (!rid[0] && !strncmp(file, "mod/ugc_", 8)) { int k = 0; for (const char* c = file + 8; *c >= '0' && *c <= '9' && k < 20; c++) rid[k++] = *c; rid[k] = 0; }
        s_put(&b, "  "); s_dec(&b, ++i); s_put(&b, ". "); s_put(&b, name[0] ? name : "(no .mod file)");
        if (ver[0]) { s_put(&b, " | version "); s_put(&b, ver); }
        if (sup[0]) { s_put(&b, " | for "); s_put(&b, sup); }
        s_put(&b, rid[0] ? " | Workshop " : " | local"); s_put(&b, rid);
        s_put(&b, " | "); s_put(&b, file); s_put(&b, "\r\n");
    }
    if (!i) s_put(&b, "  (none)\r\n");
    const char* x = strstr(js, "\"disabled_dlcs\""), *xe = x ? strchr(x, ']') : NULL;
    if (x && xe) {
        s_put(&b, "disabled DLCs: ");
        char dl[2048]; int n = (int)(xe - x + 1); if (n >= (int)sizeof dl) n = sizeof dl - 1;
        memcpy(dl, x, (size_t)n); dl[n] = 0; s_put(&b, dl); s_put(&b, "\r\n");
    }
}
// eu4boost_prev.log belongs to the session that started at `start` (YYYYMMDD_HHMMSS): its first line's time
// is within seconds of it. With log=0 no launch rotates the log, and an older session's would be attached.
static int log_of_session(const wchar_t* name, const wchar_t* start) {
    wchar_t p[MAX_PATH]; char head[32];
    _snwprintf(p, MAX_PATH, L"%ls%ls", g_dir, name);
    if (read_small(p, head, sizeof head) < 10 || head[0] != '[' || wcslen(start) < 15) return 0;
    int a = atoi(head + 1) * 3600 + atoi(head + 4) * 60 + atoi(head + 7);
    int b = ((start[9] - L'0') * 10 + start[10] - L'0') * 3600 + ((start[11] - L'0') * 10 + start[12] - L'0') * 60 +
            (start[13] - L'0') * 10 + start[14] - L'0';
    int d = a > b ? a - b : b - a;
    if (d > 43200) d = 86400 - d;
    return d <= 10;
}
static void attach_game_logs(const wchar_t* dir) {
    wchar_t d[MAX_PATH], s[MAX_PATH];
    if (!eb_docs_dir(d, MAX_PATH)) return;
    _snwprintf(s, MAX_PATH, L"%lslogs\\game.log", d);   copy_tail(s, dir, L"game.log", 16 * 1024);
    _snwprintf(s, MAX_PATH, L"%lslogs\\error.log", d);  copy_tail(s, dir, L"error.log", 16 * 1024);
    _snwprintf(s, MAX_PATH, L"%lslogs\\system.log", d); copy_tail(s, dir, L"system.log", 64 * 1024);
    _snwprintf(s, MAX_PATH, L"%lsdlc_load.json", d);    copy_tail(s, dir, L"dlc_load.json", 16 * 1024);
}
// ---- what ended the previous session ----
static wchar_t g_wer_dir[MAX_PATH];        // the abnormal-exit report to complete (empty = none)
static DWORD g_wer_pid;                     // its process id (0 = unknown: matched by time)
volatile LONG eb_wer_done = 1;              // report.c waits for this before sending
typedef void* EVT_H;
typedef EVT_H (WINAPI* evtquery_t)(EVT_H, LPCWSTR, LPCWSTR, DWORD);
typedef BOOL (WINAPI* evtnext_t)(EVT_H, DWORD, EVT_H*, DWORD, DWORD, PDWORD);
typedef BOOL (WINAPI* evtrender_t)(EVT_H, EVT_H, DWORD, DWORD, PVOID, PDWORD, PDWORD);
typedef BOOL (WINAPI* evtclose_t)(EVT_H);
// the n-th <Data>...</Data> of an event's XML, as UTF-8
static int xml_data(const wchar_t* x, int n, char* out, int cap) {
    out[0] = 0;
    for (const wchar_t* p = x; (p = wcsstr(p, L"<Data")); ) {
        const wchar_t* gt = wcschr(p, L'>'); if (!gt) return 0;
        if (gt[-1] == L'/') { p = gt; if (n-- == 0) return 1; continue; }   // <Data/>: empty
        const wchar_t* end = wcsstr(gt, L"</Data>"); if (!end) return 0;
        if (n-- == 0) {
            int len = (int)(end - gt - 1);
            int k = WideCharToMultiByte(CP_UTF8, 0, gt + 1, len, out, cap - 1, NULL, NULL);
            out[k > 0 ? k : 0] = 0;
            return 1;
        }
        p = end;
    }
    return 0;
}
// Windows' crash record of the previous session: Application Error 1000 for eu4.exe with its process id
static int find_wer(const char* exe, DWORD pid, char* out, int cap, char* sig, int scap) {
    HMODULE w = LoadLibraryW(L"wevtapi.dll");
    if (!w) return 0;
    evtquery_t q = (evtquery_t)(void*)GetProcAddress(w, "EvtQuery");
    evtnext_t nx = (evtnext_t)(void*)GetProcAddress(w, "EvtNext");
    evtrender_t rd = (evtrender_t)(void*)GetProcAddress(w, "EvtRender");
    evtclose_t cl = (evtclose_t)(void*)GetProcAddress(w, "EvtClose");
    if (!q || !nx || !rd || !cl) return 0;
    EVT_H rs = q(NULL, L"Application", L"*[System[Provider[@Name='Application Error'] and (EventID=1000) and "
                 L"TimeCreated[timediff(@SystemTime) <= 259200000]]]", 0x1 | 0x200);   // channel, newest first (3 days)
    if (!rs) return 0;
    static wchar_t xml[16384];
    int found = 0;
    EVT_H ev[16]; DWORD got = 0;
    while (!found && nx(rs, 16, ev, 2000, 0, &got) && got) {
        for (DWORD i = 0; i < got; i++) {
            DWORD used = 0, props = 0;
            if (!found && rd(NULL, ev[i], 1, sizeof xml - 2, xml, &used, &props)) {   // EvtRenderEventXml
                xml[used / 2 < 16383 ? used / 2 : 16383] = 0;
                char app[64], mod[96], mver[48], code[24], off[40], epid[24];
                xml_data(xml, 0, app, sizeof app);
                xml_data(xml, 8, epid, sizeof epid);
                if (!_stricmp(app, exe) && (pid == 0 || strtoul(epid, NULL, 16) == pid)) {
                    xml_data(xml, 3, mod, sizeof mod); xml_data(xml, 4, mver, sizeof mver);
                    xml_data(xml, 6, code, sizeof code); xml_data(xml, 7, off, sizeof off);
                    const wchar_t* tc = wcsstr(xml, L"SystemTime='"); char when[32] = "";
                    if (!tc) tc = wcsstr(xml, L"SystemTime=\"");
                    if (tc) { int k = WideCharToMultiByte(CP_UTF8, 0, tc + 12, 19, when, sizeof when - 1, NULL, NULL); when[k > 0 ? k : 0] = 0; }
                    uint64_t o = _strtoui64(off, NULL, 16);
                    unsigned long c = strtoul(code, NULL, 16);
                    snprintf(out, cap, "windows: Application Error %s UTC (process %s%s): %s %s, exception 0x%08lx%s at %s+0x%llx\r\n",
                             when, epid, pid ? "" : ", matched by time", mod, mver, c, c == 0xc0000409ul ? " (__fastfail)" : "",
                             mod, (unsigned long long)o);
                    snprintf(sig, scap, "ABNORMAL_EXIT 0x%08lx %s+0x%llx", c, mod, (unsigned long long)o);
                    found = 1;
                }
            }
            cl(ev[i]);
        }
        got = 0;
    }
    cl(rs);
    return found;
}
static DWORD WINAPI winerr_thread(LPVOID unused) {
    (void)unused;
    char line[512] = "", sig[200] = "";
    if (!find_wer("eu4.exe", g_wer_pid, line, sizeof line, sig, sizeof sig))
        snprintf(line, sizeof line, "windows: no crash record of that process in the Application log - it was closed or ended from "
                 "outside (Task Manager, Steam, a shutdown) or the PC lost power\r\n");
    wchar_t p[MAX_PATH]; _snwprintf(p, MAX_PATH, L"%lsreport.txt", g_wer_dir);
    static char r[16384], o[17000];
    int n = read_small(p, r, sizeof r);
    if (n > 0) {
        const char* s1 = strstr(r, "signature: ABNORMAL_EXIT\r\n");
        const char* body = strstr(r, "The previous session");
        if (s1 && body) {
            int k = snprintf(o, sizeof o, "%.*s", (int)(s1 - r), r);
            k += snprintf(o + k, sizeof o - k, "signature: %s\r\n", sig[0] ? sig : "ABNORMAL_EXIT");
            const char* a = s1 + strlen("signature: ABNORMAL_EXIT\r\n");
            k += snprintf(o + k, sizeof o - k, "%.*s%s%s", (int)(body - a), a, line, body);
            write_file(g_wer_dir, L"report.txt", o, k);
        }
    }
    eb_log("report: the previous session's end: %.300s", line);
    InterlockedExchange(&eb_wer_done, 1);
    return 0;
}

typedef BOOL (WINAPI* term_t)(HANDLE, UINT);
static term_t o_term;
static BOOL WINAPI hk_term(HANDLE h, UINT code) {
    if (!g_crashed && (h == GetCurrentProcess() || GetProcessId(h) == GetCurrentProcessId())) {
        char by[100]; sb_t b = { by, 0, sizeof by };
        s_put(&b, "by="); s_addr(&b, (uint64_t)(uintptr_t)__builtin_return_address(0));
        s_put(&b, " code="); s_hex(&b, code, 1);
        by[b.n < sizeof by ? b.n : sizeof by - 1] = 0;
        snprintf(g_session_extra, sizeof g_session_extra, "%s", by);
        set_session("terminated");
        eb_log("process: TerminateProcess on itself, %s", by + 3);
    }
    return o_term(h, code);
}

static void check_previous(void) {
    char t[256] = {0};
    HANDLE h = CreateFileW(g_session, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;                      // first run with reports
    DWORD rd = 0; ReadFile(h, t, sizeof t - 1, &rd, NULL); CloseHandle(h);
    const char* pv = strstr(t, "version=");
    for (int i = 0; pv && i < (int)sizeof eb_prev_version - 1 && pv[8 + i] && pv[8 + i] != '\r' && pv[8 + i] != '\n'; i++) {
        eb_prev_version[i] = pv[8 + i]; eb_prev_version[i + 1] = 0;
    }
    int clean = !strncmp(t, "state=exit", 10);
    const char* q = strstr(t, "pid=");
    DWORD pid = q ? (DWORD)strtoul(q + 4, NULL, 10) : 0;
    if (!clean && pid && pid != GetCurrentProcessId()) {
        HANDLE ph = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (ph) {
            wchar_t a[MAX_PATH], me[MAX_PATH]; DWORD an = MAX_PATH;
            int same = QueryFullProcessImageNameW(ph, 0, a, &an) && GetModuleFileNameW(NULL, me, MAX_PATH) && !_wcsicmp(a, me);
            if (same && WaitForSingleObject(ph, 0) == WAIT_TIMEOUT) {
                // the game restarted itself: its old process is still ending (and finishes its own reports)
                DWORD w = WaitForSingleObject(ph, 3000);
                CloseHandle(ph);
                eb_log("report: the previous session (pid %lu) was still running - the game restarted itself%s, no abnormal-exit report",
                       (unsigned long)pid, w == WAIT_OBJECT_0 ? " (it ended within 3 s)" : "");
                return;
            }
            CloseHandle(ph);
        }
    }
    wchar_t prev[20] = L"";
    const char* p = strstr(t, "start=");
    if (p) for (int i = 0; i < 15 && p[6 + i] && p[6 + i] != '\r'; i++) { prev[i] = (wchar_t)p[6 + i]; prev[i + 1] = 0; }
    // reports written by that session: its slow events, a crash, or a hang it never recovered from
    int found = 0;
    wchar_t pat[MAX_PATH]; _snwprintf(pat, MAX_PATH, L"%lspending\\*", eb_rep_dir);
    WIN32_FIND_DATAW fd; HANDLE f = FindFirstFileW(pat, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.' || wcsncmp(fd.cFileName, prev, 15) < 0) continue;
            wchar_t d[MAX_PATH], u[MAX_PATH]; _snwprintf(d, MAX_PATH, L"%lspending\\%ls\\", eb_rep_dir, fd.cFileName);
            size_t nl = wcslen(fd.cFileName);
            if (nl > 7 && !wcscmp(fd.cFileName + nl - 7, L"_update")) continue;   // update.c's, not about how it ended
            if (nl > 5 && !wcscmp(fd.cFileName + nl - 5, L"_slow")) {
                finalize_slow(d, log_of_session(L"eu4boost_prev.log", fd.cFileName) ? L"eu4boost_prev.log" : NULL, g_prev_mods);
                attach_game_logs(d); continue;
            }
            if (clean) continue;
            _snwprintf(u, MAX_PATH, L"%lsunconfirmed", d); DeleteFileW(u);
            attach_game_logs(d); found = 1;
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    if (clean) return;
    if (!found) {
        // that session's version (the issue is filed under it): session.txt's version=, else its last heartbeat
        char ver[40] = "";
        const char* v = strstr(t, "version=");
        const char* vb = strstr(g_prev_beat, "\nplugin: ");
        if (v) v += 8; else if (vb) v = vb + 9;
        for (int i = 0; v && i < 32 && v[i] && v[i] != '\r' && v[i] != '\n'; i++) { ver[i] = v[i]; ver[i + 1] = 0; }
        char by[96] = "";                                     // state=terminated: who called TerminateProcess
        const char* bp = strstr(t, "by=");
        for (int i = 0; bp && i < 90 && bp[3 + i] && bp[3 + i] != '\r' && bp[3 + i] != '\n'; i++) { by[i] = bp[3 + i]; by[i + 1] = 0; }
        char r[900]; sb_t b = { r, 0, sizeof r };
        s_put(&b, "eu4boost crash report\r\nkind: exit\r\nsignature: ABNORMAL_EXIT");
        if (by[0]) { s_put(&b, " TerminateProcess "); s_put(&b, by); }
        s_put(&b, "\r\n");
        if (ver[0]) { s_put(&b, "plugin: "); s_put(&b, ver); s_put(&b, "\r\n"); }
        if (by[0]) { s_put(&b, "ended by: TerminateProcess on itself, called from "); s_put(&b, by); s_put(&b, "\r\n"); }
        s_put(&b, "The previous session (started "); s_w(&b, prev); s_put(&b, ") ended without a normal exit and without a crash the plugin "
                  "could catch: a crash through __fastfail (heap corruption, abort), a killed process, a shutdown or a power loss.\r\n");
        wchar_t dir[MAX_PATH]; new_dir(dir, prev[0] ? prev : g_start, L"exit");
        write_file(dir, L"report.txt", r, b.n);
        if (!by[0]) { wcscpy(g_wer_dir, dir); g_wer_pid = pid; }   // winerr_thread looks for Windows' record
        append_file(dir, L"report.txt", g_prev_mods, (int)strlen(g_prev_mods));
        append_file(dir, L"report.txt", g_prev_saves, (int)strlen(g_prev_saves));
        append_file(dir, L"report.txt", g_prev_beat, (int)strlen(g_prev_beat));
        if (log_of_session(L"eu4boost_prev.log", prev)) copy_log(dir, L"eu4boost_prev.log");
        else write_file(dir, L"eu4boost.log", g_nolog, (int)strlen(g_nolog));
        attach_game_logs(dir);
    }
    eb_log("report: the previous session did not exit normally (%s) - report kept for sending", t[6] == 'c' ? "crash" : found ? "hang" : "abnormal exit");
}

void eb_rm_dir(const wchar_t* dir) {                          // a report folder (files only, no subfolders)
    wchar_t pat[MAX_PATH]; _snwprintf(pat, MAX_PATH, L"%ls*", dir);
    WIN32_FIND_DATAW fd; HANDLE f = FindFirstFileW(pat, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            wchar_t p[MAX_PATH]; _snwprintf(p, MAX_PATH, L"%ls%ls", dir, fd.cFileName); DeleteFileW(p);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    RemoveDirectoryW(dir);
}

int eb_crash_install(void) {
    LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); g_ms = 1000.0 / (double)f.QuadPart; g_t0 = t.QuadPart;
    _snwprintf(eb_rep_dir, MAX_PATH, L"%lscrash_reports\\", g_dir);
    InitializeCriticalSection(&g_slow_cs);                    // uitime.c uses the load samples even without reports
    if (!g_cfg.report_capture) return 0;
#ifdef EB_TEST
    return 0;
#else
    wchar_t p[MAX_PATH];
    CreateDirectoryW(eb_rep_dir, NULL);
    _snwprintf(p, MAX_PATH, L"%lspending", eb_rep_dir); CreateDirectoryW(p, NULL);
    _snwprintf(p, MAX_PATH, L"%lssent", eb_rep_dir); CreateDirectoryW(p, NULL);
    _snwprintf(g_session, MAX_PATH, L"%lssession.txt", eb_rep_dir);
    s_stamp(g_start, 20);
    _snwprintf(p, MAX_PATH, L"%lsmods.txt", eb_rep_dir); read_small(p, g_prev_mods, sizeof g_prev_mods);
    _snwprintf(p, MAX_PATH, L"%lssaves.txt", eb_rep_dir); read_small(p, g_prev_saves, sizeof g_prev_saves);
    _snwprintf(p, MAX_PATH, L"%lsheartbeat.txt", eb_rep_dir); read_small(p, g_prev_beat, sizeof g_prev_beat); DeleteFileW(p);
    check_previous();
    mods_list(); write_file(eb_rep_dir, L"mods.txt", g_mods, (int)strlen(g_mods));
    set_session("running");
    guard_init();
    g_main_thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_main_tid);
    if (g_main_thread && !thread_stack(g_main_thread, &g_main_lo, &g_main_hi)) eb_log("report: the main thread's stack was not found - no stack samples");
    g_next_filter = SetUnhandledExceptionFilter(eb_filter);
    MH_STATUS st = MH_Initialize();
    void* target = NULL;
    int hooked = (st == MH_OK || st == MH_ERROR_ALREADY_INITIALIZED) &&
                 MH_CreateHookApiEx(L"kernelbase", "SetUnhandledExceptionFilter", (void*)hk_set_uef, (void**)&o_set_uef, &target) == MH_OK &&
                 MH_EnableHook(target) == MH_OK;
    if (!hooked) eb_log("report: SetUnhandledExceptionFilter hook failed - a filter installed later replaces ours");
    void* tt = NULL;
    if (!((st == MH_OK || st == MH_ERROR_ALREADY_INITIALIZED) &&
          MH_CreateHookApiEx(L"kernelbase", "TerminateProcess", (void*)hk_term, (void**)&o_term, &tt) == MH_OK && MH_EnableHook(tt) == MH_OK))
        eb_log("report: TerminateProcess hook failed - an abnormal exit cannot name a caller");
    if (g_wer_dir[0]) {                                        // after DllMain: wevtapi.dll is loaded there
        InterlockedExchange(&eb_wer_done, 0);
        HANDLE h = CreateThread(NULL, 0, winerr_thread, NULL, 0, NULL);
        if (h) CloseHandle(h); else InterlockedExchange(&eb_wer_done, 1);
    }
    o_rcb = (pops_rcb_t)patch_iat(g_exe, "pops_api.dll", "POPS_RunCallbacks", (void*)hk_rcb);
    if (o_rcb && g_main_thread) {                              // hangs, stalls and the heartbeat
        HANDLE h = CreateThread(NULL, 256 * 1024, watchdog, NULL, 0, NULL);
        if (h) CloseHandle(h);
    }
    eb_log("report: crash, hang (%d s), abnormal-exit%s reports are written to crash_reports\\%s", g_cfg.report_hang_s,
           g_cfg.report_slow ? " and slow (stalls over 2 s, save lists, loads)" : "",
           g_cfg.report_test_keys ? " - TEST KEYS ON (Ctrl+Alt+Shift+F9 crash, F10 hang, F11 stall, F8 stack samples)" : "");
    int nm = 0; for (const char* s = g_mods; (s = strstr(s, "\r\n  ")); s += 4) nm++;
    eb_log("report: %d mods listed for the reports%s", nm, g_guard_veh ? "" : " - fault guard missing, no stack walks");
    return 1;
#endif
}

void eb_crash_exit(void) {                                    // DLL_PROCESS_DETACH at process exit
    if (!g_cfg.report_capture || !g_session[0] || g_crashed) return;
    if (g_samples) eb_log("report: %lld main thread stack samples, suspended %.1f us on average, %.1f us at most; fault guard %ld",
                          (long long)g_samples, g_sample_t * g_ms * 1000.0 / g_samples, g_sample_max * g_ms * 1000.0, g_guard_faults);
    drop_unconfirmed(g_start);
    if (g_slow_dir[0]) finalize_slow(g_slow_dir, L"eu4boost.log", g_mods);
    // a new session may have started already (the game restarting itself): its record stays
    char t[256], mine[64]; sb_t b = { mine, 0, sizeof mine };
    s_put(&b, "start="); s_w(&b, g_start); mine[b.n < sizeof mine ? b.n : sizeof mine - 1] = 0;
    if (!read_small(g_session, t, sizeof t) || strstr(t, mine)) set_session("exit");
}

#ifdef EB_TEST
// self-test: a thread that keeps changing its stack (recursion through frames that use rbp, of varying
// size) is sampled again and again. resume_first = 1 walks after resuming it, as 0.4.1 did.
static volatile LONG t_stop;
static volatile uint64_t t_sink;
__attribute__((noinline)) static uint64_t t_churn(int d) {
    volatile uint8_t* a = (volatile uint8_t*)__builtin_alloca(16 + (size_t)(d * 40 % 700));
    a[0] = (uint8_t)d; uint64_t r = a[0];
    if (d > 0 && !t_stop) r += t_churn(d - 1);
    else for (volatile int k = 0; k < 200; k++) r += (uint64_t)k;
    return r;
}
static DWORD WINAPI t_churner(LPVOID u) {
    (void)u; unsigned s = 1;
    while (!t_stop) { s = s * 1103515245u + 12345u; t_sink += t_churn((int)(s >> 16) % 48); }
    return 0;
}
__declspec(dllexport) int eb_test_stackwalk(int samples, int resume_first, double* avg_us, int* deep, int* faults) {
    guard_init();
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_ms = 1000.0 / (double)f.QuadPart;
    t_stop = 0;
    HANDLE th = CreateThread(NULL, 1 << 20, t_churner, NULL, 0, NULL);
    uint64_t lo, hi; int ok = 0; *deep = 0;
    if (!th || !thread_stack(th, &lo, &hi)) return -1;
    guarded(mods_build, &g_wd_mods);
    const mod_t* me = mods_find(&g_wd_mods, (uint64_t)(uintptr_t)t_churn);
    const RUNTIME_FUNCTION* fc = me ? fn_find(me, (uint64_t)(uintptr_t)t_churn) : NULL;
    if (!fc) return -2;
    uint64_t c0 = me->base + fc->BeginAddress, c1 = me->base + fc->EndAddress;
    LONG f0 = g_guard_faults; LONG64 s0 = g_samples, t0 = g_sample_t;
    for (int i = 0; i < samples; i++) {
        CONTEXT c; uint64_t pcs[64]; int n = 0;
        if (!resume_first) n = sample_thread(th, lo, hi, &g_wd_mods, &c, pcs, 64);
        else {                                                // registers, resume, then walk the changing stack
            walk_t w; memset(&w, 0, sizeof w); w.c.ContextFlags = CONTEXT_FULL;
            w.lo = lo; w.hi = hi; w.mods = &g_wd_mods; w.pcs = pcs; w.max = 64;
            if (SuspendThread(th) == (DWORD)-1) continue;
            int got = GetThreadContext(th, &w.c);
            ResumeThread(th);
            if (got) { guarded(walk_body, &w); n = w.n; }
        }
        if (n > 0) ok++;
        int in = 0; for (int k = 0; k < n; k++) in += pcs[k] >= c0 && pcs[k] < c1;
        if (in >= 3) (*deep)++;
    }
    t_stop = 1; WaitForSingleObject(th, INFINITE); CloseHandle(th);
    *faults = (int)(g_guard_faults - f0);
    *avg_us = g_samples > s0 ? (g_sample_t - t0) * g_ms * 1000.0 / (g_samples - s0) : 0.0;
    return ok;
}
#endif

#ifdef EB_TEST
// manual check (not in the smoke tests: it needs the Windows event log): the record of process pid of exe
__declspec(dllexport) int eb_test_wer(const char* exe, unsigned long pid) {
    char line[512], sig[200];
    int f = find_wer(exe, (DWORD)pid, line, sizeof line, sig, sizeof sig);
    printf("%s%s%s\n", f ? line : "not found\n", f ? "signature: " : "", f ? sig : "");
    return f;
}
#endif

