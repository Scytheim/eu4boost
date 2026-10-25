// eu4boost - performance plugin for Europa Universalis IV 1.37.5 (eu4.exe, x64)
//
// Loaded as a plugin DLL (e.g. by EU4dll's version.dll loader from the game's plugins\ folder).
// Everything happens in DllMain on the loading thread, before eu4.exe's own CRT starts:
//   * replaces eu4.exe's statically linked CRT heap (malloc/free/...) with mimalloc
//   * optionally starts a low-overhead sampling profiler (toggle with Ctrl+Alt+P in game)
//   * logs the real time per game day (speed.c)
#include "common.h"
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <stdlib.h>

eb_config_t g_cfg;
HMODULE g_self;
HMODULE g_exe;
DWORD g_main_tid;
wchar_t g_dir[MAX_PATH];

static CRITICAL_SECTION g_log_cs;
static HANDLE g_log_file = INVALID_HANDLE_VALUE;

void eb_log(const char* fmt, ...) {
    if (!g_cfg.log || g_log_file == INVALID_HANDLE_VALUE) return;
    char buf[1024];
    SYSTEMTIME st; GetLocalTime(&st);
    int n = snprintf(buf, sizeof buf, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof buf - n - 2, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    n += m; if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
    buf[n++] = '\r'; buf[n++] = '\n';
    EnterCriticalSection(&g_log_cs);
    DWORD w; WriteFile(g_log_file, buf, (DWORD)n, &w, NULL);
    LeaveCriticalSection(&g_log_cs);
}

static int ini_int(const wchar_t* ini, const wchar_t* sec, const wchar_t* key, int def) {
    return (int)GetPrivateProfileIntW(sec, key, def, ini);
}

static void load_config(void) {
    wchar_t ini[MAX_PATH];
    _snwprintf(ini, MAX_PATH, L"%lseu4boost.ini", g_dir);
    eb_config_t* c = &g_cfg;
    c->log                  = ini_int(ini, L"general",   L"log", 1);
    c->speed_log            = ini_int(ini, L"general",   L"speed_log", 1);
    c->state_checksum       = ini_int(ini, L"debug",     L"state_checksum", 0);
    c->fixed_seed           = ini_int(ini, L"debug",     L"fixed_seed", 0);
    c->par_stats            = ini_int(ini, L"parallel",  L"stats", 0);
    c->par_min_items        = ini_int(ini, L"parallel",  L"min_items_per_task", 0);
    c->par_spin_us          = ini_int(ini, L"parallel",  L"worker_spin_us", 0);
    c->par_spin_workers     = ini_int(ini, L"parallel",  L"spin_workers", 4);
    c->par_ab               = ini_int(ini, L"parallel",  L"ab_test", 0);
    c->par_chunked          = ini_int(ini, L"parallel",  L"chunked", 1);           // on since 0.3.0 (validated)
    c->par_caps             = ini_int(ini, L"parallel",  L"small_loop_caps", 1);   // on since 0.3.0 (validated)
    c->par_lpt              = ini_int(ini, L"parallel",  L"longest_first", 0);     // experimental
    c->ai_item_stats        = ini_int(ini, L"parallel",  L"ai_item_stats", 0);     // diagnostics
    c->ai_split             = ini_int(ini, L"parallel",  L"ai_split", 0);          // experimental
    c->ai_split_min         = ini_int(ini, L"parallel",  L"ai_split_min", 8);
    c->ai_prov_split        = ini_int(ini, L"parallel",  L"ai_prov_split", 0);     // experimental
    c->ai_prov_min          = ini_int(ini, L"parallel",  L"ai_prov_min", 50);
    c->ai_ae_cache          = ini_int(ini, L"parallel",  L"ai_ae_cache", 0);       // experimental
    c->ai_army_fast         = ini_int(ini, L"parallel",  L"ai_army_fast", 0);      // experimental
    c->alloc_enabled        = ini_int(ini, L"allocator", L"enabled", 1);
    c->alloc_guard_foreign  = ini_int(ini, L"allocator", L"guard_foreign", 1);
    c->alloc_large_pages    = ini_int(ini, L"allocator", L"large_pages", 0);
    c->alloc_reserve_mb     = ini_int(ini, L"allocator", L"reserve_mb", 0);
    c->alloc_purge_delay_ms = ini_int(ini, L"allocator", L"purge_delay_ms", 250);
    c->alloc_zero_fill      = ini_int(ini, L"allocator", L"zero_fill", -1);
    c->prof_enabled         = ini_int(ini, L"profiler",  L"enabled", 0);
    c->prof_interval_us     = ini_int(ini, L"profiler",  L"interval_us", 1000);
    c->prof_max_depth       = ini_int(ini, L"profiler",  L"max_depth", 48);
    c->prof_max_seconds     = ini_int(ini, L"profiler",  L"max_seconds", 600);
    c->prof_autostart       = ini_int(ini, L"profiler",  L"autostart", 0);
    c->prof_all_threads_wall= ini_int(ini, L"profiler",  L"wall_clock_all_threads", 0);
    c->bg_full_speed        = ini_int(ini, L"threads",   L"background_full_speed", 0);
    c->speed5_midtick_ms    = ini_int(ini, L"render",    L"speed5_midtick_frame_ms", 50);
    c->days_per_frame       = ini_int(ini, L"render",    L"speed5_days_per_frame", 1);    // experimental
    c->frame_budget_ms      = ini_int(ini, L"render",    L"speed5_frame_budget_ms", 100);
    c->online_poll_ms       = ini_int(ini, L"online",    L"poll_interval_ms", 100);
    c->online_running_only  = ini_int(ini, L"online",    L"only_while_running", 1);
    c->zero_min_bytes       = ini_int(ini, L"allocator", L"zero_fill_min_bytes", 65536);
    c->zero_os_kb           = ini_int(ini, L"allocator", L"zero_fill_windows_kb", 1024);
    c->save_par             = ini_int(ini, L"save",      L"parallel_compress", 0); // experimental
    c->save_block_kb        = ini_int(ini, L"save",      L"block_kb", 2048);
    c->save_threads         = ini_int(ini, L"save",      L"threads", 0);
    c->save_stats           = ini_int(ini, L"save",      L"stats", 0);
    c->save_fmt             = ini_int(ini, L"save",      L"fast_format", 0);      // experimental
    c->save_countries       = ini_int(ini, L"save",      L"parallel_countries", 0); // experimental
    c->save_list_cache      = ini_int(ini, L"save",      L"list_cache", 1);
    c->script_fast_vars     = ini_int(ini, L"script",    L"fast_variables", 1);
    c->mod_fast             = ini_int(ini, L"modifiers", L"fast_add", 0);         // experimental
    c->mod_verify           = ini_int(ini, L"modifiers", L"verify", 0);
    c->mod_search           = ini_int(ini, L"modifiers", L"fast_search", 1);
    c->prov_text_cache      = ini_int(ini, L"modifiers", L"prov_text_cache", 0);   // experimental
    c->mod_stats            = ini_int(ini, L"modifiers", L"stats", 0);
    c->country_stats        = ini_int(ini, L"country",   L"stats", 0);
    c->country_parallel_refresh = ini_int(ini, L"country", L"parallel_refresh", 0); // experimental
    c->country_refresh_fixup = ini_int(ini, L"country", L"refresh_fixup", 1);
    c->country_parallel_war = ini_int(ini, L"country", L"parallel_war", 0);
    c->country_test_war_all = ini_int(ini, L"country", L"test_war_all", 0);       // testing only, not in the ini files
    c->country_test_truce_end = ini_int(ini, L"country", L"test_truce_end", 0);   // testing only, not in the ini files
    c->country_test_no_defer = ini_int(ini, L"country", L"test_no_defer", 0);
    {
        wchar_t w[32]; char a[32];                              // testing only, not in the ini files
        GetPrivateProfileStringW(L"test", L"autorun_until", L"", w, 32, ini);
        WideCharToMultiByte(CP_ACP, 0, w, -1, a, sizeof a, NULL, NULL);
        c->autorun_until = a[0] ? eb_autorun_date(a) : 0;
        c->autorun_stuck_s = ini_int(ini, L"test", L"autorun_stuck_s", 300);
        c->autorun_minimize = ini_int(ini, L"test", L"autorun_minimize", 1);
        c->test_warview = ini_int(ini, L"test", L"warview", 0);
        c->test_ironman_name = ini_int(ini, L"test", L"ironman_name", 0);
        c->autorun_fastfail = ini_int(ini, L"test", L"autorun_fastfail", 0);
    }
    c->console_hotkey       = ini_int(ini, L"console",   L"ironman_hotkey", 0);
    c->guard_tooltip        = ini_int(ini, L"guard",     L"tooltip", 1);
    c->guard_relation       = ini_int(ini, L"guard",     L"relation", 1);
    c->guard_great_project  = ini_int(ini, L"guard",     L"great_project", 1);
    c->guard_ironman_name   = ini_int(ini, L"guard",     L"ironman_name", 1);
    c->ui_timing            = ini_int(ini, L"general",   L"ui_timing", 0);       // diagnostics
    c->mem_sites            = ini_int(ini, L"diag",      L"mem_sites", 0);
    c->mem_sites_s          = ini_int(ini, L"diag",      L"mem_sites_s", 60);
    c->report_capture       = ini_int(ini, L"report",    L"capture", 1);
    c->report_upload        = ini_int(ini, L"report",    L"upload", -1);
    c->report_hang_s        = ini_int(ini, L"report",    L"hang_seconds", 45);
    c->report_test_keys     = ini_int(ini, L"report",    L"test_keys", 0);
    c->report_slow          = ini_int(ini, L"report",    L"slow", 1);
    c->slow_ui_ms           = ini_int(ini, L"report",    L"slow_list_ms", 300);
    c->slow_stall_ms        = ini_int(ini, L"report",    L"slow_stall_ms", 2000);
    c->slow_load_s          = ini_int(ini, L"report",    L"slow_load_s", 90);
    if (c->slow_ui_ms < 1) c->slow_ui_ms = 1;
    if (c->slow_stall_ms < 1000) c->slow_stall_ms = 1000;
    if (c->slow_load_s < 1) c->slow_load_s = 1;
    GetPrivateProfileStringW(L"report", L"url", L"", c->report_url, 256, ini);
    c->update_auto          = ini_int(ini, L"update",    L"auto", 1);
    GetPrivateProfileStringW(L"update", L"url", L"", c->update_url, 256, ini);
    {
        wchar_t v[32];
        GetPrivateProfileStringW(L"threads", L"main_thread", L"free", v, 32, ini);
        if (_wcsicmp(v, L"game") == 0) c->main_affinity = -1;
        else if (_wcsnicmp(v, L"cpu", 3) == 0) c->main_affinity = _wtoi(v + 3) + 1;
        else c->main_affinity = 0;
    }
    if (c->prof_interval_us < 250) c->prof_interval_us = 250;
    if (c->prof_max_depth < 1) c->prof_max_depth = 1;
    if (c->prof_max_depth > 64) c->prof_max_depth = 64;
}

static void open_log(void) {
    if (!g_cfg.log) return;
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%lseu4boost.log", g_dir);
    // keep the previous run's log (eu4boost_prev.log), e.g. for A/B pairs
    wchar_t prev[MAX_PATH];
    _snwprintf(prev, MAX_PATH, L"%lseu4boost_prev.log", g_dir);
    MoveFileExW(path, prev, MOVEFILE_REPLACE_EXISTING);
    g_log_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

static void init(void) {
    InitializeCriticalSection(&g_log_cs);
    GetModuleFileNameW(g_self, g_dir, MAX_PATH);
    wchar_t* slash = wcsrchr(g_dir, L'\\');
    if (slash) slash[1] = 0;
    load_config();
    open_log();
    g_exe = GetModuleHandleW(NULL);
    g_main_tid = GetCurrentThreadId();

    // Never let this DLL be unloaded: hooked code points into it.
    HMODULE pinned;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)(void*)&init, &pinned);

    wchar_t exe_path[MAX_PATH]; GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    eb_log("eu4boost %s loaded into %ls (base %p), loader thread %lu", EU4BOOST_VERSION, exe_path, (void*)g_exe, (unsigned long)g_main_tid);
    eb_log("config: allocator=%d guard=%d large_pages=%d reserve_mb=%d purge_delay_ms=%d | profiler=%d autostart=%d interval_us=%d max_seconds=%d | main_thread=%d background_full_speed=%d | speed5_midtick_frame_ms=%d | online_poll_ms=%d zero_fill=%d/%d",
           g_cfg.alloc_enabled, g_cfg.alloc_guard_foreign, g_cfg.alloc_large_pages, g_cfg.alloc_reserve_mb, g_cfg.alloc_purge_delay_ms,
           g_cfg.prof_enabled, g_cfg.prof_autostart, g_cfg.prof_interval_us, g_cfg.prof_max_seconds, g_cfg.main_affinity, g_cfg.bg_full_speed, g_cfg.speed5_midtick_ms, g_cfg.online_poll_ms, g_cfg.alloc_zero_fill, g_cfg.zero_min_bytes);
    if (g_cfg.par_ab) {
        // Alternate between "experiments off" (A) and "as configured" (B) on every launch, so two
        // runs in a row give an A/B pair without editing the ini in between.
        wchar_t path[MAX_PATH]; _snwprintf(path, MAX_PATH, L"%lseu4boost_ab.txt", g_dir);
        int n = 0; char buf[32] = {0}; DWORD rd = 0;
        HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) { ReadFile(h, buf, sizeof buf - 1, &rd, NULL); CloseHandle(h); n = atoi(buf); }
        h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) { int len = snprintf(buf, sizeof buf, "%d", n + 1); WriteFile(h, buf, (DWORD)len, &rd, NULL); CloseHandle(h); }
        // ab_test=1: A = all parallel experiments off; 2: A = longest_first and modifiers fast_add off;
        // 3: A = one day per frame at speed 5; 4: A = country parallel_refresh off; 5: A = modifiers fast_search off
        if ((n & 1) == 0) {
            if (g_cfg.par_ab == 2) { g_cfg.par_lpt = 0; g_cfg.mod_fast = 0; }
            else if (g_cfg.par_ab == 3) g_cfg.days_per_frame = 1;
            else if (g_cfg.par_ab == 4) g_cfg.country_parallel_refresh = 0;
            else if (g_cfg.par_ab == 5) g_cfg.mod_search = 0;
            else { g_cfg.par_spin_us = 0; g_cfg.par_min_items = 0; g_cfg.par_chunked = 0; g_cfg.par_caps = 0; g_cfg.par_lpt = 0; }
        }
        eb_log("A/B test: launch #%d -> variant %s", n + 1, (n & 1) ? "B (experiments ON)" : "A (experiments OFF)");
    }
    SYSTEM_INFO si; GetSystemInfo(&si);
    eb_log("logical processors: %lu", (unsigned long)si.dwNumberOfProcessors);

    if (g_cfg.alloc_enabled) {
        if (eb_alloc_install()) eb_log("allocator: mimalloc active");
        else eb_log("allocator: NOT installed (game uses its original heap)");
    } else {
        eb_log("allocator: disabled in eu4boost.ini");
    }
    eb_threads_install();
    eb_online_install();
    eb_speed_install();
    eb_parallel_install();
    eb_savezip_install();
    eb_savepar_install();
    eb_savelist_install();       // before uitime: in its diagnostics mode it hooks the save list item
    eb_script_install();
    eb_modifier_install();
    eb_country_install();
    eb_console_install();
    eb_guard_install();
    eb_aiprof_install();
    eb_uitime_install();
    eb_autorun_install();
    eb_crash_install();          // last: its frame counter chains to the other POPS hooks
    eb_report_start();
    eb_update_start();
    if (g_cfg.prof_enabled) {
        if (eb_prof_start_thread()) eb_log("profiler: ready (Ctrl+Alt+P to start/stop)");
    }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        init();
    } else if (reason == DLL_PROCESS_DETACH) {
        // reserved != NULL means the process is exiting: other threads are already gone.
        if (g_cfg.prof_enabled) eb_prof_shutdown();
        if (g_cfg.alloc_enabled) eb_alloc_report();
        eb_online_report();
        eb_speed_report();
        eb_parallel_report();
        eb_savezip_report();
        eb_savepar_report();
        eb_savelist_report();
        eb_script_report();
        eb_modifier_report();
        eb_country_report();
        eb_guard_report();
        eb_crash_exit();
        eb_log("process exit");
    }
    return TRUE;
}
