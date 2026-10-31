// Game memory leaks ([leaks]), found in the play measurement of 2026-10-29 ([diag] mem_sites, a new
// game 1444 -> 1581 in 2.5 h): after the thread-exit fix (alloc.c) the process grew ~10 MB per game
// year; these three were about half of it, and diplo_actions came from the next play (10-29 evening).
// Three delete objects the game made and then dropped, through their own virtual destructor (slot 0,
// "deleting"), the way the game deletes them elsewhere; province_items does not make objects that would
// be dropped.
//
// subject_modifiers: a country's modifier container (country +0x28c8) is destroyed and built again on
// every CalculateModifier (FUN_140327dc0, and FUN_14093d130 at its start: FUN_14093ae10, then
// FUN_140939ef0). FUN_140931380 (called by FUN_14093d130) walks the countries in the list at +0x1a18;
// for each it rebuilds that country's modifier at +0x96f8 (FUN_14093da40), copies it into a new
// 0x178-byte modifier (FUN_1403eaf10), adjusts the copy, adds it to the sum (FUN_1408ea420, which keeps
// a raw source pointer) and appends it to the pointer vector at container +0x6440. The destructor frees
// that vector's array (FUN_14008eac0) but not the copies. A copy also holds shared_ptrs to the parts of
// the other country's modifier (FUN_1408ea610 keeps them through shared_ptr), so those stay too:
// memsites +285 MB in 128 game years. The hook takes the copies before the destructor runs and deletes
// them two seconds later: the sum that pointed at them is rebuilt at once, and the delay leaves a
// tooltip that was walking the old sum time to finish.
//
// ai_actions: the foreign minister's FUN_140133ea0 (ai_foreignminister.cpp, via FUN_14013b810) tries 9
// diplomatic action types. Each one FUN_14035a8f0 allows is created (FUN_14035d210), checked (virtual
// +0x80, +0x88) and scored (+0xc8); the best is kept and the one it beats deleted, but a candidate that
// fails a check or does not beat the best is never deleted (the three jumps to 0x14013403d). memsites:
// +121 MB in 128 years. The hook at 0x14013403d (MinHook moves the instruction there to its
// trampoline) deletes rsi there when it holds a candidate that is not the kept one (r14). When
// FUN_14035a8f0 says no, rsi holds the action type the loop loaded into esi (0x3bcb..0x3bd4).
//
// province_items: the province view's FUN_1413468d0 first calls the modifier list builder
// FUN_1413483b0 with its last argument 0, only to fill vectors it compares with the shown list; the
// builder then has no listbox (NULL) to add items to. Two of its lines, core cost and hostile
// attrition, are still made: operator new(0xc0) and FUN_141342950, a "province_modifier_item" whose
// constructor makes a window (0xa80 bytes and its children, FUN_1414e9030) and adds it to the GUI's
// child list (FUN_1414eb360: +0x110). The returned item is used only by the listbox add the NULL test
// skips (rbx at 0x141348fb9 and 0x1413493bb). memsites: about +93 MB of items and +86 MB of windows
// in 2.5 h of play, and the windows stay in that list, which the GUI walks every frame (FUN_1414e9fa0
// looks for windows marked dead). So during such a call the constructor is not run: the memory goes
// back at once (FUN_1417394f0, the game's free) and the caller gets the pointer it passed, which it
// drops. (An item's destructor FUN_14152e030 would close the window: window virtual +0x130 is
// FUN_141547520, which marks it dead for that walk to delete.)
//
// diplo_actions: the country view's FUN_140f0bdc0 fills its diplomatic action list. When the actions
// change (another country is selected), it clears the listbox (virtual +0xb8, FUN_1415320f0, which
// deletes the listbox's items at once), then empties its groups at view +0xa98 (0x20 bytes each, the
// entries a vector at +8) with FUN_14008eac0, which frees only the arrays, and makes new entries
// (FUN_140ef1db0, "diploactionentry", each with a window). An entry is a listbox item only while its
// category is open (or in the group 0x165 shown without a category), so the entries of closed
// categories are dropped with their windows: memsites +5 to +10 MB of windows an hour in the 10-29
// evening play. The hook takes the groups' entries before the call, sees which ones the call deletes
// (FUN_140ef2150, the entry's deleting destructor, also reached from the listbox through the thunk
// at 0x140f24788) and deletes the rest that are in no group afterwards.
//
// message_popups: a message whose settings send it to the message list (FUN_1408c07b0, messagehandler.cpp)
// gets a popup made at once (FUN_1408c1430: "DefaultPopup" and others, 0x5b0 bytes, its window hidden by
// virtual +0x70) and a "message_item" (FUN_14123f800, 0x1d8) that keeps it at +0x130. A click on the item
// (FUN_14123ffb0) hands the popup to the popup queue (FUN_1406ecae0, gamestate +0x1e00, which deletes it
// when it is closed) and clears +0x130; otherwise the item is deleted when it is dismissed or 90 days old
// (+0x1cc), and its destructor FUN_14123ff50 leaves the popup with its window and ~18 children in the GUI
// root's list. The 10-30 play: ~2,300 such popups after 1.6 h (44,000 of the 79,000 windows). The hook
// notes the popup of a deleted item and deletes it, through its own deleting destructor (FUN_1408c22a0),
// when the next message item is made: the game is running then (never while it shuts down), with the same
// popup queue and no visit to the menu or load since. Only "DefaultPopup" (message kind 0 at the message
// definition +0x58): the combat popups' destructors also reset the shared 3D view.
#include "common.h"
#include <string.h>
#include "MinHook.h"

