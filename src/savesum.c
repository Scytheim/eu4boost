// A save's checksum: the same value in about a tenth of the time ([save] fast_checksum, experiment 2026-11-10).
//
// Why: after the gamestate is written, FUN_1414e3f00 appends "checksum=<MD5>". The MD5 (FUN_1414e3390) is
// only over two short strings: the stream's size and a 32-bit sum of the whole stream from the stream's
// virtual +0x98, FUN_14171f3f0(stream, n). That sum walks all 82 MB one byte at a time on the main thread:
//     a = a + byte;  if (a >= 0x10000) a >>= 16;      (so a falls back to 1 when it passes 65535)
//     b = b + a;     if (b >= 0x10000) b >>= 16;
//     result = a << 16 | b
// about 40 ms of every save (profile 2026-11-10, a late-game ironman save; loading a save sums it again).
// What: the same two numbers, exactly, without following b through the whole stream:
//   - a is a running sum of the bytes that starts again at 1 when it would pass 65535. 64 bytes are added
//     at once (SSE2) while the sum stays below 65536 - then no restart can lie inside them - and only the
//     block with the restart is walked byte by byte.
//   - b depends on its own past only until it next passes 65535 (then it is 1, whatever it was). So b is
//     followed over the last 4 KB only (64 KB if needed), starting from EVERY value it could have there: the
//     set of possible values is a range plus a few single values, the range runs empty after a few bytes
//     and the single values merge when they pass 65535 together. If exactly one value is left at the end,
//     that is b; if not (e.g. a stream that ends in zero bytes), the game's own loop is used from the start.
// So the result is the game's or the game's loop itself; nothing is approximated.
// How: FUN_14171f3f0 is hooked (MinHook). Streams under 1 MB go to the original. The replacement does what
// the function does (the total for n = -1 read before the stream's virtual +0x70 is called, the buffer at
// +0x40 / +0x48 first, then the vector at +0x38, the position +0x4c moved on) with the two loops replaced.
// fast_checksum=2 runs the original and compares (the original's value is returned): a difference is logged.
#include "common.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <emmintrin.h>
#include "MinHook.h"

#define RVA_STREAM_SUM 0x171f3f0u     // uint FUN_14171f3f0(stream, int n): the stream class's virtual +0x98
static const uint8_t sig_sum[16] = { 0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48 };
#define SUM_MIN_BYTES (1 << 20)
#define SUM_TAIL 4096
#define SUM_TAIL2 65536
#define SUM_PTS 16

typedef uint32_t (*sum_t)(void** s, int n);
static sum_t o_sum;
static double g_ms;
static LONG64 g_fast, g_fell_back, g_small, g_bytes, g_ticks, g_verified, g_mismatch, g_orig_ticks;

