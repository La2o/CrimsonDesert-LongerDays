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

    Config Config::Load(const std::wstring& path)
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

        constexpr const wchar_t* kGolden = L"GoldenHour";
        c.goldenHourMultiplier = ini.Float(kGolden, L"Multiplier", c.goldenHourMultiplier, 0.25f, 10.0f);
        c.goldenHourLength     = ini.Float(kGolden, L"LengthHours", c.goldenHourLength, 0.0f, 6.0f);

        constexpr const wchar_t* kReal = L"RealTime";
        c.dayRealMinutes   = ini.Float(kReal, L"DayRealMinutes", c.dayRealMinutes, 0.0f, 1440.0f);
        c.nightRealMinutes = ini.Float(kReal, L"NightRealMinutes", c.nightRealMinutes, 0.0f, 1440.0f);

        constexpr const wchar_t* kSeasons = L"Seasons";
        c.seasonStrength = ini.Float(kSeasons, L"Strength", c.seasonStrength, 0.0f, 0.9f);
        c.yearLengthDays = ini.Float(kSeasons, L"YearLengthDays", c.yearLengthDays, 1.0f, 10000.0f);
        c.summerPeakDay  = ini.Float(kSeasons, L"SummerPeakDay", c.summerPeakDay, 0.0f, 10000.0f);
        return c;
    }
}
