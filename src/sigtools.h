#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// Small, platform-free helpers for turning a signature hit into the thing we
// actually want. Header-only so the unit tests can run them on real game bytes.
namespace longerdays::mem
{
    // Start of the function that contains `anchor`, found by walking back to
    // the nearest 16-byte-aligned address that follows int3/ret padding (MSVC
    // aligns x64 functions to 16 and pads between them with 0xCC). Never walks
    // below `lowest` or further than `maxBack` bytes. nullptr if not found.
    //
    // This lets a signature anchor on a function's distinctive body and
    // survive the prologue changes a recompile tends to make (an extra saved
    // register, a bigger stack frame).
    inline const uint8_t* FunctionStartBefore(const uint8_t* anchor, const uint8_t* lowest, size_t maxBack)
    {
        for (size_t back = 0; back <= maxBack; ++back)
        {
            const uint8_t* p = anchor - back;
            if (p <= lowest) return nullptr;
            if ((reinterpret_cast<uintptr_t>(p) & 15) != 0) continue;
            if (p[-1] == 0xCC || p[-1] == 0xC3) return p;
        }
        return nullptr;
    }

    // Offset of the `nth` (0-based) occurrence of `needle` in [p, p + len),
    // or -1. Used to find an instruction inside a matched signature instead of
    // trusting a hard-coded offset.
    inline ptrdiff_t FindInSpan(const uint8_t* p, size_t len, const uint8_t* needle, size_t needleLen, int nth)
    {
        for (size_t i = 0; i + needleLen <= len; ++i)
            if (std::memcmp(p + i, needle, needleLen) == 0 && nth-- == 0)
                return static_cast<ptrdiff_t>(i);
        return -1;
    }

    // Shortest signed distance between two hours on a 24h dial.
    inline float HourDistance(float a, float b)
    {
        float d = a - b;
        while (d > 12.0f) d -= 24.0f;
        while (d < -12.0f) d += 24.0f;
        return d < 0 ? -d : d;
    }
}