// the game's loop, as it is
static void sum_plain(const uint8_t* p, size_t n, uint32_t* pa, uint32_t* pb) {
    uint32_t a = *pa, b = *pb;
    for (size_t i = 0; i < n; i++) {
        uint32_t t = p[i] + a; a = t < 0x10000 ? t : t >> 16;
        t = a + b; b = t < 0x10000 ? t : t >> 16;
    }
    *pa = a; *pb = b;
}
// a alone over n bytes: 64 at a time while no restart can lie inside them (a < 0x10000 on entry)
static uint32_t a_scan(const uint8_t* p, size_t n, uint32_t a) {
    const __m128i z = _mm_setzero_si128();
    size_t i = 0;
    for (; n - i >= 64; i += 64) {
        __m128i s = _mm_add_epi64(_mm_add_epi64(_mm_sad_epu8(_mm_loadu_si128((const __m128i*)(p + i)), z),
                                                _mm_sad_epu8(_mm_loadu_si128((const __m128i*)(p + i + 16)), z)),
                                  _mm_add_epi64(_mm_sad_epu8(_mm_loadu_si128((const __m128i*)(p + i + 32)), z),
                                                _mm_sad_epu8(_mm_loadu_si128((const __m128i*)(p + i + 48)), z)));
        uint32_t sum = (uint32_t)_mm_cvtsi128_si32(s) + (uint32_t)_mm_cvtsi128_si32(_mm_srli_si128(s, 8));
        if (a + sum < 0x10000) { a += sum; continue; }            // every partial sum inside is below 65536 too
        for (int k = 0; k < 64; k++) { uint32_t t = p[i + k] + a; a = t < 0x10000 ? t : t >> 16; }
    }
    for (; i < n; i++) { uint32_t t = p[i] + a; a = t < 0x10000 ? t : t >> 16; }
    return a;
}
// b at the end of p[0..n) when it can have any value 0..65535 at p[0] and a is *pa there. 1 = exactly one
// value is possible at the end (*pb), 0 = more than one (nothing is known). *pa is moved to the end either way.
static int b_tail(const uint8_t* p, size_t n, uint32_t* pa, uint32_t* pb) {
    uint32_t a = *pa, lo = 0, pt[SUM_PTS]; int iv = 1, np = 0, lost = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t t = p[i] + a; a = t < 0x10000 ? t : t >> 16;
        if (lost) continue;                                       // too many values at once: only a is carried on
        for (int k = 0; k < np; k++) { t = pt[k] + a; pt[k] = t < 0x10000 ? t : t >> 16; }
        if (iv) {
            // the range lo..65535: its values below 65536 - a move up by a, the others become 1 (there are some when a >= 1)
            if (lo + a > 0xffff) iv = 0; else lo += a;
            if (a) { if (np == SUM_PTS) { lost = 1; continue; } pt[np++] = 1; }
        }
        for (int x = 0; x < np; x++)
            for (int y = x + 1; y < np; ) { if (pt[y] == pt[x]) pt[y] = pt[--np]; else y++; }
    }
    *pa = a;
    if (lost || iv || np != 1) return 0;
    *pb = pt[0];
    return 1;
}
// both sums over n bytes, exactly. Returns 1 when the quick way gave them, 0 when the game's loop did.
static int sum_fast(const uint8_t* p, size_t n, uint32_t* pa, uint32_t* pb) {
    if (n < 4 * (size_t)SUM_TAIL2 || *pa >= 0x10000 || *pb >= 0x10000) { sum_plain(p, n, pa, pb); return 0; }
    uint32_t a2 = a_scan(p, n - SUM_TAIL2, *pa);                  // a at the start of the long tail
    uint32_t a1 = a_scan(p + (n - SUM_TAIL2), SUM_TAIL2 - SUM_TAIL, a2), a = a1, b = 0;
    if (b_tail(p + (n - SUM_TAIL), SUM_TAIL, &a, &b)) { *pa = a; *pb = b; return 1; }
    a = a2;
    if (b_tail(p + (n - SUM_TAIL2), SUM_TAIL2, &a, &b)) { *pa = a; *pb = b; return 1; }
    sum_plain(p, n, pa, pb);
    return 0;
}

