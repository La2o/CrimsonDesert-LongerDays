#include <windows.h>

#include "longer_days.h"

namespace
{
    HMODULE g_self = nullptr;

    DWORD WINAPI InitThread(LPVOID)
    {
        longerdays::Initialize(g_self);
        return 0;
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        g_self = module;
        DisableThreadLibraryCalls(module);
        // Scanning and hooking happen off the loader lock.
        if (HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr))
            CloseHandle(t);
        break;

    case DLL_PROCESS_DETACH:
        // On process exit (reserved != null) other threads are already gone
        // and the game is tearing down - leave its memory alone.
        if (!reserved)
            longerdays::Shutdown();
        break;
    }
    return TRUE;
}
