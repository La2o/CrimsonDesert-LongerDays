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

        // Decodes a trivial field getter: `mov rax,[rcx+d]` or `lea rax,[rcx+d]`
        // followed by ret, optionally behind one jmp thunk.
        bool DecodeGetter(uintptr_t fn, uintptr_t& off, bool& embedded)
        {
            const mem::Module& mod = mem::GameModule();
            for (int hop = 0; hop < 2 && mod.Contains(fn); ++hop)
            {
                uint8_t b[8];
                if (!SafeRead(fn, b, sizeof(b))) return false;
                if (b[0] == 0xE9) // jmp rel32 thunk
                {
                    int32_t rel;
                    std::memcpy(&rel, b + 1, 4);
                    fn = fn + 5 + rel;
                    continue;
                }
                if (b[0] != 0x48 || (b[1] != 0x8B && b[1] != 0x8D)) return false;
                embedded = b[1] == 0x8D;
                if (b[2] == 0x81 && b[7] == 0xC3) { int32_t d; std::memcpy(&d, b + 3, 4); off = static_cast<uintptr_t>(d); return d >= 0; }
                if (b[2] == 0x41 && b[4] == 0xC3) { off = b[3]; return true; }
                return false;
            }
            return false;
        }

        // Manager behind `engine` via the getter in its vtable at `slot`.
        bool ManagerVia(uintptr_t engine, int slot, uintptr_t& managerOff, bool& embedded)
        {
            uintptr_t vt = 0, fn = 0;
            if (!SafeRead(engine, &vt, 8) || !mem::GameModule().Contains(vt)) return false;
            if (!SafeRead(vt + static_cast<uintptr_t>(slot), &fn, 8)) return false;
            return DecodeGetter(fn, managerOff, embedded);
        }

        uintptr_t ManagerAt(uintptr_t engine, uintptr_t managerOff, bool embedded)
        {
            return embedded ? engine + managerOff : ReadPtr(engine + managerOff);
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

    TodLayout LocateTodLayout()
    {
        const auto hits = mem::FindAll(kSig_TodLimitHandler, 4);
        if (hits.size() != 2) return {};
        int slot[2];
        int32_t lim[2];
        for (int i = 0; i < 2; ++i)
        {
            const auto* p = reinterpret_cast<const uint8_t*>(hits[i]);
            slot[i] = p[kOff_TodLimitHandler_Slot];
            std::memcpy(&lim[i], p + kOff_TodLimitHandler_Limit, 4);
        }
        // One handler writes lower, its twin upper = lower + 4, via one getter.
        const int32_t lo = std::min(lim[0], lim[1]), hi = std::max(lim[0], lim[1]);
        if (slot[0] != slot[1] || slot[0] == 0 || (slot[0] & 7) || hi != lo + 4 || lo < 4 || lo > 0x10000)
            return {};
        return {slot[0], static_cast<uintptr_t>(lo)};
    }

    uintptr_t TodManager(const TodBinding& b)
    {
        if (!b) return 0;
        const uintptr_t engine = ReadPtr(b.global);
        if (!engine) return 0;
        const uintptr_t mgr = ManagerAt(engine, b.managerOff, b.embedded);
        if (!mgr || !mem::IsWritable(mgr + b.hourOff, 4) || !mem::IsWritable(mgr + b.lowerOff, 8)) return 0;
        return mgr;
    }

    std::vector<TodBinding> TodCandidates(const std::vector<uintptr_t>& globals, const TodLayout& layout,
                                          float clockHour, bool search)
    {
        std::vector<TodBinding> out;

        // The sun and the field clock run off different accumulators, so
        // allow them to have drifted a little apart.
        auto tryAt = [&](uintptr_t g, uintptr_t engine, uintptr_t managerOff, bool embedded, uintptr_t lowerOff)
        {
            const uintptr_t mgr = ManagerAt(engine, managerOff, embedded);
            if (!mgr || !HasGameVtable(mgr)) return false;
            TodBinding b{g, managerOff, embedded, lowerOff - 4, lowerOff, false};
            if (!ReadTriple(mgr + b.hourOff, b.hour, b.lower, b.upper) ||
                !TodAgrees(b.hour, b.lower, b.upper, clockHour, 1.0f))
                return false;
            out.push_back(b);
            return true;
        };

        for (const uintptr_t g : globals)
        {
            const uintptr_t engine = ReadPtr(g);
            if (!engine || !HasGameVtable(engine)) continue;
            // The way the game itself gets there, then the 2.03.00 layout.
            uintptr_t managerOff = 0;
            bool embedded = false;
            if (layout && ManagerVia(engine, layout.slot, managerOff, embedded) &&
                tryAt(g, engine, managerOff, embedded, layout.lowerOff))
                continue;
            tryAt(g, engine, kOff_Tod_Manager, false, kOff_Tod_LowerLimit);
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
                    const uintptr_t hourOff = kTodHourSearchLo + ho;
                    out.push_back({g, kTodManagerSearchLo + mo, false, hourOff, hourOff + 4, true, cur, lo, hi});
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

    void LogTodDiagnostics(const std::vector<uintptr_t>& globals, const TodLayout& layout, float clockHour)
    {
        const mem::Module& mod = mem::GameModule();
        LOG("Sun diagnostics: clock hour %.3f, %zu engine candidate(s), layout from game code: %s "
            "(getter slot 0x%X, lower limit +0x%llX).", clockHour, globals.size(),
            layout ? "yes" : "no", layout.slot, static_cast<unsigned long long>(layout.lowerOff));
        int shown = 0;
        for (const uintptr_t g : globals)
        {
            const uintptr_t engine = ReadPtr(g);
            if (!engine || !HasGameVtable(engine)) continue;
            uintptr_t managerOff = kOff_Tod_Manager, lowerOff = kOff_Tod_LowerLimit;
            bool embedded = false, viaGetter = false;
            if (layout && ManagerVia(engine, layout.slot, managerOff, embedded))
            {
                viaGetter = true;
                lowerOff = layout.lowerOff;
            }
            const uintptr_t mgr = ManagerAt(engine, managerOff, embedded);
            float cur = NAN, lo = NAN, hi = NAN;
            const bool objOk = mgr && HasGameVtable(mgr);
            if (objOk) ReadTriple(mgr + lowerOff - 4, cur, lo, hi);
            LOG("  exe+0x%llX: manager %s at engine%s0x%llX (%s), hour %.3f limits %.3f..%.3f",
                static_cast<unsigned long long>(g - mod.base), !mgr ? "null" : objOk ? "ok" : "not an object",
                embedded ? "+" : "->+", static_cast<unsigned long long>(managerOff),
                viaGetter ? "game getter" : "2.03.00 offset", cur, lo, hi);
            if (++shown >= 12) break;
        }
        if (!shown) LOG("  (no candidate points at a game object yet)");
    }
}