// the two ranges of FUN_14171f3f0 after its call of the virtual +0x70: the buffer (+0x40, +0x48 bytes), then the
// vector at +0x38 {begin, end}; n = how many bytes at most. move = the position +0x4c is moved on as the game does.
static uint32_t sum_ranges(void** s, int n, int move, int* quick) {
    uint32_t a = 0, b = 0;
    int first = *(int32_t*)((uint8_t*)s + 0x48), q = 1;
    if (first > 0) {
        int k = first < n ? first : n;
        if (k > 0) { q &= sum_fast((const uint8_t*)s[8], (size_t)k, &a, &b) || k < SUM_MIN_BYTES; if (move) *(int32_t*)((uint8_t*)s + 0x4c) += k; }
        n -= k;
    }
    void** v = (void**)s[7];
    if (v) {
        const uint8_t* p = (const uint8_t*)v[0];
        int avail = (int32_t)(uint32_t)(uintptr_t)v[1] - (int32_t)(uint32_t)(uintptr_t)p;
        if (avail < n) n = avail;
        if (n > 0) { q &= sum_fast(p, (size_t)n, &a, &b); if (move) *(int32_t*)((uint8_t*)s + 0x4c) += n; }
    }
    if (quick) *quick = q;
    return a << 16 | b;
}
static uint32_t stream_sum(void** s, int n, int mode, sum_t orig) {
    int total = n;
    if (n == -1) {
        int32_t* v = (int32_t*)s[7];
        total = *(int32_t*)((uint8_t*)s + 0x48) + (v ? v[2] - v[0] : 0);
    }
    if (total < SUM_MIN_BYTES) { g_small++; return orig(s, n); }
    LARGE_INTEGER t0, t1, t2; int quick = 0;
    if (mode == 2) {                                              // the original, then ours over what it summed
        QueryPerformanceCounter(&t0);
        uint32_t r0 = orig(s, n);
        QueryPerformanceCounter(&t1);
        uint32_t r1 = sum_ranges(s, total, 0, &quick);
        QueryPerformanceCounter(&t2);
        g_orig_ticks += t1.QuadPart - t0.QuadPart; g_ticks += t2.QuadPart - t1.QuadPart; g_bytes += total; g_verified++;
        if (quick) g_fast++; else g_fell_back++;
        if (r0 != r1) {
            if (g_mismatch++ < 5) eb_log("save: TEST fast checksum differs: %08x, the game's %08x (%d bytes) - the game's is used", r1, r0, total);
        }
        return r0;
    }
    QueryPerformanceCounter(&t0);
    ((void (*)(void*))(*(void***)s)[0x70 / 8])(s);
    uint32_t r = sum_ranges(s, total, 1, &quick);
    QueryPerformanceCounter(&t1);
    g_ticks += t1.QuadPart - t0.QuadPart; g_bytes += total;
    if (quick) g_fast++; else g_fell_back++;
    return r;
}
static uint32_t hk_sum(void** s, int n) { return stream_sum(s, n, g_cfg.save_fast_checksum, o_sum); }

int eb_savesum_install(void) {
    int mode = g_cfg.save_fast_checksum;
    if (mode < 0 || mode > 2) mode = g_cfg.save_fast_checksum = 0;
    if (!mode) return 0;
#ifdef EB_TEST
    return 0;
#else
    uint8_t* b = (uint8_t*)g_exe;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_ms = 1000.0 / (double)f.QuadPart;
    if (nt->FileHeader.TimeDateStamp != 0x66FE6AB9u || memcmp(b + RVA_STREAM_SUM, sig_sum, sizeof sig_sum)) {
        eb_log("save: checksum signature mismatch - fast_checksum off");
        g_cfg.save_fast_checksum = 0;
        return 0;
    }
    MH_STATUS st = MH_Initialize();
    eb_hookset hs; eb_hs_init(&hs, "save checksum");
    if ((st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) || !eb_hs_add(&hs, b + RVA_STREAM_SUM, (void*)hk_sum, (void**)&o_sum) || !eb_hs_enable(&hs)) {
        eb_log("save: hook failed - the checksum is summed as the game does it");
        o_sum = NULL; g_cfg.save_fast_checksum = 0;
        return 0;
    }
    eb_log("save: the checksum's sum over the save is computed the quick way (EXPERIMENT%s)", mode == 2 ? ", TEST: the game's own sum is computed too and compared" : "");
    return 1;
#endif
}
void eb_savesum_report(void) {
    if (!o_sum) return;
    LONG64 n = g_fast + g_fell_back;
    if (g_verified)
        eb_log("save: fast checksum: %lld streams (%lld MB) verified, %lld differences | ours %.1f ms each, the game's %.1f ms each | %lld by the game's loop, %lld small",
               (long long)g_verified, (long long)(g_bytes >> 20), (long long)g_mismatch, (double)g_ticks * g_ms / (double)g_verified,
               (double)g_orig_ticks * g_ms / (double)g_verified, (long long)g_fell_back, (long long)g_small);
    else if (n)
        eb_log("save: fast checksum: %lld streams (%lld MB) in %.1f ms each | %lld by the game's loop, %lld small streams left to the game",
               (long long)n, (long long)(g_bytes >> 20), (double)g_ticks * g_ms / (double)n, (long long)g_fell_back, (long long)g_small);
}

