#pragma once
#include <string>

namespace longerdays
{
    // Mirrors CrimsonDesertLongerDays.ini. See that file for what each key does.
    struct Config
    {
        bool  enabled      = true;
        float dayLength    = 2.0f; // multiplier on how long daytime lasts
        float nightLength  = 1.0f; // multiplier on how long night lasts
        float dayStartHour = 6.0f;
        float dayEndHour   = 20.0f;
        bool  syncSun      = true;
        int   toggleKey    = 0x77; // VK_F8; 0 disables the hotkey

        // Reads the INI, clamping every value to a safe range. Missing keys
        // keep their defaults.
        static Config Load(const std::wstring& iniPath);

        // Multiplier in effect at the given in-game hour (0..24).
        float LengthAt(float hour) const;
        bool  ChangesAnything() const { return dayLength != 1.0f || nightLength != 1.0f; }
    };
}
