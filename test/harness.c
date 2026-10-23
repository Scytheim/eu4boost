// Smoke test for eu4boost.dll under Wine/Windows.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

// Stand-ins for eu4.exe's CRT heap functions (process heap, like UCRT). Exported so the
// EB_TEST build of the DLL can find and hook them. noinline + some body so MinHook can patch.
__declspec(dllexport) __attribute__((noinline)) void* t_malloc(size_t n) { volatile int k = 0; k++; return HeapAlloc(GetProcessHeap(), 0, n ? n : 1); }
__declspec(dllexport) __attribute__((noinline)) void* t_calloc(size_t c, size_t n) { volatile int k = 0; k++; return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, c * n ? c * n : 1); }
__declspec(dllexport) __attribute__((noinline)) void t_free(void* p) { volatile int k = 0; k++; if (p) HeapFree(GetProcessHeap(), 0, p); }
__declspec(dllexport) __attribute__((noinline)) void* t_realloc(void* p, size_t n) { volatile int k = 0; k++; if (p && !n) { HeapFree(GetProcessHeap(), 0, p); return NULL; } return p ? HeapReAlloc(GetProcessHeap(), 0, p, n) : t_malloc(n); }
__declspec(dllexport) __attribute__((noinline)) size_t t_msize(void* p) { volatile int k = 0; k++; return HeapSize(GetProcessHeap(), 0, p); }
__declspec(dllexport) __attribute__((noinline)) void* t_recalloc(void* p, size_t c, size_t n) { volatile int k = 0; k++; return HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, p, c * n); }

__declspec(dllimport) void POPS_RunCallbacks(void* h);
__declspec(dllimport) long POPS_TestCount(void* h);

// Stand-ins for the gamestate pointer and CDailyTickCommand::Execute: the daily tick advances
// the CDate (hours + 43800000) at gamestate+0x1dd0 by 24 hours.
static char gs_buf[0x2000];
__declspec(dllexport) char* t_gs = gs_buf;
__declspec(dllexport) __attribute__((noinline)) void t_daily_cmd(void* cmd) { (void)cmd; *(volatile int*)(t_gs + 0x1dd0) += 24; Sleep(2); }
// Stand-in for the session update wrapper FUN_14020f810: runs one day per call while days are allowed
// (the game's timer: always at speed 5, at most one per frame at slower speeds, none while paused).
// The game timer FUN_140757ad0 queues one day when allowed (speed 5 and not paused); the session
// update executes the queued day. allow_days limits how many days the "timer" may still queue.
static volatile LONG allow_days, queued_day;
__declspec(dllexport) __attribute__((noinline)) void t_game_timer(void* gs, float seconds) { volatile int k = 0; k++; (void)gs; (void)seconds; if (allow_days > 0 && !queued_day) { allow_days--; queued_day = 1; } }
__declspec(dllexport) __attribute__((noinline)) void t_session_frame(void* s) { volatile int k = 0; k++; (void)s; int cmd = 0; if (queued_day) { queued_day = 0; t_daily_cmd(&cmd); } }

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// Stand-ins for the save zip path (savezip.c): a "zlib" that writes stored blocks (a valid raw
// deflate stream, just not compressed), the game's zip entry function FUN_14153c790 rewritten
// from its disassembly, and the game's stream objects (virtual read/seek/size/write).
typedef struct { const unsigned char* next_in; unsigned avail_in, total_in; unsigned char* next_out; unsigned avail_out, total_out;
                 const char* msg; void* state; void* zalloc; void* zfree; void* opaque; int data_type; unsigned adler, reserved; } zs_t;
static volatile LONG z_open;
__declspec(dllexport) __attribute__((noinline)) int t_z_init2(zs_t* s, int level, int method, int wbits, int mem, int strat, const char* ver, int size) {
    if (size != (int)sizeof(zs_t) || method != 8 || wbits != -15 || mem != 9 || strat != 0 || level != 3 || !ver || ver[0] != '1') return -6;
    s->total_in = s->total_out = 0; s->state = (void*)1; InterlockedIncrement(&z_open);
    return 0;
}
static void z_put_stored(zs_t* s, unsigned k, int final) {
    unsigned char* o = s->next_out;
    o[0] = (unsigned char)final; o[1] = k & 0xff; o[2] = (k >> 8) & 0xff; o[3] = ~k & 0xff; o[4] = (~k >> 8) & 0xff;
    if (k) memcpy(o + 5, s->next_in, k);
    s->next_out += k + 5; s->avail_out -= k + 5; s->total_out += k + 5;
    s->next_in += k; s->avail_in -= k; s->total_in += k;
}
__declspec(dllexport) __attribute__((noinline)) int t_z_deflate(zs_t* s, int flush) {
    if (s->state != (void*)1) return -2;
    while (s->avail_in) {
        if (s->avail_out <= 5) return 0;                 // out of room: Z_OK, the caller continues
        unsigned k = s->avail_in > 65535 ? 65535 : s->avail_in;
        if (k > s->avail_out - 5) k = s->avail_out - 5;
        int fin = flush == 4 && k == s->avail_in;
        z_put_stored(s, k, fin);
        if (fin) { s->state = (void*)2; return 1; }
    }
    if (flush == 4 || flush == 2) {                      // Z_FINISH / Z_SYNC_FLUSH: (final) empty block
        if (s->avail_out < 5) return 0;
        z_put_stored(s, 0, flush == 4);
        if (flush == 4) { s->state = (void*)2; return 1; }
    }
    return 0;
}
__declspec(dllexport) __attribute__((noinline)) int t_z_end(zs_t* s) { volatile int k = 0; k++; s->state = NULL; InterlockedDecrement(&z_open); return 0; }

