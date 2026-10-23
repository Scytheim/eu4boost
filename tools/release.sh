#!/bin/sh
# Builds a release: sh tools/release.sh  ->  build/release/eu4boost_v<version>.zip, the only file of the
# GitHub release <version>: plugins\eu4boost.dll, plugins\eu4boost.ini from dist/ and README_KO.md. For the
# auto-update (src/update.c) the DLL is stored uncompressed and the archive comment is its signature,
# made with the private key EB_SIGN_KEY (default ../keys/eu4boost_update_p256.key, outside the
# repository; tools/sign.c). Set EU4BOOST_VERSION in src/common.h first (without "-dev") and commit.
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

mkdir -p build/release
ZIP=build/release/eu4boost_v$VER.zip
SIG=build/eu4boost.dll.sig
. ./toolchain.sh; clang --target=x86_64-w64-windows-gnu -O2 -o build/sign.exe tools/sign.c -lbcrypt
build/sign.exe sign "$KEY" "$VER" build/eu4boost.dll $SIG
build/sign.exe verify "$KEY" "$VER" build/eu4boost.dll $SIG
PY=; for c in python3 python; do if $c -c "" 2>/dev/null; then PY=$c; break; fi; done
[ -n "$PY" ] || { echo "python is needed for the zip"; exit 1; }
$PY - "$ZIP" $SIG <<'PYEOF'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED) as z:
    z.write('build/eu4boost.dll', 'plugins/eu4boost.dll', compress_type=zipfile.ZIP_STORED)
    z.write('dist/plugins/eu4boost.ini', 'plugins/eu4boost.ini')
    z.write('dist/README_KO.md', 'README_KO.md')
    z.comment = open(sys.argv[2], 'rb').read()
with zipfile.ZipFile(sys.argv[1]) as z:            # read back: the DLL stored and unchanged, the comment in place
    i = z.getinfo('plugins/eu4boost.dll')
    assert i.compress_type == zipfile.ZIP_STORED and z.read(i) == open('build/eu4boost.dll', 'rb').read()
    assert z.comment == open(sys.argv[2], 'rb').read() and z.testzip() is None
PYEOF
ls -l "$ZIP"
echo "Upload $ZIP to the GitHub release $VER (the only file)."
