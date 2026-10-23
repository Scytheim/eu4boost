// Release signing for the auto-update (src/update.c checks the signature before it installs a DLL).
//
//   sign keygen <private.key>                  new ECDSA P-256 key pair; prints the public key for update.c
//   sign sign <private.key> <version> <dll> <out.sig>
//   sign verify <private.key> <version> <dll> <sig>
//
// The signed message is "eu4boost-update\n<version>\n<sha256 of the dll, hex>\n", so a signature also
// fixes the version it was made for. The .sig file is text: "eu4boost-update-sig 1", the version and the
// 64-byte signature (r||s) in hex, one per line. Keep private.key out of the repository and back it up:
// a plugin trusts only the public key it was built with.
//
// Build: clang --target=x86_64-w64-windows-gnu -O2 -o build/sign.exe tools/sign.c -lbcrypt
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

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
static uint8_t* read_all(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long k = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = (uint8_t*)malloc((size_t)k + 1);
    if (b && fread(b, 1, (size_t)k, f) != (size_t)k) { free(b); b = NULL; }
    fclose(f);
    if (b) { b[k] = 0; *n = (size_t)k; }
    return b;
}
static void hex(char* out, const uint8_t* p, int n) { for (int i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", p[i]); }
static int message_hash(const char* version, const char* dll, uint8_t mh[32]) {
    size_t n; uint8_t* d = read_all(dll, &n); uint8_t h[32]; char hx[65], msg[256];
    if (!d) { fprintf(stderr, "cannot read %s\n", dll); return 0; }
    int ok = sha256(d, n, h); free(d);
    if (!ok) return 0;
    hex(hx, h, 32);
    int m = snprintf(msg, sizeof msg, "eu4boost-update\n%s\n%s\n", version, hx);
    printf("dll sha256 %s, %zu bytes\n", hx, n);
    return sha256(msg, (size_t)m, mh);
}
static BCRYPT_KEY_HANDLE load_private(BCRYPT_ALG_HANDLE a, const char* path) {
    size_t n; uint8_t* b = read_all(path, &n); BCRYPT_KEY_HANDLE k = NULL;
    if (!b) { fprintf(stderr, "cannot read %s\n", path); return NULL; }
    if (BCryptImportKeyPair(a, NULL, BCRYPT_ECCPRIVATE_BLOB, &k, b, (ULONG)n, 0)) k = NULL;
    SecureZeroMemory(b, n); free(b);
    return k;
}

int main(int argc, char** argv) {
    BCRYPT_ALG_HANDLE a;
    if (argc < 3 || BCryptOpenAlgorithmProvider(&a, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0)) {
        fprintf(stderr, "usage: sign keygen <private.key> | sign <private.key> <version> <dll> <out.sig> | verify <private.key> <version> <dll> <sig>\n");
        return 2;
    }
    if (!strcmp(argv[1], "keygen")) {
        BCRYPT_KEY_HANDLE k; uint8_t blob[256]; ULONG n = 0;
        if (GetFileAttributesA(argv[2]) != INVALID_FILE_ATTRIBUTES) { fprintf(stderr, "%s exists - not overwritten\n", argv[2]); return 1; }
        if (BCryptGenerateKeyPair(a, &k, 256, 0) || BCryptFinalizeKeyPair(k, 0) ||
            BCryptExportKey(k, NULL, BCRYPT_ECCPRIVATE_BLOB, blob, sizeof blob, &n, 0)) { fprintf(stderr, "key generation failed\n"); return 1; }
        FILE* f = fopen(argv[2], "wb"); if (!f || fwrite(blob, 1, n, f) != n) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
        fclose(f);
        BCRYPT_ECCKEY_BLOB* hd = (BCRYPT_ECCKEY_BLOB*)blob;          // header, X, Y, (d)
        printf("public key (X || Y) for src/update.c:\n");
        for (ULONG i = 0; i < 2 * hd->cbKey; i++) printf("0x%02x,%s", blob[sizeof *hd + i], i % 16 == 15 ? "\n" : "");
        SecureZeroMemory(blob, sizeof blob);
        return 0;
    }
    if (argc < 6) { fprintf(stderr, "missing arguments\n"); return 2; }
    BCRYPT_KEY_HANDLE k = load_private(a, argv[2]);
    uint8_t mh[32];
    if (!k || !message_hash(argv[3], argv[4], mh)) return 1;
    if (!strcmp(argv[1], "sign")) {
        uint8_t sig[64]; ULONG n = 0; char sx[129];
        if (BCryptSignHash(k, NULL, mh, 32, sig, sizeof sig, &n, 0) || n != 64) { fprintf(stderr, "signing failed\n"); return 1; }
        hex(sx, sig, 64);
        FILE* f = fopen(argv[5], "wb"); if (!f) { fprintf(stderr, "cannot write %s\n", argv[5]); return 1; }
        fprintf(f, "eu4boost-update-sig 1\n%s\n%s\n", argv[3], sx); fclose(f);
        printf("signed %s %s -> %s\n", argv[3], argv[4], argv[5]);
        return 0;
    }
    if (!strcmp(argv[1], "verify")) {
        size_t n; char* t = (char*)read_all(argv[5], &n); uint8_t sig[64];
        if (!t) return 1;
        char* p = strstr(t, "\n"); p = p ? strstr(p + 1, "\n") : NULL;
        if (!p || strlen(p + 1) < 128) { fprintf(stderr, "bad sig file\n"); return 1; }
        for (int i = 0; i < 64; i++) { unsigned v; sscanf(p + 1 + 2 * i, "%2x", &v); sig[i] = (uint8_t)v; }
        int ok = BCryptVerifySignature(k, NULL, mh, 32, sig, 64, 0) == 0;
        printf("signature %s\n", ok ? "OK" : "BAD");
        return ok ? 0 : 1;
    }
    fprintf(stderr, "unknown command %s\n", argv[1]);
    return 2;
}
