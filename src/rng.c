// The random generator restore of a load ([load] fast_rng_replay, EXPERIMENT, default 0; 2026-10-31).
//
// Loading a save runs the seed sync command FUN_140847960(cmd): DAT_1423494b0 = draws (cmd +0x54),
// DAT_1423494b4 = seed (cmd +0x50), FUN_1414c6f50(&DAT_142325220, seed) seeds the generator's state (624
// words + the index at +0x9c0, set to 0), then FUN_1414c71b0(&DAT_142325220) is called `draws` times and each
// value is dropped. In a long game that is hundreds of millions of calls: the load stops for 4-6 s (the play
// measurements of 10-29/30, report 95; report 121 on 0.4.10: 3.8 s in eu4.exe+0x14c7253).
//
// FUN_1414c71b0 regenerates all 624 words when the index is 0, then returns a tempered word and sets the index
// to (index + 1) % 624. It is not the reference MT19937 (checked on the instructions, 0x1414c71e0..0x1414c727d):
// each word i becomes  y = (s[i] & 0x80000000) | (s[i+1] & 0x7fffffff),  v = sar(y, 1) ^ s[i+397]  (an
// arithmetic shift: the top bit of y stays), and v ^= 0x9908b0df only when y & 0x80000001 == 1 (the game tests
// the signed remainder y % 2 == 1, so an odd y with the top bit set gets no xor); in place, i = 0..623 in
// order, indexes mod 624. Its seeding (FUN_1414c6f50) is not the reference one either (sar 30), so it is not
// copied here: the game's own function seeds the state.
// From the seeded state (index 0), n draws regenerate ceil(n / 624) times and leave the index at n % 624; the
// tempered words are dropped and change nothing. So mode 1 does what FUN_140847960 does with the draws replaced
// by the regenerations alone (the same words, the same order, about 624 times fewer calls but every word of
// every regeneration is still computed). Mode 2 also runs the game's draws on a copy of the seeded state, compares
// all 625 words and the next 1,300 draws (on copies; the game's state is not moved on), keeps the game's result
// and turns the fast way off for the session if anything differs. Before the first fast restore a self-test
// compares both ways through the game's own functions on scratch states (counts around the regeneration edges,
// seeds 0, 1, 1234, top bit set, 2^32-1, one from the clock); a difference turns the fast way off.
// EU4dll patches nothing within 32 bytes of the three functions (pattern_eu4_jps_2.log, 10-31).
#include "common.h"
#include <stdio.h>
#include <string.h>
#include "MinHook.h"

#define RVA_REPLAY 0x847960u      // FUN_140847960(cmd)
#define RVA_SEED   0x14c6f50u     // FUN_1414c6f50(uint32_t state[625], int seed)
#define RVA_DRAW   0x14c71b0u     // FUN_1414c71b0(uint32_t state[625]) -> tempered word
#define RVA_STATE  0x2325220u     // DAT_142325220
#define RVA_GCOUNT 0x23494b0u     // DAT_1423494b0: draws since the seed
#define RVA_GSEED  0x23494b4u     // DAT_1423494b4: the seed
#define NW 624
static const uint8_t sig_replay[24] = { 0x40,0x53,0x48,0x83,0xec,0x20,0x8b,0x51,0x50,0x8b,0x59,0x54,0x48,0x8d,0x0d,0xad,0xd8,0xad,0x01,0x89,0x1d,0x37,0x1b,0xb0 };
static const uint8_t sig_draw[24]   = { 0x48,0x89,0x5c,0x24,0x08,0x8b,0x91,0xc0,0x09,0x00,0x00,0x48,0x8b,0xd9,0x85,0xd2,0x0f,0x85,0xad,0x00,0x00,0x00,0x41,0xba };
static const uint8_t sig_seed[24]   = { 0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0x85,0xd2,0x75,0x69,0x83,0x3d,0xfc,0xd8,0xe5,0x00,0xff,0x75,0x3b,0x89,0x15 };

typedef void (*replay_t)(uint8_t* cmd);
typedef void (*seed_t)(uint32_t* st, int seed);
typedef uint32_t (*draw_t)(uint32_t* st);
static replay_t o_replay;
static seed_t g_seed;
static draw_t g_draw;
static uint32_t* g_state;
static volatile uint32_t *g_gcount, *g_gseed;
static int g_mode, g_tested;
static double g_ms;

