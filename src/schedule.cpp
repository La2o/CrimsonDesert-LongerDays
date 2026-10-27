#include "schedule.h"

#include <algorithm>
#include <cmath>

namespace longerdays
{
    namespace
    {
        constexpr float kTau = 6.28318530718f;
        constexpr float kMinLength = 0.05f;
        constexpr float kMaxLength = 50.0f;

        float Wrap24(float h)
        {
            h = std::fmod(h, 24.0f);
            return h < 0.0f ? h + 24.0f : h;
        }

        // 0 below e0, 1 above e1, smooth in between. A zero-width edge is a step.
        float SmoothStep(float e0, float e1, float x)
        {
            if (e1 <= e0) return x >= e0 ? 1.0f : 0.0f;
            const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        // Raised-cosine bump: 1 at x = 0, falling to 0 at |x| = width / 2.
        float Bell(float x, float width)
        {
            if (width <= 0.0f || std::fabs(x) >= width * 0.5f) return 0.0f;
            return 0.5f * (1.0f + std::cos(kTau * x / width));
        }

        // Multiplier that makes `hours` of in-game time take `minutes` of real
        // time, given the vanilla clock speed.
        float FromRealMinutes(float minutes, float hours, float vanillaRate)
        {
            const float vanillaMinutes = hours * 3600.0f / vanillaRate / 60.0f;
            return minutes / vanillaMinutes;
        }
    }

    SpeedSample LengthAt(const Config& cfg, float hour, int day, float vanillaRate)
    {
        SpeedSample s;

        float dayLen = Wrap24(cfg.dayEndHour - cfg.dayStartHour);
        if (dayLen == 0.0f) dayLen = 24.0f; // start == end: always day
        const float nightLen = 24.0f - dayLen;

        // Hours since sunrise, shifted so the night before sunrise is negative:
        // sunrise sits at 0 and sunset at dayLen, both away from the wrap.
        float t = Wrap24(hour - cfg.dayStartHour);
        if (t > (dayLen + 24.0f) * 0.5f) t -= 24.0f;

        if (nightLen <= 0.0f)
        {
            s.dayness = 1.0f;
        }
        else
        {
            const float half = std::min({cfg.transitionHours, dayLen, nightLen}) * 0.5f;
            s.dayness = SmoothStep(-half, half, t) * (1.0f - SmoothStep(dayLen - half, dayLen + half, t));

            const float gl = std::min({cfg.goldenHourLength, dayLen, nightLen});
            s.golden = std::max(Bell(t, gl), Bell(t - dayLen, gl));
        }

        float dayMult   = cfg.dayLength;
        float nightMult = cfg.nightLength;
        if (vanillaRate > 0.0f)
        {
            if (cfg.dayRealMinutes > 0.0f)
                dayMult = FromRealMinutes(cfg.dayRealMinutes, dayLen, vanillaRate);
            if (cfg.nightRealMinutes > 0.0f && nightLen > 0.0f)
                nightMult = FromRealMinutes(cfg.nightRealMinutes, nightLen, vanillaRate);
        }

        if (cfg.seasonStrength > 0.0f && cfg.seasonLengthDays > 0.0f)
        {
            // Summer is days 1..N, so midsummer is the middle of them; a
            // locked season sits at its own midpoint.
            const float len = cfg.seasonLengthDays;
            const float phase = cfg.fixedSeason >= 0
                ? static_cast<float>(cfg.fixedSeason % 4) / 4.0f
                : (static_cast<float>(day) - (1.0f + len) / 2.0f) / (4.0f * len);
            s.season = 1.0f + cfg.seasonStrength * std::cos(kTau * phase);
            dayMult *= s.season;
        }

        dayMult   = std::clamp(dayMult, kMinLength, kMaxLength);
        nightMult = std::clamp(nightMult, kMinLength, kMaxLength);

        const float base = nightMult + (dayMult - nightMult) * s.dayness;
        s.length = std::clamp(base * (1.0f + (cfg.goldenHourMultiplier - 1.0f) * s.golden),
                              kMinLength, kMaxLength);
        return s;
    }
}
