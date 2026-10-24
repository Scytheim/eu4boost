// Save list cache ([save] list_cache).
//
// The save dialog (FUN_141172e30), the load screen (FUN_14112dce0) and deleting a save build the list,
// and every item opens its save: FUN_1411bfbc0 -> FUN_141710440 (file stream on a PhysFS handle at
// +0x38, 128 KB buffer) -> FUN_1411bf350 -> FUN_140b28ee0, which reads the save's meta. A compressed
// save gives its "meta" entry (FUN_1405cdb80 -> FUN_1405cd120), which is read to its end. An
// uncompressed text save is opened again in text mode by the stream's own path (FUN_1417105d0) and read
// one character at a time (FUN_141710680) until the reader meets the key start_date or institutions.
// The reader (FUN_14170ada0) takes "key op value" statements and does not skip the value of a key it
// does not know, so after every "}" it loses the next token: in the user's 1444 save it missed the
// top-level start_date and institutions and stopped at the institutions of the first province, 3 MB in
// (measured: PHYSFS_tell at the close, 57 ms per save). The length of the file (PHYSFS_fileLength)
// goes into the item's text (SAVEITEM_FILESIZE).
// User reports: 895 saves, 4.9 of 5.6 s opening the files (report 4, ~5.4 ms each - an antivirus scan
// on open, by the samples); 397 saves (310 of them uncompressed text, 19 GB): 20-23 s every time the
// save dialog opened (report 20, the samples in opening and in reading).
//
// The cache keeps, per save (real path, size, last write time), a few KB the list can read instead:
// for a zip a minimal zip that holds only the original "meta" entry (its compressed bytes as they are);
// for a text save the start of the file to the end of the line of its first top-level start_date or
// institutions, followed by bare "institutions" lines that stop the reader whatever statement it is in.
// That is the same result only if the original read meets none of the keys that change the meta between
// that line and where it stopped: when an entry is made (right after the game's own read of the save,
// FUN_1411bf350 hooked, from the same handle, PHYSFS_tell = where it stopped) that stretch is searched
// for the names of those keys, quoted or not, and a save that has one is not cached. The entries are the
// files of one stored zip in memory, mounted into PhysFS (PHYSFS_mountMemory, mount point
// eb_savelist_cache). When the item's open (the call at 0x1411bfc11) asks for a save whose path, size and
// time match an entry, the stream opens the entry instead, the game's own code reads it, and the one
// PHYSFS_fileLength call that sizes the text (0x1411bfd5a) gets the save's real size. If the read of an
// entry fails, the entry is dropped and the stream reopens the save itself (its path set back, its end of
// file mark cleared, the zip streams cached in the item's reader freed) for a second read, so the item
// is not lost. Uncompressed
// binary saves, zips without a meta entry and saves whose read fails are not cached. The entries are kept
// in plugins\eu4boost_savelist.dat for the next launch; entries of saves that are gone or changed are
// dropped when it is written.
// Mode 2 (diagnostics) also logs every item after FUN_1411bfbc0: its texts by length and CRC, its flags,
// the files it opened and where it closed them (it takes that function's hook, so ui timing cannot time
// the items then).
#include "common.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "MinHook.h"

#define RVA_STREAM_OPEN 0x1710440u    // FUN_141710440(stream, std::string* path, mode, open mode)
#define RVA_ITEM_CALL   0x11bfc11u    // its call in FUN_1411bfbc0 (save list item)
#define RVA_LEN_CALL    0x11bfd5au    // FUN_1411bfbc0's PHYSFS_fileLength call: the size in the item's text
#define RVA_META        0x11bf350u    // FUN_1411bf350(item, reader streams, missing DLCs, missing mods) -> ok
#define RVA_ITEM        0x11bfbc0u    // FUN_1411bfbc0(item): diagnostics only
#define RVA_STR_ASSIGN  0x095110u     // std::string::assign(const char*, size_t)
static const uint8_t sig_open[24] = { 0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x48,0x89,0x4c,0x24,0x08,0x57,0x48,0x81,0xec,0xb0,0x00,0x00,0x00,0x41 };
static const uint8_t sig_meta[24] = { 0x48,0x89,0x5c,0x24,0x10,0x4c,0x89,0x4c,0x24,0x20,0x4c,0x89,0x44,0x24,0x18,0x48,0x89,0x4c,0x24,0x08,0x55,0x56,0x57,0x41 };
static const uint8_t sig_item[16] = { 0x48,0x89,0x4c,0x24,0x08,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57 };
static const uint8_t sig_assign[24] = { 0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x56,0x57,0x41,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0x69,0x18,0x49,0x8b };
#define S_PATH   0x18                 // stream: std::string with its path (FUN_141720700)
#define S_MODE   0x10                 // stream: 1 = text mode
#define S_EOF    0x40                 // stream: a read went past the end (cleared by its rewind FUN_141710b70, not by a reopen)
#define S_REOPEN 0x18                 // stream vtable: close and open its path again (FUN_1417105d0(s, mode, open mode))
#define I_PATH   0x260                // list item: std::string with the save's path (the open's path argument)
#define MOUNT_POINT "eb_savelist_cache"
#define MAX_ENTRIES 8192
#define MAX_BLOB    (16u << 20)
#define MAX_TEXT    (256u << 20)      // a text save read further than this by the game is not cached
enum { K_ZIP = 1, K_TEXT = 2 };