#define VT(o) (*(void***)(o))
#define ST_SIZE(s)        ((int (*)(void*))VT(s)[0x88 / 8])(s)
#define ST_SEEK(s, p)     ((void (*)(void*, int))VT(s)[0x60 / 8])((s), (p))
#define ST_READ(s, d, n)  ((int (*)(void*, void*, int))VT(s)[0x20 / 8])((s), (d), (n))
#define ST_WRITE(s, p, n) ((void (*)(void*, const void*, long long))VT(s)[0x50 / 8])((s), (p), (n))
static unsigned crc_tab[256];
__declspec(dllexport) __attribute__((noinline)) int t_zip_deflate(void** zip, void* in, unsigned char* buf, unsigned long long buf_size, unsigned* crc, int level) {
    zs_t s; memset(&s, 0, sizeof s);
    if (t_z_init2(&s, level, 8, -15, 9, 0, "1.2.3", (int)sizeof s)) return 0;
    unsigned half = (unsigned)(buf_size >> 1), rest = (unsigned)(buf_size - half);
    int left = ST_SIZE(in), written = 0;
    while (left > 0) {
        ST_SEEK(in, ST_SIZE(in) - left);
        int k = left < (int)half ? left : (int)half;
        ST_READ(in, buf, k);
        unsigned before = s.total_in;
        s.next_in = buf; s.avail_in = (unsigned)k; s.next_out = buf + half; s.avail_out = rest;
        if (t_z_deflate(&s, 0)) { t_z_end(&s); return 0; }
        unsigned c = *crc;
        for (unsigned i = 0, n = s.total_in - before; i < n; i++) c = c >> 8 ^ crc_tab[(buf[i] ^ c) & 0xff];
        *crc = c;
        int out = (int)s.total_out - written;
        left = ST_SIZE(in) - (int)s.total_in;
        if (out > 0) { ST_WRITE(*zip, buf + half, out); written += out; }
    }
    s.next_in = NULL; s.avail_in = 0; s.next_out = buf + half; s.avail_out = rest;
    int r;
    while ((r = t_z_deflate(&s, 4)) == 0) {}
    if (r != 1) return 0;
    int out = (int)s.total_out - written;
    if (out > 0) { ST_WRITE(*zip, buf + half, out); written += out; }
    t_z_end(&s);
    *crc = ~*crc;
    return written;
}
typedef struct { void** vt; const unsigned char* data; int size, pos; } in_t;
static int in_read(in_t* s, void* d, int n) { if (n > s->size - s->pos) n = s->size - s->pos; memcpy(d, s->data + s->pos, (size_t)n); s->pos += n; return n; }
static void in_seek(in_t* s, int p) { s->pos = p; }
static int in_size(in_t* s) { return s->size; }
typedef struct { void** vt; unsigned char* data; size_t n, cap; } out_t;
static void out_write(out_t* s, const void* p, long long n) {
    if (s->n + (size_t)n > s->cap) { s->cap = (s->n + (size_t)n) * 2; s->data = realloc(s->data, s->cap); }
    memcpy(s->data + s->n, p, (size_t)n); s->n += (size_t)n;
}

