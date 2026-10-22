// Faster compressed saves: the zip entries of a save are deflated on all cores.
//
// Every compressed save (manual saves, autosaves and the ironman saves the game writes every
// quarter) is built by eu4savegamehelper.cpp FUN_1405ce5f0 as a zip with the entries meta /
// gamestate / ai. Each entry goes through FUN_14153c790(zip, in_stream, buf, buf_size, &crc,
// level 3): one raw zlib deflate stream (windowBits -15, memLevel 9) fed half a buffer at a time
// plus a byte-at-a-time CRC-32, all on the main thread while the game stands still. With the
// user's modded 1543 ironman save the gamestate is 68 MB and this is ~0.4 s of the ~0.9 s each
// save takes (profile 2026-10-15: 6.5% of all main-thread time at speed 5).
//
// The replacement reads the entry once, cuts it into blocks (default 2 MB) and compresses them on
// all cores with the game's own zlib (deflateInit2_/deflate/deflateEnd in eu4.exe, same
// parameters). Every block but the last ends with Z_SYNC_FLUSH (byte-aligned, not final) and the
// last with Z_FINISH, so the concatenation is one valid raw deflate stream (the pigz method). The
// blocks share no dictionary, which costs ~0.6% in size at 2 MB. The CRC-32 is computed per block
// (slice-by-8) and combined. The uncompressed data - and so the game loaded from the save - is
// unchanged; only the compressed bytes differ.
// Entries smaller than two blocks and any failure (out of memory, zlib error) go to the original
// function. Nothing is written before every block is done, so a fallback never leaves half an
// entry behind.
#include "common.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "MinHook.h"

#define RVA_ZIP_DEFLATE 0x153c790u   // (zip, in_stream, buf, buf_size, uint* crc, int level) -> compressed size, 0 = error
#define RVA_Z_INIT2     0x181fc70u   // deflateInit2_
#define RVA_Z_DEFLATE   0x181f210u   // deflate
#define RVA_Z_END       0x181fb90u   // deflateEnd
#define RVA_SAVE        0x5ce5f0u    // eu4savegamehelper: write one save (stats only)
#define RVA_CRC_TABLE   0x1d91cd0u   // the CRC table FUN_14153c790 uses (standard CRC-32)
static const uint8_t sig_zip_deflate[16] = { 0x48,0x8b,0xc4,0x48,0x89,0x58,0x10,0x48,0x89,0x68,0x18,0x48,0x89,0x70,0x20,0x48 };
static const uint8_t sig_z_init2[16]     = { 0x40,0x53,0x56,0x41,0x54,0x41,0x55,0x41,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0x84 };
static const uint8_t sig_z_deflate[16]   = { 0x40,0x53,0x57,0x41,0x56,0x48,0x83,0xec,0x20,0x44,0x8b,0xf2,0x48,0x8b,0xf9,0x48 };
static const uint8_t sig_z_end[16]       = { 0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0x48,0x85,0xc9 };
static const uint8_t sig_save[16]        = { 0x48,0x89,0x4c,0x24,0x08,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57 };
// call sites inside FUN_14153c790 that must reach the three zlib functions above
#define OFF_CALL_INIT2   0x6e
#define OFF_CALL_DEFLATE 0xf4
#define OFF_CALL_END     0x1e1

// zlib z_stream on Windows x64 (uLong is 32 bit): 0x58 bytes, as passed to deflateInit2_
typedef struct {
    const uint8_t* next_in; uint32_t avail_in; uint32_t total_in;
    uint8_t* next_out; uint32_t avail_out; uint32_t total_out;
    const char* msg; void* state; void* zalloc; void* zfree; void* opaque;
    int data_type; uint32_t adler; uint32_t reserved;
} zstream_t;
_Static_assert(sizeof(zstream_t) == 0x58, "z_stream layout");

typedef int (*zip_deflate_t)(void** zip, void* in, uint8_t* buf, uint64_t buf_size, uint32_t* crc, int level);
typedef int (*z_init2_t)(zstream_t* s, int level, int method, int window_bits, int mem_level, int strategy, const char* version, int stream_size);
typedef int (*z_deflate_t)(zstream_t* s, int flush);
typedef int (*z_end_t)(zstream_t* s);
typedef void (*save_t)(void* helper);
static zip_deflate_t o_zip_deflate;
static save_t o_save;
static z_init2_t z_init2;
static z_deflate_t z_deflate;
static z_end_t z_end;

