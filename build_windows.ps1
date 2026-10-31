# Build eu4boost.dll natively on Windows with llvm-mingw (https://github.com/mstorsjo/llvm-mingw,
# llvm-mingw-<ver>-msvcrt-x86_64.zip unpacked to C:\tools, or its bin\ on PATH, or $env:LLVM_MINGW).
# Same flags as build.sh, which also runs on Windows from Git Bash (sh build.sh) - that is the usual path.
param([string]$Clang = "")
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
if (-not $Clang) {
    if ($env:LLVM_MINGW) { $Clang = Join-Path $env:LLVM_MINGW "bin\x86_64-w64-mingw32-clang.exe" }
    elseif (Get-Command x86_64-w64-mingw32-clang -ErrorAction SilentlyContinue) { $Clang = "x86_64-w64-mingw32-clang" }
    else {
        $dir = Get-ChildItem "C:\tools" -Directory -Filter "llvm-mingw-*" -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1
        if (-not $dir) { throw "llvm-mingw not found: unpack it to C:\tools or set LLVM_MINGW" }
        $Clang = Join-Path $dir.FullName "bin\x86_64-w64-mingw32-clang.exe"
    }
}
$MI = "third_party/mimalloc"; $MH = "third_party/minhook"; $OUT = "build"
New-Item -ItemType Directory -Force $OUT | Out-Null
$CFLAGS = @("-O2", "-DNDEBUG", "-D__USE_MINGW_ANSI_STDIO=1", "-D_WIN32_WINNT=0x0A00", "-Wall", "-Wno-unused-function", "-fno-strict-aliasing")
function Run([string[]]$a) { & $Clang @a; if ($LASTEXITCODE -ne 0) { throw "clang failed: $a" } }
Run @("-O3", "-DNDEBUG", "-DMI_STAT=0", "-DMI_SECURE=0", "-I$MI/include", "-c", "$MI/src/static.c", "-o", "$OUT/mimalloc.o")
foreach ($f in "hook", "buffer", "trampoline") { Run ($CFLAGS + @("-w", "-I$MH/include", "-c", "$MH/src/$f.c", "-o", "$OUT/mh_$f.o")) }
Run ($CFLAGS + @("-w", "-c", "$MH/src/hde/hde64.c", "-o", "$OUT/mh_hde64.o"))
$objs = @()
foreach ($f in "main", "alloc", "profiler", "threads", "online", "speed", "parallel", "savezip", "savepar", "modifier", "country", "console", "guard", "aiprof", "uitime", "crash", "report", "memdiag", "update", "savelist", "script", "autorun", "playmon", "leakfix", "rng") {
    Run ($CFLAGS + @("-I$MI/include", "-I$MH/include", "-c", "src/$f.c", "-o", "$OUT/$f.o")); $objs += "$OUT/$f.o"
}
# same link order as build.sh (its mh_*.o glob is alphabetical), so both scripts give the same code
$objs += "$OUT/mimalloc.o", "$OUT/mh_buffer.o", "$OUT/mh_hde64.o", "$OUT/mh_hook.o", "$OUT/mh_trampoline.o"
Run (@("-shared", "-static", "-fuse-ld=lld", "-s", "-o", "$OUT/eu4boost.dll") + $objs + @("-Wl,--exclude-all-symbols", "-lpsapi", "-lwinmm", "-ladvapi32", "-lwinhttp", "-lbcrypt", "-luser32", "-lkernel32"))
Get-Item "$OUT/eu4boost.dll" | Select-Object Name, Length, LastWriteTime
