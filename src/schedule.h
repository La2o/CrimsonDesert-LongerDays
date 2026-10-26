#pragma once
#include "config.h"

namespace longerdays
{
    // How much slower than vanilla the clock runs at one moment, and why.
    // Pure math, no game or Windows dependencies, so it is unit-tested on
    // any host (tests/tests.cpp).
    struct SpeedSample
    {
        float length  = 1.0f; // final multiplier: 2.0 = time passes half as fast
        float dayness = 0.0f; // 0 = night, 1 = day, in between during dawn/dusk
        float golden  = 0.0f; // 0..1, how deep into a golden hour we are
        float season  = 1.0f; // seasonal factor applied to the day multiplier
    };

    // hour: in-game hour 0..24. day: in-game day counter (for seasons).
    // vanillaRate: in-game seconds per real second at vanilla speed, used to
    // turn the [RealTime] minute targets into multipliers.
    SpeedSample LengthAt(const Config& cfg, float hour, int day, float vanillaRate);
}
