#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace longerdays::mem
{
    // Base and SizeOfImage of the running game executable.
    struct Module
    {
        uintptr_t base = 0;
        size_t    size = 0;
        bool Contains(uintptr_t a) const { return a >= base && a < base + size; }
    };
    const Module& GameModule();

    // IDA-style pattern scan ("48 8B 05 ?? ?? ?? ??") over the game module's
    // committed, readable pages. Returns the first match or 0.
    uintptr_t FindPattern(std::string_view pattern);

    // Number of matches, stopping at maxCount. Used to warn on ambiguity.
    size_t CountMatches(std::string_view pattern, size_t maxCount = 4);

    // Target of a RIP-relative instruction whose disp32 is its last 4 bytes.
    inline uintptr_t ResolveRipAt(uintptr_t instr, int instrLen)
    {
        const int32_t disp = *reinterpret_cast<const int32_t*>(instr + instrLen - 4);
        return instr + instrLen + disp;
    }

    // True if [addr, addr+len) is committed and readable+writable. Used instead
    // of SEH so the mod builds the same under MSVC and MinGW.
    bool IsWritable(uintptr_t addr, size_t len);
}
