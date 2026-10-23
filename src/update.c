// Auto-update ([update] auto=1, on by default): a newer release is downloaded in the background and used
// from the next launch.
//
// 20 s after the game starts, a thread asks GitHub for the latest release of Scytheim/eu4boost (one
// HTTPS request; [update] url= points elsewhere for tests). If its version is newer than this DLL's, it
// downloads the release's eu4boost.dll and eu4boost.dll.sig and installs the DLL only when
//   - the signature is valid: ECDSA P-256 over "eu4boost-update\n<version>\n<sha256 of the dll>\n" with
//     the key below (tools/sign.c makes it at release time; the private key never leaves the developer's
//     PC), and the signed version is the release's version,
//   - the file is a 64-bit DLL.
// A loaded DLL cannot be overwritten but can be renamed: eu4boost.dll becomes eu4boost.dll.old and the new
// file takes its name, so the next launch loads it; the launch after that deletes the .old file. eu4boost.ini
// is never touched. Development builds (a version with '-') do not update unless a test url is set.
#include "common.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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
static int is_dll64(const uint8_t* b, size_t n) {
    if (n < 0x400 || b[0] != 'M' || b[1] != 'Z') return 0;
    uint32_t pe = *(const uint32_t*)(b + 0x3c);
    if (pe + 0x18 > n || memcmp(b + pe, "PE\0\0", 4)) return 0;
    const IMAGE_FILE_HEADER* fh = (const IMAGE_FILE_HEADER*)(b + pe + 4);
    return fh->Machine == IMAGE_FILE_MACHINE_AMD64 && (fh->Characteristics & IMAGE_FILE_DLL);
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
    char tag[40], du[1024], su[1024];
    if (!json_str(j, "tag_name", tag, sizeof tag)) { eb_log("update: the release list has no version"); free(j); return 0; }
    if (!newer(tag, EU4BOOST_VERSION)) { eb_log("update: %s is the latest (this is %s)", tag, EU4BOOST_VERSION); free(j); return 0; }
    int have = asset_url(j, "eu4boost.dll", du, sizeof du) && asset_url(j, "eu4boost.dll.sig", su, sizeof su);
    free(j);
    if (!have) { eb_log("update: %s has no eu4boost.dll and eu4boost.dll.sig - not updated", tag); return 0; }
    wchar_t wd[1024], ws[1024];
    MultiByteToWideChar(CP_UTF8, 0, du, -1, wd, 1024); MultiByteToWideChar(CP_UTF8, 0, su, -1, ws, 1024);
    size_t dn, sn; int dst, sst;
    uint8_t* d = http_get(wd, 16u << 20, &dn, &dst);
    char* s = d ? (char*)http_get(ws, 4096, &sn, &sst) : NULL;
    if (!d || !s) { eb_log("update: %s download failed (HTTP %d/%d) - try again at the next launch", tag, dst, s ? sst : 0); free(d); free(s); return 0; }
    if (!is_dll64(d, dn) || !verify(d, dn, s, tag, g_pub)) {
        eb_log("update: %s REJECTED - the signature or the file is not valid; nothing was changed", tag);
        free(d); free(s); return 0;
    }
    free(s);
    wchar_t dll[MAX_PATH], old[MAX_PATH], neu[MAX_PATH]; self_paths(dll, old, neu);
    HANDLE h = CreateFileW(neu, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD w = 0; int wrote = h != INVALID_HANDLE_VALUE && WriteFile(h, d, (DWORD)dn, &w, NULL) && w == dn;
    if (h != INVALID_HANDLE_VALUE) { FlushFileBuffers(h); CloseHandle(h); }
    free(d);
    if (!wrote) { DeleteFileW(neu); eb_log("update: cannot write %ls (error %lu) - not updated", neu, GetLastError()); return 0; }
    if (!MoveFileExW(dll, old, MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(neu); eb_log("update: cannot rename the running DLL (error %lu) - not updated", GetLastError()); return 0; }
    if (!MoveFileExW(neu, dll, MOVEFILE_REPLACE_EXISTING)) {
        DWORD e = GetLastError();
        MoveFileExW(old, dll, MOVEFILE_REPLACE_EXISTING);        // put the running one back
        DeleteFileW(neu);
        eb_log("update: cannot put the new DLL in place (error %lu) - not updated", e);
        return 0;
    }
    eb_log("update: %s downloaded and verified - it is used from the next launch (the old DLL is kept as eu4boost.dll.old until then)", tag);
    return 0;
}

void eb_update_start(void) {
    wchar_t dll[MAX_PATH], old[MAX_PATH], neu[MAX_PATH]; self_paths(dll, old, neu);
    if (GetFileAttributesW(old) != INVALID_FILE_ATTRIBUTES && DeleteFileW(old)) eb_log("update: running the updated DLL (%s); the previous one was removed", EU4BOOST_VERSION);
    DeleteFileW(neu);                                          // a download cut short
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
    BCryptDestroyKey(k); BCryptCloseAlgorithmProvider(a, 0);
    eb_log("TEST update: %d wrong answers", bad);
    return bad;
}
#endif
