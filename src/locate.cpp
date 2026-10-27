#include "locate.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

#include "log.h"
#include "offsets.h"
#include "scanner.h"
#include "sigtools.h"

namespace longerdays::locate
{
    using namespace offsets;

    namespace
    {
        // Reads that fail cleanly on a bad address, without SEH or a
        // VirtualQuery per value. One call can cover a whole object window.
        bool SafeRead(uintptr_t a, void* out, size_t n)
        {
            SIZE_T got = 0;
            return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(a), out, n, &got) && got == n;
        }

        bool LooksLikeHeapPointer(uintptr_t v)
        {
            return v >= kMinPointer && v < 0x00007FFFFFFFFFFFull && (v & 7) == 0;
        }

        uintptr_t ReadPtr(uintptr_t a)
        {
            uintptr_t v = 0;
            return SafeRead(a, &v, sizeof(v)) && LooksLikeHeapPointer(v) ? v : 0;
        }

        // A C++ object of the game's: its first qword is a vtable in the exe.
        bool HasGameVtable(uintptr_t obj)
        {
            uintptr_t vt = 0;
            return SafeRead(obj, &vt, sizeof(vt)) && mem::GameModule().Contains(vt);
        }

        float F32At(const uint8_t* p) { float v; std::memcpy(&v, p, 4); return v; }

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

        // (hour, lower, upper) that could be a live TimeOfDay agreeing with the
        // field clock.
        bool TodAgrees(float cur, float lo, float hi, float clockHour, float tolerance)
        {
            return PlausibleHour(cur) && PlausibleHour(lo) && PlausibleHour(hi) && lo <= hi &&
                   mem::HourDistance(cur, clockHour) <= tolerance;
        }

        bool ReadTriple(uintptr_t at, float& cur, float& lo, float& hi)
        {
            float v[3];
            if (!SafeRead(at, v, sizeof(v))) return false;
            cur = v[0]; lo = v[1]; hi = v[2];
            return true;
        }

