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
// hang: a watchdog thread counts frames (one POPS_RunCallbacks call with the first handle seen, as
//   in uitime.c). With no frame for hang_seconds (45) the main thread's stack is written; the report
//   is dropped if frames resume or the game exits normally, and sent if the session ends abnormally.
// slow ([report] slow=1): uitime.c times the save lists (the save dialog, the load screen, deleting)
//   and loads; one over its limit becomes an event. The watchdog also catches stalls - no frame for
//   slow_stall_ms (2 s) outside a load - and samples the main thread's stack at 1, 2, 4 and 8 s of the
//   stall; during a load it samples every 5 s. Events go to pending\<session>_slow\, which is sent
//   once the session is over (at most one slow report a day, report.c).
// test_keys=1 (testing only): Ctrl+Alt+Shift+F9 crashes the main thread, +F10 hangs it for 70 s,
//   +F11 stalls it for 3 s.
#include "common.h"
#include <stdio.h>
#include <string.h>
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

// call stack by unwinding (x64 unwind data of every module)
static int walk(const CONTEXT* c0, uint64_t* pcs, int max) {
    CONTEXT c = *c0; int n = 0;
    while (n < max) {
        uint64_t pc = c.Rip, sp = c.Rsp;
        if (!pc || !readable(pc, 1)) break;
        pcs[n++] = pc;
        if (!readable(sp, 512)) break;
        DWORD64 base = 0; PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(pc, &base, NULL);
        if (f) { PVOID hd; DWORD64 ef; RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, pc, f, &c, &hd, &ef, NULL); }
        else { c.Rip = *(DWORD64*)sp; c.Rsp = sp + 8; }       // leaf function: return address on top
        if (!c.Rip || (c.Rip == pc && c.Rsp == sp)) break;
    }
    return n;
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
static void s_stack(sb_t* b, const CONTEXT* c) {
    uint64_t pcs[64]; int n = walk(c, pcs, 64);
    s_put(b, "stack (unwound):\r\n");
    for (int i = 0; i < n; i++) { s_put(b, "  "); s_dec(b, i); s_put(b, "  "); s_addr(b, pcs[i]); s_put(b, "\r\n"); }
    s_put(b, "return addresses on the stack:\r\n");
    s_scan(b, c->Rsp, 48);
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
static void set_session(const char* state) {
    char t[160]; sb_t b = { t, 0, sizeof t };
    s_put(&b, "state="); s_put(&b, state); s_put(&b, "\r\nstart="); s_w(&b, g_start);
    s_put(&b, "\r\nversion=" EU4BOOST_VERSION "\r\n");
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
static void get_ctx(HANDLE th, CONTEXT* c) {                // another thread's registers (stopped only meanwhile)
    memset(c, 0, sizeof *c); c->ContextFlags = CONTEXT_FULL;
    if (SuspendThread(th) == (DWORD)-1) { c->Rip = 0; return; }
    if (!GetThreadContext(th, c)) c->Rip = 0;
    ResumeThread(th);
}
static DWORD WINAPI crash_writer(LPVOID unused) {
    (void)unused;
    EXCEPTION_RECORD* er = g_cep->ExceptionRecord; CONTEXT* c = g_cep->ContextRecord;
    sb_t b = { g_cbuf, 0, sizeof g_cbuf };
    s_put(&b, "eu4boost crash report\r\nkind: crash\r\nsignature: "); s_put(&b, exc_name(er->ExceptionCode)); s_put(&b, " ");
    uint64_t pcs[64]; int n = walk(c, pcs, 64), top = 0;
    while (top < n && !interesting(pcs[top])) top++;
    s_addr(&b, top < n ? pcs[top] : (uint64_t)(uintptr_t)er->ExceptionAddress);
    s_put(&b, "\r\nexception: "); s_hex(&b, er->ExceptionCode, 8); s_put(&b, " "); s_put(&b, exc_name(er->ExceptionCode));
    s_put(&b, " at "); s_addr(&b, (uint64_t)(uintptr_t)er->ExceptionAddress);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        ULONG_PTR k = er->ExceptionInformation[0];
        s_put(&b, "\r\naccess: "); s_put(&b, k == 0 ? "read" : k == 1 ? "write" : k == 8 ? "execute" : "?"); s_put(&b, " "); s_hex(&b, er->ExceptionInformation[1], 16);
    }
    s_put(&b, "\r\nthread: "); s_put(&b, g_ctid == g_main_tid ? "main" : "other"); s_put(&b, "\r\n");
    s_state(&b);
    s_regs(&b, c);
    s_stack(&b, c);
    if (g_ctid != g_main_tid && g_main_thread) {
        CONTEXT mc; get_ctx(g_main_thread, &mc);
        if (mc.Rip) { s_put(&b, "main thread:\r\n  rip="); s_addr(&b, mc.Rip); s_put(&b, "\r\n"); s_stack(&b, &mc); }
    }
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
static void write_hang(int seconds) {
    static char t[64 * 1024];
    sb_t b = { t, 0, sizeof t };
    CONTEXT mc; get_ctx(g_main_thread, &mc);
    uint64_t pcs[64]; int n = mc.Rip ? walk(&mc, pcs, 64) : 0, top = 0;
    while (top < n && !interesting(pcs[top])) top++;
    s_put(&b, "eu4boost crash report\r\nkind: hang\r\nsignature: HANG ");
    if (top < n) s_addr(&b, pcs[top]); else s_put(&b, "unknown");
    s_put(&b, "\r\nno frame for: "); s_dec(&b, seconds); s_put(&b, " s\r\n");
    s_state(&b);
    if (mc.Rip) { s_put(&b, "main thread:\r\n"); s_regs(&b, &mc); s_stack(&b, &mc); }
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
    }
    LeaveCriticalSection(&g_slow_cs);
}
// the session is over: events.txt becomes report.txt, which report.c sends
static void finalize_slow(const wchar_t* dir, const wchar_t* log) {
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
    copy_log(dir, log);
    DeleteFileW(u);
}
// one sample of the main thread's stack (the thread is stopped only while its registers are read)
static int sample_main(char* out, int cap, char* top, int tcap) {
    CONTEXT mc; get_ctx(g_main_thread, &mc);
    out[0] = 0; if (top) top[0] = 0;
    if (!mc.Rip) return 0;
    uint64_t pcs[32]; int n = walk(&mc, pcs, 32), first = -1;
    sb_t b = { out, 0, cap };
    for (int i = 0; i < n && i < 16; i++) { if (i) s_put(&b, " < "); s_addr(&b, pcs[i]); }
    for (int i = 0; i < n && first < 0; i++) if (interesting(pcs[i])) first = i;
    if (top) { sb_t t = { top, 0, tcap }; if (first >= 0) s_addr(&t, pcs[first]); else s_put(&t, "unknown"); }
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

static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    LONG64 seen = -1; ULONGLONG since = 0, load_t0 = 0, load_next = 0; int reported = 0, ns = 0, in_load = 0;
    static char samp[4][1024], tops[4][160], tmp[1024];
    int samp_s[4];
    for (;;) {
        Sleep(250);
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
                static char d[6 * 1024]; int k = snprintf(d, sizeof d, "no frame for %llu ms; main thread at", (unsigned long long)gap);
                for (int i = 0; i < ns; i++) k += snprintf(d + k, sizeof d - k, "\r\n  %d s: %s", samp_s[i], samp[i]);
                char what[200]; snprintf(what, sizeof what, "stall %s", tops[0]);
                eb_slow_event(what, (double)gap, g_cfg.slow_stall_ms, d);
            }
            seen = f; since = now; reported = 0; ns = 0; in_load = 0; continue;
        }
        if (f == 0 || g_crashed) continue;                     // not started yet / already written
        ULONGLONG gap = now - g_frame_ms;
        if (eb_loading) in_load = 1;
        if (g_cfg.report_slow && !in_load && ns < 4 && gap >= (1000ull << ns) && g_main_thread) {
            sample_main(samp[ns], sizeof samp[0], tops[ns], sizeof tops[0]); samp_s[ns] = (int)(gap / 1000); ns++;
        }
        if (g_cfg.report_hang_s > 0 && !reported && now - since >= (ULONGLONG)g_cfg.report_hang_s * 1000) {
            reported = 1;
            eb_log("report: no frame for %d s - main thread stack written (dropped if the game recovers)", (int)((now - since) / 1000));
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
            g_frames++; g_frame_ms = now;
            if (g_cfg.report_test_keys && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000) &&
                (GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
                if (GetAsyncKeyState(VK_F9) & 0x8000) { eb_log("report: TEST crash (Ctrl+Alt+Shift+F9)"); *(volatile int*)(uintptr_t)8 = 1; }
                if (GetAsyncKeyState(VK_F10) & 0x8000) { eb_log("report: TEST hang for 70 s (Ctrl+Alt+Shift+F10)"); Sleep(70000); }
                if (GetAsyncKeyState(VK_F11) & 0x8000) { eb_log("report: TEST stall for 3 s (Ctrl+Alt+Shift+F11)"); Sleep(3000); }
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
static void attach_game_logs(const wchar_t* dir) {
    wchar_t d[MAX_PATH], s[MAX_PATH];
    if (!eb_docs_dir(d, MAX_PATH)) return;
    _snwprintf(s, MAX_PATH, L"%lslogs\\game.log", d);   copy_tail(s, dir, L"game.log", 16 * 1024);
    _snwprintf(s, MAX_PATH, L"%lslogs\\error.log", d);  copy_tail(s, dir, L"error.log", 16 * 1024);
    _snwprintf(s, MAX_PATH, L"%lslogs\\system.log", d); copy_tail(s, dir, L"system.log", 64 * 1024);
    _snwprintf(s, MAX_PATH, L"%lsdlc_load.json", d);    copy_tail(s, dir, L"dlc_load.json", 16 * 1024);
}
static void check_previous(void) {
    char t[256] = {0};
    HANDLE h = CreateFileW(g_session, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;                      // first run with reports
    DWORD rd = 0; ReadFile(h, t, sizeof t - 1, &rd, NULL); CloseHandle(h);
    int clean = !strncmp(t, "state=exit", 10);
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
            if (nl > 5 && !wcscmp(fd.cFileName + nl - 5, L"_slow")) { finalize_slow(d, L"eu4boost_prev.log"); attach_game_logs(d); continue; }
            if (clean) continue;
            _snwprintf(u, MAX_PATH, L"%lsunconfirmed", d); DeleteFileW(u);
            attach_game_logs(d); found = 1;
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    if (clean) return;
    if (!found) {
        char r[512]; sb_t b = { r, 0, sizeof r };
        s_put(&b, "eu4boost crash report\r\nkind: exit\r\nsignature: ABNORMAL_EXIT\r\n"
                  "The previous session (started "); s_w(&b, prev); s_put(&b, ") ended without a normal exit and without a crash the plugin "
                  "could catch: a crash through __fastfail (heap corruption, abort), a killed process, a shutdown or a power loss.\r\n");
        wchar_t dir[MAX_PATH]; new_dir(dir, prev[0] ? prev : g_start, L"exit");
        write_file(dir, L"report.txt", r, b.n);
        copy_log(dir, L"eu4boost_prev.log");
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
    check_previous();
    set_session("running");
    g_main_thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_main_tid);
    g_next_filter = SetUnhandledExceptionFilter(eb_filter);
    MH_STATUS st = MH_Initialize();
    void* target = NULL;
    int hooked = (st == MH_OK || st == MH_ERROR_ALREADY_INITIALIZED) &&
                 MH_CreateHookApiEx(L"kernelbase", "SetUnhandledExceptionFilter", (void*)hk_set_uef, (void**)&o_set_uef, &target) == MH_OK &&
                 MH_EnableHook(target) == MH_OK;
    if (!hooked) eb_log("report: SetUnhandledExceptionFilter hook failed - a filter installed later replaces ours");
    o_rcb = (pops_rcb_t)patch_iat(g_exe, "pops_api.dll", "POPS_RunCallbacks", (void*)hk_rcb);
    if (o_rcb && g_main_thread && (g_cfg.report_hang_s > 0 || g_cfg.report_slow)) {
        HANDLE h = CreateThread(NULL, 256 * 1024, watchdog, NULL, 0, NULL);
        if (h) CloseHandle(h);
    }
    eb_log("report: crash, hang (%d s), abnormal-exit%s reports are written to crash_reports\\%s", g_cfg.report_hang_s,
           g_cfg.report_slow ? " and slow (stalls over 2 s, save lists, loads)" : "",
           g_cfg.report_test_keys ? " - TEST KEYS ON (Ctrl+Alt+Shift+F9 crash, F10 hang, F11 stall)" : "");
    return 1;
#endif
}

void eb_crash_exit(void) {                                    // DLL_PROCESS_DETACH at process exit
    if (!g_cfg.report_capture || !g_session[0] || g_crashed) return;
    drop_unconfirmed(g_start);
    if (g_slow_dir[0]) finalize_slow(g_slow_dir, L"eu4boost.log");
    set_session("exit");
}
