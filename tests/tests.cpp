// Host-side unit tests for the game-independent logic. No framework: build
// with -DLONGERDAYS_TESTS=ON and run longerdays_tests (exit code = failures).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/pattern.h"
#include "../src/rate_meter.h"
#include "../src/schedule.h"
#include "../src/sigtools.h"
#include "../src/offsets.h"

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

// The field-time tick as shipped in game build 1.0.0.2976 (2.03.02+), copied
// from CrimsonDesert.exe: the tail of the previous function, its int3
// padding, then the tick. This build saves r12 in the prologue, which is what
// broke the 2.03.00 signature.
static const uint8_t kTick2976[] = {
    0x74, 0x24, 0x68, 0x48, 0x83, 0xC4, 0x20, 0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5F,
    0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18,
    0x4C, 0x89, 0x64, 0x24, 0x20, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC, 0x48, 0x83,
    0xEC, 0x70, 0x48, 0x8B, 0xF9, 0xC5, 0xF2, 0x58, 0x41, 0x2C, 0xC5, 0xFA, 0x11, 0x41, 0x2C,
    0x80, 0x79, 0x28, 0x00, 0x0F, 0x84, 0xF0, 0x02, 0x00, 0x00, 0x80, 0x79, 0x2B, 0x00, 0x0F,
    0x85, 0xE6, 0x02, 0x00, 0x00, 0x48, 0x8B};
constexpr size_t kTick2976Start = 29; // first byte of the tick function

static void TestTickSignatures()
{
    using mem::BytePattern;
    // Place the bytes so the function start lands on a 16-byte boundary, as
    // it does in the game.
    alignas(16) uint8_t buf[160] = {};
    const size_t shift = 32 - kTick2976Start;
    std::memcpy(buf + shift, kTick2976, sizeof(kTick2976));
    const uint8_t* b = buf;
    const uint8_t* e = buf + sizeof(buf);
    const uint8_t* start = buf + shift + kTick2976Start;

    // The old exact signature no longer matches this build...
    auto exact = BytePattern::Parse(offsets::kSig_FieldTimeTick_Exact);
    CHECK(exact && exact->Find(b, e) == nullptr);

    // ...the body signature does, and walking back finds the real start.
    auto body = BytePattern::Parse(offsets::kSig_FieldTimeTick_Body);
    const uint8_t* hit = body ? body->Find(b, e) : nullptr;
    CHECK(hit != nullptr);
    CHECK(hit && mem::FunctionStartBefore(hit, b, offsets::kMaxPrologueBytes) == start);

    // The loose signature matches too, with the add and store at one offset.
    auto loose = BytePattern::Parse(offsets::kSig_FieldTimeTick_Loose);
    const uint8_t* lh = loose ? loose->Find(b, e) : nullptr;
    CHECK(lh != nullptr);
    CHECK(lh && lh[offsets::kOff_Loose_AddDisp] == lh[offsets::kOff_Loose_StoreDisp]);
    CHECK(lh && mem::FunctionStartBefore(lh, b, offsets::kMaxPrologueBytes) == start);

    // Not found when the padding is further back than allowed.
    CHECK(hit && mem::FunctionStartBefore(hit, b, 8) == nullptr);
}

static void TestSigTools()
{
    const uint8_t span[] = {0xC5, 0xFC, 0x10, 0x05, 1, 2, 3, 4, 0xEB, 0x08, 0xC5, 0xFC, 0x10, 0x05};
    const uint8_t vmovups[] = {0xC5, 0xFC, 0x10, 0x05};
    CHECK(mem::FindInSpan(span, sizeof(span), vmovups, 4, 0) == 0);
    CHECK(mem::FindInSpan(span, sizeof(span), vmovups, 4, 1) == 10);
    CHECK(mem::FindInSpan(span, sizeof(span), vmovups, 4, 2) == -1);

    CHECK(Near(mem::HourDistance(23.5f, 0.5f), 1.0f));
    CHECK(Near(mem::HourDistance(0.5f, 23.5f), 1.0f));
    CHECK(Near(mem::HourDistance(6.0f, 18.0f), 12.0f));
    CHECK(Near(mem::HourDistance(12.25f, 12.0f), 0.25f));
}

// The game's /settimeofdaylowerlimit handler in build 1.0.0.2976: it gets the
// TimeOfDay manager through engine vtable+0x60 and stores the lower limit at
// +0x3B4. The mod reads both numbers from here instead of hard-coding them.
static const uint8_t kLowerLimitHandler2976[] = {
    0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x83, 0x7A, 0x08, 0x02, 0x48, 0x8B, 0xFA, 0x72, 0x2F, 0x48,
    0x8B, 0x09, 0x48, 0x89, 0x5C, 0x24, 0x30, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x60, 0x48, 0x8B, 0x0F,
    0x48, 0x8B, 0xD8, 0x48, 0x8B, 0x49, 0x08, 0xFF, 0x15, 0x9B, 0x66, 0x86, 0x01, 0xC5, 0xFB, 0x5A,
    0xC8, 0xC5, 0xFA, 0x11, 0x8B, 0xB4, 0x03, 0x00, 0x00, 0x48, 0x8B, 0x5C, 0x24, 0x30, 0x48, 0x83,
    0xC4, 0x20, 0x5F, 0xC3};

static void TestTodLimitHandler()
{
    const uint8_t* b = kLowerLimitHandler2976;
    const uint8_t* e = b + sizeof(kLowerLimitHandler2976);
    auto p = mem::BytePattern::Parse(offsets::kSig_TodLimitHandler);
    const uint8_t* hit = p ? p->Find(b, e) : nullptr;
    CHECK(hit != nullptr);
    if (!hit) return;
    CHECK(hit[offsets::kOff_TodLimitHandler_Slot] == 0x60);
    int32_t lower = 0;
    std::memcpy(&lower, hit + offsets::kOff_TodLimitHandler_Limit, 4);
    CHECK(lower == 0x3B4);
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
    TestTickSignatures();
    TestSigTools();
    TestTodLimitHandler();
    std::printf(g_failures ? "%d failure(s)\n" : "all tests passed\n", g_failures);
    return g_failures;
}