#define RVA_CONT_DTOR  0x93ae10u   // FUN_14093ae10(container): destructor of a country's modifier container
#define RVA_COPY_NEW   0x93168eu   // in FUN_140931380: operator new(0x178) for the copy
#define RVA_COPY_PUSH  0x931d69u   // in FUN_140931380: the copy appended to container +0x6440
#define RVA_FM_CHECKS  0x133fe4u   // in FUN_140133ea0: the checks, the score compare and the loop end
#define RVA_FM_TAIL    0x13403du   // ... where the three skips and the kept candidate meet
#define RVA_PROV_LIST  0x13483b0u  // FUN_1413483b0(view, v1, v2, v3, char with_listbox)
#define RVA_PROV_ITEM  0x1342950u  // FUN_141342950(mem, gui, a, b, c) -> mem: province_modifier_item constructor
#define RVA_FREE       0x17394f0u  // FUN_1417394f0(p, size): the game's operator delete (jmp _free_base)
#define RVA_DIPLO_LIST 0xf0bdc0u   // FUN_140f0bdc0(view, a): the country view fills its diplomatic action list
#define RVA_DIPLO_DEL  0xef2150u   // FUN_140ef2150(entry, flags): an action entry's deleting destructor
#define RVA_DIPLO_VT   0x1d3b810u  // the entry's vtable
#define RVA_MSG_NEW    0x123f800u  // FUN_14123f800(item, queue, popup, message): message_item constructor
#define RVA_MSG_DEL    0x123ff50u  // FUN_14123ff50(item, flags): its deleting destructor
#define RVA_MSG_VT     0x1d6b528u  // the item's vtable
#define RVA_POPUP_VT   0x1cc1df8u  // the popup's vtable (FUN_1408c1f80), slot 0 = FUN_1408c22a0
#define RVA_POPUP_DEL  0x8c22a0u
#define RVA_GS         0x233fe78u  // the gamestate pointer
#define OFF_COPIES     0x6440      // container: std::vector<modifier*> of the copies
#define OFF_GROUPS     0xa98       // country view: std::vector of 0x20-byte action groups
#define DELAY_MS       2000