// Stand-ins for the modifier-container add (FUN_1408ea420, modifier.c): the game's algorithm
// rewritten from its disassembly, twice - t_modadd gets hooked, ref_modadd stays the original -
// plus the operator new, the vector grow-and-insert and the container's allocator object.
__declspec(dllexport) __attribute__((noinline)) void* t_new(unsigned long long n) { volatile int k = 0; k++; return malloc(n); }
__declspec(dllexport) __attribute__((noinline)) void* t_pushback(void** vec, void** where, void** val) {
    size_t size = (size_t)((char*)vec[1] - (char*)vec[0]) / 8, at = (size_t)((char*)where - (char*)vec[0]) / 8;
    size_t ncap = size ? size * 2 : 1;
    void** nb = malloc(ncap * 8);
    for (size_t i = 0; i < at; i++) nb[i] = ((void**)vec[0])[i];
    nb[at] = *val;
    for (size_t i = at; i < size; i++) nb[i + 1] = ((void**)vec[0])[i];
    free(vec[0]); vec[0] = nb; vec[1] = nb + size + 1; vec[2] = nb + ncap;
    return nb + at;
}
static void mc_free(void* self, long long unused) { (void)unused; free(*(void**)((char*)self + 8)); }
static void* mc_vt[3] = { NULL, NULL, (void*)mc_free };
static void* modadd_ref(unsigned char* dst, unsigned char* src) {
    int n = *(int*)(src + 0x64);
    for (int k = 0; k < n; k++) {
        int* sd = *(int**)(src + 0x58);
        int id = sd[k * 2], value;
        if (*(char*)(src + 0x10) && id <= 0x286) value = sd[id * 2 + 1];
        else value = k < *(int*)(src + 0x64) ? sd[k * 2 + 1] : 0;
        if (id == 0x286) continue;
        int* dd = *(int**)(dst + 0x58);
        if (*(char*)(dst + 0x10) && id < 0x286) { dd[id * 2] = id; dd[id * 2 + 1] += value; continue; }
        int c = *(int*)(dst + 0x64), j;
        for (j = 0; j < c; j++) if (dd[j * 2] == id) break;
        if (j < c) { dd[j * 2 + 1] += value; continue; }
        int cap = *(int*)(dst + 0x60);
        if (c + 1 > cap) {
            int ncap = cap * 2; if (c + 1 > ncap) ncap = c + 1;
            int* nd = t_new((unsigned long long)(unsigned)ncap * 8);
            for (int i = 0; i < c; i++) { nd[i * 2] = dd[i * 2]; nd[i * 2 + 1] = dd[i * 2 + 1]; }
            void** alloc = (void**)(dst + 0x50);
            ((void (*)(void*, long long))(*(void***)alloc)[2])(alloc, 0);
            *(int**)(dst + 0x58) = nd; *(int*)(dst + 0x60) = ncap; dd = nd;
        }
        *(int*)(dst + 0x64) = c + 1; dd[c * 2] = id; dd[c * 2 + 1] = value;
    }
    void*** vec = (void***)(dst + 0x38);
    if (vec[1] == vec[2]) { void* v = src; t_pushback((void**)vec, (void**)vec[1], &v); }
    else { *vec[1] = src; vec[1]++; }
    return dst;
}
__declspec(dllexport) __attribute__((noinline)) void* t_modadd(unsigned char* dst, unsigned char* src) { volatile int k = 0; k++; return modadd_ref(dst, src); }
static __attribute__((noinline)) void* ref_modadd(unsigned char* dst, unsigned char* src) { volatile int k = 0; k++; return modadd_ref(dst, src); }

static unsigned mx = 99;
static unsigned rnd(unsigned n) { mx = mx * 1103515245u + 12345u; return (mx >> 8) % (n ? n : 1); }
static int rnd_id(void) {
    switch (rnd(8)) {
    case 0: return 0x286;                                  // skipped
    case 1: return 20000 + (int)rnd(8);                    // beyond the fast path's table
    case 2: return 0x287 + (int)rnd(40);                   // few distinct: duplicates
    case 3: case 4: return (int)rnd(0x290);                // dense range
    default: return 0x287 + (int)rnd(3000);
    }
}
static unsigned char* mc_new(int dense, int count, int cap, int vec_n, int vec_cap, int allow_neg) {
    unsigned char* c = calloc(1, 0x70);
    c[0x10] = (unsigned char)dense;
    int alloc_n = dense && cap < 0x287 ? 0x287 : cap;
    int* d = calloc((size_t)(alloc_n ? alloc_n : 1), 8);
    for (int i = 0; i < count; i++) { d[i * 2] = dense ? (rnd(4) ? (int)rnd(0x287) : rnd_id()) : rnd_id(); d[i * 2 + 1] = (int)rnd(2000) - 1000; }
    if (dense) for (int i = count; i < alloc_n; i++) d[i * 2 + 1] = (int)rnd(100);
    if (allow_neg && !dense && rnd(5) == 0 && count) d[(count - 1) * 2] = -1 - (int)rnd(3);   // negative id (never added into a dense dst: the game would write before its array)
    *(void**)(c + 0x50) = mc_vt; *(int**)(c + 0x58) = d; *(int*)(c + 0x60) = alloc_n; *(int*)(c + 0x64) = count;
    void** v = malloc((size_t)(vec_cap ? vec_cap : 1) * 8);
    for (int i = 0; i < vec_n; i++) v[i] = (void*)(uintptr_t)(0x1000 + i);
    ((void**)(c + 0x38))[0] = v; ((void**)(c + 0x38))[1] = v + vec_n; ((void**)(c + 0x38))[2] = v + vec_cap;
    return c;
}
static unsigned char* mc_clone(unsigned char* s) {
    unsigned char* c = malloc(0x70); memcpy(c, s, 0x70);
    int cap = *(int*)(s + 0x60);
    int* d = malloc((size_t)(cap ? cap : 1) * 8); memcpy(d, *(int**)(s + 0x58), (size_t)cap * 8); *(int**)(c + 0x58) = d;
    void** sv = (void**)(s + 0x38); size_t vn = (size_t)((char*)sv[1] - (char*)sv[0]) / 8, vc = (size_t)((char*)sv[2] - (char*)sv[0]) / 8;
    void** v = malloc((vc ? vc : 1) * 8); memcpy(v, sv[0], vn * 8);
    ((void**)(c + 0x38))[0] = v; ((void**)(c + 0x38))[1] = v + vn; ((void**)(c + 0x38))[2] = v + vc;
    return c;
}
static int mc_same(unsigned char* a, unsigned char* b) {
    int n = *(int*)(a + 0x64);
    if (n != *(int*)(b + 0x64) || *(int*)(a + 0x60) != *(int*)(b + 0x60)) return 0;
    if (memcmp(*(int**)(a + 0x58), *(int**)(b + 0x58), (size_t)n * 8)) return 0;
    void** va = (void**)(a + 0x38); void** vb = (void**)(b + 0x38);
    size_t na = (size_t)((char*)va[1] - (char*)va[0]), nb = (size_t)((char*)vb[1] - (char*)vb[0]);
    size_t ca = (size_t)((char*)va[2] - (char*)va[0]), cb = (size_t)((char*)vb[2] - (char*)vb[0]);
    return na == nb && ca == cb && memcmp(va[0], vb[0], na) == 0;
}
static void mc_free_all(unsigned char* c) { free(*(void**)(c + 0x58)); free(((void**)(c + 0x38))[0]); free(c); }

