#include "scanner.h"

#include <windows.h>
#include <vector>

namespace longerdays::mem
{
    const Module& GameModule()
    {
        static const Module mod = []
        {
            Module m{};
            const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return m;
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) return m;
            m.base = base;
            m.size = nt->OptionalHeader.SizeOfImage;
            return m;
        }();
        return mod;
    }

    namespace
    {
        struct Pattern
        {
            std::vector<uint8_t> bytes;
            std::vector<bool>    mask; // true = must match
        };

        int Nibble(char c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        Pattern Parse(std::string_view s)
        {
            Pattern p;
            for (size_t i = 0; i < s.size();)
            {
                if (s[i] == ' ') { ++i; continue; }
                if (s[i] == '?')
                {
                    p.bytes.push_back(0);
                    p.mask.push_back(false);
                    i += (i + 1 < s.size() && s[i + 1] == '?') ? 2 : 1;
                    continue;
                }
                const int hi = Nibble(s[i]);
                const int lo = i + 1 < s.size() ? Nibble(s[i + 1]) : -1;
                if (hi < 0 || lo < 0) { ++i; continue; }
                p.bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
                p.mask.push_back(true);
                i += 2;
            }
            return p;
        }

        bool Readable(DWORD protect)
        {
            if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
            return (protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                               PAGE_EXECUTE_WRITECOPY)) != 0;
        }

        // The game is packed, so section flags are not trusted: walk the
        // committed readable spans of the image instead, merging neighbours so
        // a pattern straddling a protection boundary is still found.
        std::vector<std::pair<uintptr_t, uintptr_t>> Spans()
        {
            std::vector<std::pair<uintptr_t, uintptr_t>> spans;
            const Module& mod = GameModule();
            const uintptr_t end = mod.base + mod.size;
            MEMORY_BASIC_INFORMATION mbi{};
            for (uintptr_t a = mod.base; a < end;)
            {
                if (VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi)) != sizeof(mbi) ||
                    mbi.RegionSize == 0)
                    break;
                const uintptr_t lo = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
                const uintptr_t hi = lo + mbi.RegionSize < end ? lo + mbi.RegionSize : end;
                if (mbi.State == MEM_COMMIT && Readable(mbi.Protect))
                {
                    if (!spans.empty() && spans.back().second == lo) spans.back().second = hi;
                    else spans.emplace_back(lo, hi);
                }
                a = lo + mbi.RegionSize;
            }
            return spans;
        }

        std::vector<uintptr_t> Scan(std::string_view pattern, size_t maxResults)
        {
            std::vector<uintptr_t> hits;
            const Pattern p = Parse(pattern);
            const size_t n = p.bytes.size();
            if (n == 0 || !GameModule().base) return hits;

            for (const auto& [lo, hi] : Spans())
            {
                if (hi - lo < n) continue;
                const auto* cur  = reinterpret_cast<const uint8_t*>(lo);
                const auto* last = reinterpret_cast<const uint8_t*>(hi - n);
                for (; cur <= last; ++cur)
                {
                    if (cur[0] != p.bytes[0] && p.mask[0]) continue;
                    size_t i = 0;
                    while (i < n && (!p.mask[i] || cur[i] == p.bytes[i])) ++i;
                    if (i == n)
                    {
                        hits.push_back(reinterpret_cast<uintptr_t>(cur));
                        if (hits.size() >= maxResults) return hits;
                    }
                }
            }
            return hits;
        }
    }

    uintptr_t FindPattern(std::string_view pattern)
    {
        const auto hits = Scan(pattern, 1);
        return hits.empty() ? 0 : hits.front();
    }

    size_t CountMatches(std::string_view pattern, size_t maxCount)
    {
        return Scan(pattern, maxCount).size();
    }

    bool IsWritable(uintptr_t addr, size_t len)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) != sizeof(mbi))
            return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return false;
        if (!(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return false;
        return addr + len <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
}
