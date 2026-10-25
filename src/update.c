// Auto-update ([update] auto=1, on by default): a newer release is downloaded in the background and used
// from the next launch.
//
// 20 s after the game starts, a thread asks GitHub for the latest release of Scytheim/eu4boost (one
// HTTPS request; [update] url= points elsewhere for tests). If its version is newer than this DLL's, it
// downloads the release's only file, eu4boost_v<version>.zip, in which tools/release.sh stores
// plugins/eu4boost.dll uncompressed (so it is read without inflating) and puts the signature in the
// archive comment (not a file, so unpacking the zip shows nothing extra). It installs the DLL only when
//   - the signature is valid: ECDSA P-256 over "eu4boost-update\n<version>\n<sha256 of the dll>\n" with
//     the key below (tools/sign.c makes it at release time; the private key never leaves the developer's
//     PC), and the signed version is the release's version,
//   - the file is a 64-bit DLL.
// A loaded DLL cannot be overwritten but can be renamed: eu4boost.dll becomes eu4boost.dll.old and the new
// file takes its name, so the next launch loads it; the launch after that deletes the .old file. eu4boost.ini
// is never touched. Development builds (a version with '-') do not update unless a test url is set.
//
// Update reports (with [report] upload=1, sent by report.c like the others, kind "update"): the updater
// notes how an attempt that reached a newer release ended in crash_reports\update.txt (ok, or failed and
// why). The next launch turns it into one short report: UPDATE_OK when this DLL replaced an older one,
// UPDATE_FAILED <reason> once per failure (the same failure again is not reported again). A report holds
// the versions, the result, the reason and the times, nothing about the PC (no PC specs, logs or mods).
#include "common.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wctype.h>
#include <winhttp.h>
#include <bcrypt.h>

#define UPDATE_URL L"https://api.github.com/repos/Scytheim/eu4boost/releases/latest"
// public key of the release signing key (X || Y), from "sign keygen"
static const uint8_t g_pub[64] = {
    0x64,0x34,0x30,0x76,0x73,0x66,0x0b,0x14,0x24,0x59,0xce,0x3c,0xa4,0xdb,0x2e,0xf2,
    0x73,0xa1,0x90,0x78,0x7c,0xde,0x6d,0x33,0x57,0x0b,0xf2,0x0d,0x55,0xd0,0x92,0x99,
    0x5a,0x22,0x8d,0x83,0x2e,0xf5,0x97,0xe7,0x77,0xf2,0x5d,0x56,0xf3,0xc7,0x5d,0x58,
    0xa7,0xaf,0xc4,0x41,0xcd,0xc7,0xe2,0x6f,0x18,0x15,0x22,0xde,0xc6,0x40,0x12,0x2c,
};