static void modadd_test(void) {
    int bad = 0, trials = 8000;
    for (int t = 0; t < trials; t++) {
        int dcount = rnd(4) ? (int)rnd(60) : (int)rnd(900), dcap = dcount + (rnd(3) ? 0 : (int)rnd(20));   // large dst: the SIMD search
        int vn = (int)rnd(4), vcap = vn + (int)rnd(2);
        int ddense = t % 17 == 0;
        unsigned char* a = mc_new(ddense, dcount, dcap, vn, vcap, 1);
        int scount = rnd(2) ? (int)rnd(4) : rnd(4) ? 16 + (int)rnd(700) : (int)rnd(20);   // 1-3 pairs: the small-add path
        unsigned char* src = mc_new(rnd(3) == 0, scount, scount, 0, 0, !ddense);
        unsigned char* b = mc_clone(a);
        void* ra = t_modadd(a, src);
        void* rb = ref_modadd(b, src);
        if (ra != a || rb != b || !mc_same(a, b)) { if (bad++ < 5) printf("modadd mismatch in trial %d (dst %d pairs, src %d)\n", t, dcount, scount); }
        if (t % 50 == 0) {                                 // dst == src goes through the original
            unsigned char* s2 = mc_clone(src); unsigned char* s3 = mc_clone(src);
            if (s2[0x10]) { s2[0x10] = 0; s3[0x10] = 0; }  // dense self-add reads/writes by id: keep it sparse
            t_modadd(s2, s2); ref_modadd(s3, s3);
            if (*(int*)(s2 + 0x64) != *(int*)(s3 + 0x64) || memcmp(*(int**)(s2 + 0x58), *(int**)(s3 + 0x58), (size_t)*(int*)(s2 + 0x64) * 8)) bad++;
            mc_free_all(s2); mc_free_all(s3);
        }
        mc_free_all(a); mc_free_all(b); mc_free_all(src);
    }
    printf("modadd: %d trials, %d mismatches\n", trials, bad);
    CHECK(bad == 0);
}

// crash guards: t_tip mimics the game's modifier breakdown FUN_1408e8ca0 (reads the container,
// calls each source's virtual +0x60, recurses through its own entry so the hook sees every level),
// t_rel mimics FUN_140a5e130 (dereferences the relation)
static int tip_query(void* self) { (void)self; return 1; }
typedef int (*tip_query_t)(void*);
static void* const tip_vt[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, (void*)tip_query, 0, 0, 0 };
typedef void* (*tip_fn_t)(unsigned char*, unsigned long long*, unsigned char*);
static tip_fn_t volatile tip_entry;
__declspec(dllexport) __attribute__((noinline)) void* t_tip(unsigned char* c, unsigned long long* out, unsigned char* desc) {
    int id = *(int*)desc, v = 0;
    int* d = *(int**)(c + 0x58);
    if (c[0x10]) v = d[id * 2 + 1];
    else for (int i = 0; i < *(int*)(c + 0x64); i++) if (d[i * 2] == id) { v = d[i * 2 + 1]; break; }
    unsigned long long visited = 1;
    void** b = ((void***)(c + 0x38))[0]; void** e = ((void***)(c + 0x38))[1];
    for (void** s = b; s < e; s++) {
        if (!*s) continue;
        void** vt = **(void****)s;
        if (!((tip_query_t)vt[12])(*s)) continue;
        unsigned long long sub[4]; int subtotal = 0; unsigned char d2[0x20];
        memcpy(d2, desc, 0x20); *(int**)(d2 + 0x10) = &subtotal;
        tip_entry((unsigned char*)*s, sub, d2);
        visited += sub[0]; v += subtotal;
    }
    int* total = *(int**)(desc + 0x10);
    if (total) *total = v;
    out[0] = visited; out[1] = 0; out[2] = 0; out[3] = 15;
    return out;
}
__declspec(dllexport) __attribute__((noinline)) void t_rel(unsigned char* rel) { volatile int k = 0; k++; rel[0x28] = 1; }
typedef void (*tick_fn_t)(void*);
static tick_fn_t volatile tick_body;
__declspec(dllexport) __attribute__((noinline)) unsigned long long t_midtick(void* gs, char a, void* b, unsigned c) { (void)a; (void)b; (void)c; if (tick_body) tick_body(gs); return 0; }
__declspec(dllexport) __attribute__((noinline)) unsigned long long t_day(void* gs) { if (tick_body) tick_body(gs); return 0; }
static unsigned long long tick_visited; static int tick_total;