// the game's stream objects (virtual calls, offsets from FUN_14153c790)
#define VT(o) (*(void***)(o))
typedef int  (*st_size_t)(void* self);                          // +0x88 total size
typedef void (*st_seek_t)(void* self, int pos);                 // +0x60 absolute position
typedef int  (*st_read_t)(void* self, void* dst, int n);        // +0x20
typedef void (*st_write_t)(void* self, const void* src, int64_t n); // +0x50
#define ST_SIZE(s)        ((st_size_t)VT(s)[0x88 / 8])(s)
#define ST_SEEK(s, p)     ((st_seek_t)VT(s)[0x60 / 8])((s), (p))
#define ST_READ(s, d, n)  ((st_read_t)VT(s)[0x20 / 8])((s), (d), (n))
#define ST_WRITE(s, p, n) ((st_write_t)VT(s)[0x50 / 8])((s), (p), (n))

static int g_block;      // bytes per block
static int g_nthreads;   // threads per entry, this one included
static double g_qpf_ms;

// ---- CRC-32 (same polynomial and table as the game's) ----
static uint32_t g_crc[8][256];

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crc[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; i++)
        for (int t = 1; t < 8; t++) g_crc[t][i] = (g_crc[t - 1][i] >> 8) ^ g_crc[0][g_crc[t - 1][i] & 0xff];
}

