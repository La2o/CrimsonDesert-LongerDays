#pragma once
#include <string>

namespace longerdays
{
    // Mirrors CrimsonDesertLongerDays.ini. See that file for what each key does.
    struct Config
    {
        // [LongerDays]
        bool  enabled      = true;
        float dayLength    = 2.0f; // multiplier on how long daytime lasts
        float nightLength  = 1.0f; // multiplier on how long night lasts
        float dayStartHour = 6.0f;
        float dayEndHour   = 20.0f;
        float transitionHours = 1.0f; // dawn/dusk blend width; 0 = hard switch
        bool  syncSun      = true;
        int   toggleKey    = 0x77; // VK_F8; 0 disables the hotkey
        bool  showToast    = true; // on-screen ON/OFF note when the hotkey is pressed
        int   fastForwardKey   = 0x76;  // VK_F7, held; 0 disables
        float fastForwardSpeed = 30.0f; // how many times faster time runs while held

        // [GoldenHour] - sunrise and sunset linger on top of the day/night speed
        float goldenHourMultiplier = 1.5f; // 1.0 = off
        float goldenHourLength     = 1.0f; // hours, centred on sunrise / sunset

        // [RealTime] - minutes of real time; > 0 overrides the multipliers
        float dayRealMinutes   = 0.0f;
        float nightRealMinutes = 0.0f;

        // [Seasons] - daytime grows and shrinks over the in-game year. The
        // year is laid out like Seasons of Pywel's: four seasons of
        // seasonLengthDays each, summer first from day 1.
        float seasonStrength   = 0.0f;  // 0 = off, 0.3 = summer days +30%, winter -30%
        float seasonLengthDays = 30.0f; // in-game days per season
        int   fixedSeason      = -1;    // -1 = follow the day; 0 summer, 1 autumn, 2 winter, 3 spring
        bool  seasonsFromPywel = false; // the two above were read from Seasons.ini

        // Reads the INI (Windows only), clamping every value to a safe range.
        // Missing keys keep their defaults. With SeasonLengthDays=0, the
        // season length (and a locked season) come from Seasons of Pywel's
        // seasonsIniPath when it exists.
        static Config Load(const std::wstring& iniPath, const std::wstring& seasonsIniPath);
    };
}
