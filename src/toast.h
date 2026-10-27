#pragma once
#include <windows.h>

namespace longerdays::toast
{
    // Briefly shows "Longer Days: ON/OFF" near the top of `over` (the game
    // window), then fades out. Safe to call from any thread; never blocks.
    //
    // The toast is a separate topmost, click-through window, so it does not
    // touch the game's renderer. It shows over borderless and windowed modes
    // but not over exclusive fullscreen.
    void Show(HWND over, bool enabled);
}
