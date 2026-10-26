// Host-side unit tests for the game-independent logic. No framework: build
// with -DLONGERDAYS_TESTS=ON and run longerdays_tests (exit code = failures).
#include <cmath>
#include <cstdio>
#include <vector>

#include "../src/pattern.h"
#include "../src/rate_meter.h"
#include "../src/schedule.h"

using namespace longerdays;

static int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
    } while (0)

static bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

// Plain day x2 / night x1 with no blending or golden hour.
static Config HardSwitch()
{
    Config c;
    c.transitionHours = 0.0f;
    c.goldenHourMultiplier = 1.0f;
    return c;
}

static void TestDayNight()
{
    const Config c = HardSwitch();
    CHECK(Near(LengthAt(c, 12.0f, 0, 12.0f).length, 2.0f));
    CHECK(Near(LengthAt(c, 2.0f, 0, 12.0f).length, 1.0f));
    CHECK(Near(LengthAt(c, 23.5f, 0, 12.0f).length, 1.0f));
    CHECK(Near(LengthAt(c, 6.0f, 0, 12.0f).length, 2.0f));  // sunrise is day
    CHECK(Near(LengthAt(c, 20.0f, 0, 12.0f).length, 1.0f)); // sunset is night
    CHECK(Near(LengthAt(c, 19.99f, 0, 12.0f).length, 2.0f));
}

static void TestWrappingWindow()
{
    Config c = HardSwitch();
    c.dayStartHour = 22.0f; // "day" 22:00 -> 04:00
    c.dayEndHour = 4.0f;
    CHECK(Near(LengthAt(c, 23.0f, 0, 12.0f).length, 2.0f));
    CHECK(Near(LengthAt(c, 1.0f, 0, 12.0f).length, 2.0f));
    CHECK(Near(LengthAt(c, 12.0f, 0, 12.0f).length, 1.0f));
}

static void TestTransitionIsSmooth()
{
    Config c = HardSwitch();
    c.transitionHours = 2.0f;
    CHECK(Near(LengthAt(c, 6.0f, 0, 12.0f).dayness, 0.5f));  // midpoint of the blend
    CHECK(Near(LengthAt(c, 5.0f, 0, 12.0f).dayness, 0.0f));
    CHECK(Near(LengthAt(c, 7.0f, 0, 12.0f).dayness, 1.0f));
    // Monotonic through dawn: no step larger than a smooth curve allows.
    float prev = LengthAt(c, 4.5f, 0, 12.0f).length;
    for (float h = 4.5f; h <= 7.5f; h += 0.01f)
    {
        const float cur = LengthAt(c, h, 0, 12.0f).length;
        CHECK(cur >= prev - 1e-5f);
        CHECK(cur - prev < 0.02f);
        prev = cur;
    }
}

static void TestGoldenHour()
{
    Config c = HardSwitch();
    c.goldenHourMultiplier = 3.0f;
    c.goldenHourLength = 1.0f;
    CHECK(Near(LengthAt(c, 6.0f, 0, 12.0f).golden, 1.0f));
    CHECK(Near(LengthAt(c, 6.0f, 0, 12.0f).length, 6.0f));   // day x2 * golden x3
    CHECK(Near(LengthAt(c, 20.0f, 0, 12.0f).length, 3.0f));  // night x1 * golden x3
    CHECK(Near(LengthAt(c, 12.0f, 0, 12.0f).golden, 0.0f));
    CHECK(Near(LengthAt(c, 6.6f, 0, 12.0f).golden, 0.0f));   // outside the 1h window
}

static void TestRealMinutes()
{
    Config c = HardSwitch();
    c.dayRealMinutes = 140.0f; // 14 h of day; vanilla 12x -> 70 real min -> x2
    c.nightRealMinutes = 20.0f; // 10 h of night; vanilla 50 min -> x0.4
    CHECK(Near(LengthAt(c, 12.0f, 0, 12.0f).length, 2.0f));
    CHECK(Near(LengthAt(c, 2.0f, 0, 12.0f).length, 0.4f));
    // Same targets on a game that runs its clock twice as fast need x2 more.
    CHECK(Near(LengthAt(c, 12.0f, 0, 24.0f).length, 4.0f));
}