// ---- HTTP GET (redirects followed: GitHub sends release files from another host) ----
static uint8_t* http_get(const wchar_t* url, size_t cap, size_t* len, int* status) {
    *len = 0; *status = 0;
    URL_COMPONENTS uc; memset(&uc, 0, sizeof uc); uc.dwStructSize = sizeof uc;
    wchar_t host[256], path[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) return NULL;
    uint8_t* buf = (uint8_t*)malloc(cap + 1);
    HINTERNET s = buf ? WinHttpOpen(L"eu4boost/" EB_W(EU4BOOST_VERSION), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0) : NULL;
    HINTERNET c = s ? WinHttpConnect(s, host, uc.nPort, 0) : NULL;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : NULL;
    int ok = 0;
    if (r) {
        WinHttpSetTimeouts(r, 10000, 10000, 20000, 30000);
        if (WinHttpSendRequest(r, L"Accept: application/vnd.github+json, application/octet-stream\r\n", (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(r, NULL)) {
            DWORD code = 0, sz = sizeof code;
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
            *status = (int)code;
            DWORD got = 0; size_t n = 0; ok = 1;
            for (;;) {
                if (!WinHttpReadData(r, buf + n, (DWORD)(cap - n), &got)) { ok = 0; break; }
                if (!got) break;
                n += got;
                if (n >= cap) { ok = 0; break; }            // bigger than allowed
            }
            buf[n] = 0; *len = n;
        }
        WinHttpCloseHandle(r);
    }
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    if (!ok || *status != 200) { free(buf); return NULL; }
    return buf;
}

// ---- versions, JSON ----
static int parse_ver(const char* s, int v[3]) {               // "0.4.12" -> 0 4 12; a suffix ("-dev") fails
    v[0] = v[1] = v[2] = 0;
    for (int i = 0; i < 3; i++) {
        if (*s < '0' || *s > '9') return 0;
        while (*s >= '0' && *s <= '9') v[i] = v[i] * 10 + (*s++ - '0');
        if (i < 2) { if (*s == '.') s++; else return *s == 0; }
    }
    return *s == 0;
}
static int newer(const char* a, const char* b) {               // a > b
    int x[3], y[3];
    if (!parse_ver(a, x) || !parse_ver(b, y)) return 0;
    for (int i = 0; i < 3; i++) if (x[i] != y[i]) return x[i] > y[i];
    return 0;
}
static int json_str(const char* j, const char* key, char* out, size_t n) {    // first "key": "value"
    char k[64]; snprintf(k, sizeof k, "\"%s\"", key);
    const char* p = strstr(j, k); if (!p) return 0;
    p = strchr(p + strlen(k), ':'); if (!p) return 0;
    while (*++p == ' ');
    if (*p != '"') return 0;
    size_t i = 0;
    for (p++; *p && *p != '"' && i < n - 1; p++) out[i++] = *p;
    out[i] = 0;
    return *p == '"';
}
static int asset_url(const char* j, const char* name, char* out, size_t n) {  // browser_download_url ending in /name
    char tail[64]; snprintf(tail, sizeof tail, "/%s\"", name);
    for (const char* p = j; (p = strstr(p, "\"browser_download_url\"")); p++) {
        const char* q = strchr(p + 22, '"'); if (!q) return 0;
        const char* e = strchr(q + 1, '"'); if (!e) return 0;
        if ((size_t)(e - q) >= strlen(tail) && !strncmp(e - strlen(tail) + 1, tail, strlen(tail)) && (size_t)(e - q - 1) < n) {
            memcpy(out, q + 1, (size_t)(e - q - 1)); out[e - q - 1] = 0;
            return 1;
        }
    }
    return 0;
}

// ---- signature ----
static int sha256(const void* p, size_t n, uint8_t out[32]) {
    BCRYPT_ALG_HANDLE a; BCRYPT_HASH_HANDLE h; int ok = 0;
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, NULL, 0)) return 0;
    if (!BCryptCreateHash(a, &h, NULL, 0, NULL, 0, 0)) {
        ok = !BCryptHashData(h, (PUCHAR)p, (ULONG)n, 0) && !BCryptFinishHash(h, out, 32, 0);
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(a, 0);
    return ok;
}
// 1 when sig (the .sig text) is a valid signature of dll for version `ver` by the key pub (X || Y)
static int verify(const uint8_t* dll, size_t n, const char* sig, const char* ver, const uint8_t pub[64]) {
    char v[40]; uint8_t s[64], h[32], mh[32]; char hx[65], msg[160];
    const char* p = strstr(sig, "eu4boost-update-sig 1\n");
    if (p != sig) return 0;
    p += 22;
    size_t i = 0; while (*p && *p != '\n' && i < sizeof v - 1) v[i++] = *p++;
    v[i] = 0;
    if (*p++ != '\n' || strcmp(v, ver)) return 0;                 // signed for another version
    for (int k = 0; k < 64; k++) {
        unsigned x;
        if (sscanf(p + 2 * k, "%2x", &x) != 1) return 0;
        s[k] = (uint8_t)x;
    }
    if (!sha256(dll, n, h)) return 0;
    for (int k = 0; k < 32; k++) sprintf(hx + 2 * k, "%02x", h[k]);
    int m = snprintf(msg, sizeof msg, "eu4boost-update\n%s\n%s\n", ver, hx);
    if (!sha256(msg, (size_t)m, mh)) return 0;
    BCRYPT_ALG_HANDLE a; BCRYPT_KEY_HANDLE key; int ok = 0;
    uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
    BCRYPT_ECCKEY_BLOB* hd = (BCRYPT_ECCKEY_BLOB*)blob;
    hd->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC; hd->cbKey = 32;
    memcpy(blob + sizeof *hd, pub, 64);
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0)) return 0;
    if (!BCryptImportKeyPair(a, NULL, BCRYPT_ECCPUBLIC_BLOB, &key, blob, sizeof blob, 0)) {
        ok = BCryptVerifySignature(key, NULL, mh, 32, s, 64, 0) == 0;
        BCryptDestroyKey(key);
    }
    BCryptCloseAlgorithmProvider(a, 0);
    return ok;
}
// the release zip: plugins/eu4boost.dll stored, the signature text as the archive comment
static unsigned rd16(const uint8_t* p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static int zip_dll(const uint8_t* z, size_t n, const uint8_t** dll, size_t* dn, char* sig, size_t sn) {
    if (n < 22) return 0;
    size_t e = n - 22, stop = n - 22 > 65535 ? n - 22 - 65535 : 0;
    while (rd32(z + e) != 0x06054b50) { if (e == stop) return 0; e--; }       // end of central directory
    unsigned cn = rd16(z + e + 10), cl = rd16(z + e + 20);
    uint32_t cs = rd32(z + e + 12), co = rd32(z + e + 16);
    if (e + 22 + cl > n || (size_t)co + cs > e || cl >= sn) return 0;
    memcpy(sig, z + e + 22, cl); sig[cl] = 0;
    for (size_t p = co, k = 0; k < cn; k++) {
        if (p + 46 > (size_t)co + cs || rd32(z + p) != 0x02014b50) return 0;
        unsigned meth = rd16(z + p + 10), nl = rd16(z + p + 28), xl = rd16(z + p + 30), ml = rd16(z + p + 32);
        uint32_t csz = rd32(z + p + 20), usz = rd32(z + p + 24), lo = rd32(z + p + 42);
        if (p + 46 + nl > (size_t)co + cs) return 0;
        if (nl == 20 && !memcmp(z + p + 46, "plugins/eu4boost.dll", 20)) {
            if (meth != 0 || csz != usz || (size_t)lo + 30 > n || rd32(z + lo) != 0x04034b50) return 0;
            size_t d = (size_t)lo + 30 + rd16(z + lo + 26) + rd16(z + lo + 28);
            if (d + usz > n) return 0;
            *dll = z + d; *dn = usz;
            return 1;
        }
        p += 46 + nl + xl + ml;
    }
    return 0;
}
static int is_dll64(const uint8_t* b, size_t n) {
    if (n < 0x400 || b[0] != 'M' || b[1] != 'Z') return 0;
    uint32_t pe = *(const uint32_t*)(b + 0x3c);
    if (pe + 0x18 > n || memcmp(b + pe, "PE\0\0", 4)) return 0;
    const IMAGE_FILE_HEADER* fh = (const IMAGE_FILE_HEADER*)(b + pe + 4);
    return fh->Machine == IMAGE_FILE_MACHINE_AMD64 && (fh->Characteristics & IMAGE_FILE_DLL);
}

// ---- the note for the update report (crash_reports\update.txt) ----
typedef struct { char result[8], from[40], to[40], reason[80], detail[200], time[24]; int reported; } note_t;
static void note_path(wchar_t* p) { _snwprintf(p, MAX_PATH, L"%lsupdate.txt", eb_rep_dir); }
static void note_kv(const char* t, const char* key, char* out, size_t n) {    // "key=value" at a line start
    size_t kl = strlen(key);
    out[0] = 0;
    for (const char* p = t; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            size_t i = 0;
            for (p += kl + 1; *p && *p != '\r' && *p != '\n' && i < n - 1; p++) out[i++] = *p;
            out[i] = 0;
            return;
        }
}
static int note_parse(const char* t, note_t* u) {
    char r[8];
    memset(u, 0, sizeof *u);
    note_kv(t, "result", u->result, sizeof u->result); note_kv(t, "from", u->from, sizeof u->from);
    note_kv(t, "to", u->to, sizeof u->to); note_kv(t, "reason", u->reason, sizeof u->reason);
    note_kv(t, "detail", u->detail, sizeof u->detail); note_kv(t, "time", u->time, sizeof u->time);
    note_kv(t, "reported", r, sizeof r); u->reported = r[0] == '1';
    return !strcmp(u->result, "ok") || !strcmp(u->result, "failed");
}
static int note_text(const note_t* u, char* out, size_t n) {
    return snprintf(out, n, "result=%s\r\nfrom=%s\r\nto=%s\r\nreason=%s\r\ndetail=%s\r\ntime=%s\r\nreported=%d\r\n",
                    u->result, u->from, u->to, u->reason, u->detail, u->time, u->reported);
}
static int note_read(note_t* u) {
    wchar_t p[MAX_PATH]; note_path(p);
    char t[1024] = {0};
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { memset(u, 0, sizeof *u); return 0; }
    DWORD rd = 0; ReadFile(h, t, sizeof t - 1, &rd, NULL); CloseHandle(h);
    return note_parse(t, u);
}
static void note_save(const note_t* u) {
    wchar_t p[MAX_PATH]; note_path(p);
    char t[1024]; int n = note_text(u, t, sizeof t);
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, t, (DWORD)n, &w, NULL); CloseHandle(h); }
}
static void now_text(char* out, size_t n) {
    SYSTEMTIME s; GetLocalTime(&s);
    snprintf(out, n, "%04d-%02d-%02d %02d:%02d:%02d", s.wYear, s.wMonth, s.wDay, s.wHour, s.wMinute, s.wSecond);
}
// how this attempt ended; a failure noted and reported before (the same version, target and reason) stays reported
static void note(const char* result, const char* to, const char* reason, const char* detail) {
    note_t u, o;
    memset(&u, 0, sizeof u);
    snprintf(u.result, sizeof u.result, "%s", result); snprintf(u.from, sizeof u.from, "%s", EU4BOOST_VERSION);
    snprintf(u.to, sizeof u.to, "%s", to); snprintf(u.reason, sizeof u.reason, "%s", reason);
    snprintf(u.detail, sizeof u.detail, "%s", detail); now_text(u.time, sizeof u.time);
    if (!strcmp(result, "failed") && note_read(&o) && o.reported && !strcmp(o.result, "failed") && !strcmp(o.from, u.from) &&
        !strcmp(o.to, u.to) && !strcmp(o.reason, u.reason)) u.reported = 1;
    note_save(&u);
}
// The report of this launch: 1 = UPDATE_OK (replaced: an older DLL was replaced by this one; from = the
// note's version, else the previous session's), 2 = UPDATE_FAILED (a failure of this version not reported
// yet), 0 = nothing. in_pf: the game folder is under Program Files (writing there needs rights).
static int update_report(const note_t* u, int have, int replaced, const char* prev, int in_pf, char* out, size_t n) {
    int ok = have && !strcmp(u->result, "ok");
    if (replaced || (ok && !strcmp(u->to, EU4BOOST_VERSION))) {
        const char* from = ok && u->from[0] ? u->from : prev && prev[0] && strcmp(prev, EU4BOOST_VERSION) ? prev : "unknown";
        char now[24]; now_text(now, sizeof now);
        snprintf(out, n, "eu4boost crash report\r\nkind: update\r\nsignature: UPDATE_OK\r\nplugin: %s\r\nresult: ok\r\nfrom: %s\r\nto: %s\r\n"
                 "downloaded: %s\r\nfirst launch: %s\r\n", EU4BOOST_VERSION, from, EU4BOOST_VERSION, ok ? u->time : "(before update reports)", now);
        return 1;
    }
    if (ok && !strcmp(u->from, EU4BOOST_VERSION)) {
        snprintf(out, n, "eu4boost crash report\r\nkind: update\r\nsignature: UPDATE_FAILED not loaded\r\nplugin: %s\r\nresult: failed\r\n"
                 "from: %s\r\nto: %s\r\nreason: not loaded\r\ndetail: %s was put in place at %s, but this launch runs %s again\r\n%s",
                 EU4BOOST_VERSION, u->from, u->to, u->to, u->time, EU4BOOST_VERSION, in_pf ? "game folder: under Program Files\r\n" : "");
        return 2;
    }
    if (have && !strcmp(u->result, "failed") && !u->reported && !strcmp(u->from, EU4BOOST_VERSION)) {
        snprintf(out, n, "eu4boost crash report\r\nkind: update\r\nsignature: UPDATE_FAILED %s\r\nplugin: %s\r\nresult: failed\r\nfrom: %s\r\nto: %s\r\n"
                 "reason: %s\r\ndetail: %s\r\nwhen: %s\r\n%s", u->reason, EU4BOOST_VERSION, u->from, u->to, u->reason, u->detail, u->time,
                 in_pf ? "game folder: under Program Files\r\n" : "");
        return 2;
    }
    return 0;
}
static int in_program_files(const wchar_t* dll) {
    wchar_t lo[MAX_PATH]; size_t i = 0;
    for (; dll[i] && i < MAX_PATH - 1; i++) lo[i] = (wchar_t)towlower(dll[i]);
    lo[i] = 0;
    return wcsstr(lo, L"\\program files") != NULL;
}