typedef void* (*stream_open_t)(void* s, void* path, uintptr_t mode, uint32_t open_mode);
typedef char (*meta_t)(uint8_t* item, void** st, void* dlcs, void* mods);
typedef int (*reopen_t)(void* s, int mode, int open_mode);
typedef void* (*str_assign_t)(void* s, const char* p, size_t n);
typedef int64_t (*fs_len_t)(void* h);
typedef int (*fs_close_t)(void* h);
typedef int (*fs_mount_t)(const void* buf, uint64_t len, void (*del)(void*), const char* dir, const char* mp, int append);
typedef int (*fs_unmount_t)(const char* dir);
typedef const char* (*fs_realdir_t)(const char* path);
typedef int (*fs_exists_t)(const char* path);
typedef int (*fs_seek_t)(void* h, uint64_t pos);
typedef int64_t (*fs_read_t)(void* h, void* buf, uint64_t len);
typedef int64_t (*fs_tell_t)(void* h);
typedef void* (*fs_openread_t)(const char* path);
typedef void (*item_t)(uint8_t* item);
static stream_open_t o_open;
static meta_t o_meta;
static fs_len_t o_len;
static fs_close_t o_close;
static fs_openread_t o_openread;
static item_t o_item;
static str_assign_t str_assign;
static fs_mount_t fs_mount;
static fs_unmount_t fs_unmount;
static fs_realdir_t fs_realdir;
static fs_exists_t fs_exists;
static fs_seek_t fs_seek;
static fs_read_t fs_read;
static fs_tell_t fs_tell;
static uintptr_t g_ret;               // return address of the item's open
static uintptr_t g_len_ret;           // ... and of its length for the text

typedef struct {
    wchar_t* path;
    uint32_t hash;
    uint64_t size, mtime;
    uint8_t kind;
    uint32_t id, n, crc;
    uint8_t* blob;
    int gen;                          // mount generation that holds it (0: not mounted yet)
} ent_t;
static ent_t* g_e;
static int g_ne, g_cap;
static uint32_t g_next_id = 1;
static CRITICAL_SECTION g_cs;
static int g_gen, g_disk_dirty, g_off, g_diag;
static char g_mount_name[48];
static uint8_t* g_img;

// the current item (main thread, one at a time): set by its open, used by its meta read and its length
static struct {
    uint8_t* item;
    int hit;                          // 1 = it opened an entry
    wchar_t path[1024];
    uint64_t size, mtime;
} g_cur;
static uint64_t g_len_size;           // != 0: the size the item's text gets (it read an entry)
static const char* g_last;            // "hit", "miss", ... for the diagnostics line
static LONG64 g_hits, g_misses, g_added, g_skipped, g_nokey, g_hit_fail;