static const uint8_t sig_cont_dtor[16] = { 0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0x48,0x81,0xc1,0x78,0x6c,0x00,0x00 };
static const uint8_t sig_copy_new[10] = { 0xb9,0x78,0x01,0x00,0x00,0xe8,0x3c,0x1c,0x10,0x01 };
static const uint8_t sig_copy_push[32] = { 0x48,0x8b,0x5d,0x67,0x48,0x8b,0x0b,0x48,0x81,0xc1,0x40,0x64,0x00,0x00,0x48,0x8b,
                                           0x51,0x08,0x48,0x3b,0x51,0x10,0x74,0x14,0x4c,0x89,0x2a,0x48,0x83,0x41,0x08,0x08 };
static const uint8_t sig_fm[110] = {
    0xff,0x90,0x80,0x00,0x00,0x00,0x84,0xc0,0x74,0x4f,0x48,0x8b,0x06,0x45,0x33,0xc0,0x33,0xd2,0x48,0x8b,0xce,0xff,
    0x90,0x88,0x00,0x00,0x00,0x84,0xc0,0x74,0x3a,0x48,0x8b,0x06,0x48,0x8d,0x94,0x24,0xa8,0x00,0x00,0x00,0x45,0x0f,
    0xb6,0xc4,0x48,0x8b,0xce,0xff,0x90,0xc8,0x00,0x00,0x00,0x8b,0x84,0x24,0xa8,0x00,0x00,0x00,0x3b,0xc7,0x7e,0x17,
    0x8b,0xf8,0x4d,0x85,0xf6,0x74,0x0d,0x49,0x8b,0x06,0xba,0x01,0x00,0x00,0x00,0x49,0x8b,0xce,0xff,0x10,0x4c,0x8b,
    0xf6,0x48,0x8b,0x84,0x24,0xb8,0x00,0x00,0x00,0x48,0xff,0xc5,0x48,0x83,0xfd,0x09,0x0f,0x8c,0x1e,0xff,0xff,0xff };
static const uint8_t sig_prov_list[16] = { 0x48,0x89,0x5c,0x24,0x18,0x4c,0x89,0x4c,0x24,0x20,0x55,0x56,0x57,0x41,0x54,0x41 };
static const uint8_t sig_prov_item[16] = { 0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x48,0x89,0x4c,0x24,0x08,0x56 };
static const uint8_t sig_free[5] = { 0xe9,0x2b,0x45,0x02,0x00 };
static const uint8_t sig_diplo_list[24] = { 0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x4c,0x24,0x08,0x55,0x56,0x57,0x41,0x54,0x41,
                                            0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xac };
static const uint8_t sig_msg_new[24] = { 0x48,0x89,0x5c,0x24,0x10,0x4c,0x89,0x4c,0x24,0x20,0x48,0x89,0x4c,0x24,0x08,0x55,
                                         0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56 };
static const uint8_t sig_msg_del[24] = { 0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xf9,0x8b,0xda,0x48,
                                         0x81,0xc1,0x98,0x01,0x00,0x00,0xe8,0xf5 };
static const uint8_t sig_diplo_del[24] = { 0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8d,0x05,0xaf,0x96,0xe4,
                                           0x00,0x48,0x8b,0xd9,0x48,0x89,0x01,0x8b };

static void del(void* p) { (**(void (***)(void*, int))p)(p, 1); }

// ---- subject_modifiers ----
typedef void (*cont_dtor_t)(uint8_t* c);
static cont_dtor_t o_cont_dtor;
typedef struct { void* p; ULONGLONG t; } dead_t;
static SRWLOCK g_dead_lock = SRWLOCK_INIT;
static dead_t* g_dead;                      // FIFO: [g_dead_head, g_ndead) wait for their time
static size_t g_ndead, g_dead_head, g_dead_cap;
static volatile LONG g_waiting;             // g_ndead - g_dead_head, read without the lock
static volatile LONG64 g_copies_freed, g_copies_max_waiting;

