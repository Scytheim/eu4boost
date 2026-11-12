#!/bin/sh
# Cross-compile eu4boost.dll (x64) with clang targeting MinGW-w64 (native TLS, needed by mimalloc).
# Third-party code (approved by the user, vendored in third_party/): microsoft/mimalloc v2.2.4 (MIT), TsudaKageyu/minhook (BSD-2).
# Needs: clang + lld + mingw-w64 headers/libs (Ubuntu/WSL: sudo apt install clang lld mingw-w64),
# or on Windows llvm-mingw run from Git Bash (see toolchain.sh).
set -e
cd "$(dirname "$0")"
. ./toolchain.sh
MI=${MI:-third_party/mimalloc}
MH=${MH:-third_party/minhook}
OUT=build; mkdir -p $OUT
CC="clang --target=x86_64-w64-windows-gnu"
CFLAGS="-O2 -DNDEBUG -D__USE_MINGW_ANSI_STDIO=1 -D_WIN32_WINNT=0x0A00 -Wall -Wno-unused-function -fno-strict-aliasing"
# version resource (src/version.rc): product name and version, from EU4BOOST_VERSION in src/common.h
VER=$(sed -n 's/^#define EU4BOOST_VERSION "\(.*\)"$/\1/p' src/common.h)
[ -n "$VER" ] || { echo "no EU4BOOST_VERSION in src/common.h"; exit 1; }
NUM=$(printf '%s' "$VER" | sed 's/[^0-9.].*$//' | awk -F. '{ printf "%d,%d,%d,0", $1, $2, $3 }')
printf '#define EB_VER_STR "%s"\n#define EB_VER_NUM %s\n' "$VER" "$NUM" > $OUT/version_gen.h
x86_64-w64-mingw32-windres -I $OUT src/version.rc -O coff -o $OUT/version.o
# SOURCE_DATE_EPOCH: mimalloc has __DATE__ and __TIME__ in a message; a fixed value keeps the DLL the same
# from build to build (tools/pe_check.py compares the build on GitHub with a local one)
SOURCE_DATE_EPOCH=0 $CC -O3 -DNDEBUG -DMI_STAT=0 -DMI_SECURE=0 -I$MI/include -c $MI/src/static.c -o $OUT/mimalloc.o
for f in hook buffer trampoline; do $CC $CFLAGS -w -I$MH/include -c $MH/src/$f.c -o $OUT/mh_$f.o; done
$CC $CFLAGS -w -c $MH/src/hde/hde64.c -o $OUT/mh_hde64.o
for f in main alloc profiler threads online speed parallel savezip savepar modifier country console guard aiprof uitime crash report memdiag update savelist script autorun playmon leakfix rng warmup alerts daygap savesum buildview aistrat; do $CC $CFLAGS -I$MI/include -I$MH/include -c src/$f.c -o $OUT/$f.o; done
$CC -shared -static -fuse-ld=lld -s -o $OUT/eu4boost.dll $OUT/main.o $OUT/alloc.o $OUT/profiler.o $OUT/threads.o $OUT/online.o $OUT/speed.o $OUT/parallel.o $OUT/savezip.o $OUT/savepar.o $OUT/modifier.o $OUT/country.o $OUT/console.o $OUT/guard.o $OUT/aiprof.o $OUT/uitime.o $OUT/crash.o $OUT/report.o $OUT/memdiag.o $OUT/update.o $OUT/savelist.o $OUT/script.o $OUT/autorun.o $OUT/playmon.o $OUT/leakfix.o $OUT/rng.o $OUT/warmup.o $OUT/alerts.o $OUT/daygap.o $OUT/savesum.o $OUT/buildview.o $OUT/aistrat.o $OUT/mimalloc.o $OUT/mh_*.o $OUT/version.o \
    -Wl,--exclude-all-symbols -lpsapi -lwinmm -ladvapi32 -lwinhttp -lbcrypt -luser32 -lgdi32 -lkernel32
ls -la $OUT/eu4boost.dll
