// Stand-in for pops_api.dll: counts POPS_RunCallbacks calls per handle.
#include <windows.h>
static void* keys[8]; static volatile LONG counts[8];
__declspec(dllexport) void POPS_RunCallbacks(void* h) {
    for (int i = 0; i < 8; i++) {
        if (keys[i] == h || !keys[i]) { keys[i] = h; InterlockedIncrement(&counts[i]); return; }
    }
}
__declspec(dllexport) long POPS_TestCount(void* h) {
    for (int i = 0; i < 8; i++) if (keys[i] == h) return counts[i];
    return 0;
}