static void TestSeasons()
{
    Config c = HardSwitch();
    c.seasonStrength = 0.5f;
    c.yearLengthDays = 120.0f;
    c.summerPeakDay = 15.0f;
    CHECK(Near(LengthAt(c, 12.0f, 15, 12.0f).length, 3.0f));  // x2 * 1.5 at midsummer
    CHECK(Near(LengthAt(c, 12.0f, 75, 12.0f).length, 1.0f));  // x2 * 0.5 at midwinter
    CHECK(Near(LengthAt(c, 2.0f, 75, 12.0f).length, 1.0f));   // nights unaffected
    CHECK(Near(LengthAt(c, 12.0f, 135, 12.0f).length, 3.0f)); // next year's summer
}

static void TestRateMeter()
{
    RateMeter m(12.0f);
    CHECK(!m.Calibrated());

    // 60 fps, clock running at 10 game-s per applied second.
    double applied = 0.0;
    double clock = 1000.0;
    for (int i = 0; i < 60 * 70; ++i)
    {
        applied += 1.0 / 60.0;
        clock += 10.0 / 60.0;
        m.Feed(applied, static_cast<int64_t>(clock));
    }
    CHECK(m.Calibrated());
    CHECK(Near(m.Rate(), 10.0f, 0.05f));

    // A sleep (8 h jump) restarts the window instead of skewing the rate.
    clock += 8 * 3600;
    for (int i = 0; i < 60 * 70; ++i)
    {
        applied += 1.0 / 60.0;
        clock += 10.0 / 60.0;
        m.Feed(applied, static_cast<int64_t>(clock));
    }
    CHECK(Near(m.Rate(), 10.0f, 0.05f));

    // Paused game: no applied time, no clock movement - nothing measured.
    RateMeter p(12.0f);
    for (int i = 0; i < 10000; ++i) p.Feed(5.0, 500);
    CHECK(!p.Calibrated());
    CHECK(Near(p.Rate(), 12.0f));
}

static void TestPattern()
{
    using mem::BytePattern;
    const std::vector<uint8_t> buf = {0x90, 0x48, 0x8B, 0x05, 0x11, 0x22, 0x33, 0x44, 0xC3,
                                      0x48, 0x8B, 0x05, 0xAA, 0xBB, 0xCC, 0xDD, 0x90, 0xCC};
    const uint8_t* b = buf.data();
    const uint8_t* e = b + buf.size();

    auto p = BytePattern::Parse("48 8B 05 ?? ?? ?? ?? C3");
    CHECK(p && p->size() == 8);
    CHECK(p && p->Find(b, e) == b + 1);

    auto q = BytePattern::Parse("48 8B 05 ? ? ? ? 90");
    CHECK(q && q->Find(b, e) == b + 9);

    auto leadingWild = BytePattern::Parse("?? 48 8B");
    CHECK(leadingWild && leadingWild->Find(b, e) == b);

    auto trailing = BytePattern::Parse("90 CC");
    CHECK(trailing && trailing->Find(b, e) == b + 16);

    auto none = BytePattern::Parse("DE AD");
    CHECK(none && none->Find(b, e) == nullptr);

    auto tooLong = BytePattern::Parse("90 CC ??"); // only fits past the end
    CHECK(tooLong && tooLong->Find(b, e) == nullptr);

    CHECK(!BytePattern::Parse("?? ??"));  // nothing to anchor on
    CHECK(!BytePattern::Parse("4"));      // malformed
    CHECK(!BytePattern::Parse("ZZ 00"));  // malformed
}

int main()
{
    TestDayNight();
    TestWrappingWindow();
    TestTransitionIsSmooth();
    TestGoldenHour();
    TestRealMinutes();
    TestSeasons();
    TestRateMeter();
    TestPattern();
    std::printf(g_failures ? "%d failure(s)\n" : "all tests passed\n", g_failures);
    return g_failures;
}