// One regeneration as FUN_1414c71b0 does it (see above), without the index arithmetic of the game's loop.
static inline uint32_t step(uint32_t a, uint32_t b, uint32_t c) {   // a = s[i], b = s[i+1], c = s[i+397]
    uint32_t y = (a & 0x80000000u) | (b & 0x7fffffffu);
    uint32_t v = ((y >> 1) | (y & 0x80000000u)) ^ c;                  // sar(y, 1) on the bits
    return (y & 0x80000001u) == 1u ? v ^ 0x9908b0dfu : v;
}
static void regen(uint32_t* s) {
    int i = 0;
    for (; i < NW - 397; i++) s[i] = step(s[i], s[i + 1], s[i + 397]);       // s[i+397] not updated yet
    for (; i < NW - 1; i++) s[i] = step(s[i], s[i + 1], s[i - (NW - 397)]);   // s[i-227]: updated in this pass
    s[NW - 1] = step(s[NW - 1], s[0], s[396]);                                // s[0], s[396]: updated
}
// What `n` draws from a seeded state (index 0) leave: ceil(n / 624) regenerations, the index n % 624.
static void fast_restore(uint32_t* s, uint32_t n) {
    for (uint64_t k = ((uint64_t)n + NW - 1) / NW; k; k--) regen(s);
    s[NW] = n % NW;
}