// ---- CRC-32 ----
static uint32_t g_crc_tab[256];
static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; g_crc_tab[i] = c; }
}
static uint32_t crc32b(const uint8_t* p, size_t n) {
    uint32_t c = ~0u;
    while (n--) c = g_crc_tab[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}

static void put16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t* p, uint32_t v) { put16(p, v & 0xffff); put16(p + 2, v >> 16); }
static uint32_t get16(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t get32(const uint8_t* p) { return get16(p) | get16(p + 2) << 16; }

// ---- reading a save: a PhysFS handle or (tests) memory ----
typedef struct { void* h; const uint8_t* mem; uint64_t len; } src_t;
static int src_read(src_t* s, uint64_t off, void* buf, uint32_t n) {
    if (off > s->len || n > s->len - off) return 0;
    if (s->mem) { memcpy(buf, s->mem + off, n); return 1; }
    return fs_seek(s->h, off) && fs_read(s->h, buf, n) == (int64_t)n;
}

// zip: a new zip with only the "meta" entry, its bytes as they are (method, CRC, sizes kept)
static uint8_t* zip_meta(src_t* s, uint32_t* out_n) {
    uint8_t tail[65536 + 22];
    uint32_t tn = s->len < sizeof tail ? (uint32_t)s->len : (uint32_t)sizeof tail;
    if (tn < 22 || !src_read(s, s->len - tn, tail, tn)) return NULL;
    int e = -1;
    for (int i = (int)tn - 22; i >= 0; i--) if (get32(tail + i) == 0x06054b50) { e = i; break; }
    if (e < 0) return NULL;
    uint32_t cd_n = get16(tail + e + 10), cd_size = get32(tail + e + 12), cd_off = get32(tail + e + 16);
    if (cd_size > (8u << 20) || (uint64_t)cd_off + cd_size > s->len) return NULL;
    uint8_t* cd = malloc(cd_size ? cd_size : 1);
    if (!cd) return NULL;
    uint8_t* out = NULL;
    if (!src_read(s, cd_off, cd, cd_size)) goto done;
    for (uint32_t p = 0, k = 0; k < cd_n && p + 46 <= cd_size; k++) {
        const uint8_t* c = cd + p;
        if (get32(c) != 0x02014b50) break;
        uint32_t nl = get16(c + 28), xl = get16(c + 30), cl = get16(c + 32);
        if (p + 46 + nl > cd_size) break;
        if (nl == 4 && !memcmp(c + 46, "meta", 4)) {
            uint32_t flags = get16(c + 8) & ~8u, method = get16(c + 10), crc = get32(c + 16);
            uint32_t csize = get32(c + 20), usize = get32(c + 24), lho = get32(c + 42);
            uint8_t lh[30];
            if (csize > MAX_BLOB || usize == 0xffffffffu || !src_read(s, lho, lh, 30) || get32(lh) != 0x04034b50) break;
            uint64_t data = (uint64_t)lho + 30 + get16(lh + 26) + get16(lh + 28);
            uint32_t n = 30 + 4 + csize + 46 + 4 + 22;
            out = malloc(n);
            if (!out) break;
            uint8_t* q = out;
            put32(q, 0x04034b50); put16(q + 4, 20); put16(q + 6, flags); put16(q + 8, method);
            memcpy(q + 10, c + 12, 4);                                   // time, date
            put32(q + 14, crc); put32(q + 18, csize); put32(q + 22, usize); put16(q + 26, 4); put16(q + 28, 0);
            memcpy(q + 30, "meta", 4);
            if (!src_read(s, data, q + 34, csize)) { free(out); out = NULL; break; }
            q += 34 + csize;
            put32(q, 0x02014b50); put16(q + 4, 20); put16(q + 6, 20); put16(q + 8, flags); put16(q + 10, method);
            memcpy(q + 12, c + 12, 4);
            put32(q + 16, crc); put32(q + 20, csize); put32(q + 24, usize); put16(q + 28, 4);
            memset(q + 30, 0, 12); put32(q + 42, 0);
            memcpy(q + 46, "meta", 4);
            q += 50;
            put32(q, 0x06054b50); put16(q + 4, 0); put16(q + 6, 0); put16(q + 8, 1); put16(q + 10, 1);
            put32(q + 12, 50); put32(q + 16, 34 + csize); put16(q + 20, 0);
            *out_n = n;
            break;
        }
        p += 46 + nl + xl + cl;
    }
done:
    free(cd);
    return out;
}

// text: the end of the line of the first top-level start_date or institutions (quoted strings and
// nested blocks skipped); 0 = none in [0, n) or the line goes on past n
static int key_at(const uint8_t* p, size_t n, size_t i, const char* k) {
    size_t kl = strlen(k);
    if (i + kl > n || memcmp(p + i, k, kl)) return 0;
    if (i && !(p[i - 1] == '\n' || p[i - 1] == '\r' || p[i - 1] == ' ' || p[i - 1] == '\t' || p[i - 1] == '}' || p[i - 1] == '{')) return 0;
    size_t j = i + kl;
    while (j < n && (p[j] == ' ' || p[j] == '\t')) j++;
    return j < n && p[j] == '=';
}
static size_t text_cut(const uint8_t* p, size_t n) {
    int depth = 0, inq = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (inq) { if (c == '"') inq = 0; continue; }
        if (c == '"') inq = 1;
        else if (c == '{') depth++;
        else if (c == '}') { if (depth) depth--; }
        else if (!depth && (c == 's' || c == 'i') && (key_at(p, n, i, "start_date") || key_at(p, n, i, "institutions"))) {
            for (size_t j = i; j < n; j++) if (p[j] == '\n') return j + 1;
            return 0;
        }
    }
    return 0;
}
// the keys FUN_140b28ee0 stores into the item's meta (start_date and institutions stop it; flags,
// id_counters and save_game are read into temporaries; date counts only the first time)
static const char* const g_meta_keys[] = {
    "checksum", "player", "gameplaysettings", "dlc_enabled", "mod_enabled", "ironman", "is_ironman", "local_ironman",
    "multi_player", "not_observer", "colonial_father", "colonial_flag_color", "is_random_new_world", "custom_country_name",
    "displayed_country_name", "country_colors", "savegame_versions", "savegame_version", "no_edit_save", "co_host",
    "mods_enabled_names",
};
static int word_char(uint8_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == ':' || c == '@' || c >= 0x80; }
// one of those names as a whole word anywhere in [from, to) (quoted or not)
static int meta_key_in(const uint8_t* p, size_t from, size_t to) {
    for (size_t i = from; i < to; i++) {
        uint8_t c = p[i];
        if (!(c >= 'c' && c <= 'p') || (i > from && word_char(p[i - 1])) || (i == from && i && word_char(p[i - 1]))) continue;
        for (size_t k = 0; k < sizeof g_meta_keys / sizeof g_meta_keys[0]; k++) {
            size_t kl = strlen(g_meta_keys[k]);
            if (i + kl <= to && !memcmp(p + i, g_meta_keys[k], kl) && (i + kl == to || !word_char(p[i + kl]))) return 1;
        }
    }
    return 0;
}
static const char g_tail[] = "\ninstitutions\ninstitutions\ninstitutions\ninstitutions\n";
// a text save's entry from its first `stop` bytes (the game's read stopped there): the bytes to the
// stop line, then the tail; NULL = not cacheable
static uint8_t* text_entry(const uint8_t* p, size_t n, size_t stop, uint32_t* out_n) {
    if (!stop || stop > n) return NULL;
    size_t cut = text_cut(p, n);
    size_t at_stop = stop;                                    // the end of the line the game stopped in
    while (at_stop < n && p[at_stop - 1] != '\n') at_stop++;
    if (!cut || at_stop < cut) cut = at_stop;
    if (cut < stop && meta_key_in(p, cut, stop)) return NULL;
    if (cut + sizeof g_tail > MAX_BLOB) return NULL;
    uint8_t* b = malloc(cut + sizeof g_tail - 1);
    if (!b) return NULL;
    memcpy(b, p, cut);
    memcpy(b + cut, g_tail, sizeof g_tail - 1);
    *out_n = (uint32_t)(cut + sizeof g_tail - 1);
    return b;
}

// ---- entries ----
static uint32_t path_hash(const wchar_t* p) { uint32_t h = 2166136261u; while (*p) h = (h ^ *p++) * 16777619u; return h; }
static ent_t* find(const wchar_t* path) {
    uint32_t h = path_hash(path);
    for (int i = 0; i < g_ne; i++) if (g_e[i].hash == h && !wcscmp(g_e[i].path, path)) return &g_e[i];
    return NULL;
}
static int add(const wchar_t* path, uint64_t size, uint64_t mtime, uint8_t kind, uint8_t* blob, uint32_t n) {
    ent_t* e = find(path);
    if (e) { free(e->blob); }
    else {
        if (g_ne >= MAX_ENTRIES) { free(blob); return 0; }
        if (g_ne == g_cap) {
            int nc = g_cap ? g_cap * 2 : 256;
            ent_t* ne = realloc(g_e, (size_t)nc * sizeof *ne);
            if (!ne) { free(blob); return 0; }
            g_e = ne; g_cap = nc;
        }
        e = &g_e[g_ne];
        e->path = _wcsdup(path);
        if (!e->path) { free(blob); return 0; }
        e->hash = path_hash(path);
        g_ne++;
    }
    e->size = size; e->mtime = mtime; e->kind = kind; e->blob = blob; e->n = n;
    e->crc = crc32b(blob, n); e->id = g_next_id++; e->gen = 0;
    return 1;
}

