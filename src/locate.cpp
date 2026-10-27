#include "locate.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <cstring>

#include "offsets.h"
#include "scanner.h"
#include "sigtools.h"

namespace longerdays::locate
{
    using namespace offsets;

    namespace
    {
        uintptr_t ReadPtr(uintptr_t a)
        {
            if (!mem::IsReadable(a, 8)) return 0;
            const uintptr_t v = *reinterpret_cast<volatile const uintptr_t*>(a);
            return v >= kMinPointer ? v : 0;
        }

        float ReadF32(uintptr_t a)
        {
            float v;
            std::memcpy(&v, reinterpret_cast<const void*>(a), 4);
            return v;
        }

        // Function start for a hit inside its body. The unwind table is the
        // authority when the game registers one; otherwise walk back to the
        // alignment padding.
        uintptr_t FunctionStart(uintptr_t anchor)
        {
            const mem::Module& mod = mem::GameModule();
            DWORD64 imageBase = 0;
            if (const RUNTIME_FUNCTION* rf = RtlLookupFunctionEntry(anchor, &imageBase, nullptr))
            {
                const uintptr_t start = static_cast<uintptr_t>(imageBase) + rf->BeginAddress;
                if (start <= anchor && anchor - start <= kMaxPrologueBytes && mod.Contains(start))
                    return start;
            }
            if (!mem::IsReadable(anchor - kMaxPrologueBytes - 1, kMaxPrologueBytes + 1)) return 0;
            const auto* p = mem::FunctionStartBefore(reinterpret_cast<const uint8_t*>(anchor),
                                                     reinterpret_cast<const uint8_t*>(mod.base),
                                                     kMaxPrologueBytes);
            return reinterpret_cast<uintptr_t>(p);
        }

        bool PlausibleHour(float h) { return std::isfinite(h) && h >= 0.0f && h <= 24.0f; }

        // Is (hour, lower, upper) at `at` a live, unclamped-or-clamped TimeOfDay
        // that agrees with the field clock?
        bool TodAgrees(uintptr_t at, float clockHour, float tolerance)
        {
            if (!mem::IsWritable(at, 12)) return false;
            const float cur = ReadF32(at), lo = ReadF32(at + 4), hi = ReadF32(at + 8);
            return PlausibleHour(cur) && PlausibleHour(lo) && PlausibleHour(hi) && lo <= hi &&
                   mem::HourDistance(cur, clockHour) <= tolerance;
        }
    }

    Hit FieldTimeTick()
    {
        if (const uintptr_t a = mem::FindPattern(kSig_FieldTimeTick_Exact))
            return {a, "exact signature (build 2.03.00)"};

        // A body signature must be unique: hooking the wrong function is the
        // one thing worse than not hooking at all.
        auto hits = mem::FindAll(kSig_FieldTimeTick_Body, 2);
        if (hits.size() == 1)
            if (const uintptr_t s = FunctionStart(hits[0]))
                return {s, "function body"};

        hits = mem::FindAll(kSig_FieldTimeTick_Loose, 2);
        if (hits.size() == 1)
        {
            const auto* p = reinterpret_cast<const uint8_t*>(hits[0]);
            if (p[kOff_Loose_AddDisp] == p[kOff_Loose_StoreDisp])
                if (const uintptr_t s = FunctionStart(hits[0]))
                    return {s, "loose function body"};
        }
        return {};
    }

    Hit FieldClock()
    {
        static const char* const kHow[] = {"exact signature (build 2.03.00)", "realm select",
                                           "realm test"};
        static_assert(std::size(kHow) == std::size(kSig_FieldTimeRealm));
        constexpr uint8_t kVmovups[] = {0xC5, 0xFC, 0x10, 0x05};

        const mem::Module& mod = mem::GameModule();
        for (size_t i = 0; i < std::size(kSig_FieldTimeRealm); ++i)
        {
            const RealmSig& sig = kSig_FieldTimeRealm[i];
            const auto hits = mem::FindAll(sig.pattern, 2);
            // The exact signature has always been unique; looser ones must be.
            if (hits.empty() || (i > 0 && hits.size() != 1)) continue;

            const uintptr_t instr = hits[0] + sig.clientVmovups;
            if (std::memcmp(reinterpret_cast<const void*>(instr), kVmovups, sizeof(kVmovups)) != 0)
                continue;
            const uintptr_t clock = mem::ResolveRipAt(instr, kLen_FieldTime_Vmovups);
            if (mod.Contains(clock) && (clock & 3) == 0 && mem::IsReadable(clock, 16))
                return {clock, kHow[i]};
        }
        return {};
    }

    std::vector<uintptr_t> TodEngineGlobals()
    {
        std::vector<uintptr_t> out;
        const mem::Module& mod = mem::GameModule();
        for (const uintptr_t hit : mem::FindAll(kSig_TodEngineGlobal, kMaxTodCandidates))
        {
            const uintptr_t g = mem::ResolveRipAt(hit + kOff_TodEngineGlobal_Mov, kLen_TodEngineGlobal_Mov);
            if (mod.Contains(g) && (g & 7) == 0 && std::find(out.begin(), out.end(), g) == out.end())
                out.push_back(g);
        }
        return out;
    }

    uintptr_t TodManager(const TodBinding& b)
    {
        if (!b) return 0;
        const uintptr_t engine = ReadPtr(b.global);
        if (!engine) return 0;
        const uintptr_t mgr = ReadPtr(engine + b.managerOff);
        if (!mgr || !mem::IsWritable(mgr + b.hourOff, 12)) return 0;
        return mgr;
    }

    bool BindTod(const std::vector<uintptr_t>& globals, float clockHour, TodBinding& out)
    {
        // Known offsets first. The sun and the field clock run off different
        // accumulators, so allow them to have drifted a little apart.
        for (const uintptr_t g : globals)
        {
            const TodBinding b{g, kOff_Tod_Manager, kOff_Tod_CurrentHour, false};
            if (const uintptr_t mgr = TodManager(b))
                if (TodAgrees(mgr + b.hourOff, clockHour, 1.0f))
                {
                    out = b;
                    return true;
                }
        }

        // Offsets moved: search. Stricter tolerance, and the answer must be
        // unique - a stray float that happens to look like an hour is not
        // something to write into every frame.
        TodBinding found{};
        int matches = 0;
        for (const uintptr_t g : globals)
        {
            const uintptr_t engine = ReadPtr(g);
            if (!engine || !mem::IsReadable(engine + kTodManagerSearchLo, kTodManagerSearchHi - kTodManagerSearchLo))
                continue;
            for (uintptr_t mo = kTodManagerSearchLo; mo < kTodManagerSearchHi; mo += 8)
            {
                const uintptr_t mgr = ReadPtr(engine + mo);
                if (!mgr || !mem::IsReadable(mgr + kTodHourSearchLo, kTodHourSearchHi - kTodHourSearchLo + 12))
                    continue;
                for (uintptr_t ho = kTodHourSearchLo; ho < kTodHourSearchHi; ho += 4)
                {
                    if (!TodAgrees(mgr + ho, clockHour, 0.25f)) continue;
                    // Skip the all-zero / all-equal noise that is common in
                    // fresh objects: a real time of day has a range around it.
                    const float lo = ReadF32(mgr + ho + 4), hi = ReadF32(mgr + ho + 8);
                    if (hi - lo < 1.0f) continue;
                    found = {g, mo, ho, true};
                    if (++matches > 1) return false;
                }
            }
        }
        if (matches != 1) return false;
        out = found;
        return true;
    }
}