static LONGLONG qpc(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

// Both ways on copies of `seeded`: 0 = equal (all 625 words, then the next `next` draws), else 1 + the first word
// that differs (625 = a later draw). `game_out` gets the game's result (for the caller to use).
static uint32_t g_ta[NW + 1], g_tb[NW + 1];
static int compare(const uint32_t* seeded, uint32_t n, int next, uint32_t* game_out, double* ms_game, double* ms_fast) {
    memcpy(g_ta, seeded, sizeof g_ta); memcpy(g_tb, seeded, sizeof g_tb);
    LONGLONG t0 = qpc();
    for (uint64_t k = n; k; k--) g_draw(g_ta);
    LONGLONG t1 = qpc();
    fast_restore(g_tb, n);
    LONGLONG t2 = qpc();
    if (ms_game) *ms_game = (double)(t1 - t0) * g_ms;
    if (ms_fast) *ms_fast = (double)(t2 - t1) * g_ms;
    if (game_out) memcpy(game_out, g_ta, sizeof g_ta);
    for (int i = 0; i <= NW; i++) if (g_ta[i] != g_tb[i]) return 1 + i;
    for (int k = 0; k < next; k++) if (g_draw(g_ta) != g_draw(g_tb)) return 1 + NW;
    return 0;
}

static int self_test(void) {
    static const uint32_t counts[] = { 0, 1, 2, 623, 624, 625, 1247, 1248, 1249, 1871, 1872, 1873, 6239, 6240, 6241, 62401, 100003 };
    uint32_t seeds[] = { 0, 1, 1234, 0x80000000u, 0x80000001u, 0xffffffffu, 0x7fffffffu, (uint32_t)qpc() | 1u };
    static uint32_t seeded[NW + 1];
    int cases = 0;
    LONGLONG t0 = qpc();
    for (size_t a = 0; a < sizeof seeds / sizeof seeds[0]; a++)
        for (size_t c = 0; c < sizeof counts / sizeof counts[0]; c++) {
            memset(seeded, 0xcc, sizeof seeded);
            g_seed(seeded, (int)seeds[a]);           // the game's seeding (seed 0: its table, built once as the game would)
            int d = compare(seeded, counts[c], 2 * NW + 52, NULL, NULL, NULL);
            cases++;
            if (d) {
                eb_log("rng: SELF-TEST DIFFERS for seed %u, %u draws, at %s %d - fast restore off", seeds[a], counts[c],
                       d <= NW + 1 ? "word" : "a later draw", d - 1);
                return 0;
            }
        }
    eb_log("rng: self-test against the game's functions: %d cases (17 counts x 8 seeds, 625 words and the next 1,300 draws each) equal in %.1f ms",
           cases, (double)(qpc() - t0) * g_ms);
    return 1;
}

static LONG64 g_restores;
static double g_ms_total;
static void hk_replay(uint8_t* cmd) {
    uint32_t seed = *(uint32_t*)(cmd + 0x50), n = *(uint32_t*)(cmd + 0x54);
    // testing ([test] rng_count=N): a load in a fresh process restores only the draws made while loading; the
    // seconds-long restores come from a load after playing in the same process (report 121: 13 minutes of play,
    // then a load). The count of such a load is put into the command here, in both modes, to time and verify it.
    if (g_cfg.test_rng_count > 0 && n) {
        eb_log("rng: TEST %u draws instead of %u", (uint32_t)g_cfg.test_rng_count, n);
        n = (uint32_t)g_cfg.test_rng_count; *(uint32_t*)(cmd + 0x54) = n;
    }
    if (g_mode && !g_tested) { g_tested = 1; if (!self_test()) g_mode = 0; }
    if (!g_mode || !n) {
        LONGLONG t0 = qpc();
        o_replay(cmd);
        double ms = (double)(qpc() - t0) * g_ms;
        if (n) eb_log("rng: random generator restored the game's way: %u draws (seed %u) in %.1f ms", n, seed, ms);
        return;
    }
    LONGLONG t0 = qpc();
    *g_gcount = n;                               // as FUN_140847960: the globals, the seeding, then the draws
    *g_gseed = seed;
    g_seed(g_state, (int)seed);
    LONGLONG t1 = qpc();
    double seed_ms = (double)(t1 - t0) * g_ms;
    if (g_mode == 2) {
        static uint32_t seeded[NW + 1];
        double ms_game, ms_fast;
        memcpy(seeded, g_state, sizeof seeded);
        int d = compare(seeded, n, 2 * NW + 52, g_state, &ms_game, &ms_fast);   // g_state = the game's result either way
        eb_log("rng: VERIFY %u draws (seed %u): seeding %.2f ms, the game's draws %.1f ms, the fast restore %.2f ms (%u state updates) - %s",
               n, seed, seed_ms, ms_game, ms_fast, (unsigned)(((uint64_t)n + NW - 1) / NW),
               d ? "DIFFERENT" : "all 625 words and the next 1,300 draws equal");
        if (d) {
            eb_log("rng: the first difference is at %s %d - the game's result is used, fast restore off", d <= NW + 1 ? "word" : "a later draw", d - 1);
            g_mode = 0;
        }
    } else {
        fast_restore(g_state, n);
        double ms = (double)(qpc() - t1) * g_ms;
        g_ms_total += seed_ms + ms;
        eb_log("rng: random generator restored for %u draws (seed %u): seeding %.2f ms, %u state updates %.1f ms",
               n, seed, seed_ms, (unsigned)(((uint64_t)n + NW - 1) / NW), ms);
    }
    g_restores++;
}

int eb_rng_install(void) {
    if (!g_cfg.load_fast_rng && g_cfg.test_rng_count <= 0) return 0;
#ifdef EB_TEST
    return 0;
#else
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_ms = 1000.0 / (double)f.QuadPart;
    uint8_t* b = (uint8_t*)g_exe;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != 0x66FE6AB9u || memcmp(b + RVA_REPLAY, sig_replay, sizeof sig_replay) ||
        memcmp(b + RVA_DRAW, sig_draw, sizeof sig_draw) || memcmp(b + RVA_SEED, sig_seed, sizeof sig_seed)) {
        eb_log("rng: signature mismatch - the random generator is restored the game's way");
        g_cfg.load_fast_rng = 0; return 0;
    }
    g_seed = (seed_t)(b + RVA_SEED);
    g_draw = (draw_t)(b + RVA_DRAW);
    g_state = (uint32_t*)(b + RVA_STATE);
    g_gcount = (volatile uint32_t*)(b + RVA_GCOUNT);
    g_gseed = (volatile uint32_t*)(b + RVA_GSEED);
    MH_STATUS st = MH_Initialize();
    eb_hookset hs; eb_hs_init(&hs, "rng");
    if ((st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) || !eb_hs_add(&hs, b + RVA_REPLAY, (void*)hk_replay, (void**)&o_replay) ||
        !eb_hs_enable(&hs)) {
        eb_log("rng: hook failed - the random generator is restored the game's way");
        g_cfg.load_fast_rng = 0; return 0;
    }
    g_mode = g_cfg.load_fast_rng;
    if (g_mode) eb_log("rng: EXPERIMENT a load restores the random generator with its state updates only%s",
                       g_mode == 2 ? " (VERIFY: the game's draws run too on a copy and are compared)" : "");
    else eb_log("rng: TEST the game's restore is timed (rng_count %d)", g_cfg.test_rng_count);
    return 1;
#endif
}

void eb_rng_report(void) {
    if (g_restores) eb_log("rng: %lld restores, %.1f ms in total (mode %d at the end)", (long long)g_restores, g_ms_total, g_mode);
}

