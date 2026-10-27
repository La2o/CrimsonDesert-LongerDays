#pragma once
#include <cstdint>
#include <vector>

// Finds the game code and data this mod needs, trying every signature in
// offsets.h and checking each candidate before it is used. Windows only.
namespace longerdays::locate
{
    struct Hit
    {
        uintptr_t   addr = 0;
        const char* how  = "";  // which signature matched, for the log
        explicit operator bool() const { return addr != 0; }
    };

    // Start of the field-time tick function (the hook target).
    Hit FieldTimeTick();

    // The client-realm {day, hour, minute, second} global.
    Hit FieldClock();

    // Every plausible TimeOfDay engine global, most likely first. Which one is
    // real can only be told in-world (TodCandidates + StillMoving).
    std::vector<uintptr_t> TodEngineGlobals();

    // How to reach the TimeOfDay manager's hour and limits.
    struct TodBinding
    {
        uintptr_t global     = 0;
        uintptr_t managerOff = 0;
        uintptr_t hourOff    = 0;     // lower limit at +4, upper at +8
        bool      searched   = false; // offsets found by search, not the known ones
        float     hour = 0, lower = 0, upper = 0; // as last read
        explicit operator bool() const { return global != 0; }
    };

    // Places whose time of day agrees with the field clock right now: first
    // at the known offsets, then (if `search`) anywhere near them. Cheap
    // enough for a background thread; never call it on the game thread.
    std::vector<TodBinding> TodCandidates(const std::vector<uintptr_t>& globals, float clockHour, bool search);

    // Re-reads candidates a few seconds later and keeps those that behave like
    // a running sun: the hour moved forward a little, the limits stayed put,
    // and it still agrees with the clock.
    std::vector<TodBinding> StillMoving(const std::vector<TodBinding>& before, float clockHour);

    // Logs what the known chain reads for the first few candidates, for bug
    // reports when the sun can't be found.
    void LogTodDiagnostics(const std::vector<uintptr_t>& globals, float clockHour);

    // The live manager for a binding, or 0 (not in-world, or torn down).
    uintptr_t TodManager(const TodBinding& b);
}