static unsigned char* tip_new(unsigned char* at, int dense, int id, int value, void** srcs, int nsrc) {
    unsigned char* c = at ? at : calloc(1, 0x68);
    memset(c, 0, 0x68);
    *(void* const**)c = tip_vt;
    c[0x10] = (unsigned char)dense;
    int cap = dense ? 0x287 : 4;
    int* d = calloc((size_t)cap, 8);
    if (dense) { d[id * 2] = id; d[id * 2 + 1] = value; *(int*)(c + 0x64) = cap; }
    else { d[0] = id; d[1] = value; *(int*)(c + 0x64) = 1; }
    *(int**)(c + 0x58) = d; *(int*)(c + 0x60) = cap;
    void** v = calloc((size_t)nsrc + 1, 8);
    if (nsrc) memcpy(v, srcs, (size_t)nsrc * 8);
    ((void***)(c + 0x38))[0] = v; ((void***)(c + 0x38))[1] = v + nsrc; ((void***)(c + 0x38))[2] = v + nsrc;
    return c;
}

static int tip_run(unsigned char* c, unsigned long long* visited) {
    unsigned long long out[4] = { 99, 99, 99, 99 }; int total = -1; unsigned char desc[0x20] = { 0 };
    *(int*)desc = 7; *(int**)(desc + 0x10) = &total;
    void* r = t_tip(c, out, desc);
    CHECK(r == out && out[3] == 15);
    *visited = out[0];
    return total;
}

