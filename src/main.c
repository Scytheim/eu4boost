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
#include <string.h>
#include "MinHook.h"

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

// ---- hook sets (see common.h) ----
void eb_hs_init(eb_hookset* s, const char* name) { memset(s, 0, sizeof *s); s->name = name; }

static int hs_test_fail(eb_hookset* s) {        // [test] hook_fail_set / hook_fail_at: this operation fails
    return g_cfg.test_hook_fail_at > 0 && s->ops == g_cfg.test_hook_fail_at && !strcmp(s->name, g_cfg.test_hook_fail_set);
}

void eb_hs_undo(eb_hookset* s, const char* why) {
    int bad = 0, n = s->n;
    for (int i = n - 1; i >= 0; i--) {
        MH_RemoveHook(s->t[i]);                 // disables it first if it is on
        if (s->orig[i]) *s->orig[i] = NULL;
        if (memcmp(s->t[i], s->before[i], 16)) bad++;
    }
    s->n = 0; s->failed = 1;
    eb_log("%s: %s - %d hook(s) of the set taken out again, %s", s->name, why, n,
           bad ? "BUT THE GAME'S CODE DIFFERS FROM BEFORE" : "the game's code is as before");
}

int eb_hs_add(eb_hookset* s, void* t, void* fn, void** orig) {
    if (s->failed) return 0;
    char why[96];
    if (s->n >= EB_HS_MAX) { eb_hs_undo(s, "too many hooks for a set"); return 0; }
    s->ops++;
    memcpy(s->before[s->n], t, 16);
    MH_STATUS st = hs_test_fail(s) ? MH_ERROR_NOT_EXECUTABLE : MH_CreateHook(t, fn, orig);
    if (st != MH_OK) {
        snprintf(why, sizeof why, "hook %d could not be made (%s)%s", s->n + 1, MH_StatusToString(st), hs_test_fail(s) ? " - TEST" : "");
        eb_hs_undo(s, why);
        return 0;
    }
    s->t[s->n] = t; s->orig[s->n] = orig; s->n++;
    return 1;
}

int eb_hs_enable(eb_hookset* s) {
    if (s->failed || !s->n) return 0;
    for (int i = 0; i < s->n; i++) {
        s->ops++;
        MH_STATUS st = hs_test_fail(s) ? MH_ERROR_MEMORY_PROTECT : MH_EnableHook(s->t[i]);
        if (st != MH_OK) {
            char why[96];
            snprintf(why, sizeof why, "hook %d could not be enabled (%s)%s", i + 1, MH_StatusToString(st), hs_test_fail(s) ? " - TEST" : "");
            eb_hs_undo(s, why);
            return 0;
        }
    }
    return 1;
}

// ---- requested and active features (see common.h) ----
static char g_feat[3000];
static size_t g_feat_n;
void eb_feature(const char* name, int requested, int active, const char* why_off) {
    if (g_feat_n >= sizeof g_feat - 120) return;
    int k;
    if (active) k = snprintf(g_feat + g_feat_n, sizeof g_feat - g_feat_n, " %s=%d", name, active);
    else if (requested) k = snprintf(g_feat + g_feat_n, sizeof g_feat - g_feat_n, " %s=OFF(asked %d: %s)", name, requested, why_off ? why_off : "not installed");
    else k = snprintf(g_feat + g_feat_n, sizeof g_feat - g_feat_n, " %s=0", name);
    if (k > 0) g_feat_n += (size_t)k;
}
static void log_features(void) {                 // in lines of at most ~900 characters, cut between features
    const char* p = g_feat;
    while (*p == ' ') p++;
    while (*p) {
        size_t n = strlen(p);
        if (n > 900) { n = 900; while (n && p[n] != ' ') n--; if (!n) n = 900; }
        eb_log("features:%s %.*s", p == g_feat + 1 ? "" : " (cont.)", (int)n, p);
        p += n; while (*p == ' ') p++;
    }
}

