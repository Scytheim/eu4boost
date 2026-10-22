# Sourced by build.sh and test/run_tests.sh: puts a clang that can target x86_64-w64-windows-gnu on PATH.
#   Linux/WSL: clang + lld + mingw-w64 from the distribution (already on PATH).
#   Windows (Git Bash): llvm-mingw (https://github.com/mstorsjo/llvm-mingw, msvcrt build), from $LLVM_MINGW
#   or else the newest C:\tools\llvm-mingw-*. Its clang finds its own mingw sysroot.
if [ -n "$LLVM_MINGW" ]; then
    PATH="$LLVM_MINGW/bin:$PATH"
elif ! command -v clang >/dev/null 2>&1; then
    d=$(ls -d /c/tools/llvm-mingw-*/bin 2>/dev/null | tail -1)
    [ -n "$d" ] && PATH="$d:$PATH"
fi
command -v clang >/dev/null 2>&1 || { echo "clang not found: install llvm-mingw to C:\\tools or set LLVM_MINGW" >&2; exit 1; }
export PATH
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) EB_NATIVE_WINDOWS=1 ;; *) EB_NATIVE_WINDOWS= ;; esac