static void hk_cont_dtor(uint8_t* c) {
    void** b = *(void***)(c + OFF_COPIES);
    void** e = *(void***)(c + OFF_COPIES + 8);
    if (b == e && !g_waiting) { o_cont_dtor(c); return; }   // most calls: no copies made, none waiting
    ULONGLONG now = GetTickCount64();
    void* due[64];
    int ndue = 0;
    AcquireSRWLockExclusive(&g_dead_lock);
    if (b && e > b && e - b < 100000) {
        size_t n = (size_t)(e - b);
        if (g_dead_head && g_dead_head == g_ndead) g_dead_head = g_ndead = 0;
        if (g_ndead + n > g_dead_cap) {
            if (g_dead_head) {                  // move the waiting ones to the front first
                memmove(g_dead, g_dead + g_dead_head, (g_ndead - g_dead_head) * sizeof *g_dead);
                g_ndead -= g_dead_head; g_dead_head = 0;
            }
            if (g_ndead + n > g_dead_cap) {
                size_t cap = g_dead_cap ? g_dead_cap * 2 : 1024;
                while (cap < g_ndead + n) cap *= 2;
                dead_t* d = (dead_t*)(g_dead ? HeapReAlloc(GetProcessHeap(), 0, g_dead, cap * sizeof *d)
                                             : HeapAlloc(GetProcessHeap(), 0, cap * sizeof *d));
                if (d) { g_dead = d; g_dead_cap = cap; }      // without memory the rest stay leaked, as before
            }
        }
        for (void** q = b; q < e && g_ndead < g_dead_cap; q++)
            if (*q) { g_dead[g_ndead].p = *q; g_dead[g_ndead].t = now; g_ndead++; }
        if ((LONG64)(g_ndead - g_dead_head) > g_copies_max_waiting) g_copies_max_waiting = (LONG64)(g_ndead - g_dead_head);
    }
    while (g_dead_head < g_ndead && ndue < 64 && now - g_dead[g_dead_head].t >= DELAY_MS) due[ndue++] = g_dead[g_dead_head++].p;
    g_waiting = (LONG)(g_ndead - g_dead_head);
    ReleaseSRWLockExclusive(&g_dead_lock);
    o_cont_dtor(c);
    for (int i = 0; i < ndue; i++) del(due[i]);
    if (ndue) InterlockedAdd64(&g_copies_freed, ndue);
}

// ---- ai_actions ----
void* eb_lf_fm_tramp;                       // MinHook trampoline: "mov rax, [rsp+0xb8]", then on at 0x140134045
volatile LONG64 eb_lf_fm_freed;
void eb_lf_fm_tail(void);
__asm__(".text\n"
        ".globl eb_lf_fm_tail\n"
        "eb_lf_fm_tail:\n"
        "    cmpq %r14, %rsi\n"                 // the kept candidate
        "    je 1f\n"
        "    cmpq $0x10000, %rsi\n"             // an action type: FUN_14035a8f0 said no, nothing was made
        "    jb 1f\n"
        "    movq %rsi, %rcx\n"
        "    movq (%rsi), %rax\n"
        "    movl $1, %edx\n"
        "    callq *(%rax)\n"                   // rsp is 16-aligned here; rsi, rdi, rbp, r12-r15 survive the call
        "    lock incq eb_lf_fm_freed(%rip)\n"
        "1:  jmpq *eb_lf_fm_tramp(%rip)\n");

// ---- province_items ----
typedef void (*prov_list_t)(uint8_t* view, void* v1, void* v2, void* v3, char with_listbox);
typedef void* (*prov_item_t)(void* mem, void* gui, void* a, void* b, void* c);
static prov_list_t o_prov_list;
static prov_item_t o_prov_item;
static void (*game_free)(void* p, size_t n);
static __thread int t_check;                // inside FUN_1413483b0 without a listbox
static volatile LONG64 g_items_skipped;

static void* hk_prov_item(void* mem, void* gui, void* a, void* b, void* c) {
    if (!t_check || !mem) return o_prov_item(mem, gui, a, b, c);
    game_free(mem, 0xc0);
    InterlockedIncrement64(&g_items_skipped);
    return mem;
}