// ---- the update ----
static void self_paths(wchar_t* dll, wchar_t* old, wchar_t* neu) {
    GetModuleFileNameW(g_self, dll, MAX_PATH);
    _snwprintf(old, MAX_PATH, L"%ls.old", dll);
    _snwprintf(neu, MAX_PATH, L"%ls.new", dll);
}
static DWORD WINAPI updater(LPVOID unused) {
    (void)unused;
    Sleep(20000);                                              // after the game's own start-up work
    const wchar_t* url = g_cfg.update_url[0] ? g_cfg.update_url : UPDATE_URL;
    size_t n; int st;
    char* j = (char*)http_get(url, 1 << 20, &n, &st);
    if (!j) { eb_log("update: no answer from the release list (HTTP %d) - try again at the next launch", st); return 0; }
    char tag[40], zu[1024], name[64];
    if (!json_str(j, "tag_name", tag, sizeof tag)) { eb_log("update: the release list has no version"); free(j); return 0; }
    if (!newer(tag, EU4BOOST_VERSION)) { eb_log("update: %s is the latest (this is %s)", tag, EU4BOOST_VERSION); free(j); return 0; }
    snprintf(name, sizeof name, "eu4boost_v%s.zip", tag);
    int have = asset_url(j, name, zu, sizeof zu);
    free(j);
    if (!have) { eb_log("update: %s has no %s - not updated", tag, name); note("failed", tag, "no zip", "the release has no zip"); return 0; }
    wchar_t wz[1024]; MultiByteToWideChar(CP_UTF8, 0, zu, -1, wz, 1024);
    size_t zn; int zst;
    uint8_t* z = http_get(wz, 16u << 20, &zn, &zst);
    if (!z) {
        eb_log("update: %s download failed (HTTP %d) - try again at the next launch", tag, zst);
        char why[40]; snprintf(why, sizeof why, "download (HTTP %d)", zst);
        note("failed", tag, why, "the zip did not download");
        return 0;
    }
    const uint8_t* d = NULL; size_t dn = 0; char sig[512];
    if (!zip_dll(z, zn, &d, &dn, sig, sizeof sig) || !is_dll64(d, dn) || !verify(d, dn, sig, tag, g_pub)) {
        eb_log("update: %s REJECTED - the zip, its signature or the DLL in it is not valid; nothing was changed", tag);
        note("failed", tag, "rejected", "the zip, its signature or the DLL in it is not valid");
        free(z); return 0;
    }
    wchar_t dll[MAX_PATH], old[MAX_PATH], neu[MAX_PATH]; self_paths(dll, old, neu);
    HANDLE h = CreateFileW(neu, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD w = 0; int wrote = h != INVALID_HANDLE_VALUE && WriteFile(h, d, (DWORD)dn, &w, NULL) && w == dn;
    if (h != INVALID_HANDLE_VALUE) { FlushFileBuffers(h); CloseHandle(h); }
    free(z);
    char why[40];
    if (!wrote) {
        DWORD e = GetLastError();
        DeleteFileW(neu); eb_log("update: cannot write %ls (error %lu) - not updated", neu, e);
        snprintf(why, sizeof why, "write (error %lu)", e); note("failed", tag, why, "cannot write eu4boost.dll.new");
        return 0;
    }
    if (!MoveFileExW(dll, old, MOVEFILE_REPLACE_EXISTING)) {
        DWORD e = GetLastError();
        DeleteFileW(neu); eb_log("update: cannot rename the running DLL (error %lu) - not updated", e);
        snprintf(why, sizeof why, "rename (error %lu)", e); note("failed", tag, why, "cannot rename the running eu4boost.dll");
        return 0;
    }
    if (!MoveFileExW(neu, dll, MOVEFILE_REPLACE_EXISTING)) {
        DWORD e = GetLastError();
        MoveFileExW(old, dll, MOVEFILE_REPLACE_EXISTING);        // put the running one back
        DeleteFileW(neu);
        eb_log("update: cannot put the new DLL in place (error %lu) - not updated", e);
        snprintf(why, sizeof why, "place (error %lu)", e); note("failed", tag, why, "cannot put the new eu4boost.dll in place");
        return 0;
    }
    note("ok", tag, "", "");
    eb_log("update: %s downloaded and verified - it is used from the next launch (the old DLL is kept as eu4boost.dll.old until then)", tag);
    return 0;
}

void eb_update_start(void) {
    wchar_t dll[MAX_PATH], old[MAX_PATH], neu[MAX_PATH]; self_paths(dll, old, neu);
    int replaced = GetFileAttributesW(old) != INVALID_FILE_ATTRIBUTES && DeleteFileW(old);
    if (replaced) eb_log("update: running the updated DLL (%s); the previous one was removed", EU4BOOST_VERSION);
    DeleteFileW(neu);                                          // a download cut short
    if (g_cfg.report_capture && g_cfg.report_upload != 0) {   // the update report (sent with [report] upload=1)
        note_t u; int have = note_read(&u);
        char r[1024]; int k = update_report(&u, have, replaced, eb_prev_version, in_program_files(dll), r, sizeof r);
        if (k && eb_pending_report(L"update", r)) eb_log("update: %s report written for sending", k == 1 ? "an update" : "a failed-update");
        wchar_t np[MAX_PATH]; note_path(np);
        if (k == 2 && !strcmp(u.result, "failed")) { u.reported = 1; note_save(&u); }   // the same failure is not reported again
        else if (have && (!strcmp(u.result, "ok") || strcmp(u.from, EU4BOOST_VERSION))) DeleteFileW(np);
    }
    if (!g_cfg.update_auto) { eb_log("update: auto-update off ([update] auto=0)"); return; }
    if (strchr(EU4BOOST_VERSION, '-') && !g_cfg.update_url[0]) { eb_log("update: development build - no auto-update"); return; }
#ifndef EB_TEST
    HANDLE h = CreateThread(NULL, 256 * 1024, updater, NULL, 0, NULL);
    if (h) CloseHandle(h);
#endif
}

#ifdef EB_TEST
// self-test of the version compare, the release-list parsing and the signature check (a key made here)
__declspec(dllexport) int eb_test_update(void) {
    int bad = 0;
    bad += !newer("0.4.3", "0.4.2") + newer("0.4.2", "0.4.2") + newer("0.4.1", "0.4.2") + !newer("0.10.0", "0.9.9") +
           !newer("1.0.0", "0.99.99") + newer("0.4.3-dev", "0.4.2") + newer("0.4.3", "0.4.2-dev") + !newer("0.4.10", "0.4.9");
    const char* js = "{\"tag_name\": \"0.4.3\", \"assets\": [{\"name\": \"eu4boost_v0.4.3.zip\", \"browser_download_url\": \"https://x/download/0.4.3/eu4boost_v0.4.3.zip\"},"
                     "{\"name\": \"eu4boost.dll.sig\", \"browser_download_url\": \"https://x/download/0.4.3/eu4boost.dll.sig\"},"
                     "{\"name\": \"eu4boost.dll\", \"browser_download_url\": \"https://x/download/0.4.3/eu4boost.dll\"}]}";
    char tag[40], u1[256], u2[256];
    bad += !json_str(js, "tag_name", tag, sizeof tag) || strcmp(tag, "0.4.3");
    bad += !asset_url(js, "eu4boost.dll", u1, sizeof u1) || strcmp(u1, "https://x/download/0.4.3/eu4boost.dll");
    bad += !asset_url(js, "eu4boost.dll.sig", u2, sizeof u2) || strcmp(u2, "https://x/download/0.4.3/eu4boost.dll.sig");
    // a key pair, a signed file, then: valid / another version / a changed byte / another key
    BCRYPT_ALG_HANDLE a; BCRYPT_KEY_HANDLE k; uint8_t blob[256]; ULONG bn = 0;
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0) || BCryptGenerateKeyPair(a, &k, 256, 0) ||
        BCryptFinalizeKeyPair(k, 0) || BCryptExportKey(k, NULL, BCRYPT_ECCPUBLIC_BLOB, blob, sizeof blob, &bn, 0)) return 100;
    uint8_t pub[64]; memcpy(pub, blob + sizeof(BCRYPT_ECCKEY_BLOB), 64);
    static uint8_t file[5000]; for (int i = 0; i < 5000; i++) file[i] = (uint8_t)(i * 7);
    uint8_t h[32], mh[32], sig[64]; char hx[65], msg[160], st[300]; ULONG sn = 0;
    sha256(file, sizeof file, h); for (int i = 0; i < 32; i++) sprintf(hx + 2 * i, "%02x", h[i]);
    int m = snprintf(msg, sizeof msg, "eu4boost-update\n0.4.3\n%s\n", hx); sha256(msg, (size_t)m, mh);
    if (BCryptSignHash(k, NULL, mh, 32, sig, 64, &sn, 0)) return 101;
    int o = snprintf(st, sizeof st, "eu4boost-update-sig 1\n0.4.3\n");
    for (int i = 0; i < 64; i++) o += snprintf(st + o, sizeof st - (size_t)o, "%02x", sig[i]);
    snprintf(st + o, sizeof st - (size_t)o, "\n");
    bad += !verify(file, sizeof file, st, "0.4.3", pub);       // valid
    bad += verify(file, sizeof file, st, "0.4.4", pub);        // the release says another version
    file[1234] ^= 1; bad += verify(file, sizeof file, st, "0.4.3", pub); file[1234] ^= 1;
    bad += verify(file, sizeof file, st, "0.4.3", g_pub);      // the real key did not sign it
    // the release zip as tools/release.sh writes it: the DLL stored, the signature as the archive comment
    static uint8_t zb[8192]; size_t zp = 0, sl = strlen(st);