#ifdef EB_TEST
// hook sets: every operation of a three-hook set fails once (1-3 = a create, 4-6 = an enable); the functions
// must then run as before, their bytes be as before and the handed-out trampolines be NULL
static volatile int hs_sink;
static __attribute__((noinline)) int hs_t1(int x) { hs_sink += x; return x * 3 + 1; }
static __attribute__((noinline)) int hs_t2(int x) { hs_sink ^= x; return x * 5 + 2; }
static __attribute__((noinline)) int hs_t3(int x) { hs_sink -= x; return x * 7 + 3; }
static int hs_d1(int x) { (void)x; return -1; }
static int hs_d2(int x) { (void)x; return -2; }
static int hs_d3(int x) { (void)x; return -3; }
__declspec(dllexport) int eb_test_hookset(void) {
    typedef int (*fn_t)(int);
    fn_t volatile t[3] = { hs_t1, hs_t2, hs_t3 };
    void* d[3] = { (void*)hs_d1, (void*)hs_d2, (void*)hs_d3 };
    void* o[3];
    uint8_t before[3][16];
    for (int i = 0; i < 3; i++) memcpy(before[i], (void*)t[i], 16);
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return 100;
    int bad = 0;
    for (int at = 0; at <= 6; at++) {
        strcpy(g_cfg.test_hook_fail_set, "test set"); g_cfg.test_hook_fail_at = at;
        eb_hookset hs; eb_hs_init(&hs, "test set");
        o[0] = o[1] = o[2] = (void*)1;
        int ok = 1;
        for (int i = 0; i < 3 && ok; i++) ok = eb_hs_add(&hs, (void*)t[i], d[i], &o[i]);
        ok = ok && eb_hs_enable(&hs);
        if (at == 0) {                              // all on: the detours answer, then the undo puts the code back
            if (!ok || t[0](1) != -1 || t[1](1) != -2 || t[2](1) != -3) bad++;
            eb_hs_undo(&hs, "test: taken out after the success case");
        } else if (ok) bad++;
        for (int i = 0; i < 3; i++) {
            if (t[i](2) != 2 * (3 + 2 * i) + 1 + i) bad++;           // 7, 12, 17: the originals
            if (memcmp(before[i], (void*)t[i], 16)) bad++;
            if (o[i] != (void*)1 && o[i] != NULL) bad++;             // made ones are NULL again, the others untouched
        }
        printf("hook set, operation %d fails: %s\n", at, ok ? "installed" : "taken out");
    }
    g_cfg.test_hook_fail_at = 0; g_cfg.test_hook_fail_set[0] = 0;
    return bad;
}
__declspec(dllexport) void eb_test_cfg(int* refresh, int* ignored) { *refresh = g_cfg.country_parallel_refresh; *ignored = g_cfg.country_refresh_ignored; }
#endif

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
    c->ai_land_memo         = ini_int(ini, L"parallel",  L"ai_land_memo", 0);      // experimental
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
    c->country_parallel_refresh = ini_int(ini, L"country", L"parallel_refresh", 0); // experimental, 2-4 = analysis modes
    // 0 = off, 2-4 = the analysis modes. 1 (on) is ignored since 0.4.10: the ini of 0.4.1-0.4.9 shipped it,
    // and a worker crashed in it (report 105). Any other value is off too.
    if (c->country_parallel_refresh != 0 && (c->country_parallel_refresh < 2 || c->country_parallel_refresh > 4)) {
        c->country_refresh_ignored = c->country_parallel_refresh; c->country_parallel_refresh = 0;
    }
    c->country_refresh_fixup = ini_int(ini, L"country", L"refresh_fixup", 1);
    c->country_parallel_war = ini_int(ini, L"country", L"parallel_war", 0);
    c->country_parallel_annex = ini_int(ini, L"country", L"parallel_annex", 0);
    c->country_test_annex = ini_int(ini, L"country", L"test_annex", 0);          // testing only, not in the ini files
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
        c->autorun_offscreen = ini_int(ini, L"test", L"autorun_offscreen", 0);
        c->autorun_input = ini_int(ini, L"test", L"autorun_input", 0);
        c->test_warview = ini_int(ini, L"test", L"warview", 0);
        c->test_ironman_name = ini_int(ini, L"test", L"ironman_name", 0);
        c->test_loc_fiber = ini_int(ini, L"test", L"loc_fiber", 0);
        c->test_diplo_freed = ini_int(ini, L"test", L"diplo_freed", 0);
        c->test_diplo_removed = ini_int(ini, L"test", L"diplo_removed", 0);
        c->test_slow_item_ms = ini_int(ini, L"test", L"slow_item_ms", 0);
        c->autorun_fastfail = ini_int(ini, L"test", L"autorun_fastfail", 0);
        GetPrivateProfileStringW(L"test", L"hook_fail_set", L"", w, 32, ini);
        WideCharToMultiByte(CP_ACP, 0, w, -1, c->test_hook_fail_set, sizeof c->test_hook_fail_set, NULL, NULL);
        c->test_hook_fail_at = ini_int(ini, L"test", L"hook_fail_at", 0);
        c->test_queue_fail = ini_int(ini, L"test", L"queue_fail", 0);
        c->test_msg_fail = ini_int(ini, L"test", L"msg_fail", 0);
        c->test_rng_count = ini_int(ini, L"test", L"rng_count", 0);
    }
    c->load_fast_rng        = ini_int(ini, L"load",      L"fast_rng_replay", 0);    // experimental: 1 = on, 2 = + verify
    c->console_hotkey       = ini_int(ini, L"console",   L"ironman_hotkey", 0);
    c->guard_tooltip        = ini_int(ini, L"guard",     L"tooltip", 1);
    c->guard_relation       = ini_int(ini, L"guard",     L"relation", 1);
    c->guard_great_project  = ini_int(ini, L"guard",     L"great_project", 1);
    c->guard_ironman_name   = ini_int(ini, L"guard",     L"ironman_name", 1);
    c->guard_loc_duplicates = ini_int(ini, L"guard",     L"loc_duplicates", 1);
    c->guard_diplo_reset    = ini_int(ini, L"guard",     L"diplomacy_reset", 1);
    c->leak_subject_modifiers = ini_int(ini, L"leaks",   L"subject_modifiers", 1);
    c->leak_ai_actions      = ini_int(ini, L"leaks",     L"ai_actions", 1);
    c->leak_province_items  = ini_int(ini, L"leaks",     L"province_items", 0);
    c->leak_diplo_actions   = ini_int(ini, L"leaks",     L"diplo_actions", 0);
    c->leak_message_popups  = ini_int(ini, L"leaks",     L"message_popups", 0);
    c->ui_timing            = ini_int(ini, L"general",   L"ui_timing", 0);       // diagnostics
    c->play_monitor         = ini_int(ini, L"diag",      L"play_monitor", 0);
    c->play_monitor_s       = ini_int(ini, L"diag",      L"play_monitor_s", 30);
    c->play_monitor_census_s = ini_int(ini, L"diag",     L"play_monitor_census_s", 600);
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
    if (g_cfg.country_refresh_ignored == 1)
        eb_log("config: [country] parallel_refresh=1 is ignored since 0.4.10 (a worker crashed in the parallel RefreshKnowledge, report 105)");
    else if (g_cfg.country_refresh_ignored)
        eb_log("config: [country] parallel_refresh=%d is not a valid value (0 = off, 2-4 = analysis modes) - off", g_cfg.country_refresh_ignored);
    if (g_cfg.test_hook_fail_at > 0)
        eb_log("config: TEST operation %d of the hook set \"%s\" fails", g_cfg.test_hook_fail_at, g_cfg.test_hook_fail_set);
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

    const eb_config_t req = g_cfg;   // what the ini asked for; the modules turn off what they could not install
    int r_alloc = 0;
    if (g_cfg.alloc_enabled) {
        if ((r_alloc = eb_alloc_install())) eb_log("allocator: mimalloc active");
        else eb_log("allocator: NOT installed (game uses its original heap)");
    } else {
        eb_log("allocator: disabled in eu4boost.ini");
    }
    int r_threads = eb_threads_install();
    int r_online = eb_online_install();
    eb_speed_install();
    eb_parallel_install();
    int r_zip = eb_savezip_install();
    eb_savepar_install();
    eb_savelist_install();       // before uitime: in its diagnostics mode it hooks the save list item
    int r_script = eb_script_install();
    int r_mod = eb_modifier_install();
    eb_country_install();
    eb_console_install();
    eb_guard_install();
    eb_leakfix_install();
    eb_aiprof_install();
    eb_uitime_install();
    eb_rng_install();
    eb_autorun_install();
    int r_crash = eb_crash_install();   // last: its frame counter chains to the other POPS hooks
    eb_playmon_install();
    eb_report_start();
    eb_update_start();
    if (g_cfg.prof_enabled) {
        if (eb_prof_start_thread()) eb_log("profiler: ready (Ctrl+Alt+P to start/stop)");
    }
    eb_feature("allocator", req.alloc_enabled, req.alloc_enabled && r_alloc, NULL);
    eb_feature("main_thread_free", req.main_affinity >= 0, req.main_affinity >= 0 && r_threads, NULL);
    eb_feature("online_poll_ms", req.online_poll_ms > 0 ? req.online_poll_ms : 0, req.online_poll_ms > 0 && r_online ? req.online_poll_ms : 0, NULL);
    eb_feature("days_per_frame", req.days_per_frame > 1 ? req.days_per_frame : 0, g_cfg.days_per_frame > 1 ? g_cfg.days_per_frame : 0, NULL);
    eb_feature("chunked", req.par_chunked, g_cfg.par_chunked, NULL);
    eb_feature("small_loop_caps", req.par_caps, g_cfg.par_caps, NULL);
    eb_feature("save_parallel_compress", req.save_par, req.save_par && r_zip, NULL);
    eb_feature("save_fast_format", req.save_fmt, g_cfg.save_fmt, NULL);
    eb_feature("save_parallel_countries", req.save_countries, g_cfg.save_countries, NULL);
    eb_feature("save_list_cache", req.save_list_cache, g_cfg.save_list_cache, NULL);
    eb_feature("fast_add", req.mod_fast, req.mod_fast && r_mod ? req.mod_fast : 0, NULL);
    eb_feature("fast_search", req.mod_search, req.mod_search && r_mod ? req.mod_search : 0, req.mod_fast ? NULL : "needs fast_add");
    eb_feature("prov_text_cache", req.prov_text_cache, g_cfg.prov_text_cache, NULL);
    eb_feature("fast_variables", req.script_fast_vars, req.script_fast_vars && r_script ? req.script_fast_vars : 0, NULL);
    eb_feature("parallel_refresh", req.country_parallel_refresh ? req.country_parallel_refresh : g_cfg.country_refresh_ignored,
               g_cfg.country_parallel_refresh, g_cfg.country_refresh_ignored == 1 ? "ignored since 0.4.10" : g_cfg.country_refresh_ignored ? "not a valid value" : NULL);
    eb_feature("parallel_war", req.country_parallel_war, g_cfg.country_parallel_war, NULL);
    eb_feature("parallel_annex", req.country_parallel_annex, g_cfg.country_parallel_annex, NULL);
    eb_feature("ai_prov_split", req.ai_prov_split, g_cfg.ai_prov_split, NULL);
    eb_feature("ai_ae_cache", req.ai_ae_cache, g_cfg.ai_ae_cache, NULL);
    eb_feature("ai_army_fast", req.ai_army_fast, g_cfg.ai_army_fast, NULL);
    eb_feature("ai_land_memo", req.ai_land_memo, g_cfg.ai_land_memo, req.ai_ae_cache ? NULL : "needs ai_ae_cache");
    eb_feature("guard_tooltip", req.guard_tooltip, g_cfg.guard_tooltip, NULL);
    eb_feature("guard_relation", req.guard_relation, g_cfg.guard_relation, NULL);
    eb_feature("guard_great_project", req.guard_great_project, g_cfg.guard_great_project, NULL);
    eb_feature("guard_ironman_name", req.guard_ironman_name, g_cfg.guard_ironman_name, NULL);
    eb_feature("guard_loc_duplicates", req.guard_loc_duplicates, g_cfg.guard_loc_duplicates, NULL);
    eb_feature("guard_diplomacy_reset", req.guard_diplo_reset, g_cfg.guard_diplo_reset, NULL);
    eb_feature("leak_subject_modifiers", req.leak_subject_modifiers, g_cfg.leak_subject_modifiers, NULL);
    eb_feature("leak_ai_actions", req.leak_ai_actions, g_cfg.leak_ai_actions, NULL);
    eb_feature("leak_province_items", req.leak_province_items, g_cfg.leak_province_items, NULL);
    eb_feature("leak_diplo_actions", req.leak_diplo_actions, g_cfg.leak_diplo_actions, NULL);
    eb_feature("leak_message_popups", req.leak_message_popups, g_cfg.leak_message_popups, NULL);
    eb_feature("fast_rng_replay", req.load_fast_rng, g_cfg.load_fast_rng, NULL);
    eb_feature("report_capture", req.report_capture, req.report_capture && r_crash, NULL);
    eb_feature("update_auto", req.update_auto, g_cfg.update_auto, NULL);
    if (req.play_monitor || req.mem_sites || req.ui_timing || req.country_stats || req.par_stats || req.ai_item_stats)
        eb_feature("diagnostics", 1, 1, NULL);
    log_features();
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        init();
    } else if (reason == DLL_THREAD_DETACH) {
        eb_alloc_thread_done();
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
        eb_rng_report();
        eb_guard_report();
        eb_leakfix_report();
        eb_playmon_report();
        eb_crash_exit();
        eb_log("process exit");
    }
    return TRUE;
}