static void hk_prov_list(uint8_t* view, void* v1, void* v2, void* v3, char with_listbox) {
    if (with_listbox || t_check) { o_prov_list(view, v1, v2, v3, with_listbox); return; }
    t_check = 1;
    o_prov_list(view, v1, v2, v3, with_listbox);
    t_check = 0;
}

// ---- diplo_actions (main thread: the GUI) ----
typedef void (*diplo_list_t)(uint8_t* view, uint64_t a);
typedef void* (*diplo_del_t)(void* entry, unsigned flags);
static diplo_list_t o_diplo_list;
static diplo_del_t o_diplo_del;
static void* g_diplo_vt;
static __thread int t_diplo;                // inside FUN_140f0bdc0: deleted entries are noted
static __thread void* t_gone[512];
static __thread int t_ngone, t_gone_over;
static volatile LONG64 g_entries_freed;

static void* hk_diplo_del(void* entry, unsigned flags) {
    if (t_diplo) { if (t_ngone < 512) t_gone[t_ngone++] = entry; else t_gone_over = 1; }
    return o_diplo_del(entry, flags);
}

static int groups_entries(uint8_t* view, void** out, int max) {   // -1 = more than max
    uint8_t* gb = *(uint8_t**)(view + OFF_GROUPS);
    uint8_t* ge = *(uint8_t**)(view + OFF_GROUPS + 8);
    int n = 0;
    if (!gb || ge < gb || (ge - gb) % 0x20 || ge - gb > 0x20 * 256) return gb == ge ? 0 : -1;
    for (uint8_t* g = gb; g < ge; g += 0x20) {
        void** b = *(void***)(g + 8);
        void** e = *(void***)(g + 0x10);
        if (e < b) return -1;
        for (void** q = b; q < e; q++) { if (n >= max) return -1; if (*q) out[n++] = *q; }
    }
    return n;
}
static int has(void* const* v, int n, const void* p) { for (int i = 0; i < n; i++) if (v[i] == p) return 1; return 0; }

static void hk_diplo_list(uint8_t* view, uint64_t a) {
    void* old[512];
    int nold = t_diplo ? -1 : groups_entries(view, old, 512);
    if (nold <= 0) { o_diplo_list(view, a); return; }
    t_diplo = 1; t_ngone = 0; t_gone_over = 0;
    o_diplo_list(view, a);
    t_diplo = 0;
    void* now[512];
    int nnow = groups_entries(view, now, 512);
    if (nnow < 0 || t_gone_over) return;    // something unexpected: leave them, as without the fix
    int freed = 0;
    for (int i = 0; i < nold; i++) {
        void* p = old[i];
        if (has(t_gone, t_ngone, p) || has(now, nnow, p) || has(old, i, p) || *(void**)p != g_diplo_vt) continue;
        del(p);
        freed++;
    }
    if (freed) InterlockedAdd64(&g_entries_freed, freed);
}

// ---- message_popups (main thread: the GUI) ----
typedef void* (*msg_new_t)(void* item, void* queue, void* popup, void* message);
typedef void* (*msg_del_t)(uint8_t* item, unsigned flags);
static msg_new_t o_msg_new;
static msg_del_t o_msg_del;
static void* g_popup_vt;
static uint8_t* g_gs_slot;
#define NPOP 4096
static struct { void* popup; void* queue; } g_pop[NPOP];
static int g_npop;
static LONG64 g_pop_front;                  // the menu's frame count when the waiting popups were noted
static volatile LONG64 g_popups_freed;