#define W16(v) (zb[zp] = (uint8_t)(v), zb[zp + 1] = (uint8_t)((unsigned)(v) >> 8), zp += 2)
#define W32(v) (W16((uint32_t)(v) & 0xffff), W16((uint32_t)(v) >> 16))
    W32(0x04034b50); W16(10); W16(0); W16(0); W16(0); W16(0); W32(0); W32(sizeof file); W32(sizeof file); W16(20); W16(0);
    memcpy(zb + zp, "plugins/eu4boost.dll", 20); zp += 20;
    memcpy(zb + zp, file, sizeof file); zp += sizeof file;
    size_t co = zp;
    W32(0x02014b50); W16(20); W16(10); W16(0); W16(0); W16(0); W16(0); W32(0); W32(sizeof file); W32(sizeof file);
    W16(20); W16(0); W16(0); W16(0); W16(0); W32(0); W32(0);
    memcpy(zb + zp, "plugins/eu4boost.dll", 20); zp += 20;
    size_t cs = zp - co;
    W32(0x06054b50); W16(0); W16(0); W16(1); W16(1); W32(cs); W32(co); W16(sl);
    memcpy(zb + zp, st, sl); zp += sl;
#undef W16
#undef W32
    const uint8_t* zd = NULL; size_t zn = 0; char zs[512];
    bad += !zip_dll(zb, zp, &zd, &zn, zs, sizeof zs) || zd != zb + 50 || zn != sizeof file || !verify(zd, zn, zs, "0.4.3", pub);
    zb[co + 10] = 8; bad += zip_dll(zb, zp, &zd, &zn, zs, sizeof zs); zb[co + 10] = 0;     // deflated: not read
    bad += zip_dll(zb, zp - 30, &zd, &zn, zs, sizeof zs);       // cut short
    BCryptDestroyKey(k); BCryptCloseAlgorithmProvider(a, 0);
    // the update report from the note
    note_t u, w; char rt[1024], nt[1024];
    memset(&u, 0, sizeof u);
    strcpy(u.result, "failed"); strcpy(u.from, EU4BOOST_VERSION); strcpy(u.to, "9.9.9"); strcpy(u.reason, "rename (error 5)");
    strcpy(u.detail, "cannot rename"); strcpy(u.time, "2026-10-26 12:00:00");
    note_text(&u, nt, sizeof nt);
    bad += !note_parse(nt, &w) || strcmp(w.reason, u.reason) || strcmp(w.to, "9.9.9") || w.reported;
    bad += update_report(&w, 1, 0, "0.4.5", 1, rt, sizeof rt) != 2 || !strstr(rt, "\r\nsignature: UPDATE_FAILED rename (error 5)\r\n") ||
           !strstr(rt, "\r\nplugin: " EU4BOOST_VERSION "\r\n") || !strstr(rt, "Program Files");
    w.reported = 1; bad += update_report(&w, 1, 0, "0.4.5", 0, rt, sizeof rt) != 0;               // reported once
    strcpy(w.from, "0.0.1"); w.reported = 0; bad += update_report(&w, 1, 0, "0.4.5", 0, rt, sizeof rt) != 0;   // another version's
    bad += update_report(&w, 0, 1, "0.4.5", 0, rt, sizeof rt) != 1 || !strstr(rt, "\r\nfrom: 0.4.5\r\nto: " EU4BOOST_VERSION "\r\n");   // no note
    bad += update_report(&w, 0, 1, "", 0, rt, sizeof rt) != 1 || !strstr(rt, "\r\nfrom: unknown\r\n");
    strcpy(w.result, "ok"); strcpy(w.from, "0.4.1"); strcpy(w.to, EU4BOOST_VERSION);
    bad += update_report(&w, 1, 1, "0.4.5", 0, rt, sizeof rt) != 1 || !strstr(rt, "\r\nfrom: 0.4.1\r\n") || !strstr(rt, "\r\nsignature: UPDATE_OK\r\n");
    bad += update_report(&w, 1, 0, "0.4.5", 0, rt, sizeof rt) != 1;                               // .old already gone
    strcpy(w.from, EU4BOOST_VERSION); strcpy(w.to, "9.9.9");
    bad += update_report(&w, 1, 0, "0.4.5", 0, rt, sizeof rt) != 2 || !strstr(rt, "UPDATE_FAILED not loaded");   // still the old one
    bad += update_report(&w, 0, 0, "0.4.5", 0, rt, sizeof rt) != 0;                               // nothing happened
    bad += !in_program_files(L"C:\\Program Files (x86)\\Steam\\x\\eu4boost.dll") + in_program_files(L"E:\\Games\\x\\eu4boost.dll");
    eb_log("TEST update: %d wrong answers", bad);
    return bad;
}
#endif
