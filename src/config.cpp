#include "config.h"

#include <windows.h>
#include <cwchar>

namespace longerdays
{
    namespace
    {
        struct Ini
        {
            const std::wstring& path;

            bool Raw(const wchar_t* section, const wchar_t* key, wchar_t (&buf)[64]) const
            {
                buf[0] = 0;
                GetPrivateProfileStringW(section, key, L"", buf, 64, path.c_str());
                return buf[0] != 0;
            }

            float Float(const wchar_t* section, const wchar_t* key, float def, float lo, float hi) const
            {
                wchar_t buf[64];
                if (!Raw(section, key, buf)) return def;
                wchar_t* end = nullptr;
                const float v = std::wcstof(buf, &end);
                if (end == buf) return def;
                return v < lo ? lo : (v > hi ? hi : v);
            }

            int Int(const wchar_t* section, const wchar_t* key, int def) const
            {
                wchar_t buf[64];
                if (!Raw(section, key, buf)) return def;
                // Base 0 accepts both decimal (119) and hex (0x77) key codes.
                wchar_t* end = nullptr;
                const long v = std::wcstol(buf, &end, 0);
                return end == buf ? def : static_cast<int>(v);
            }
        };
    }

    Config Config::Load(const std::wstring& path, const std::wstring& seasonsPath)
    {
        const Ini ini{path};
        Config c;

        constexpr const wchar_t* kMain = L"LongerDays";
        c.enabled         = ini.Int(kMain, L"Enabled", c.enabled) != 0;
        c.dayLength       = ini.Float(kMain, L"DayLengthMultiplier", c.dayLength, 0.1f, 20.0f);
        c.nightLength     = ini.Float(kMain, L"NightLengthMultiplier", c.nightLength, 0.1f, 20.0f);
        c.dayStartHour    = ini.Float(kMain, L"DayStartHour", c.dayStartHour, 0.0f, 24.0f);
        c.dayEndHour      = ini.Float(kMain, L"DayEndHour", c.dayEndHour, 0.0f, 24.0f);
        c.transitionHours = ini.Float(kMain, L"TransitionHours", c.transitionHours, 0.0f, 6.0f);
        c.syncSun         = ini.Int(kMain, L"SyncSun", c.syncSun) != 0;
        c.toggleKey       = ini.Int(kMain, L"ToggleKey", c.toggleKey);
        if (c.toggleKey < 0 || c.toggleKey > 0xFE) c.toggleKey = 0;
        c.showToast       = ini.Int(kMain, L"ShowToast", c.showToast) != 0;
        c.fastForwardKey  = ini.Int(kMain, L"FastForwardKey", c.fastForwardKey);
        if (c.fastForwardKey < 0 || c.fastForwardKey > 0xFE) c.fastForwardKey = 0;
        c.fastForwardSpeed = ini.Float(kMain, L"FastForwardSpeed", c.fastForwardSpeed, 2.0f, 200.0f);

        constexpr const wchar_t* kGolden = L"GoldenHour";
        c.goldenHourMultiplier = ini.Float(kGolden, L"Multiplier", c.goldenHourMultiplier, 0.25f, 10.0f);
        c.goldenHourLength     = ini.Float(kGolden, L"LengthHours", c.goldenHourLength, 0.0f, 6.0f);

        constexpr const wchar_t* kReal = L"RealTime";
        c.dayRealMinutes   = ini.Float(kReal, L"DayRealMinutes", c.dayRealMinutes, 0.0f, 1440.0f);
        c.nightRealMinutes = ini.Float(kReal, L"NightRealMinutes", c.nightRealMinutes, 0.0f, 1440.0f);

        constexpr const wchar_t* kSeasons = L"Seasons";
        c.seasonStrength = ini.Float(kSeasons, L"Strength", c.seasonStrength, 0.0f, 0.9f);
        const float seasonLength = ini.Float(kSeasons, L"SeasonLengthDays", 0.0f, 0.0f, 365.0f);
        if (seasonLength >= 1.0f)
        {
            c.seasonLengthDays = seasonLength;
        }
        else if (GetFileAttributesW(seasonsPath.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            // Follow Seasons of Pywel: the length picked in its F5 menu, and
            // the season it is locked to in "Fixed" mode.
            const Ini seasons{seasonsPath};
            const int days = seasons.Int(L"Seasons", L"days_per_season", 30);
            c.seasonLengthDays = static_cast<float>(days >= 1 && days <= 365 ? days : 30);
            c.seasonsFromPywel = true;

            wchar_t buf[64];
            if (seasons.Raw(L"Seasons", L"mode", buf) && _wcsicmp(buf, L"fixed") == 0)
            {
                static const wchar_t* const kNames[] = {L"summer", L"autumn", L"winter", L"spring"};
                c.fixedSeason = 0; // Seasons of Pywel's own default
                if (seasons.Raw(L"Seasons", L"season", buf))
                    for (int i = 0; i < 4; ++i)
                        if (_wcsicmp(buf, kNames[i]) == 0) c.fixedSeason = i;
            }
        }
        return c;
    }
}