#ifdef EB_TEST
// ---- the test build: the same restore against two references with no game code ----
// ref_seed / ref_draw: FUN_1414c6f50 / FUN_1414c71b0 written again from their instructions, word for word
// (the game's index arithmetic, signed shifts on int32 values written as bit operations).
static uint32_t sar32(uint32_t x, int k) { return (x >> k) | (x & 0x80000000u ? ~(0xffffffffu >> k) : 0u); }
static void ref_seed(uint32_t* s, uint32_t seed) {
    s[0] = seed;
    for (uint32_t i = 1; i < NW; i++) s[i] = (sar32(s[i - 1], 30) ^ s[i - 1]) * 0x6c078965u + i;
    s[NW] = 0;
}
static uint32_t ref_draw(uint32_t* s) {
    uint32_t idx = s[NW];
    if (idx == 0) {
        for (uint32_t i = 0; i < NW; i++) {
            uint32_t y = ((s[i] ^ s[(i + 1) % NW]) & 0x7fffffffu) ^ s[i];
            uint32_t v = sar32(y, 1) ^ s[(i + 397) % NW];
            s[i] = v;
            uint32_t r = y & 0x80000001u;
            if (r & 0x80000000u) r = ((r - 1) | 0xfffffffeu) + 1;   // the signed remainder of an odd negative y: -1
            if (r == 1) s[i] = v ^ 0x9908b0dfu;
        }
    }
    uint32_t x = s[idx];
    x ^= sar32(x, 11);
    x ^= (x & 0xff3a58adu) << 7;
    x ^= (x & 0xffffdf8cu) << 15;
    s[NW] = (idx + 1) % NW;
    return sar32(x, 18) ^ x;
}
static uint32_t crc32b(uint32_t crc, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p; crc = ~crc;
    while (n--) { crc ^= *b++; for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u))); }
    return ~crc;
}
// The third reference: CRC-32 of the 625 words (little endian) and of the next 1,300 draws after n draws from the
// seed, computed by a separate Python implementation of the same instructions (tools/analysis/rng_oracle.py).
static const struct { uint32_t seed, n, crc_state, crc_next; } g_oracle[] = {
#include "rng_oracle.inc"
};
__declspec(dllexport) int eb_test_rng(void) {
    static uint32_t a[NW + 1], b[NW + 1];
    int bad = 0, cases = 0;
    for (size_t k = 0; k < sizeof g_oracle / sizeof g_oracle[0]; k++) {
        ref_seed(a, g_oracle[k].seed); ref_seed(b, g_oracle[k].seed);
        for (uint32_t i = 0; i < g_oracle[k].n; i++) ref_draw(a);
        fast_restore(b, g_oracle[k].n);
        uint32_t ca = crc32b(0, a, sizeof a), cb = crc32b(0, b, sizeof b);
        uint32_t na[1300], nb[1300];
        for (int i = 0; i < 1300; i++) { na[i] = ref_draw(a); nb[i] = ref_draw(b); }
        uint32_t cna = crc32b(0, na, sizeof na), cnb = crc32b(0, nb, sizeof nb);
        cases++;
        if (ca != g_oracle[k].crc_state || cb != g_oracle[k].crc_state || cna != g_oracle[k].crc_next || cnb != g_oracle[k].crc_next) {
            printf("rng: seed %u n %u: state ref %08x fast %08x python %08x | next ref %08x fast %08x python %08x\n", g_oracle[k].seed,
                   g_oracle[k].n, ca, cb, g_oracle[k].crc_state, cna, cnb, g_oracle[k].crc_next);
            bad++;
        }
    }
    // a big count only against the C reference (the Python one would take minutes), and the time of both
    uint32_t n = 50000017u;
    ref_seed(a, 0x9e3779b9u); ref_seed(b, 0x9e3779b9u);
    LONGLONG f; QueryPerformanceFrequency((LARGE_INTEGER*)&f);
    LONGLONG t0 = qpc();
    for (uint32_t i = 0; i < n; i++) ref_draw(a);
    LONGLONG t1 = qpc();
    fast_restore(b, n);
    LONGLONG t2 = qpc();
    cases++;
    if (memcmp(a, b, sizeof a)) { printf("rng: %u draws: the fast restore differs from the reference\n", n); bad++; }
    // the count's edges: 2^32 - 1 and its neighbours, on the arithmetic only (no draws: ceil and the index)
    uint32_t edge[] = { 0xffffffffu, 0xfffffffeu, 624u * 6882960u, 624u * 6882960u + 623u };
    for (int i = 0; i < 4; i++) {
        uint64_t regens = ((uint64_t)edge[i] + NW - 1) / NW, want = edge[i] / NW + (edge[i] % NW ? 1 : 0);
        cases++;
        if (regens != want) { printf("rng: count %u: %llu regenerations, want %llu\n", edge[i], (unsigned long long)regens, (unsigned long long)want); bad++; }
    }
    printf("rng: %d cases, %d differ; %u draws: reference %.0f ms, fast restore %.1f ms\n", cases, bad, n,
           (double)(t1 - t0) * 1000.0 / (double)f, (double)(t2 - t1) * 1000.0 / (double)f);
    return bad == 0;
}
#endif
