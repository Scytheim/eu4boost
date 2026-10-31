// eu4boost - shared declarations
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stddef.h>

#define EU4BOOST_VERSION "0.4.11"
#define EB_W2(x) L##x
#define EB_W(x) EB_W2(x)        // wide string literal of a macro

typedef struct {
    int log;
    int speed_log;                // 1 = write real time per game day (eu4boost_speed_*.csv)
    int state_checksum;           // 1 = write the game's 140 OOS checksums every day (validation only)
    int fixed_seed;               // != 0: behave like the game's -seed=N option (deterministic runs)
    // parallel-for experiments (parallel.c)
    int par_stats;                // 1 = per-loop timing statistics in the log
    int par_min_items;            // > 0: at most count/K tasks per loop (loops without completion callback)
    int par_spin_us;              // > 0: idle workers spin this long before sleeping
    int par_spin_workers;         // max. workers spinning at once
    int par_ab;                   // 1 = alternate experiments off/on on every launch
    int par_chunked;              // 1 = claim several items per atomic op, main thread pauses while waiting
    int par_caps;                 // 1 = fewer tasks for known tiny loops (built-in table)
    int par_lpt;                  // 1 = uneven loops (built-in table) take their slowest items first
    int ai_item_stats;            // aiprof.c: 1 = time every country of the AI loop FUN_14077d8a0 (diagnostics)
    int ai_split;                 // aiprof.c: EXPERIMENT 1 = a country's army AI runs beside its other AI tasks
    int ai_split_min;             // ... only for countries with at least this many army/fleet AI nodes
    int ai_prov_split;            // aiprof.c: EXPERIMENT 1 = province evaluation scored on all cores, 2 = + verify
    int ai_prov_min;              // ... only for countries with at least this many provinces
    int ai_ae_cache;              // aiprof.c: EXPERIMENT 1 = per-thread lock-free table for the AI loop's AE cache, 2 = + verify, 3 = A/B
    int ai_army_fast;             // aiprof.c: EXPERIMENT 1 = the army move check tests its cheap conditions first, 2 = + verify, 3 = A/B
    int ai_land_memo;             // aiprof.c: EXPERIMENT 1 = the land searches of a deal fill in the AI loop are kept for that fill (needs ai_ae_cache), 2 = + verify, 3 = A/B
    // allocator
    int alloc_enabled;
    int alloc_guard_foreign;      // also catch mimalloc pointers freed through the Windows heap API
    int alloc_large_pages;        // use 2MB large pages (needs "Lock pages in memory")
    int alloc_reserve_mb;         // pre-reserve this much (MB) for mimalloc; with large pages it is committed up front
    int alloc_purge_delay_ms;     // mimalloc purge delay (-1 = never return memory to the OS)
    int alloc_zero_fill;          // -1 auto (EU4dll present -> 2), 0 off, 1 zero every malloc, 2 zero only large mallocs
    // profiler
    int prof_enabled;
    int prof_interval_us;         // sampling period
    int prof_max_depth;           // stack frames per sample
    int prof_max_seconds;         // auto stop after this many seconds (0 = until toggled off / exit)
    int prof_autostart;           // start sampling immediately at launch
    int prof_all_threads_wall;    // 1 = sample every thread every tick (wall clock); 0 = only threads that used CPU
    // threads
    int speed5_midtick_ms;        // min. time between frames rendered in the middle of a long tick at speed 5 (game: 50)
    int days_per_frame;           // speed 5: run up to this many days per frame (1 = game default)
    int frame_budget_ms;          // ... while the frame has used less than this
    int bg_full_speed;            // 1 = do not throttle the game when its window is not focused
    int main_affinity;            // -1 = game default (pinned), 0 = free, k+1 = pin to logical CPU k
    // online services
    int online_poll_ms;           // min. interval between POPS/Nakama callback pumps (0 = every frame, game default)
    int online_running_only;      // 1 = ... only while game days are processed (paused, menus, loading: every frame)
    // allocator zero-fill threshold
    int zero_min_bytes;           // in large-only zero mode, zero allocations of at least this size
    int zero_os_kb;               // zeroed allocations of at least this many KB come from the Windows heap (0 = mimalloc zeroes them)
    // saves (savezip.c)
    int save_par;                 // 1 = compress the zip entries of a save on all cores
    int save_block_kb;            // block size for the parallel compression
    int save_threads;             // threads per entry incl. the calling one (0 = all logical processors)
    int save_stats;               // 1 = log the time of every save and of its compression
    int save_fmt;                 // 1 = exact fast integer formatting at two sprintf call sites of the save writer
    int save_countries;           // savepar.c: 1 = serialize the countries of a save on all cores, 2/3 = + verify
    int save_list_cache;          // savelist.c: 1 = unchanged saves are listed from cached meta without opening them, 2 = + log every item
    int script_fast_vars;         // script.c: 1 = variable reads search the scope's store without building store lists, 2 = + verify
    // modifiers (modifier.c)
    int mod_fast;                 // 1 = O(n) add of modifier containers into sparse ones (same result)
    int mod_verify;               // 1 = diagnostics: run the game's add and compare the fast path with it
    int mod_stats;                // 1 = count the adds by path (diagnostics)
    int mod_search;               // 1 = small adds and dense containers: the game's algorithm with a SIMD search
    int prov_text_cache;          // guard.c: EXPERIMENT 1 = the monthly province text of a country's modifier 0x98 is built once a day, 2 = + verify, 3 = A/B
    // country update (country.c)
    int country_stats;            // 1 = diagnostics: time the country loop and its dirty-bundle functions
    int country_parallel_refresh; // EXPERIMENT: RefreshKnowledge of dirty countries on all cores at the loop start, 2/3 = + verify, 4 = race check (1 = on is ignored since 0.4.10)
    int country_refresh_ignored;  // the ini's parallel_refresh value when it was not used (1 since 0.4.10, or out of range), logged
    int country_refresh_fixup;    // 1 = recompute the cached values of FUN_1402f1120 (+0xe1c..+0xe34) at the country's turn
    int country_parallel_war;     // 1 = the war refresh (all participants of a war joined or declared) on all cores, 2 = + verify
    int country_parallel_annex;   // 1 = the opinions recomputed when a country disappears on all cores, 2 = + verify
    int country_test_annex;       // testing only ([country] test_annex=D): every D country loops the same for a random country
    int country_test_war_all;     // testing only: every N country loops all countries that are not dirty go through the war refresh
    int country_test_truce_end;   // testing only: N precomputed countries a day get a truce with the player that has just ended
    int country_test_no_defer;    // testing only: 1 = workers run the message handler themselves (as 0.4.3 did)
    int autorun_until;            // testing only ([test] autorun_until=1877.01.01): after the load speed 5 to this date, then exit (0 = off)
    int autorun_stuck_s;          // testing only: close the game after this many seconds without a new day
    int autorun_fastfail;         // testing only: end the autorun with 1 = __fastfail, 2 = TerminateProcess instead of closing the window
    int test_warview;             // testing only ([test] warview=N): the war view's peace computations every N country loops (aiprof.c)
    int autorun_minimize;         // testing only: 1 = keep the game window minimized during an autorun
    int autorun_offscreen;        // testing only: 1 = keep it drawn off the desktop instead (never activated)
    int autorun_input;            // testing only: with autorun_offscreen, a simulated player with this seed (0 = none)
    // console (console.c)
    int console_hotkey;           // 1 = Ctrl+Alt+C unlocks console commands in single-player ironman until the next game start
    // crash guards (guard.c)
    int guard_tooltip;            // 1 = modifier tooltips skip freed/invalid source containers
    int guard_relation;           // 1 = a missing relation on annexation is skipped instead of crashing
    int guard_great_project;      // 1 = has_construction triggers that name a great project follow the array when it moves
    int guard_ironman_name;       // 1 = an ironman save name in EU4dll's escapes gets the UTF-8 file name ordinary saves get
    int guard_loc_duplicates;     // 1 = the duplicate localisation check runs on a larger stack when the main thread's would overflow
    int guard_diplo_reset;        // 1 = relations freed before a game is reset are skipped instead of called and freed again
    // game memory leaks (leakfix.c)
    int leak_subject_modifiers;   // 1 = the modifier copies a country keeps from related countries are deleted after its next recalculation
    int leak_ai_actions;          // 1 = the foreign minister's diplomatic action candidates that are not chosen are deleted
    int leak_province_items;      // 1 = the province view's modifier lines made only for its change check are not made
    int leak_diplo_actions;       // 1 = the country view's diplomatic action entries of closed categories are deleted when the list is made again
    int leak_message_popups;      // 1 = the popups of message list items that were never opened are deleted
    int test_diplo_freed;         // testing only ([test] diplo_freed=1): the next diplomacy reset finds one relation freed
    int test_diplo_removed;       // testing only ([test] diplo_removed=1): in the next diplomacy reset a relation removes and deletes the next one
    int test_slow_item_ms;        // testing only ([test] slow_item_ms=N): at most once a minute a worker's chunk of a big parallel loop sleeps N ms
    int test_loc_fiber;           // testing only ([test] loc_fiber): 1 = that check always runs on the larger stack, 2 = as usual; both log each table and result
    int test_ironman_name;        // testing only ([test] ironman_name=1): the ironman save of eb_auto is named "eb_시험" in EU4dll's escapes
    char test_hook_fail_set[32];  // testing only ([test] hook_fail_set=name, hook_fail_at=K): operation K of that hook set fails
    int test_hook_fail_at;        // ... (operations 1..n create the set's hooks, n+1..2n enable them)
    int test_queue_fail;          // testing only ([test] queue_fail=N): a worker's queue cannot grow after N entries (country.c)
    int test_msg_fail;            // testing only ([test] msg_fail=N): every Nth message copy of a worker fails (country.c)
    int test_rng_count;           // testing only ([test] rng_count=N): the seed sync command of a load restores N draws instead (rng.c)
    // loading (rng.c)
    int load_fast_rng;            // 1 = a load restores the random generator with its state updates only (no draws), 2 = + verify against the game's draws
    // diagnostics (uitime.c)
    int ui_timing;                // 1 = log how long save lists, the save dialog, deleting and loading take
    int play_monitor;             // playmon.c: 1 = rows of memory, frames, days and handles while playing (diagnostics)
    int play_monitor_s;           // ... a row every this many seconds
    int play_monitor_census_s;    // ... the GUI's windows by type every this many seconds (and a minute after a load)
    int mem_sites;                // memdiag.c: keep the call sites of 1 in N allocations (0 = off; needs the allocator)
    int mem_sites_s;              // ... and write a snapshot every this many seconds
    // crash reports (crash.c writes them, report.c sends them)
    int report_capture;           // 1 = write a local report when the game crashes, hangs or ends abnormally
    int report_upload;            // -1 = ask once, 0 = never send, 1 = send pending reports at launch
    int report_hang_s;            // no frame for this many seconds counts as a hang (0 = no hang reports)
    int report_test_keys;         // 1 = Ctrl+Alt+Shift+F9 crashes, F10 hangs, F11 stalls the main thread (testing only)
    int report_slow;              // 1 = slow save lists, loads and stalls become events of a slow report
    int slow_ui_ms;               // ... a save list (save dialog, load screen, deleting) over this many ms
    int slow_stall_ms;            // ... no frame for this many ms outside a load
    int slow_load_s;              // ... a load (start -> first game frame) over this many seconds
    wchar_t report_url[256];      // report endpoint ("" = the one built in)
    // auto-update (update.c)
    int update_auto;              // 1 = a newer signed release is downloaded and used from the next launch
    wchar_t update_url[256];      // release list ("" = GitHub's latest release of Scytheim/eu4boost; tests only)
} eb_config_t;

