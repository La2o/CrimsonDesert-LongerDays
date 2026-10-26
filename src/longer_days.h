#pragma once
#include <windows.h>

namespace longerdays
{
    // Runs on a worker thread: loads the INI, waits for the game code to be
    // scannable, resolves signatures and installs the clock hook.
    void Initialize(HMODULE self);

    // Unhooks and restores the sun's time-of-day limits. Only called on an
    // explicit unload, never during process exit.
    void Shutdown();
}