static int tip_run(unsigned char* c, unsigned long long* visited);
static void tip_tick_valid(void* c) { tick_total = tip_run((unsigned char*)c, &tick_visited); }
static void tip_tick_reused(void* c) { tick_total = tip_run((unsigned char*)c, &tick_visited); }
static void tick_then_mid(void* c) { tick_body = (tick_fn_t)(void*)tip_tick_reused; t_midtick(c, 0, NULL, 0); }
static void guard_test(void) {
    tip_entry = t_tip;
    unsigned long long n;
    unsigned char* a = tip_new(NULL, 0, 7, 10, NULL, 0);
    unsigned char* b = tip_new(NULL, 1, 7, 20, NULL, 0);
    void* s1[3] = { a, b, NULL };
    unsigned char* root = tip_new(NULL, 0, 7, 1, s1, 3);
    int t = tip_run(root, &n);
    printf("guard: valid tree visited %llu total %d\n", n, t);
    CHECK(n == 3 && t == 31);
    // a source freed and reused for float data (the 20:08 crash): hidden, the vector is restored
    float* g = malloc(0x200); for (int i = 0; i < 0x80; i++) g[i] = 2849.0f + (float)i;
    void* s2[3] = { a, g, b };
    unsigned char* root2 = tip_new(NULL, 0, 7, 1, s2, 3);
    void** v2 = ((void***)(root2 + 0x38))[0];
    t = tip_run(root2, &n);
    printf("guard: reused source visited %llu total %d\n", n, t);
    CHECK(n == 3 && t == 31 && ((void***)(root2 + 0x38))[0] == v2 && ((void***)(root2 + 0x38))[1] == v2 + 3);
    // a source on a decommitted page
    unsigned char* page = VirtualAlloc(NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    tip_new(page, 0, 7, 5, NULL, 0);
    VirtualFree(page, 65536, MEM_DECOMMIT);
    void* s3[2] = { a, page };
    unsigned char* root3 = tip_new(NULL, 0, 7, 1, s3, 2);
    t = tip_run(root3, &n);
    printf("guard: decommitted source visited %llu total %d\n", n, t);
    CHECK(n == 2 && t == 11);
    // the container itself is garbage: empty text and total
    t = tip_run((unsigned char*)g, &n);
    printf("guard: invalid container visited %llu total %d\n", n, t);
    CHECK(n == 0 && t == 0);
    // inside the daily tick the guard steps aside (a valid tree still works), in its mid-tick frames it checks again
    tick_body = (tick_fn_t)(void*)tip_tick_valid; t_day(root);
    printf("guard: inside the daily tick visited %llu total %d\n", tick_visited, tick_total);
    CHECK(tick_visited == 3 && tick_total == 31);
    tick_body = (tick_fn_t)(void*)tick_then_mid; t_day(root2);   // daily tick -> mid-tick frame -> tooltip with a reused source
    printf("guard: mid-tick frame visited %llu total %d\n", tick_visited, tick_total);
    CHECK(tick_visited == 3 && tick_total == 31);
    tick_body = NULL;
    // a missing relation is skipped, a real one still runs
    unsigned char rel[0x40] = { 0 };
    t_rel(NULL);
    t_rel(rel);
    CHECK(rel[0x28] == 1);
}

// zip-entry round trip: the output must be one valid stored-block stream with exactly one final
// block that gives back the input, and the CRC must be the plain byte-wise CRC-32
static void savezip_test(void) {
    for (unsigned i = 0; i < 256; i++) { unsigned c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_tab[i] = c; }
    static void* in_vt[18]; static void* out_vt[11];
    in_vt[0x20 / 8] = (void*)in_read; in_vt[0x60 / 8] = (void*)in_seek; in_vt[0x88 / 8] = (void*)in_size;
    out_vt[0x50 / 8] = (void*)out_write;
    static const int sizes[] = { 0, 1000, 300000, 2097152, 2097153, 5000000, 9437191 };
    unsigned char* buf = malloc(1 << 20);
    for (size_t t = 0; t < sizeof sizes / sizeof sizes[0]; t++) {
        int size = sizes[t];
        unsigned char* data = malloc((size_t)size + 1); unsigned char* back = malloc((size_t)size + 1);
        unsigned x = 12345u + (unsigned)t;
        for (int i = 0; i < size; i++) { x = x * 1103515245u + 12345u; data[i] = (unsigned char)(x >> 24 & (i & 0x1000 ? 0x0f : 0xff)); }
        in_t in = { in_vt, data, size, 0 }; out_t out = { out_vt, NULL, 0, 0 }; void* zip[12] = { &out };
        unsigned crc = 0xffffffffu;
        int r = t_zip_deflate(zip, &in, buf, 1 << 20, &crc, 3);
        unsigned ref = 0xffffffffu;
        for (int i = 0; i < size; i++) ref = ref >> 8 ^ crc_tab[(data[i] ^ ref) & 0xff];
        ref = ~ref;
        size_t p = 0, got = 0; int finals = 0, ok = 1;
        while (p + 5 <= out.n) {
            int fin = out.data[p] & 1; ok &= (out.data[p] & ~1) == 0;
            unsigned len = out.data[p + 1] | out.data[p + 2] << 8, nlen = out.data[p + 3] | out.data[p + 4] << 8;
            ok &= (len ^ nlen) == 0xffff; p += 5;
            if (p + len > out.n || got + len > (size_t)size) { ok = 0; break; }
            memcpy(back + got, out.data + p, len); p += len; got += len;
            if (fin) { finals++; break; }
        }
        int same = got == (size_t)size && memcmp(back, data, (size_t)size) == 0;
        CHECK(r == (int)out.n); CHECK(crc == ref); CHECK(ok && finals == 1 && p == out.n && same);
        printf("savezip: %7d bytes -> %7d, crc %08x (ref %08x), stream %s\n", size, r, crc, ref, ok && finals == 1 && p == out.n && same ? "ok" : "BAD");
        free(data); free(back); free(out.data);
    }
    free(buf);
    CHECK(z_open == 0);
}

static volatile LONG stop;
static DWORD WINAPI churn(LPVOID arg) {
    uintptr_t seed = (uintptr_t)arg * 2654435761u + 1;
    void* slots[256] = {0};
    long iters = 0;
    while (!stop) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        int i = (int)((seed >> 33) & 255);
        size_t sz = (size_t)((seed >> 13) & 1023) + 1;
        if (slots[i]) { if ((seed >> 7) & 1) slots[i] = t_realloc(slots[i], sz); else { t_free(slots[i]); slots[i] = NULL; } }
        else { slots[i] = t_malloc(sz); memset(slots[i], 0xAB, sz); }
        iters++;
    }
    for (int i = 0; i < 256; i++) t_free(slots[i]);
    return (DWORD)iters;
}

static DWORD WINAPI other_thread_pops(LPVOID h) { for (int i = 0; i < 50; i++) POPS_RunCallbacks(h); return 0; }