extern eb_config_t g_cfg;
extern HMODULE g_self;
extern HMODULE g_exe;
extern DWORD g_main_tid;
extern wchar_t g_dir[MAX_PATH];   // directory of this DLL (with trailing backslash)

void eb_log(const char* fmt, ...);

// A set of hooks that only work together (main.c): all of them are made and enabled, or none stays in the
// game's code. On a failure the set's hooks come out again (MH_RemoveHook restores the bytes, which is
// checked), and the trampolines it handed out are set to NULL.
#define EB_HS_MAX 16
typedef struct {
    const char* name;
    int n, ops, failed;
    void* t[EB_HS_MAX];
    void** orig[EB_HS_MAX];
    uint8_t before[EB_HS_MAX][16];
} eb_hookset;
void eb_hs_init(eb_hookset* s, const char* name);
int  eb_hs_add(eb_hookset* s, void* target, void* detour, void** orig);   // MH_CreateHook; 0 = the set is undone
int  eb_hs_enable(eb_hookset* s);                                          // enables all; 0 = the set is undone
void eb_hs_undo(eb_hookset* s, const char* why);
// What each feature was asked for in the ini and what actually runs (main.c prints one "features:" line
// after the install, so a report's log shows the features of that session).
void eb_feature(const char* name, int requested, int active, const char* why_off);