// all entries as one stored zip, mounted at MOUNT_POINT (the previous one unmounted first)
static int remount(void) {
    size_t total = 22;
    char nm[16];
    for (int i = 0; i < g_ne; i++) total += 30 + 46 + 2 * (size_t)sprintf(nm, "%u.eu4", g_e[i].id) + g_e[i].n;
    if (total > 0xffffffffu) return 0;
    uint8_t* img = malloc(total);
    if (!img) return 0;
    uint8_t* q = img;
    uint32_t* lho = malloc((size_t)(g_ne ? g_ne : 1) * 4);
    if (!lho) { free(img); return 0; }
    for (int i = 0; i < g_ne; i++) {
        ent_t* e = &g_e[i];
        int nl = sprintf(nm, "%u.eu4", e->id);
        lho[i] = (uint32_t)(q - img);
        put32(q, 0x04034b50); put16(q + 4, 10); put16(q + 6, 0); put16(q + 8, 0); put32(q + 10, 0);
        put32(q + 14, e->crc); put32(q + 18, e->n); put32(q + 22, e->n); put16(q + 26, nl); put16(q + 28, 0);
        memcpy(q + 30, nm, nl); memcpy(q + 30 + nl, e->blob, e->n);
        q += 30 + nl + e->n;
    }
    uint32_t cd = (uint32_t)(q - img);
    for (int i = 0; i < g_ne; i++) {
        ent_t* e = &g_e[i];
        int nl = sprintf(nm, "%u.eu4", e->id);
        put32(q, 0x02014b50); put16(q + 4, 10); put16(q + 6, 10); put16(q + 8, 0); put16(q + 10, 0); put32(q + 12, 0);
        put32(q + 16, e->crc); put32(q + 20, e->n); put32(q + 24, e->n); put16(q + 28, nl);
        memset(q + 30, 0, 12); put32(q + 42, lho[i]);
        memcpy(q + 46, nm, nl);
        q += 46 + nl;
    }
    free(lho);
    put32(q, 0x06054b50); put16(q + 4, 0); put16(q + 6, 0); put16(q + 8, g_ne > 0xffff ? 0xffff : g_ne); put16(q + 10, g_ne > 0xffff ? 0xffff : g_ne);
    put32(q + 12, (uint32_t)(q - img) - cd); put32(q + 16, cd); put16(q + 20, 0);
    if (g_mount_name[0]) {
        if (!fs_unmount(g_mount_name)) { free(img); return 0; }   // a file of it still open: keep the old one
        g_mount_name[0] = 0;
        free(g_img); g_img = NULL;
    }
    char name[48];
    snprintf(name, sizeof name, "eb_savelist_%d.zip", g_gen + 1);
    if (!fs_mount(img, total, NULL, name, MOUNT_POINT, 1)) {
        eb_log("save: save list cache: mounting the cache failed - off");
        free(img); g_off = 1;
        return 0;
    }
    g_gen++;
    memcpy(g_mount_name, name, sizeof name);
    g_img = img;
    for (int i = 0; i < g_ne; i++) g_e[i].gen = g_gen;
    return 1;
}

// ---- the cache file (plugins\eu4boost_savelist.dat) ----
static void cache_path(wchar_t* out, size_t n) { _snwprintf(out, n, L"%seu4boost_savelist.dat", g_dir); out[n - 1] = 0; }
static int still_same(const ent_t* e) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(e->path, GetFileExInfoStandard, &a)) return 0;
    return ((uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow) == e->size &&
           ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime) == e->mtime;
}
static void cache_write(void) {
    wchar_t p[MAX_PATH + 16], t[MAX_PATH + 24];
    cache_path(p, MAX_PATH + 16);
    _snwprintf(t, MAX_PATH + 24, L"%s.tmp", p); t[MAX_PATH + 23] = 0;
    FILE* f = _wfopen(t, L"wb");
    if (!f) return;
    int keep = 0;
    for (int i = 0; i < g_ne; i++) keep += still_same(&g_e[i]);
    uint32_t hdr[3] = { 0x4c534245u /* "EBSL" */, 3, (uint32_t)keep };
    int ok = fwrite(hdr, sizeof hdr, 1, f) == 1;
    for (int i = 0; ok && i < g_ne; i++) {
        ent_t* e = &g_e[i];
        if (!still_same(e)) continue;
        uint32_t pl = (uint32_t)(wcslen(e->path) * sizeof(wchar_t));
        ok = fwrite(&pl, 4, 1, f) == 1 && fwrite(e->path, pl, 1, f) == 1 && fwrite(&e->size, 8, 1, f) == 1 &&
             fwrite(&e->mtime, 8, 1, f) == 1 && fwrite(&e->kind, 1, 1, f) == 1 && fwrite(&e->n, 4, 1, f) == 1 &&
             fwrite(e->blob, e->n, 1, f) == 1;
    }
    ok = fclose(f) == 0 && ok;
    if (ok && MoveFileExW(t, p, MOVEFILE_REPLACE_EXISTING)) g_disk_dirty = 0;
    else DeleteFileW(t);
}
static void cache_read(void) {
    wchar_t p[MAX_PATH + 16];
    cache_path(p, MAX_PATH + 16);
    FILE* f = _wfopen(p, L"rb");
    if (!f) return;
    uint32_t hdr[3];
    if (fread(hdr, sizeof hdr, 1, f) == 1 && hdr[0] == 0x4c534245u && hdr[1] == 3 && hdr[2] <= MAX_ENTRIES) {
        for (uint32_t i = 0; i < hdr[2]; i++) {
            uint32_t pl, n; uint64_t size, mtime; uint8_t kind;
            if (fread(&pl, 4, 1, f) != 1 || pl == 0 || pl > 4096 || (pl & 1)) break;
            wchar_t* path = calloc(pl / 2 + 1, sizeof(wchar_t));
            if (!path) break;
            if (fread(path, pl, 1, f) != 1 || fread(&size, 8, 1, f) != 1 || fread(&mtime, 8, 1, f) != 1 ||
                fread(&kind, 1, 1, f) != 1 || fread(&n, 4, 1, f) != 1 || (kind != K_ZIP && kind != K_TEXT) || !n || n > MAX_BLOB) {
                free(path); break;
            }
            uint8_t* blob = malloc(n);
            if (!blob || fread(blob, n, 1, f) != 1) { free(blob); free(path); break; }
            add(path, size, mtime, kind, blob, n);
            free(path);
        }
    }
    fclose(f);
}

