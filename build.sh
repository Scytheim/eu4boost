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
$CC -O3 -DNDEBUG -DMI_STAT=0 -DMI_SECURE=0 -I$MI/include -c $MI/src/static.c -o $OUT/mimalloc.o
for f in hook buffer trampoline; do $CC $CFLAGS -w -I$MH/include -c $MH/src/$f.c -o $OUT/mh_$f.o; done
$CC $CFLAGS -w -c $MH/src/hde/hde64.c -o $OUT/mh_hde64.o
for f in main alloc profiler threads online speed parallel savezip savepar modifier country console guard aiprof uitime crash report; do $CC $CFLAGS -I$MI/include -I$MH/include -c src/$f.c -o $OUT/$f.o; done
$CC -shared -static -fuse-ld=lld -s -o $OUT/eu4boost.dll $OUT/main.o $OUT/alloc.o $OUT/profiler.o $OUT/threads.o $OUT/online.o $OUT/speed.o $OUT/parallel.o $OUT/savezip.o $OUT/savepar.o $OUT/modifier.o $OUT/country.o $OUT/console.o $OUT/guard.o $OUT/aiprof.o $OUT/uitime.o $OUT/crash.o $OUT/report.o $OUT/mimalloc.o $OUT/mh_*.o \
    -Wl,--exclude-all-symbols -lpsapi -lwinmm -ladvapi32 -lwinhttp -lbcrypt -luser32 -lkernel32
ls -la $OUT/eu4boost.dll