// alloc.c
int  eb_alloc_install(void);
void eb_alloc_report(void);
void eb_alloc_zero_stats(LONG64* n, LONG64* bytes);
void eb_alloc_thread_done(void);                 // DLL_THREAD_DETACH: hand the ending thread's mimalloc heap back

// threads.c
int  eb_threads_install(void);
void* patch_iat(HMODULE mod, const char* dll, const char* func, void* repl);

// online.c
int  eb_online_install(void);
void eb_online_report(void);

// speed.c
int  eb_speed_install(void);
void eb_speed_report(void);

// parallel.c
int  eb_parallel_install(void);
void eb_parallel_report(void);

// modifier.c
int  eb_modifier_install(void);
void eb_modifier_report(void);

// country.c
int  eb_country_install(void);
void eb_country_report(void);

// savezip.c
int  eb_savezip_install(void);
void eb_savezip_report(void);
// savepar.c
int  eb_savepar_install(void);
void eb_savepar_report(void);
void eb_savepar_mark(void);              // savezip.c, around every save: what the country loop did
const char* eb_savepar_status(void);
const char* eb_save_journal(void);       // savezip.c: the session's last saves (crash.c puts it in reports)
// savelist.c
int  eb_savelist_install(void);
void eb_savelist_report(void);
// script.c
int  eb_script_install(void);
void eb_script_report(void);

