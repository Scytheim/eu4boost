#!/bin/sh
# Smoke tests for the EB_TEST build of eu4boost (see harness.c).
# Run ./build.sh first (the test DLL reuses build/mimalloc.o and build/mh_*.o).
# Linux/WSL: runs under wine64 (sudo apt install wine64). Windows (Git Bash + llvm-mingw): runs natively.
# Results: build/test/<case>.out and <case>.log.
set -e
cd "$(dirname "$0")/.."
. ./toolchain.sh
MI=${MI:-third_party/mimalloc}; MH=${MH:-third_party/minhook}
T=build/test; mkdir -p $T
CC="clang --target=x86_64-w64-windows-gnu"
CFLAGS="-O2 -DNDEBUG -DEB_TEST -D__USE_MINGW_ANSI_STDIO=1 -D_WIN32_WINNT=0x0A00 -Wall -Wno-unused-function -fno-strict-aliasing"
# the EB_TEST build skips the eu4.exe signature checks, so those tables are unused there
CFLAGS="$CFLAGS -Wno-unused-const-variable"
for f in main alloc profiler threads online speed parallel savezip savepar modifier country console guard aiprof uitime crash report memdiag update; do $CC $CFLAGS -I$MI/include -I$MH/include -c src/$f.c -o $T/$f.o; done
$CC -shared -static -fuse-ld=lld -o $T/eu4boost_test.dll $T/main.o $T/alloc.o $T/profiler.o $T/threads.o $T/online.o $T/speed.o $T/parallel.o $T/savezip.o $T/savepar.o $T/modifier.o $T/country.o $T/console.o $T/guard.o $T/aiprof.o $T/uitime.o $T/crash.o $T/report.o $T/memdiag.o $T/update.o build/mimalloc.o build/mh_*.o \
    -Wl,--exclude-all-symbols -lpsapi -lwinmm -ladvapi32 -lwinhttp -lbcrypt -luser32 -lkernel32
$CC -shared -fuse-ld=lld -o $T/pops_api.dll test/fake_pops.c -Wl,--out-implib,$T/libpops_api.a
$CC -O1 -fuse-ld=lld -o $T/harness.exe test/harness.c -L$T -lpops_api
if [ -n "$EB_NATIVE_WINDOWS" ]; then
    WINE=
else
    export WINEPREFIX=${WINEPREFIX:-/tmp/wpfx} WINEDEBUG=-all
    WINE=${WINE:-$(command -v wine64 || echo /usr/lib/wine/wine64)}
fi
run() { # name, ini text, args...
    name=$1; ini=$2; shift 2
    printf '%b' "$ini" > $T/eu4boost.ini
    (cd $T && $WINE ./harness.exe eu4boost_test.dll "$@") > $T/$name.out 2>&1 || true
    cp $T/eu4boost.log $T/$name.log
    last=$(tail -1 $T/$name.out)
    printf '%-12s %s\n' "$name" "$last"
    case "$last" in "ALL TESTS PASSED"*) ;; *) failed="$failed $name" ;; esac
}
rm -f $T/plugin64.dll
run zero_all   '[general]\r\nlog=1\r\n[allocator]\r\nzero_fill=1\r\n' 1500 zero
touch $T/plugin64.dll
run zero_auto  '[general]\r\nlog=1\r\n[allocator]\r\nzero_fill=-1\r\n' 1500 zerolarge
rm -f $T/plugin64.dll
run plain      '[general]\r\nlog=1\r\n[profiler]\r\nenabled=1\r\n' 1500 none
run online_off '[general]\r\nlog=1\r\n[online]\r\npoll_interval_ms=0\r\n' 1500 none nothrottle
run online_always '[general]\r\nlog=1\r\n[online]\r\nonly_while_running=0\r\n' 1500 none always
run no_alloc   '[general]\r\nlog=1\r\n[allocator]\r\nenabled=0\r\n' 1500 none
rm -f $T/eu4boost_speed_*.csv
run speed      '[general]\r\nlog=1\r\n' 1000 speed
run partest    '[general]\r\nlog=1\r\n' 300 partest
run savezip    '[general]\r\nlog=1\r\n[save]\r\nparallel_compress=1\r\nblock_kb=1024\r\nstats=1\r\n' 300 savezip
run savezip_off '[general]\r\nlog=1\r\n[save]\r\nstats=1\r\n' 300 savezip
run modadd     '[general]\r\nlog=1\r\n[modifiers]\r\nfast_add=1\r\n' 300 modadd
run modadd_sse2 '[general]\r\nlog=1\r\n[modifiers]\r\nfast_add=1\r\nfast_search=2\r\n' 300 modadd
run pacing     '[general]\r\nlog=1\r\n[render]\r\nspeed5_days_per_frame=3\r\n' 300 pacing
run guard      '[general]\r\nlog=1\r\n' 300 guard
run stackwalk  '[general]\r\nlog=1\r\n' 300 stackwalk
rm -f $T/eu4boost_memsites.tsv
run memsites   '[general]\r\nlog=1\r\n[diag]\r\nmem_sites=4\r\nmem_sites_s=5\r\n' 6500 none
grep -q '^#snap' $T/eu4boost_memsites.tsv 2>/dev/null || { echo "memsites: no snapshot"; failed="$failed memsites"; }
if [ -n "$failed" ]; then echo "FAILED:$failed"; exit 1; fi
echo "all 16 cases passed"
