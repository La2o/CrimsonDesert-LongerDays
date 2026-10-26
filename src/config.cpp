#include "config.h"

#include <windows.h>
#include <cwchar>

namespace longerdays
{
    namespace
    {
        constexpr const wchar_t* kSection = L"LongerDays";

        float ReadFloat(const std::wstring& ini, const wchar_t* key, float def, float lo, float hi)
        {
            wchar_t buf[64] = {};
            GetPrivateProfileStringW(kSection, key, L"", buf, 64, ini.c_str());
            if (!buf[0]) return def;
            wchar_t* end = nullptr;
            const float v = std::wcstof(buf, &end);
            if (end == buf) return def;
            return v < lo ? lo : (v > hi ? hi : v);
        }

        int ReadInt(const std::wstring& ini, const wchar_t* key, int def)
        {
            wchar_t buf[64] = {};
            GetPrivateProfileStringW(kSection, key, L"", buf, 64, ini.c_str());
            if (!buf[0]) return def;
            // Base 0 accepts both decimal (119) and hex (0x77) key codes.
            wchar_t* end = nullptr;
            const long v = std::wcstol(buf, &end, 0);
            return end == buf ? def : static_cast<int>(v);
        }
    }

    Config Config::Load(const std::wstring& ini)
    {
        Config c;
        c.enabled      = ReadInt(ini, L"Enabled", c.enabled) != 0;
        c.dayLength    = ReadFloat(ini, L"DayLengthMultiplier", c.dayLength, 0.1f, 20.0f);
        c.nightLength  = ReadFloat(ini, L"NightLengthMultiplier", c.nightLength, 0.1f, 20.0f);
        c.dayStartHour = ReadFloat(ini, L"DayStartHour", c.dayStartHour, 0.0f, 24.0f);
        c.dayEndHour   = ReadFloat(ini, L"DayEndHour", c.dayEndHour, 0.0f, 24.0f);
        c.syncSun      = ReadInt(ini, L"SyncSun", c.syncSun) != 0;
        c.toggleKey    = ReadInt(ini, L"ToggleKey", c.toggleKey);
        if (c.toggleKey < 0 || c.toggleKey > 0xFE) c.toggleKey = 0;
        return c;
    }

    float Config::LengthAt(float hour) const
    {
        // A window like 20 -> 6 wraps past midnight.
        const bool day = dayStartHour <= dayEndHour
            ? (hour >= dayStartHour && hour < dayEndHour)
            : (hour >= dayStartHour || hour < dayEndHour);
        return day ? dayLength : nightLength;
    }
}