        // The `mov cs:<global>, reg` at `p`, if that's what it is.
        uintptr_t RipStoreTarget(uintptr_t p)
        {
            uint8_t b[3];
            if (!SafeRead(p, b, sizeof(b))) return 0;
            const bool rex = b[0] == 0x48 || b[0] == 0x4C;
            const bool ripRelative = (b[2] & 0xC7) == 0x05;
            if (!rex || b[1] != 0x89 || !ripRelative) return 0;
            return mem::ResolveRipAt(p, kLen_TodEngineGlobal_Mov);
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
        for (const TodGlobalSig& sig : kSig_TodEngineGlobal)
        {
            for (const uintptr_t hit : mem::FindAll(sig.pattern, kMaxTodCandidates))
            {
                const uintptr_t g = RipStoreTarget(hit + sig.movOffset);
                if (g && mod.Contains(g) && (g & 7) == 0 && std::find(out.begin(), out.end(), g) == out.end())
                    out.push_back(g);
                if (out.size() >= static_cast<size_t>(kMaxTodCandidates)) return out;
            }
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

    std::vector<TodBinding> TodCandidates(const std::vector<uintptr_t>& globals, float clockHour, bool search)
    {
        std::vector<TodBinding> out;

        // Known offsets. The sun and the field clock run off different
        // accumulators, so allow them to have drifted a little apart.
        for (const uintptr_t g : globals)
        {
            const uintptr_t engine = ReadPtr(g);
            if (!engine || !HasGameVtable(engine)) continue;
            const uintptr_t mgr = ReadPtr(engine + kOff_Tod_Manager);
            if (!mgr || !HasGameVtable(mgr)) continue;
            TodBinding b{g, kOff_Tod_Manager, kOff_Tod_CurrentHour, false};
            if (ReadTriple(mgr + b.hourOff, b.hour, b.lower, b.upper) &&
                TodAgrees(b.hour, b.lower, b.upper, clockHour, 1.0f))
                out.push_back(b);
        }
        if (!out.empty() || !search) return out;

        // Offsets moved: look for them near where they used to be. Each
        // object window is read in one go.
        constexpr size_t kEngineSpan = kTodManagerSearchHi - kTodManagerSearchLo;
        constexpr size_t kMgrSpan    = kTodHourSearchHi - kTodHourSearchLo + 12;
        static uint8_t engineBuf[kEngineSpan];
        static uint8_t mgrBuf[kMgrSpan];
        int searched = 0;
        for (const uintptr_t g : globals)
        {
            const uintptr_t engine = ReadPtr(g);
            if (!engine || !HasGameVtable(engine)) continue;
            if (++searched > kMaxTodSearchGlobals) break;
            if (!SafeRead(engine + kTodManagerSearchLo, engineBuf, kEngineSpan)) continue;

            for (uintptr_t mo = 0; mo < kEngineSpan; mo += 8)
            {
                uintptr_t mgr;
                std::memcpy(&mgr, engineBuf + mo, 8);
                if (!LooksLikeHeapPointer(mgr) || !HasGameVtable(mgr)) continue;
                if (!SafeRead(mgr + kTodHourSearchLo, mgrBuf, kMgrSpan)) continue;
                for (size_t ho = 0; ho + 12 <= kMgrSpan; ho += 4)
                {
                    const float cur = F32At(mgrBuf + ho), lo = F32At(mgrBuf + ho + 4), hi = F32At(mgrBuf + ho + 8);
                    // Tighter here, and skip the all-equal noise common in
                    // fresh objects: a real time of day has a range around it.
                    if (!TodAgrees(cur, lo, hi, clockHour, 0.5f) || hi - lo < 1.0f) continue;
                    out.push_back({g, kTodManagerSearchLo + mo, kTodHourSearchLo + ho, true, cur, lo, hi});
                    if (out.size() >= 64) return out;
                }
            }
        }
        return out;
    }

    std::vector<TodBinding> StillMoving(const std::vector<TodBinding>& before, float clockHour)
    {
        std::vector<TodBinding> out;
        for (TodBinding b : before)
        {
            const uintptr_t mgr = TodManager(b);
            float cur, lo, hi;
            if (!mgr || !ReadTriple(mgr + b.hourOff, cur, lo, hi)) continue;
            // A few seconds of vanilla time is a few thousandths of an hour.
            float moved = cur - b.hour;
            if (moved < -12.0f) moved += 24.0f; // midnight wrap
            const bool advanced = moved > 0.0003f && moved < 0.2f;
            if (advanced && lo == b.lower && hi == b.upper && TodAgrees(cur, lo, hi, clockHour, 1.0f))
            {
                b.hour = cur;
                out.push_back(b);
            }
        }
        return out;
    }

    void LogTodDiagnostics(const std::vector<uintptr_t>& globals, float clockHour)
    {
        const mem::Module& mod = mem::GameModule();
        LOG("Sun diagnostics: clock hour %.3f, %zu engine candidate(s). Known chain "
            "(engine+0x%llX -> manager+0x%llX) for those with a game object behind them:",
            clockHour, globals.size(), static_cast<unsigned long long>(kOff_Tod_Manager),
            static_cast<unsigned long long>(kOff_Tod_CurrentHour));
        int shown = 0;
        for (const uintptr_t g : globals)
        {
            const uintptr_t engine = ReadPtr(g);
            if (!engine || !HasGameVtable(engine)) continue;
            const uintptr_t mgr = ReadPtr(engine + kOff_Tod_Manager);
            float cur = NAN, lo = NAN, hi = NAN;
            const bool objOk = mgr && HasGameVtable(mgr);
            if (objOk) ReadTriple(mgr + kOff_Tod_CurrentHour, cur, lo, hi);
            LOG("  exe+0x%llX: manager %s, hour %.3f limits %.3f..%.3f",
                static_cast<unsigned long long>(g - mod.base),
                !mgr ? "null" : objOk ? "ok" : "not an object", cur, lo, hi);
            if (++shown >= 12) break;
        }
        if (!shown) LOG("  (no candidate points at a game object yet)");
    }
}
