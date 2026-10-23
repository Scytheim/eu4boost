#!/bin/sh
# Builds a release: sh tools/release.sh  ->  build/release/eu4boost_v<version>.zip
# (plugins\eu4boost.dll + plugins\eu4boost.ini from dist/ + README_KO.md), and for the auto-update
# build/release/eu4boost.dll + eu4boost.dll.sig, signed with the private key EB_SIGN_KEY (default
# ../keys/eu4boost_update_p256.key, outside the repository; tools/sign.c). Set EU4BOOST_VERSION in
# src/common.h first (without "-dev") and commit; then upload all three files to the GitHub release <version>.
set -e
cd "$(dirname "$0")/.."
VER=$(sed -n 's/^#define EU4BOOST_VERSION "\(.*\)"$/\1/p' src/common.h)
[ -n "$VER" ] || { echo "no EU4BOOST_VERSION in src/common.h"; exit 1; }
case "$VER" in *-dev*) [ -n "$FORCE" ] || { echo "version $VER is a development build (set a release version, or FORCE=1)"; exit 1; } ;; esac
[ -z "$(git status --porcelain --untracked-files=no)" ] || [ -n "$FORCE" ] || { echo "uncommitted changes (commit first, or FORCE=1)"; exit 1; }

sh build.sh
if [ "$OS" = Windows_NT ]; then EB_NATIVE_WINDOWS=1 sh test/run_tests.sh; else sh test/run_tests.sh; fi

mkdir -p build/release
ZIP=build/release/eu4boost_v$VER.zip
PY=; for c in python3 python; do if $c -c "" 2>/dev/null; then PY=$c; break; fi; done
[ -n "$PY" ] || { echo "python is needed for the zip"; exit 1; }
$PY - "$ZIP" <<'PYEOF'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED) as z:
    z.write('build/eu4boost.dll', 'plugins/eu4boost.dll')
    z.write('dist/plugins/eu4boost.ini', 'plugins/eu4boost.ini')
    z.write('dist/README_KO.md', 'README_KO.md')
PYEOF
KEY=${EB_SIGN_KEY:-../keys/eu4boost_update_p256.key}
[ -f "$KEY" ] || { echo "signing key $KEY not found (EB_SIGN_KEY)"; exit 1; }
. ./toolchain.sh; clang --target=x86_64-w64-windows-gnu -O2 -o build/sign.exe tools/sign.c -lbcrypt
cp build/eu4boost.dll build/release/eu4boost.dll
build/sign.exe sign "$KEY" "$VER" build/release/eu4boost.dll build/release/eu4boost.dll.sig
build/sign.exe verify "$KEY" "$VER" build/release/eu4boost.dll build/release/eu4boost.dll.sig
ls -l "$ZIP" build/release/eu4boost.dll build/release/eu4boost.dll.sig
echo "Upload $ZIP, eu4boost.dll and eu4boost.dll.sig to the GitHub release $VER."