// ---- hooks ----
// the save's real path, size and time from its PhysFS path (no open)
static int key_of(const char* vfs, wchar_t* path, size_t cap, uint64_t* size, uint64_t* mtime) {
    const char* dir = fs_realdir(vfs);
    if (!dir) return 0;
    char full[2048];
    size_t dl = strlen(dir);
    int sep = dl && (dir[dl - 1] == '\\' || dir[dl - 1] == '/');
    if (snprintf(full, sizeof full, "%s%s%s", dir, sep ? "" : "\\", vfs) >= (int)sizeof full) return 0;
    for (char* c = full; *c; c++) if (*c == '/') *c = '\\';
    if (!MultiByteToWideChar(CP_UTF8, 0, full, -1, path, (int)cap)) return 0;
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    *size = (uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow;
    *mtime = (uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
    return 1;
}
static const char* str_c(const void* s) { return ((const uint64_t*)s)[3] > 15 ? *(const char* const*)s : (const char*)s; }
static void* handle_of(void* stream) {
    void** w = ((void***)stream)[7];
    return w ? *w : NULL;
}

static void* hk_open(void* s, void* path, uintptr_t mode, uint32_t open_mode) {
    if (__builtin_return_address(0) != (void*)g_ret || g_off) return o_open(s, path, mode, open_mode);
    g_cur.item = (uint8_t*)path - I_PATH;
    g_cur.hit = 0; g_len_size = 0; g_last = "uncached";
    if (!key_of(str_c(path), g_cur.path, 1024, &g_cur.size, &g_cur.mtime)) {
        g_cur.item = NULL; g_nokey++; g_last = "no key";
        return o_open(s, path, mode, open_mode);
    }
    EnterCriticalSection(&g_cs);
    ent_t* e = find(g_cur.path);
    if (e && e->size == g_cur.size && e->mtime == g_cur.mtime) {
        if (e->gen != g_gen || !g_mount_name[0]) {
            if (remount() && g_disk_dirty) cache_write();
        }
        char name[64];
        snprintf(name, sizeof name, MOUNT_POINT "/%u.eu4", e->id);
        if (e->gen == g_gen && g_mount_name[0] && fs_exists(name)) {
            LeaveCriticalSection(&g_cs);
            // a std::string (MSVC layout, heap form) with the entry's path
            uint64_t fake[4] = { (uint64_t)(uintptr_t)name, 0, strlen(name), 63 };
            g_cur.hit = 1; g_hits++; g_last = "hit";
            return o_open(s, fake, mode, open_mode);          // (the game throws if a file cannot be opened)
        }
    }
    LeaveCriticalSection(&g_cs);
    g_misses++; g_last = "miss";
    return o_open(s, path, mode, open_mode);
}

// the entry's bytes from the game's handle on the save, right after the game read it
static void make_entry(void* stream, int zip) {
    void* h = handle_of(stream);
    if (!h) return;
    int64_t pos = fs_tell(h), len = o_len(h);
    if (len != (int64_t)g_cur.size || pos < 0) { g_skipped++; g_last = "miss, changed while read"; return; }
    src_t s = { h, NULL, (uint64_t)len };
    uint8_t* blob = NULL; uint32_t n = 0;
    uint8_t head[6];
    if (zip) blob = zip_meta(&s, &n);
    else if (src_read(&s, 0, head, 6) && !memcmp(head, "EU4txt", 6) && (uint64_t)pos <= MAX_TEXT) {
        // the game's read stopped at pos; its line and the stop line before it are needed
        size_t want = (size_t)pos + 4096 < (size_t)len ? (size_t)pos + 4096 : (size_t)len;
        uint8_t* p = malloc(want ? want : 1);
        if (p && src_read(&s, 0, p, (uint32_t)want)) blob = text_entry(p, want, (size_t)pos, &n);
        free(p);
    }
    fs_seek(h, (uint64_t)pos);
    if (!blob) { g_skipped++; g_last = "miss, not cacheable"; return; }
    EnterCriticalSection(&g_cs);
    if (add(g_cur.path, g_cur.size, g_cur.mtime, zip ? K_ZIP : K_TEXT, blob, n)) { g_added++; g_disk_dirty = 1; }
    LeaveCriticalSection(&g_cs);
}

// a failed read of an entry: drop it, point the stream at the save again and read that
static char reread(uint8_t* item, void** st, void* dlcs, void* mods) {
    uint8_t* s = st[0];
    EnterCriticalSection(&g_cs);
    ent_t* e = find(g_cur.path);
    if (e) { e->size = ~0ull; g_disk_dirty = 1; }             // matches no save: replaced on its next read
    LeaveCriticalSection(&g_cs);
    if (InterlockedIncrement64(&g_hit_fail) <= 10) eb_log("save: save list cache: the cached meta of %ls could not be read - read from the save", g_cur.path);
    g_cur.hit = 0; g_last = "hit failed, read again";
    for (int i = 1; i <= 4; i++) if (st[i]) { (*(void (**)(void*, int))*(void***)st[i])(st[i], 1); st[i] = NULL; }
    const uint8_t* ip = item + I_PATH;
    if (!str_assign(s + S_PATH, str_c(ip), ((const uint64_t*)ip)[2])) return 0;
    if (!((reopen_t)(*(void***)s)[S_REOPEN / 8])(s, *(int*)(s + S_MODE), 0)) return 0;
    s[S_EOF] = 0;                                            // the failed read of a text entry ended past its end
    return o_meta(item, st, dlcs, mods);
}

static char hk_meta(uint8_t* item, void** st, void* dlcs, void* mods) {
    char r = o_meta(item, st, dlcs, mods);
    if (item != g_cur.item || GetCurrentThreadId() != g_main_tid) return r;
    if (g_cur.hit) { if (!r) r = reread(item, st, dlcs, mods); }
    else if (r) make_entry(st[0], *(uint8_t*)(st + 5));
    g_len_size = g_cur.hit ? g_cur.size : 0;
    g_cur.item = NULL;
    return r;
}

static int64_t hk_len(void* h) {
    if ((uintptr_t)__builtin_return_address(0) == g_len_ret && g_len_size) {
        int64_t n = (int64_t)g_len_size;
        g_len_size = 0;
        return n;
    }
    return o_len(h);
}

// ---- diagnostics: every item after it is built ----
static int g_in_item;
static char g_reads[256];             // the files the item opened and how far it read them
static void* hk_openread(const char* path) {
    void* h = o_openread(path);
    if (g_in_item && GetCurrentThreadId() == g_main_tid) {
        size_t k = strlen(g_reads);
        snprintf(g_reads + k, sizeof g_reads - k, " open %s%s;", strncmp(path, MOUNT_POINT, sizeof MOUNT_POINT - 1) ? "save" : "cache", h ? "" : " FAILED");
    }
    return h;
}
static int hk_close(void* h) {
    if (h && g_in_item && GetCurrentThreadId() == g_main_tid) {
        size_t k = strlen(g_reads);
        snprintf(g_reads + k, sizeof g_reads - k, " close at %lld of %lld;", (long long)fs_tell(h), (long long)o_len(h));
    }
    return o_close(h);
}
static void log_str(char* out, size_t cap, const uint8_t* s) {
    const char* p = str_c(s); uint64_t n = ((const uint64_t*)s)[2];
    size_t k = 0;
    for (uint64_t i = 0; i < n && k + 4 < cap; i++) {
        uint8_t c = (uint8_t)p[i];
        if (c >= 0x20 && c < 0x7f) out[k++] = (char)c;
        else k += (size_t)snprintf(out + k, cap - k, "\\x%02x", c);
    }
    out[k < cap ? k : cap - 1] = 0;
}
static uint32_t str_crc(const uint8_t* s) { return crc32b((const uint8_t*)str_c(s), (size_t)((const uint64_t*)s)[2]); }
static void hk_item(uint8_t* item) {
    LARGE_INTEGER t0, t1, f;
    g_reads[0] = 0; g_in_item = 1;
    QueryPerformanceCounter(&t0);
    o_item(item);
    QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&f);
    g_in_item = 0;
    char a[200];
    log_str(a, sizeof a, item + 0x50);
    // the three texts by length and CRC (the full texts make lines longer than the log takes), the flags
    eb_log("save list item %s (%s, %.2f ms): text %u/%08x tooltip %u/%08x expl %u/%08x flags %d %d %d %d |%s | \"%s\"", str_c(item + I_PATH), g_last,
           (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart,
           (unsigned)((uint64_t*)(item + 0x50))[2], str_crc(item + 0x50), (unsigned)((uint64_t*)(item + 0x2c8))[2], str_crc(item + 0x2c8),
           (unsigned)((uint64_t*)(item + 0x2e8))[2], str_crc(item + 0x2e8),
           item[0x318], item[0xa8 + 0x100], item[0xa8 + 0x48], item[0xa8 + 0x49], g_reads, a);
}

int eb_savelist_install(void) {
    if (!g_cfg.save_list_cache) return 0;
#ifdef EB_TEST
    crc_init();
    return 0;
#else
    uint8_t* b = (uint8_t*)g_exe;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != 0x66FE6AB9u) { eb_log("save: unknown eu4.exe - save list cache off"); return 0; }
    void* len = (void*)GetProcAddress(g_exe, "PHYSFS_fileLength");
    uint8_t *call = b + RVA_ITEM_CALL, *lcall = b + RVA_LEN_CALL;
    if (memcmp(b + RVA_STREAM_OPEN, sig_open, sizeof sig_open) || memcmp(b + RVA_META, sig_meta, sizeof sig_meta) ||
        memcmp(b + RVA_STR_ASSIGN, sig_assign, sizeof sig_assign) ||
        call[0] != 0xe8 || call + 5 + *(int32_t*)(call + 1) != b + RVA_STREAM_OPEN ||
        !len || lcall[0] != 0xe8 || lcall + 5 + *(int32_t*)(lcall + 1) != (uint8_t*)len) {
        eb_log("save: save list cache: signature mismatch - off"); return 0;
    }
    str_assign = (str_assign_t)(b + RVA_STR_ASSIGN);
    fs_mount = (fs_mount_t)(void*)GetProcAddress(g_exe, "PHYSFS_mountMemory");
    fs_unmount = (fs_unmount_t)(void*)GetProcAddress(g_exe, "PHYSFS_unmount");
    fs_realdir = (fs_realdir_t)(void*)GetProcAddress(g_exe, "PHYSFS_getRealDir");
    fs_exists = (fs_exists_t)(void*)GetProcAddress(g_exe, "PHYSFS_exists");
    fs_seek = (fs_seek_t)(void*)GetProcAddress(g_exe, "PHYSFS_seek");
    fs_read = (fs_read_t)(void*)GetProcAddress(g_exe, "PHYSFS_readBytes");
    fs_tell = (fs_tell_t)(void*)GetProcAddress(g_exe, "PHYSFS_tell");
    if (!fs_mount || !fs_unmount || !fs_realdir || !fs_exists || !fs_seek || !fs_read || !fs_tell) {
        eb_log("save: save list cache: PhysFS functions not found - off"); return 0;
    }
    crc_init();
    InitializeCriticalSection(&g_cs);
    g_ret = (uintptr_t)(call + 5);
    g_len_ret = (uintptr_t)(lcall + 5);
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) { eb_log("save: MinHook init failed"); return 0; }
    if (MH_CreateHook(len, (void*)hk_len, (void**)&o_len) != MH_OK || MH_CreateHook(b + RVA_META, (void*)hk_meta, (void**)&o_meta) != MH_OK ||
        MH_CreateHook(b + RVA_STREAM_OPEN, (void*)hk_open, (void**)&o_open) != MH_OK ||
        MH_EnableHook(len) != MH_OK || MH_EnableHook(b + RVA_META) != MH_OK || MH_EnableHook(b + RVA_STREAM_OPEN) != MH_OK) {
        MH_DisableHook(b + RVA_STREAM_OPEN); MH_DisableHook(b + RVA_META); MH_DisableHook(len);
        eb_log("save: save list cache: hook failed - off"); return 0;
    }
    void* ord = (void*)GetProcAddress(g_exe, "PHYSFS_openRead");
    void* cls = (void*)GetProcAddress(g_exe, "PHYSFS_close");
    if (g_cfg.save_list_cache == 2 && ord && cls && !memcmp(b + RVA_ITEM, sig_item, sizeof sig_item) &&
        MH_CreateHook(b + RVA_ITEM, (void*)hk_item, (void**)&o_item) == MH_OK && MH_EnableHook(b + RVA_ITEM) == MH_OK &&
        MH_CreateHook(ord, (void*)hk_openread, (void**)&o_openread) == MH_OK && MH_EnableHook(ord) == MH_OK &&
        MH_CreateHook(cls, (void*)hk_close, (void**)&o_close) == MH_OK && MH_EnableHook(cls) == MH_OK) g_diag = 1;
    cache_read();
    eb_log("save: save list cache on - unchanged saves are listed from %d cached entries without reading them%s", g_ne,
           g_diag ? " (DIAGNOSTICS: every item logged)" : "");
    return 1;
#endif
}