// leakfix.c
int  eb_leakfix_install(void);
void eb_leakfix_report(void);
void eb_leakfix_counts(LONG64* copies, LONG64* actions, LONG64* items, LONG64* entries, LONG64* popups);   // deleted so far (playmon.c)

// console.c
int  eb_console_install(void);

// aiprof.c
int  eb_aiprof_install(void);
void eb_warview_test(uint8_t* gs);
// parallel.c: set by aiprof.c when ai_split is on
extern int (*eb_par_help)(void);
extern int (*eb_par_linger)(uint8_t* ctx);
// parallel.c, for crash.c's stall reports: the threads running items of the game's parallel loops
// (since = QPC at the start of the chunk they run, 0 = none) and whether the main thread waits for a loop
#define EB_NBUSY 64
typedef struct { volatile DWORD tid; HANDLE h; volatile LONGLONG since; volatile int item; } eb_busy_t;
extern eb_busy_t eb_busy[EB_NBUSY];
extern volatile LONG eb_nbusy, eb_par_main_waiting;

// playmon.c ([diag] play_monitor): the hooks report frames, days and events while it is on
extern int eb_pm_on;
int  eb_playmon_install(void);
void eb_playmon_report(void);
void eb_pm_frame(ULONGLONG gap_ms);
void eb_pm_day(LONGLONG qpc_ticks);
void eb_pm_event(const char* fmt, ...);
int  eb_sample_main_pcs(uint64_t* pcs, int max);   // crash.c: the main thread's call stack now (0 = no sample)
int  eb_guarded(int (*fn)(void*), void* arg);   // crash.c: fn(arg), or -1 when it faults (main thread code that reads game objects)
LONG64 eb_ui_front_frames(void);           // uitime.c: front end frames so far (the menu)

// guard.c
int  eb_guard_install(void);
void eb_guard_report(void);
// daily tick hook, installed for the tooltip guard, the province text cache or the online throttle
extern volatile ULONGLONG eb_day_ms;   // GetTickCount64() at the start and at the end of the latest daily tick
extern int eb_day_hooked;

extern volatile int eb_in_day;         // crash.c: inside the daily tick (set by guard.c)

// uitime.c
int  eb_uitime_install(void);
int  eb_autorun_install(void);       // autorun.c: test autopilot, driven from uitime.c
int  eb_rng_install(void);           // rng.c: the random generator restore of a load ([load] fast_rng_replay)
void eb_rng_report(void);
int  eb_autorun_date(const char* s);
void eb_autorun_loaded(void);
void eb_autorun_frame(void);
LONGLONG eb_autorun_input_made(UINT msg);  // autorun.c: when the simulated player posted this input (QPC), 0 = not a test

// memdiag.c (called by alloc.c's hooks while eb_md_on)
extern int eb_md_on;
int  eb_md_init(void);
void eb_md_start(void);
void eb_md_alloc(void* p, size_t n);
void eb_md_free(void* p);

// crash.c, report.c
extern wchar_t eb_rep_dir[MAX_PATH];   // <plugins>\crash_reports\ .
int  eb_crash_install(void);
void eb_crash_exit(void);
void eb_report_start(void);
void eb_update_start(void);            // update.c
void eb_rm_dir(const wchar_t* dir);    // a folder with files only
int  eb_pending_report(const wchar_t* kind, const char* text);   // crash.c: pending\<now>_<kind>\report.txt (sent by report.c)
extern char eb_prev_version[40];       // crash.c: the plugin version of the previous session (session.txt), "" if unknown
extern volatile LONG eb_wer_done;    // crash.c: 0 while an abnormal-exit report waits for Windows' crash record
extern volatile int eb_loading;        // uitime.c: a save is being loaded or a new game set up
void eb_slow_event(const char* what, double ms, double limit_ms, const char* detail);
void eb_load_samples_reset(void);      // crash.c samples the main thread every 5 s during a load
int  eb_load_samples_take(char* out, size_t n);
int  eb_docs_dir(wchar_t* out, size_t n);   // Documents\Paradox Interactive\Europa Universalis IV\ .

// profiler.c
int  eb_prof_start_thread(void);
void eb_prof_shutdown(void);
