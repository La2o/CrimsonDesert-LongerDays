#include "scanner.h"

#include <windows.h>
#include <algorithm>
#include <utility>
#include <vector>

#include "pattern.h"

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
        constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                    PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        constexpr DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY |
                                    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

        bool Query(uintptr_t addr, MEMORY_BASIC_INFORMATION& mbi)
        {
            return VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == sizeof(mbi) &&
                   mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
        }

        // The game is packed, so section headers are not trusted. Walk what is
        // actually committed and readable in the image instead, joining
        // neighbouring regions so a signature that straddles a protection
        // boundary is still one contiguous span.
        std::vector<std::pair<const uint8_t*, const uint8_t*>> ReadableSpans()
        {
            std::vector<std::pair<const uint8_t*, const uint8_t*>> spans;
            const Module& mod = GameModule();
            const uintptr_t end = mod.base + mod.size;
            MEMORY_BASIC_INFORMATION mbi{};
            for (uintptr_t a = mod.base; a < end;)
            {
                if (VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi)) != sizeof(mbi) ||
                    mbi.RegionSize == 0)
                    break;
                const auto lo = reinterpret_cast<const uint8_t*>(mbi.BaseAddress);
                const auto hi = reinterpret_cast<const uint8_t*>(
                    std::min<uintptr_t>(reinterpret_cast<uintptr_t>(lo) + mbi.RegionSize, end));
                const bool ok = mbi.State == MEM_COMMIT && (mbi.Protect & kReadable) &&
                                !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
                if (ok)
                {
                    if (!spans.empty() && spans.back().second == lo) spans.back().second = hi;
                    else spans.emplace_back(lo, hi);
                }
                a = reinterpret_cast<uintptr_t>(lo) + mbi.RegionSize;
            }
            return spans;
        }

        std::vector<uintptr_t> Scan(std::string_view text, size_t maxResults)
        {
            std::vector<uintptr_t> hits;
            const auto pattern = BytePattern::Parse(text);
            if (!pattern || !GameModule().base) return hits;

            for (const auto& [lo, hi] : ReadableSpans())
            {
                for (const uint8_t* from = lo; const uint8_t* hit = pattern->Find(from, hi); from = hit + 1)
                {
                    hits.push_back(reinterpret_cast<uintptr_t>(hit));
                    if (hits.size() >= maxResults) return hits;
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

    std::vector<uintptr_t> FindAll(std::string_view pattern, size_t maxCount)
    {
        return Scan(pattern, maxCount);
    }

    size_t CountMatches(std::string_view pattern, size_t maxCount)
    {
        return Scan(pattern, maxCount).size();
    }

    bool IsWritable(uintptr_t addr, size_t len)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!Query(addr, mbi) || !(mbi.Protect & kWritable)) return false;
        return addr + len <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }

    bool IsReadable(uintptr_t addr, size_t len)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!Query(addr, mbi) || !(mbi.Protect & kReadable)) return false;
        return addr + len <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
}