void eb_savelist_report(void) {
    if (!g_cfg.save_list_cache || !o_open) return;
    if (g_disk_dirty) cache_write();
    eb_log("save: save list cache: %lld items from the cache (%lld read again from the save), %lld read from the save (%lld entries added, %lld not cacheable), %lld without a path; %d entries",
           (long long)g_hits, (long long)g_hit_fail, (long long)g_misses, (long long)g_added, (long long)g_skipped, (long long)g_nokey, g_ne);
}

#ifdef EB_TEST
// self-test of the parts that do not need the game: the stop line, the text entry and its check of the
// stretch the game read past it, the meta zip; returns the number of wrong answers
static int text_is(const char* t, size_t stop, const char* want_prefix_end) {
    uint32_t n = 0;
    uint8_t* b = text_entry((const uint8_t*)t, strlen(t), stop, &n);
    if (!want_prefix_end) { free(b); return b == NULL; }
    size_t cut = (size_t)(want_prefix_end - t);
    int ok = b && n == cut + sizeof g_tail - 1 && !memcmp(b, t, cut) && !memcmp(b + cut, g_tail, sizeof g_tail - 1);
    free(b);
    return ok;
}
__declspec(dllexport) int eb_test_savelist(void) {
    int bad = 0;
    crc_init();
    static const char t1[] = "EU4txt\ndate=1600.1.1\nsave_game=\"x { start_date=1 }\"\nflags={\n\tstart_date=1444.1.1\n\tx={ institutions=3 }\n}\nspeed=4\nstart_date=1444.11.11\nprovinces={\n}\n";
    size_t c1 = text_cut((const uint8_t*)t1, sizeof t1 - 1);
    const char* want = strstr(t1, "\nstart_date=1444.11.11\n") + 23;
    bad += c1 != (size_t)(want - t1);
    bad += text_cut((const uint8_t*)t1, (size_t)(want - t1) - 1) != 0;               // the line not complete yet
    static const char t2[] = "EU4txt\ndate=1.1.1\nmy_start_date=3\ninstitutions_x=4\nx=\"institutions=\"\ninstitutions = {\n\t1 2\n}\n";
    size_t c2 = text_cut((const uint8_t*)t2, sizeof t2 - 1);
    bad += c2 != (size_t)(strstr(t2, "institutions = {\n") + 17 - t2);
    static const char t3[] = "EU4txt\ndate=1.1.1\nprovinces={\n}\n";
    bad += text_cut((const uint8_t*)t3, sizeof t3 - 1) != 0;
    // the game read past the stop line (as in the 1444 save: to the first province's institutions)
    static const char t4[] = "EU4txt\ndate=1444.11.11\nplayer=\"KAR\"\nflags={\n\tx=1\n}\nstart_date=1444.11.11\ninstitutions={ 1 0 }\n"
                             "rebel_faction={\n\tid=3\n\tcountry=\"SWE\"\n}\nprovinces={\n-1={\n\t\tflags={ a=1 }\n\t\tinstitutions={\n\t\t\t100.000\n\t\t}\n\t}\n}\n";
    const char* s4 = strstr(t4, "\t\tinstitutions={") + 16;                          // where the game's read stopped
    const char* e4 = strstr(t4, "start_date=1444.11.11\n") + 22;
    bad += !text_is(t4, (size_t)(s4 - t4), e4);
    // it stopped before the stop line: the line it stopped in, then the tail
    bad += !text_is(t4, (size_t)(strstr(t4, "flags={") + 3 - t4), strstr(t4, "flags={\n") + 8);
    // a key that changes the meta in the stretch: not cacheable (quoted too); a longer name is fine
    static const char t5[] = "EU4txt\ndate=1.1.1\nstart_date=1.1.1\nx={\n\tplayer=\"SWE\"\n}\nprovinces={\n-1={ institutions={ 1 } }\n}\n";
    bad += !text_is(t5, strstr(t5, "institutions={ 1") + 14 - t5, NULL);
    static const char t6[] = "EU4txt\ndate=1.1.1\nstart_date=1.1.1\nx={ \"is_ironman\" }\nprovinces={\n-1={ institutions={ 1 } }\n}\n";
    bad += !text_is(t6, strstr(t6, "institutions={ 1") + 14 - t6, NULL);
    static const char t7[] = "EU4txt\ndate=1.1.1\nstart_date=1.1.1\nx={ player_x=1 my_checksum=2 ironman.y=3 }\nprovinces={\n-1={ institutions={ 1 } }\n}\n";
    bad += !text_is(t7, strstr(t7, "institutions={ 1") + 14 - t7, strstr(t7, "start_date=1.1.1\n") + 17);
    bad += !text_is(t4, 0, NULL);                                                       // nothing read
    // a zip with entries gamestate, meta (deflated), ai, a comment and a data descriptor flag
    uint8_t z[1024]; uint8_t* q = z;
    const char* names[3] = { "gamestate", "meta", "ai" };
    const uint8_t md[] = { 0x01, 0x05, 0x00, 0xfa, 0xff, 'E', 'U', '4', 't', 'x' };     // stored deflate block "EU4tx"
    uint32_t off[3], dlen[3] = { 7, sizeof md, 3 };
    for (int i = 0; i < 3; i++) {
        uint32_t nl = (uint32_t)strlen(names[i]);
        off[i] = (uint32_t)(q - z);
        put32(q, 0x04034b50); put16(q + 4, 20); put16(q + 6, 8); put16(q + 8, i == 1 ? 8 : 0); put32(q + 10, 0x12345678);
        put32(q + 14, 0); put32(q + 18, 0); put32(q + 22, 0); put16(q + 26, nl); put16(q + 28, 2);
        memcpy(q + 30, names[i], nl); q[30 + nl] = 0xaa; q[31 + nl] = 0xbb;
        q += 32 + nl;
        if (i == 1) memcpy(q, md, sizeof md); else memset(q, 'G' + i, dlen[i]);
        q += dlen[i];
    }
    uint32_t cd = (uint32_t)(q - z);
    for (int i = 0; i < 3; i++) {
        uint32_t nl = (uint32_t)strlen(names[i]);
        put32(q, 0x02014b50); put16(q + 4, 20); put16(q + 6, 20); put16(q + 8, 8); put16(q + 10, i == 1 ? 8 : 0); put32(q + 12, 0x12345678);
        put32(q + 16, 0xdeadbeef + i); put32(q + 20, dlen[i]); put32(q + 24, i == 1 ? 5 : dlen[i]); put16(q + 28, nl); put16(q + 30, 0); put16(q + 32, 0);
        memset(q + 34, 0, 8); put32(q + 42, off[i]); memcpy(q + 46, names[i], nl);
        q += 46 + nl;
    }
    put32(q, 0x06054b50); put16(q + 4, 0); put16(q + 6, 0); put16(q + 8, 3); put16(q + 10, 3);
    put32(q + 12, (uint32_t)(q - z) - cd); put32(q + 16, cd); put16(q + 20, 3); memcpy(q + 22, "abc", 3);
    q += 25;
    src_t sz = { NULL, z, (uint64_t)(q - z) };
    uint32_t n = 0;
    uint8_t* blob = zip_meta(&sz, &n);
    bad += blob == NULL;
    if (blob) {
        // the new zip: one local entry "meta" (data as it was), its directory entry, the end record
        bad += get32(blob) != 0x04034b50 || get16(blob + 8) != 8 || get32(blob + 14) != 0xdeadbeef + 1 || get32(blob + 18) != sizeof md ||
               get32(blob + 22) != 5 || get16(blob + 26) != 4 || memcmp(blob + 30, "meta", 4) || memcmp(blob + 34, md, sizeof md) ||
               (get16(blob + 6) & 8);
        const uint8_t* c = blob + 34 + sizeof md;
        bad += get32(c) != 0x02014b50 || get32(c + 16) != 0xdeadbeef + 1 || get32(c + 42) != 0 || memcmp(c + 46, "meta", 4);
        const uint8_t* e = c + 50;
        bad += get32(e) != 0x06054b50 || get16(e + 10) != 1 || get32(e + 12) != 50 || get32(e + 16) != 34 + sizeof md || e + 22 != blob + n;
        free(blob);
    }
    sz.len -= 30;                                                                       // the end record cut off
    blob = zip_meta(&sz, &n);
    bad += blob != NULL;
    free(blob);
    bad += crc32b((const uint8_t*)"123456789", 9) != 0xCBF43926u;
    eb_log("TEST savelist: %d wrong answers", bad);
    return bad;
}
#endif
