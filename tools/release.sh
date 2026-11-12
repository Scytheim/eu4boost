#!/bin/sh
# Builds a release: sh tools/release.sh  ->  build/release/eu4boost_v<version>.zip, the only file of the
# GitHub release <version>: plugins\eu4boost.dll, plugins\eu4boost.ini from dist/ and README_KO.md. For the
# auto-update (src/update.c) the DLL is stored uncompressed and the archive comment is its signature,
# made with the private key EB_SIGN_KEY (default ../keys/eu4boost_update_p256.key, outside the
# repository; tools/sign.c). Set EU4BOOST_VERSION in src/common.h first (without "-dev") and commit.
#
# Code-signed release (README.md, "Code signing policy"): push the release commit, run the GitHub workflow
# "build" on main with "sign" ticked, approve the signing request in SignPath, download the artifact
# "eu4boost-signed" (gh run download <run> -n eu4boost-signed -D build/signed) and run
#     EB_SIGNED_DLL=build/signed/eu4boost.dll sh tools/release.sh
# The signed DLL goes into the zip only if tools/pe_check.py finds a valid signature on it, the release
# version in it, and the same code as the local build of this commit. The update signature is made last,
# over the signed DLL (code signing changes the file).
set -e
cd "$(dirname "$0")/.."
VER=$(sed -n 's/^#define EU4BOOST_VERSION "\(.*\)"$/\1/p' src/common.h)
[ -n "$VER" ] || { echo "no EU4BOOST_VERSION in src/common.h"; exit 1; }
case "$VER" in *-dev*) [ -n "$FORCE" ] || { echo "version $VER is a development build (set a release version, or FORCE=1)"; exit 1; } ;; esac
[ -z "$(git status --porcelain --untracked-files=no)" ] || [ -n "$FORCE" ] || { echo "uncommitted changes (commit first, or FORCE=1)"; exit 1; }
KEY=${EB_SIGN_KEY:-../keys/eu4boost_update_p256.key}
[ -f "$KEY" ] || { echo "signing key $KEY not found (EB_SIGN_KEY)"; exit 1; }

sh build.sh
if [ "$OS" = Windows_NT ]; then EB_NATIVE_WINDOWS=1 sh test/run_tests.sh; else sh test/run_tests.sh; fi

PY=; for c in python3 python; do if $c -c "" 2>/dev/null; then PY=$c; break; fi; done
[ -n "$PY" ] || { echo "python is needed for the zip"; exit 1; }
DLL=build/eu4boost.dll
if [ -n "$EB_SIGNED_DLL" ]; then
    $PY tools/pe_check.py "$EB_SIGNED_DLL" build/eu4boost.dll "$VER"
    DLL=$EB_SIGNED_DLL
fi

mkdir -p build/release
ZIP=build/release/eu4boost_v$VER.zip
SIG=build/eu4boost.dll.sig
. ./toolchain.sh; clang --target=x86_64-w64-windows-gnu -O2 -o build/sign.exe tools/sign.c -lbcrypt
build/sign.exe sign "$KEY" "$VER" "$DLL" $SIG
build/sign.exe verify "$KEY" "$VER" "$DLL" $SIG
$PY - "$ZIP" $SIG "$DLL" <<'PYEOF'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED) as z:
    z.write(sys.argv[3], 'plugins/eu4boost.dll', compress_type=zipfile.ZIP_STORED)
    z.write('dist/plugins/eu4boost.ini', 'plugins/eu4boost.ini')
    z.write('dist/README_KO.md', 'README_KO.md')
    z.comment = open(sys.argv[2], 'rb').read()
with zipfile.ZipFile(sys.argv[1]) as z:            # read back: the DLL stored and unchanged, the comment in place
    i = z.getinfo('plugins/eu4boost.dll')
    assert i.compress_type == zipfile.ZIP_STORED and z.read(i) == open(sys.argv[3], 'rb').read()
    assert z.comment == open(sys.argv[2], 'rb').read() and z.testzip() is None
PYEOF
ls -l "$ZIP"
[ -n "$EB_SIGNED_DLL" ] || echo "note: this DLL has no code signature (Windows Smart App Control blocks it)"
echo "Upload $ZIP to the GitHub release $VER (the only file)."