int main(int argc, char** argv) {
    // allocate a "foreign" block before the plugin is loaded
    void* early = t_malloc(100); memset(early, 1, 100);
    const char* dll = argc > 1 ? argv[1] : "eu4boost_test.dll";
    HMODULE h = LoadLibraryA(dll);
    printf("LoadLibrary(%s) = %p (err %lu)\n", dll, (void*)h, GetLastError());
    CHECK(h != NULL);

    // 0. main thread affinity hook (IAT of the exe)
    DWORD_PTR prev = SetThreadAffinityMask(GetCurrentThread(), 1);
    CHECK(prev != 0);
    // 1. foreign block freed through hooked t_free / sized / realloc'd
    CHECK(t_msize(early) == 100);
    void* early2 = t_realloc(early, 5000);  // moves into mimalloc
    CHECK(early2 != NULL);
    CHECK(((unsigned char*)early2)[99] == 1);
    t_free(early2);
    void* foreign = HeapAlloc(GetProcessHeap(), 0, 64);
    t_free(foreign);                         // foreign free path

    // 2. mimalloc block freed through the Windows heap API (guard)
    void* p = t_malloc(48);
    CHECK(p != NULL);
    memset(p, 7, 48);
    CHECK(HeapSize(GetProcessHeap(), 0, p) >= 48);
    void* q = HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, p, 4096);
    CHECK(q != NULL && ((unsigned char*)q)[47] == 7 && ((unsigned char*)q)[4095] == 0);
    CHECK(HeapFree(GetProcessHeap(), 0, q));

    // 3. calloc / recalloc semantics
    unsigned char* c = t_calloc(10, 10);
    int zero = 1; for (int i = 0; i < 100; i++) zero &= c[i] == 0;
    CHECK(zero);
    memset(c, 9, 100);
    c = t_recalloc(c, 30, 10);
    zero = 1; for (int i = 100; i < 300; i++) zero &= c[i] == 0;
    CHECK(zero && c[99] == 9);
    t_free(c);
    CHECK(t_realloc(t_malloc(10), 0) == NULL);

    // 3b. zero-filled malloc (EU4dll compatibility) when requested
    if (argc > 3 && strcmp(argv[3], "zero") == 0) {
        int ok = 1;
        for (int round = 0; round < 1000; round++) {
            size_t sz = 16 + (round * 37) % 4000;
            unsigned char* a = t_malloc(sz);
            for (size_t i = 0; i < sz; i++) ok &= a[i] == 0;
            memset(a, 0xFF, sz);
            t_free(a);
        }
        CHECK(ok);
        printf("zero-fill check %s\n", ok ? "ok" : "FAILED");
    }

    // 3c. large-only zero fill (EU4dll auto mode): blocks >= 64 KB, incl. EU4dll's 0x103D88-byte
    //     font buffer, must come back zeroed even after the same memory was dirtied and freed.
    if (argc > 3 && strcmp(argv[3], "zerolarge") == 0) {
        int ok = 1;
        static const size_t sizes[] = { 65536, 0x103D88, 200000, 3u << 20 };
        for (int round = 0; round < 200; round++) {
            size_t sz = sizes[round % 4];
            unsigned char* a = t_malloc(sz);
            for (size_t i = 0; i < sz; i += 61) ok &= a[i] == 0;
            ok &= a[sz - 1] == 0;
            memset(a, 0xEE, sz);
            t_free(a);
        }
        // a big zeroed block (from the Windows heap by default) grown with realloc keeps its bytes
        unsigned char* big = t_malloc(8u << 20);
        for (size_t i = 0; i < (8u << 20); i += 4093) ok &= big[i] == 0;
        memset(big, 0x5A, 8u << 20);
        big = t_realloc(big, 9u << 20);
        ok &= big != NULL && big[0] == 0x5A && big[(8u << 20) - 1] == 0x5A;
        t_free(big);
        CHECK(ok);
        printf("large zero-fill check %s\n", ok ? "ok" : "FAILED");
    }

    // 3d. online polling throttle: two handles pumped every "frame" (~5 ms) for ~1 s from this
    //     thread while "days" run (a daily tick every frame) must each run about every 100 ms; calls
    //     from another thread pass through. Then 0.5 s without days (paused, menus): every pump runs,
    //     unless only_while_running=0 ("always").
    {
        void* h1 = (void*)0x1111; void* h2 = (void*)0x2222; void* h3 = (void*)0x3333;
        ULONGLONG t0 = GetTickCount64(); int frames = 0;
        while (GetTickCount64() - t0 < 1000) { t_day(NULL); POPS_RunCallbacks(h1); POPS_RunCallbacks(h2); Sleep(5); frames++; }
        HANDLE ot = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)(void*)other_thread_pops, h3, 0, NULL);
        WaitForSingleObject(ot, INFINITE);
        long c1 = POPS_TestCount(h1), c2 = POPS_TestCount(h2), c3 = POPS_TestCount(h3);
        Sleep(400);
        int still = 0; t0 = GetTickCount64();
        while (GetTickCount64() - t0 < 500) { POPS_RunCallbacks(h1); POPS_RunCallbacks(h2); Sleep(5); still++; }
        long d1 = POPS_TestCount(h1) - c1, d2 = POPS_TestCount(h2) - c2;
        printf("online throttle: %d frames with days: handle1 %ld runs, handle2 %ld runs, other thread %ld/50; "
               "%d frames without days: handle1 %ld runs, handle2 %ld runs\n", frames, c1, c2, c3, still, d1, d2);
        if (argc > 4 && strcmp(argv[4], "nothrottle") == 0) { CHECK(c1 == frames && c2 == frames); CHECK(d1 == still && d2 == still); }
        else if (argc > 4 && strcmp(argv[4], "always") == 0) {
            CHECK(c1 >= 5 && c1 <= 14); CHECK(c2 >= 5 && c2 <= 14); CHECK(d1 >= 3 && d1 <= 8); CHECK(d2 >= 3 && d2 <= 8);
        } else { CHECK(c1 >= 5 && c1 <= 14); CHECK(c2 >= 5 && c2 <= 14); CHECK(d1 == still && d2 == still); }
        CHECK(c3 == 50);
    }

    // 3e. speed log: 150 "days" ~5 ms apart from 1596.09.11, a 1.3 s pause, then 30 more
    if (argc > 3 && strcmp(argv[3], "speed") == 0) {
        int cmd = 0;
        *(int*)(t_gs + 0x1dd0) = 57787032 - 24;   // 1596.09.10: the first tick processes 09.11
        for (int i = 0; i < 150; i++) { t_daily_cmd(&cmd); Sleep(3); }
        Sleep(1300);
        for (int i = 0; i < 30; i++) { t_daily_cmd(&cmd); Sleep(3); }
        printf("speed: fed 180 days\n");
    }

    // 3f. chunked parallel-for re-implementation on a mock of the game's structures
    if (argc > 3 && strcmp(argv[3], "partest") == 0) {
        typedef int (*pt_t)(int, int, int, int);
        pt_t pt = (pt_t)GetProcAddress(h, "eb_test_parallel");
        CHECK(pt != NULL);
        if (pt) {
            static const int cases[][4] = { {0,5000,11,0}, {1,4940,11,0}, {0,254,11,0}, {0,62,1,0}, {0,1380,4,0},
                                            {0,300,11,1}, {0,4000,11,1}, {0,0,11,0}, {5,1,11,1}, {0,100000,11,0} };
            void (*lpt)(int) = (void (*)(int))GetProcAddress(h, "eb_test_set_lpt");
            CHECK(lpt != NULL);
            for (int r = 0; r < 6; r++) {
                if (lpt) lpt(r >= 3);           // rounds 3-5: items taken in longest-first order
                for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
                    CHECK(pt(cases[i][0], cases[i][1], cases[i][2], cases[i][3]));
            }
            printf("parallel self-test done\n");
        }
    }

    // 3f2. several days per frame at speed 5 (speed5_days_per_frame=3)
    if (argc > 3 && strcmp(argv[3], "pacing") == 0) {
        int* speed = (int*)(t_gs + 0x1e08); int* date = (int*)(t_gs + 0x1dd0);
        static void* session[0x70]; *(void***)(t_gs + 0x1e00) = session;    // single player: session[0x6c] == 0
        // a frame = session update (runs the day queued by the previous frame's timer), then the timer
        *speed = 4; *date = 43800000 + (1600 * 365 + 100) * 24;        // 1600.04.11
        allow_days = 100; queued_day = 1; t_session_frame(NULL); CHECK(100 - allow_days == 2);   // 1 queued + 2 extra
        *speed = 3; allow_days = 100; queued_day = 1; t_session_frame(NULL); CHECK(allow_days == 100);   // slower: no extra day
        *speed = 4; *date = 43800000 + (1600 * 365 + 29) * 24;         // 1600.01.30: .31, then 02.01 ends the frame
        allow_days = 100; queued_day = 1; t_session_frame(NULL); CHECK(100 - allow_days == 1);
        queued_day = 0; allow_days = 0; t_session_frame(NULL); CHECK(allow_days == 0);   // paused
        session[0x6c] = (void*)1; *date = 43800000 + (1600 * 365 + 100) * 24;
        allow_days = 100; queued_day = 1; t_session_frame(NULL); CHECK(allow_days == 100);  // multiplayer: no extra day
        session[0x6c] = NULL;
        printf("pacing: checked\n");
    }

    // 3g. save zip entries (parallel compression or the original path, per eu4boost.ini)
    if (argc > 3 && strcmp(argv[3], "savezip") == 0) {
        savezip_test();
        int (*tf)(void) = (int (*)(void))GetProcAddress(h, "eb_test_fmt");
        CHECK(tf != NULL);
        if (tf) { int d = tf(); printf("number formatting: %d differences\n", d); CHECK(d == 0); }
    }

    // 3h. modifier-container add (fast path vs the game's algorithm)
    if (argc > 3 && strcmp(argv[3], "modadd") == 0) modadd_test();

    // 3i. crash guards (freed tooltip sources, missing relation)
    if (argc > 3 && strcmp(argv[3], "guard") == 0) guard_test();

    // 4. multi-threaded churn (also gives the profiler something to sample)
    HANDLE th[8]; for (int i = 0; i < 8; i++) th[i] = CreateThread(NULL, 0, churn, (LPVOID)(uintptr_t)(i + 1), 0, NULL);
    Sleep(argc > 2 ? atoi(argv[2]) : 3000);
    stop = 1;
    long total = 0;
    for (int i = 0; i < 8; i++) { DWORD rc; WaitForSingleObject(th[i], INFINITE); GetExitCodeThread(th[i], &rc); total += rc; }
    printf("churn iterations: %ld\n", total);
    printf("%s (%d failures)\n", fails ? "TEST FAILED" : "ALL TESTS PASSED", fails);
    return fails ? 1 : 0;
}
