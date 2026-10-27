#pragma once
#include <cstdint>

// The game's field-time snapshot and the arithmetic the game itself does on it
// (build 2.03.02). Pure math, no game or Windows dependencies, so it is
// unit-tested on any host (tests/tests.cpp).
//
// The clock is not advanced frame by frame. The game keeps a snapshot and
// derives "now" from it on every read:
//
//   units(now) = Units(snapshot) + (gameMs(now) - snapshot.stamp) * snapshot.rate
//
// where gameMs is a millisecond counter that stops while the game is paused,
// and rate is 12 in vanilla. Longer Days steers time by moving `stamp`: pushing
// it forward by part of each frame's elapsed milliseconds slows the clock,
// pulling it back speeds it up. The snapshot stays a valid game value
// throughout, so a save or a removed mod never sees anything odd.
namespace longerdays::gametime
{
    struct Snapshot
    {
        uint32_t day, hour, minute, second, ms;
        uint8_t  weekday;  // day % 7
        uint8_t  pad;
        uint16_t rate;     // time units per game millisecond (12 in vanilla)
        uint64_t stamp;    // game milliseconds at which the fields above were true
    };
    static_assert(sizeof(Snapshot) == 32, "the game copies it as one 32-byte vector");

    // A day is 75,600,000 units: real milliseconds of day, except that
    // 21:00-03:00 counts half, so night passes twice as fast as day.
    inline constexpr uint64_t kDayUnits      = 75'600'000;
    inline constexpr uint64_t kMsPerHour     = 3'600'000;
    inline constexpr uint64_t kNightEndMs    = 3 * kMsPerHour;   // 03:00
    inline constexpr uint64_t kNightStartMs  = 21 * kMsPerHour;  // 21:00
    inline constexpr uint64_t kEarlyNightUnits = kNightEndMs / 2;                 // units at 03:00
    inline constexpr uint64_t kLateNightUnits  = kNightStartMs - kEarlyNightUnits; // units at 21:00
    inline constexpr uint64_t kLateNightBias   = 2 * kLateNightUnits - kNightStartMs;

    inline bool Plausible(const Snapshot& s)
    {
        return s.hour < 24 && s.minute < 60 && s.second < 60 && s.ms < 1000 &&
               s.rate >= 1 && s.rate <= 1000 && s.day < 1'000'000;
    }

    // Snapshot fields to units (the game's own conversion).
    inline uint64_t Units(const Snapshot& s)
    {
        const uint64_t t = ((uint64_t{s.hour} * 60 + s.minute) * 60 + s.second) * 1000 + s.ms;
        uint64_t inDay;
        if (t < kNightEndMs)         inDay = t / 2;
        else if (t < kNightStartMs)  inDay = t - kEarlyNightUnits;
        else                         inDay = (t + kLateNightBias) / 2;
        return uint64_t{s.day} * kDayUnits + inDay;
    }

    // Units back to fields; rate and stamp are left alone (the game's own
    // conversion).
    inline void SetUnits(Snapshot& s, uint64_t units)
    {
        const uint64_t day = units / kDayUnits;
        const uint64_t r   = units - day * kDayUnits;
        uint64_t t;
        if (r < kEarlyNightUnits)     t = r * 2;
        else if (r < kLateNightUnits) t = r + kEarlyNightUnits;
        else                          t = r * 2 - kLateNightBias;
        s.day     = static_cast<uint32_t>(day);
        s.weekday = static_cast<uint8_t>(day % 7);
        s.ms      = static_cast<uint32_t>(t % 1000);
        t /= 1000;
        s.second  = static_cast<uint32_t>(t % 60);
        t /= 60;
        s.minute  = static_cast<uint32_t>(t % 60);
        s.hour    = static_cast<uint32_t>(t / 60 % 24);
    }

    // What the game's getter returns at game time `now` (milliseconds).
    inline uint64_t UnitsAt(const Snapshot& s, uint64_t now)
    {
        return Units(s) + (now > s.stamp ? (now - s.stamp) * s.rate : 0);
    }

    // The snapshot as the game would see it at `now`, re-anchored there.
    inline Snapshot At(const Snapshot& s, uint64_t now)
    {
        Snapshot out = s;
        SetUnits(out, UnitsAt(s, now));
        out.stamp = now;
        return out;
    }
}