// zlib crc32() semantics: crc32(0, ...) starts a new CRC
static uint32_t crc32_fast(uint32_t crc, const uint8_t* p, size_t n) {
    crc = ~crc;
    while (n && ((uintptr_t)p & 7)) { crc = g_crc[0][(crc ^ *p++) & 0xff] ^ (crc >> 8); n--; }
    while (n >= 8) {
        uint64_t v; memcpy(&v, p, 8);
        uint32_t lo = (uint32_t)v ^ crc, hi = (uint32_t)(v >> 32);
        crc = g_crc[7][lo & 0xff] ^ g_crc[6][(lo >> 8) & 0xff] ^ g_crc[5][(lo >> 16) & 0xff] ^ g_crc[4][lo >> 24] ^
              g_crc[3][hi & 0xff] ^ g_crc[2][(hi >> 8) & 0xff] ^ g_crc[1][(hi >> 16) & 0xff] ^ g_crc[0][hi >> 24];
        p += 8; n -= 8;
    }
    while (n--) crc = g_crc[0][(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

// CRC of A||B from crc(A), crc(B) and len(B) (zlib's crc32_combine, GF(2) matrix method)
static uint32_t gf2_times(const uint32_t* mat, uint32_t vec) {
    uint32_t sum = 0;
    for (; vec; vec >>= 1, mat++) if (vec & 1) sum ^= *mat;
    return sum;
}
static void gf2_square(uint32_t* sq, const uint32_t* mat) { for (int n = 0; n < 32; n++) sq[n] = gf2_times(mat, mat[n]); }
static uint32_t crc32_combine(uint32_t crc1, uint32_t crc2, uint64_t len2) {
    if (len2 == 0) return crc1;
    uint32_t even[32], odd[32], row = 1;
    odd[0] = 0xEDB88320u;                          // operator for one zero bit
    for (int n = 1; n < 32; n++) { odd[n] = row; row <<= 1; }
    gf2_square(even, odd);                         // two zero bits
    gf2_square(odd, even);                         // four zero bits
    for (;;) {                                     // apply len2 zero bytes to crc1
        gf2_square(even, odd);
        if (len2 & 1) crc1 = gf2_times(even, crc1);
        if (!(len2 >>= 1)) break;
        gf2_square(odd, even);
        if (len2 & 1) crc1 = gf2_times(odd, crc1);
        if (!(len2 >>= 1)) break;
    }
    return crc1 ^ crc2;
}

// ---- parallel deflate ----
typedef struct {
    const uint8_t* in; uint32_t n; int last;
    uint8_t* out; uint32_t out_n; uint32_t crc;
} zblock_t;

typedef struct {
    zblock_t* b; int nb; int level;
    volatile LONG next; volatile LONG failed;
} zjob_t;

static int compress_block(zblock_t* b, int level, uint8_t** tmp, uint32_t* cap) {
    uint32_t need = b->n + (b->n >> 3) + 4096;     // deflate's worst case is far below this
    if (*cap < need) {
        free(*tmp);
        *tmp = (uint8_t*)malloc(need);
        *cap = *tmp ? need : 0;
        if (!*tmp) return 0;
    }
    zstream_t s; memset(&s, 0, sizeof s);
    if (z_init2(&s, level, 8, -15, 9, 0, "1.2.3", (int)sizeof s) != 0) return 0;
    s.next_in = b->in; s.avail_in = b->n; s.next_out = *tmp; s.avail_out = need;
    int r = z_deflate(&s, b->last ? 4 /*Z_FINISH*/ : 2 /*Z_SYNC_FLUSH*/);
    int ok = b->last ? r == 1 /*Z_STREAM_END*/ : (r == 0 && s.avail_in == 0 && s.avail_out != 0);
    uint32_t n = s.total_out;
    z_end(&s);
    if (!ok) return 0;
    b->out = (uint8_t*)malloc(n ? n : 1);
    if (!b->out) return 0;
    memcpy(b->out, *tmp, n);
    b->out_n = n;
    b->crc = crc32_fast(0, b->in, b->n);
    return 1;
}

static void run_blocks(zjob_t* j) {
    uint8_t* tmp = NULL; uint32_t cap = 0;
    for (;;) {
        LONG i = InterlockedIncrement(&j->next) - 1;
        if (i >= j->nb || j->failed) break;
        if (!compress_block(&j->b[i], j->level, &tmp, &cap)) InterlockedExchange(&j->failed, 1);
    }
    free(tmp);
}

static DWORD WINAPI zworker(LPVOID p) { run_blocks((zjob_t*)p); return 0; }

// Returns the compressed size, or -1 if nothing was written and the original should run.
static int par_deflate(void** zip, void* in, int size, uint32_t* crc, int level, int* nb_out, int* nt_out) {
    int nb = (int)(((int64_t)size + g_block - 1) / g_block);
    int result = -1;
    uint8_t* data = (uint8_t*)VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    zblock_t* b = (zblock_t*)calloc((size_t)nb, sizeof *b);
    if (!data || !b) goto done;
    ST_SEEK(in, 0);
    for (int off = 0; off < size; ) {
        int k = size - off; if (k > (16 << 20)) k = 16 << 20;
        ST_READ(in, data + off, k);
        off += k;
    }
    for (int i = 0; i < nb; i++) {
        b[i].in = data + (size_t)i * g_block;
        b[i].n = (uint32_t)(i == nb - 1 ? size - (int64_t)i * g_block : g_block);
        b[i].last = i == nb - 1;
    }
    zjob_t j = { b, nb, level, 0, 0 };
    HANDLE th[64]; int nth = 0;
    int want = g_nthreads < nb ? g_nthreads : nb;
    for (int i = 1; i < want && nth < 63; i++) {
        th[nth] = CreateThread(NULL, 256 * 1024, zworker, &j, 0, NULL);
        if (th[nth]) nth++;
    }
    run_blocks(&j);
    if (nth) WaitForMultipleObjects((DWORD)nth, th, TRUE, INFINITE);
    for (int i = 0; i < nth; i++) CloseHandle(th[i]);
    *nb_out = nb; *nt_out = nth + 1;
    if (j.failed) goto done;
    // all blocks are compressed: write them in order and combine the CRCs. The game keeps the
    // running CRC inverted in *crc and stores ~state at the end, which is crc32(~*crc, data).
    uint32_t c = ~*crc;
    int64_t total = 0;
    for (int i = 0; i < nb; i++) {
        ST_WRITE(*zip, b[i].out, (int64_t)b[i].out_n);
        total += b[i].out_n;
        c = crc32_combine(c, b[i].crc, b[i].n);
    }
    *crc = c;
    result = (int)total;
done:
    if (b) { for (int i = 0; i < nb; i++) free(b[i].out); free(b); }
    if (data) VirtualFree(data, 0, MEM_RELEASE);
    return result;
}

static volatile LONG g_entries, g_par_entries;
static double g_zip_ms, g_save_ms; static long g_saves;

static int hk_zip_deflate(void** zip, void* in, uint8_t* buf, uint64_t buf_size, uint32_t* crc, int level) {
    LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
    int size = ST_SIZE(in);
    int r = -1, nb = 0, nt = 0;
    const char* how = "original";
    if (g_cfg.save_par && size >= 2 * g_block) {
        r = par_deflate(zip, in, size, crc, level, &nb, &nt);
        how = r >= 0 ? "parallel" : "parallel failed -> original";
    }
    if (r < 0) r = o_zip_deflate(zip, in, buf, buf_size, crc, level);
    else InterlockedIncrement(&g_par_entries);
    QueryPerformanceCounter(&t1);
    double ms = (double)(t1.QuadPart - t0.QuadPart) * g_qpf_ms;
    InterlockedIncrement(&g_entries);
    if (g_cfg.save_stats && size >= (1 << 20))
        eb_log("save: zip entry %d KB -> %d KB in %.1f ms (%s, %d blocks, %d threads)", size >> 10, r >> 10, ms, how, nb, nt);
    g_zip_ms += ms;
    return r;
}

static void hk_save(void* helper) {
    LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
    double zip0 = g_zip_ms;
    o_save(helper);
    QueryPerformanceCounter(&t1);
    double ms = (double)(t1.QuadPart - t0.QuadPart) * g_qpf_ms;
    g_save_ms += ms; g_saves++;
    eb_log("save: written in %.0f ms (zip compression %.0f ms)", ms, g_zip_ms - zip0);
}

// ---- number formatting while saving ----
// The save writer takes every value as text, even for the binary (ironman) format, which turns it
// back into a number. Two hot spots format with the CRT's sprintf: FUN_1404213a0 writes vectors of
// shorts with sprintf("%d") (call at 0x140421409 -> FUN_14015c230) and FUN_140246bc0 writes
// fixed-point values with snprintf("%d.%03d" / "-%d.%03d") (call at 0x140246c31 -> FUN_141657e40).
// Together 2.3% of all main-thread time at speed 5, nearly all during saves (~0.13 s per save).
// Both call sites are pointed at exact integer formatters; any other format string goes on to the
// CRT functions. The text, and so the save, is byte for byte the same.
#define RVA_SITE_D    0x421409u   // call FUN_14015c230 (sprintf) in FUN_1404213a0
#define RVA_SITE_FIX  0x246c31u   // call FUN_141657e40 (_snprintf) in FUN_140246bc0
#define RVA_SPRINTF   0x15c230u
#define RVA_SNPRINTF  0x1657e40u
static const uint8_t sig_site_d[11]   = { 0x48,0x8d,0x4c,0x24,0x24,0xc7,0x44,0x24,0x20,0x0c,0x00 };   // 21 bytes before the call
static const uint8_t sig_site_fix[8]  = { 0x4c,0x8b,0xc0,0xba,0xff,0x01,0x00,0x00 };                // 8 bytes before the call
typedef int (*sprintf3_t)(char* buf, const char* fmt, int v);
typedef int (*snprintf5_t)(char* buf, size_t n, const char* fmt, int a, int b);
static sprintf3_t g_sprintf;
static snprintf5_t g_snprintf;
static volatile LONG g_fmt_slow;

static int put_int(char* p, int v) {
    char tmp[12]; int n = 0, len = 0;
    unsigned u = v < 0 ? 0u - (unsigned)v : (unsigned)v;
    do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) p[len++] = '-';
    while (n) p[len++] = tmp[--n];
    p[len] = 0;
    return len;
}

static int fmt_d(char* buf, const char* fmt, int v) {
    if (fmt[0] != '%' || fmt[1] != 'd' || fmt[2]) { InterlockedIncrement(&g_fmt_slow); return g_sprintf(buf, fmt, v); }
    return put_int(buf, v);
}

static int fmt_fix(char* buf, size_t n, const char* fmt, int whole, int frac) {
    int neg = fmt[0] == '-';
    if (strcmp(fmt + neg, "%d.%03d") != 0 || frac < 0 || frac > 999 || n < 32) {
        InterlockedIncrement(&g_fmt_slow);
        return g_snprintf(buf, n, fmt, whole, frac);
    }
    int len = 0;
    if (neg) buf[len++] = '-';
    len += put_int(buf + len, whole);
    buf[len++] = '.';
    buf[len++] = (char)('0' + frac / 100);
    buf[len++] = (char)('0' + frac / 10 % 10);
    buf[len++] = (char)('0' + frac % 10);
    buf[len] = 0;
    return len;
}

#ifdef EB_TEST
// compares both formatters with the CRT for many values; returns the number of differences
__declspec(dllexport) int eb_test_fmt(void) {
    g_sprintf = (sprintf3_t)(void*)sprintf; g_snprintf = (snprintf5_t)(void*)_snprintf;
    static const int edge[] = { 0, 1, -1, 9, 10, -10, 99, 100, 999, 1000, -999, -1000, 32767, -32768, 65535, 123456789,
                                2147483647, -2147483647 - 1, -2147483647 };
    int bad = 0; char a[600], b[600];
    unsigned x = 1;
    for (int i = 0; i < 200000; i++) {
        int v = i < (int)(sizeof edge / sizeof edge[0]) ? edge[i] : (x = x * 1103515245u + 12345u, (int)x >> (i % 31));
        fmt_d(a, "%d", v); sprintf(b, "%d", v);
        if (strcmp(a, b)) { if (bad++ < 5) eb_log("TEST fmt %%d %d: '%s' vs '%s'", v, a, b); }
        int whole = v / 1000, frac = v % 1000;     // as FUN_140246bc0 does
        const char* f = "%d.%03d";
        if (frac < 0) { if (whole >= 0) f = "-%d.%03d"; frac = -frac; }
        fmt_fix(a, 0x1ff, f, whole, frac); _snprintf(b, 0x1ff, f, whole, frac);
        if (strcmp(a, b)) { if (bad++ < 5) eb_log("TEST fmt fixed %d: '%s' vs '%s'", v, a, b); }
    }
    fmt_d(a, "%x", 255);                            // other formats go to the CRT
    if (strcmp(a, "ff")) bad++;
    eb_log("TEST fmt: %d differences, %ld fallbacks", bad, (long)g_fmt_slow);
    return bad;
}
#endif

static uint8_t* call_target(uint8_t* at) { return at[0] == 0xe8 ? at + 5 + *(int32_t*)(at + 1) : NULL; }

#ifndef EB_TEST
// 14-byte "jmp [rip+0]; dq target" stubs in a page within +-2 GB of eu4.exe, for rel32 calls
static uint8_t* g_stub_page; static int g_stub_used;
static uint8_t* near_stub(uint8_t* site, void* target) {
    if (!g_stub_page) {
        SYSTEM_INFO si; GetSystemInfo(&si);
        uintptr_t gran = si.dwAllocationGranularity, base = (uintptr_t)g_exe & ~(gran - 1);
        for (uintptr_t a = base - gran; a > base - 0x70000000u && a > gran; a -= gran) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery((void*)a, &mbi, sizeof mbi)) break;
            if (mbi.State != MEM_FREE) continue;
            g_stub_page = (uint8_t*)VirtualAlloc((void*)a, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (g_stub_page) break;
        }
        if (!g_stub_page) return NULL;
    }
    int64_t d = (int64_t)(g_stub_page + g_stub_used) - (int64_t)(site + 5);
    if (g_stub_used + 16 > 4096 || d > 0x7fff0000 || d < -0x7fff0000) return NULL;
    uint8_t* s = g_stub_page + g_stub_used;
    s[0] = 0xff; s[1] = 0x25; memset(s + 2, 0, 4); memcpy(s + 6, &target, 8);
    g_stub_used += 16;
    FlushInstructionCache(GetCurrentProcess(), s, 14);
    return s;
}

static int patch_call(uint8_t* site, void* repl) {
    uint8_t* stub = near_stub(site, repl);
    if (!stub) return 0;
    int32_t rel = (int32_t)(stub - (site + 5));
    DWORD old;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(site + 1, &rel, 4);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return 1;
}

static void fastfmt_install(void) {
    uint8_t* b = (uint8_t*)g_exe;
    uint8_t* sd = b + RVA_SITE_D; uint8_t* sf = b + RVA_SITE_FIX;
    if (call_target(sd) != b + RVA_SPRINTF || memcmp(sd - 21, sig_site_d, sizeof sig_site_d) != 0 ||
        call_target(sf) != b + RVA_SNPRINTF || memcmp(sf - 8, sig_site_fix, sizeof sig_site_fix) != 0) {
        eb_log("save: number formatting call sites not as expected - fast_format off");
        return;
    }
    g_sprintf = (sprintf3_t)(void*)(b + RVA_SPRINTF);
    g_snprintf = (snprintf5_t)(void*)(b + RVA_SNPRINTF);
    if (patch_call(sd, (void*)fmt_d) && patch_call(sf, (void*)fmt_fix))
        eb_log("save: fast number formatting for %%d lists and fixed-point values");
    else
        eb_log("save: fast_format patch failed (sprintf %%d site %s)", call_target(sd) == b + RVA_SPRINTF ? "unchanged" : "patched");
}
#endif

int eb_savezip_install(void) {
#ifndef EB_TEST
    if (g_cfg.save_fmt) {
        IMAGE_NT_HEADERS64* nh = (IMAGE_NT_HEADERS64*)((uint8_t*)g_exe + ((IMAGE_DOS_HEADER*)g_exe)->e_lfanew);
        if (nh->FileHeader.TimeDateStamp == 0x66FE6AB9u) fastfmt_install();
        else eb_log("save: unknown eu4.exe - fast_format off");
    }
#endif
    if (!g_cfg.save_par && !g_cfg.save_stats) return 0;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpf_ms = 1000.0 / (double)f.QuadPart;
    crc_init();
    int kb = g_cfg.save_block_kb;
    if (kb < 256) kb = 256;
    if (kb > 65536) kb = 65536;
    g_block = kb * 1024;
    SYSTEM_INFO si; GetSystemInfo(&si);
    g_nthreads = g_cfg.save_threads > 0 ? g_cfg.save_threads : (int)si.dwNumberOfProcessors;
    if (g_nthreads > 64) g_nthreads = 64;
    if (g_nthreads < 1) g_nthreads = 1;
    uint8_t *t, *ts;
#ifdef EB_TEST
    t = (uint8_t*)GetProcAddress(g_exe, "t_zip_deflate");
    ts = (uint8_t*)GetProcAddress(g_exe, "t_save");
    z_init2 = (z_init2_t)(void*)GetProcAddress(g_exe, "t_z_init2");
    z_deflate = (z_deflate_t)(void*)GetProcAddress(g_exe, "t_z_deflate");
    z_end = (z_end_t)(void*)GetProcAddress(g_exe, "t_z_end");
    if (!t || !z_init2 || !z_deflate || !z_end) { eb_log("save: test targets not found"); return 0; }
#else
    uint8_t* b = (uint8_t*)g_exe;
    t = b + RVA_ZIP_DEFLATE; ts = b + RVA_SAVE;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    int ok = nt->FileHeader.TimeDateStamp == 0x66FE6AB9u && memcmp(t, sig_zip_deflate, 16) == 0 &&
             memcmp(b + RVA_Z_INIT2, sig_z_init2, 16) == 0 && memcmp(b + RVA_Z_DEFLATE, sig_z_deflate, 16) == 0 &&
             memcmp(b + RVA_Z_END, sig_z_end, 16) == 0 &&
             call_target(t + OFF_CALL_INIT2) == b + RVA_Z_INIT2 && call_target(t + OFF_CALL_DEFLATE) == b + RVA_Z_DEFLATE &&
             call_target(t + OFF_CALL_END) == b + RVA_Z_END &&
             memcmp(b + RVA_CRC_TABLE, g_crc[0], sizeof g_crc[0]) == 0;
    if (!ok) { eb_log("save: zip/zlib signature mismatch - save compression unchanged"); return 0; }
    z_init2 = (z_init2_t)(void*)(b + RVA_Z_INIT2);
    z_deflate = (z_deflate_t)(void*)(b + RVA_Z_DEFLATE);
    z_end = (z_end_t)(void*)(b + RVA_Z_END);
    if (memcmp(ts, sig_save, 16) != 0) ts = NULL;
#endif
    MH_STATUS s = MH_Initialize();
    if ((s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) ||
        MH_CreateHook(t, (void*)hk_zip_deflate, (void**)&o_zip_deflate) != MH_OK || MH_EnableHook(t) != MH_OK) {
        eb_log("save: hook failed - save compression unchanged");
        return 0;
    }
    if (g_cfg.save_stats && ts && MH_CreateHook(ts, (void*)hk_save, (void**)&o_save) == MH_OK) MH_EnableHook(ts);
    if (g_cfg.save_par)
        eb_log("save: parallel zip compression on (%d KB blocks, up to %d threads)", g_block >> 10, g_nthreads);
    else
        eb_log("save: zip compression timing only (parallel_compress=0)");
    return 1;
}

void eb_savezip_report(void) {
    if (!o_zip_deflate) return;
    if (g_entries)
        eb_log("save: %ld zip entries (%ld parallel), compression %.0f ms in total; %ld saves, %.0f ms in total",
               (long)g_entries, (long)g_par_entries, g_zip_ms, g_saves, g_save_ms);
}
