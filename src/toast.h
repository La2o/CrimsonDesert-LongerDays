#pragma once
#include <windows.h>

namespace longerdays::toast
{
    enum class Tone { On, Off, Warning };

    // Briefly shows "Longer Days  <state>" near the top of `over` (the game
    // window), then fades out. Safe to call from any thread; never blocks.
    // Warnings stay up longer.
    //
    // The toast is a separate topmost, click-through window, so it does not
    // touch the game's renderer. It shows over borderless and windowed modes
    // but not over exclusive fullscreen.
    void Show(HWND over, const wchar_t* state, Tone tone);

    inline void Show(HWND over, bool enabled)
    {
        Show(over, enabled ? L"ON" : L"OFF (vanilla time)", enabled ? Tone::On : Tone::Off);
    }
}
