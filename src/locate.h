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

    // What the game's own /settimeofday{lower,upper}limit handlers reveal:
    // which engine virtual returns the TimeOfDay manager, and where in the
    // manager the limits live.
    struct TodLayout
    {
        int       slot     = 0;  // vtable byte offset of the manager getter
        uintptr_t lowerOff = 0;  // upper is lowerOff + 4, current hour lowerOff - 4
        explicit operator bool() const { return slot != 0; }
    };
    TodLayout LocateTodLayout();

    // How to reach the TimeOfDay manager's hour and limits.
    struct TodBinding
    {
        uintptr_t global     = 0;
        uintptr_t managerOff = 0;
        bool      embedded   = false; // manager lives inside the engine (lea), not behind a pointer
        uintptr_t hourOff    = 0;
        uintptr_t lowerOff   = 0;     // upper at +4
        bool      searched   = false; // offsets found by search, not from the game's code
        float     hour = 0, lower = 0, upper = 0; // as last read
        explicit operator bool() const { return global != 0; }
    };

    // Places whose time of day agrees with the field clock right now: first
    // through the layout read from the game, then the 2.03.00 offsets, then
    // (if `search`) anywhere near them. For a background thread only.
    std::vector<TodBinding> TodCandidates(const std::vector<uintptr_t>& globals, const TodLayout& layout,
                                          float clockHour, bool search);

    // Re-reads candidates a few seconds later and keeps those that behave like
    // a running sun: the hour moved forward a little, the limits stayed put,
    // and it still agrees with the clock.
    std::vector<TodBinding> StillMoving(const std::vector<TodBinding>& before, float clockHour);

    // Logs what the known chain reads for the first few candidates, for bug
    // reports when the sun can't be found.
    void LogTodDiagnostics(const std::vector<uintptr_t>& globals, const TodLayout& layout, float clockHour);

    // The live manager for a binding, or 0 (not in-world, or torn down).
    uintptr_t TodManager(const TodBinding& b);
}