// ---- self-test (harness) ----
static uint32_t t_rng = 12345;
static uint32_t t_rand(void) { t_rng ^= t_rng << 13; t_rng ^= t_rng >> 17; t_rng ^= t_rng << 5; return t_rng; }
static int t_flushes;
static void t_vf70(void* s) { (void)s; t_flushes++; }
// FUN_14171f3f0 as decompiled, with the game's loops
static uint32_t t_ref(void** s, int n) {
    uint32_t a = 0, b = 0;
    if (n == -1) { int32_t* v = (int32_t*)s[7]; n = *(int32_t*)((uint8_t*)s + 0x48) + (v ? v[2] - v[0] : 0); }
    ((void (*)(void*))(*(void***)s)[0x70 / 8])(s);
    int first = *(int32_t*)((uint8_t*)s + 0x48);
    if (first > 0) {
        int k = first < n ? first : n, done = 0;
        if (k > 0) { sum_plain((const uint8_t*)s[8], (size_t)k, &a, &b); done = k; }
        *(int32_t*)((uint8_t*)s + 0x4c) += done;
        n -= k;
    }
    void** v = (void**)s[7];
    if (v) {
        const uint8_t* p = (const uint8_t*)v[0];
        int avail = (int32_t)(uint32_t)(uintptr_t)v[1] - (int32_t)(uint32_t)(uintptr_t)p, done = 0;
        if (avail < n) n = avail;
        if (n > 0) { sum_plain(p, (size_t)n, &a, &b); done = n; }
        *(int32_t*)((uint8_t*)s + 0x4c) += done;
    }
    return a << 16 | b;
}
__declspec(dllexport) int eb_test_savesum(void) {
    int bad = 0, quick = 0, slow = 0;
    size_t cap = 6u << 20;
    uint8_t* d = (uint8_t*)malloc(cap);
    if (!d) return 1;
    // kinds of data: 0 save-like text, 1 any bytes, 2 small values, 3 all 0xff, 4 zeros, 5 text that ends in zeros, 6 ones
    for (int kind = 0; kind < 7; kind++) {
        for (size_t i = 0; i < cap; i++) {
            uint32_t r = t_rand();
            d[i] = kind == 0 ? (uint8_t)(r % 5 == 0 ? '\n' + (r >> 8) % 2 * 22 : '0' + (r >> 8) % 75) : kind == 1 ? (uint8_t)r : kind == 2 ? (uint8_t)(r % 3) :
                   kind == 3 ? 0xff : kind == 4 ? 0 : kind == 5 ? (uint8_t)(i + 3000 < cap ? ' ' + r % 90 : 0) : 1;
        }
        static const size_t lens[] = { 0, 1, 63, 64, 65, 4095, 4096, 4097, 65535, 65536, 262143, 262144, 262145, 300001, 1 << 20, (1 << 20) + 77, (6u << 20) - 5, 6u << 20 };
        for (size_t li = 0; li < sizeof lens / sizeof lens[0]; li++) {
            for (int st = 0; st < 3; st++) {
                uint32_t a0 = st == 0 ? 0 : st == 1 ? 65535 : t_rand() & 0xffff, b0 = st == 0 ? 0 : st == 1 ? 1 : t_rand() & 0xffff;
                size_t off = st == 2 ? t_rand() % 61 : 0, n = lens[li] > off ? lens[li] - off : 0;
                uint32_t a = a0, b = b0, ra = a0, rb = b0;
                int q = sum_fast(d + off, n, &a, &b);
                sum_plain(d + off, n, &ra, &rb);
                if (a != ra || b != rb) { if (bad < 5) printf("savesum: kind %d, %zu bytes, start %u/%u: %x/%x, the game's loop %x/%x\n", kind, n, a0, b0, a, b, ra, rb); bad++; }
                if (q) quick++; else if (n >= 4 * (size_t)SUM_TAIL2) slow++;
                if (a_scan(d + off, n, a0) != ra) { if (bad < 5) printf("savesum: a_scan kind %d, %zu bytes\n", kind, n); bad++; }
            }
        }
    }
    // text, any bytes and 0xff must go the quick way; zeros and a zero tail cannot (no restart of b at the end)
    if (quick < 60 || slow < 5) { printf("savesum: %d quick, %d by the game's loop\n", quick, slow); bad++; }
    // the whole function on a made-up stream: a buffer part and a vector part, every n the game can pass
    for (size_t i = 0; i < cap; i++) d[i] = (uint8_t)(' ' + t_rand() % 90);
    static void* vt[32]; vt[0x70 / 8] = (void*)t_vf70;
    for (int c = 0; c < 6; c++) {
        struct { void* begin; void* end; } vec = { d + 100000, d + 100000 + (c == 3 ? 0 : (3u << 20) + 12345) };
        void* s1[16], *s2[16];
        memset(s1, 0, sizeof s1);
        s1[0] = vt; s1[7] = c == 4 ? NULL : (void*)&vec; s1[8] = d;
        *(int32_t*)((uint8_t*)s1 + 0x48) = c == 0 ? 0 : c == 5 ? 2 << 20 : 5000;
        *(int32_t*)((uint8_t*)s1 + 0x4c) = 7;
        memcpy(s2, s1, sizeof s1);
        static const int ns[] = { -1, 0, 3000, 5000, 5001, 1 << 20, (3 << 20) + 17345, 5 << 20, 0x7fffffff };
        for (size_t k = 0; k < sizeof ns / sizeof ns[0]; k++) {
            int before = t_flushes;
            uint32_t r0 = t_ref(s1, ns[k]);
            int ref_flushes = t_flushes - before; before = t_flushes;
            uint32_t r1 = stream_sum(s2, ns[k], 1, t_ref), r2 = stream_sum(s2, ns[k], 2, t_ref);
            // mode 1 once (as the function), mode 2 once more through the reference: the position moves twice for s2
            if (r0 != r1 || r0 != r2 || memcmp(s1, s2, 0x4c) || *(int32_t*)((uint8_t*)s2 + 0x4c) - 7 != 2 * (*(int32_t*)((uint8_t*)s1 + 0x4c) - 7) ||
                t_flushes - before != 2 * ref_flushes) {
                if (bad < 8) printf("savesum: stream case %d, n %d: %x %x, the function %x, positions %d %d\n", c, ns[k], r1, r2, r0,
                                    *(int32_t*)((uint8_t*)s2 + 0x4c), *(int32_t*)((uint8_t*)s1 + 0x4c));
                bad++;
            }
            *(int32_t*)((uint8_t*)s1 + 0x4c) = 7; *(int32_t*)((uint8_t*)s2 + 0x4c) = 7;
        }
    }
    if (g_mismatch) bad++;
    // speed on 6 MB of text (information only)
    LARGE_INTEGER f, t0, t1, t2; QueryPerformanceFrequency(&f);
    uint32_t a = 0, b = 0, ra = 0, rb = 0;
    QueryPerformanceCounter(&t0); sum_fast(d, cap, &a, &b); QueryPerformanceCounter(&t1); sum_plain(d, cap, &ra, &rb); QueryPerformanceCounter(&t2);
    if (a != ra || b != rb) bad++;
    printf("save checksum checks: %d wrong answers (%d quick, %d by the game's loop; 6 MB: %.2f ms, the game's loop %.2f ms)\n", bad, quick, slow,
           (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart, (double)(t2.QuadPart - t1.QuadPart) * 1000.0 / (double)f.QuadPart);
    g_fast = g_fell_back = g_small = g_bytes = g_ticks = g_verified = g_mismatch = g_orig_ticks = 0;
    free(d);
    return bad;
}
