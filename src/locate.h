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

    // Every plausible TimeOfDay engine global. Which one is real can only be
    // told in-world, by BindTod.
    std::vector<uintptr_t> TodEngineGlobals();

    // How to reach the TimeOfDay manager's hour and limits.
    struct TodBinding
    {
        uintptr_t global     = 0;
        uintptr_t managerOff = 0;
        uintptr_t hourOff    = 0;  // lower limit at +4, upper at +8
        bool      searched   = false; // offsets found by search, not the known ones
        explicit operator bool() const { return global != 0; }
    };

    // Picks the candidate whose visible time of day agrees with the field
    // clock (`clockHour`), first at the known offsets, then by searching the
    // engine and manager objects. False if nothing agrees (yet).
    bool BindTod(const std::vector<uintptr_t>& globals, float clockHour, TodBinding& out);

    // The live manager for a binding, or 0 (not in-world, or torn down).
    uintptr_t TodManager(const TodBinding& b);
}