static void* popup_queue(void) {            // gamestate +0x1e00: the popup queue the popups were made for
    uint8_t* gs = *(uint8_t* volatile*)g_gs_slot;
    return gs ? *(void* volatile*)(gs + 0x1e00) : NULL;
}
static void* hk_msg_del(uint8_t* item, unsigned flags) {
    uint8_t* p = *(uint8_t**)(item + 0x130);
    if (p && !eb_loading && *(void**)p == g_popup_vt) {
        const uint8_t* def = *(const uint8_t* const*)(p + 0x1c8);    // the message copy at +0x1c0: its definition
        if (def && *(const int*)(def + 0x58) == 0) {
            if (!g_npop) g_pop_front = eb_ui_front_frames();
            if (g_npop < NPOP) { g_pop[g_npop].popup = p; g_pop[g_npop].queue = *(void**)(p + 0x10); g_npop++; }
        }
    }
    return o_msg_del(item, flags);
}
static void* hk_msg_new(void* item, void* queue, void* popup, void* message) {
    if (g_npop) {
        void* now = popup_queue();
        int ok = !eb_loading && eb_ui_front_frames() == g_pop_front && now, freed = 0;
        for (int i = 0; i < g_npop && ok; i++) {
            void* p = g_pop[i].popup;
            if (g_pop[i].queue != now || *(void**)p != g_popup_vt) continue;
            del(p);
            freed++;
        }
        g_npop = 0;
        if (freed) InterlockedAdd64(&g_popups_freed, freed);
    }
    return o_msg_new(item, queue, popup, message);
}

static int hook(uint8_t* t, void* fn, void* orig, const char* what) {
    eb_hookset hs; eb_hs_init(&hs, what);
    if (!eb_hs_add(&hs, t, fn, (void**)orig) || !eb_hs_enable(&hs)) { eb_log("leaks: %s hook failed - off", what); return 0; }
    return 1;
}
// the two hooks of a fix work only together (one records, the other acts on what was recorded): both or none
static int hook2(const char* what, uint8_t* t1, void* f1, void* o1, uint8_t* t2, void* f2, void* o2) {
    eb_hookset hs; eb_hs_init(&hs, what);
    if (!eb_hs_add(&hs, t1, f1, (void**)o1) || !eb_hs_add(&hs, t2, f2, (void**)o2) || !eb_hs_enable(&hs)) {
        eb_log("leaks: %s hooks failed - off", what); return 0;
    }
    return 1;
}

int eb_leakfix_install(void) {
    if (!g_cfg.leak_subject_modifiers && !g_cfg.leak_ai_actions && !g_cfg.leak_province_items && !g_cfg.leak_diplo_actions &&
        !g_cfg.leak_message_popups) return 0;
#ifdef EB_TEST
    return 0;
#else
    uint8_t* b = (uint8_t*)g_exe;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != 0x66FE6AB9u) { eb_log("leaks: unknown eu4.exe - off"); return 0; }
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) { eb_log("leaks: MinHook init failed"); return 0; }
    int n = 0;
    if (g_cfg.leak_subject_modifiers) {
        if (memcmp(b + RVA_CONT_DTOR, sig_cont_dtor, sizeof sig_cont_dtor) || memcmp(b + RVA_COPY_NEW, sig_copy_new, sizeof sig_copy_new) ||
            memcmp(b + RVA_COPY_PUSH, sig_copy_push, sizeof sig_copy_push)) eb_log("leaks: subject_modifiers signature mismatch - off");
        else if (hook(b + RVA_CONT_DTOR, (void*)hk_cont_dtor, &o_cont_dtor, "subject_modifiers")) {
            eb_log("leaks: the modifier copies a country keeps from related countries are deleted %d ms after its next recalculation", DELAY_MS);
            n++;
        }
        if (!o_cont_dtor) g_cfg.leak_subject_modifiers = 0;
    }
    if (g_cfg.leak_ai_actions) {
        if (memcmp(b + RVA_FM_CHECKS, sig_fm, sizeof sig_fm)) eb_log("leaks: ai_actions signature mismatch - off");
        else if (hook(b + RVA_FM_TAIL, (void*)eb_lf_fm_tail, &eb_lf_fm_tramp, "ai_actions")) {
            eb_log("leaks: the foreign minister's diplomatic action candidates that are not chosen are deleted");
            n++;
        }
        if (!eb_lf_fm_tramp) g_cfg.leak_ai_actions = 0;
    }
    if (g_cfg.leak_province_items) {
        if (memcmp(b + RVA_PROV_LIST, sig_prov_list, sizeof sig_prov_list) || memcmp(b + RVA_PROV_ITEM, sig_prov_item, sizeof sig_prov_item) ||
            memcmp(b + RVA_FREE, sig_free, sizeof sig_free))
            eb_log("leaks: province_items signature mismatch - off");
        else if ((game_free = (void (*)(void*, size_t))(b + RVA_FREE)),
                 hook2("province_items", b + RVA_PROV_ITEM, (void*)hk_prov_item, &o_prov_item, b + RVA_PROV_LIST, (void*)hk_prov_list, &o_prov_list)) {
            eb_log("leaks: the province view's change check no longer makes modifier lines it drops");
            n++;
        }
        if (!o_prov_list) g_cfg.leak_province_items = 0;
    }
    if (g_cfg.leak_diplo_actions) {
        if (memcmp(b + RVA_DIPLO_LIST, sig_diplo_list, sizeof sig_diplo_list) || memcmp(b + RVA_DIPLO_DEL, sig_diplo_del, sizeof sig_diplo_del) ||
            *(void**)(b + RVA_DIPLO_VT) != (void*)(b + RVA_DIPLO_DEL))
            eb_log("leaks: diplo_actions signature mismatch - off");
        else if ((g_diplo_vt = b + RVA_DIPLO_VT),
                 hook2("diplo_actions", b + RVA_DIPLO_DEL, (void*)hk_diplo_del, &o_diplo_del, b + RVA_DIPLO_LIST, (void*)hk_diplo_list, &o_diplo_list)) {
            eb_log("leaks: the diplomatic action entries of closed categories are deleted when the country view makes its list again");
            n++;
        }
        if (!o_diplo_list) g_cfg.leak_diplo_actions = 0;
    }
    if (g_cfg.leak_message_popups) {
        if (memcmp(b + RVA_MSG_NEW, sig_msg_new, sizeof sig_msg_new) || memcmp(b + RVA_MSG_DEL, sig_msg_del, sizeof sig_msg_del) ||
            *(void**)(b + RVA_MSG_VT) != (void*)(b + RVA_MSG_DEL) || *(void**)(b + RVA_POPUP_VT) != (void*)(b + RVA_POPUP_DEL))
            eb_log("leaks: message_popups signature mismatch - off");
        else if ((g_popup_vt = b + RVA_POPUP_VT), (g_gs_slot = b + RVA_GS),
                 hook2("message_popups", b + RVA_MSG_DEL, (void*)hk_msg_del, &o_msg_del, b + RVA_MSG_NEW, (void*)hk_msg_new, &o_msg_new)) {
            eb_log("leaks: the popups of message list items that were not opened are deleted");
            n++;
        }
        if (!o_msg_new) g_cfg.leak_message_popups = 0;
    }
    return n;
#endif
}

void eb_leakfix_counts(LONG64* copies, LONG64* actions, LONG64* items, LONG64* entries, LONG64* popups) {
    *copies = g_copies_freed; *actions = eb_lf_fm_freed; *items = g_items_skipped; *entries = g_entries_freed; *popups = g_popups_freed;
}

void eb_leakfix_report(void) {
    if (o_cont_dtor || eb_lf_fm_tramp || o_prov_list || o_diplo_list || o_msg_new)
        eb_log("leaks: deleted %lld modifier copies (at most %lld waiting) and %lld AI action candidates, %lld province view lines not made, "
               "%lld diplomatic action entries and %lld message popups deleted",
               (long long)g_copies_freed, (long long)g_copies_max_waiting, (long long)eb_lf_fm_freed, (long long)g_items_skipped,
               (long long)g_entries_freed, (long long)g_popups_freed);
}
