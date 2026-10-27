#include "longer_days.h"

#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>

#include "config.h"
#include "log.h"
#include "offsets.h"
#include "rate_meter.h"
#include "scanner.h"
#include "schedule.h"
#include "version.h"

namespace longerdays
{
    using namespace offsets;

    namespace
    {
        // Community measurement of the vanilla clock (one real hour = 12
        // in-game hours). Only used until the RateMeter has measured it.
        constexpr float kAssumedVanillaRate = 12.0f;

        // --- Shared with the clock hook (any game thread) -------------------
        // The multiplier currently in force. AfterTick recomputes it; the hook
        // only reads it, so the hot path is one load and one divide.
        std::atomic<float> g_length{1.0f};

        // Clock-tick seconds actually handed to the game, summed for ONE tick
        // manager (the tick may run once per realm per frame; counting both
        // would double it). If that manager stops ticking - torn down on a
        // reload - another one is adopted after a run of misses.
        std::atomic<void*>    g_meterMgr{nullptr};
        std::atomic<double>   g_appliedSeconds{0.0};
        std::atomic<uint32_t> g_meterMisses{0};
        constexpr uint32_t    kAdoptAfterMisses = 600;

        // --- Owned by AfterTick (one thread at a time via g_tickBusy) --------
        std::atomic_flag g_tickBusy = ATOMIC_FLAG_INIT;
        std::wstring     g_iniPath;
        Config           g_cfg;
        bool             g_enabled = true; // INI Enabled, flipped by the hotkey
        RateMeter        g_meter{kAssumedVanillaRate};
        bool             g_reportedCalibration = false;
        ULONGLONG        g_nextIniCheck = 0;
        FILETIME         g_iniStamp{};
        bool             g_keyWasDown = false;
        int              g_lastLoggedHour = -1;

        bool      g_clampApplied = false;
        uintptr_t g_clampMgr     = 0;
        float     g_origLower    = 0.0f;
        float     g_origUpper    = 24.0f;

        // --- Resolved at load; 0 means that sub-feature is unavailable -------
        uintptr_t g_clock           = 0; // client-realm {day, hour, min, sec}
        uintptr_t g_todEngineGlobal = 0;

        using FieldTimeTick_t = void (*)(void* mgr, float delta, float unused);
        FieldTimeTick_t g_origTick   = nullptr;
        void*           g_tickTarget = nullptr;

        float ReadF32(uintptr_t a)           { float v; std::memcpy(&v, reinterpret_cast<const void*>(a), 4); return v; }
        void  WriteF32(uintptr_t a, float v) { std::memcpy(reinterpret_cast<void*>(a), &v, 4); }
        int   ReadI32(uintptr_t a)           { return *reinterpret_cast<volatile const int32_t*>(a); }

        struct Clock
        {
            int day = 0, hour = 0, minute = 0, second = 0;
            float   Hours() const        { return hour + minute / 60.0f + second / 3600.0f; }
            int64_t TotalSeconds() const { return ((int64_t{day} * 24 + hour) * 60 + minute) * 60 + second; }
        };

        // False when the clock didn't resolve or holds no sane time yet
        // (main menu, still loading).
        bool ReadClock(Clock& c)
        {
            if (!g_clock) return false;
            c.day    = ReadI32(g_clock + kOff_FieldTime_Day);
            c.hour   = ReadI32(g_clock + kOff_FieldTime_Hour);
            c.minute = ReadI32(g_clock + kOff_FieldTime_Min);
            c.second = ReadI32(g_clock + kOff_FieldTime_Sec);
            return c.day >= 0 && c.hour >= 0 && c.hour < 24 &&
                   c.minute >= 0 && c.minute < 60 && c.second >= 0 && c.second < 60;
        }

        // engine = *global, manager = *(engine + 0x2F8). 0 until in-world.
        uintptr_t TodManager()
        {
            if (!g_todEngineGlobal) return 0;
            const uintptr_t engine = *reinterpret_cast<volatile const uintptr_t*>(g_todEngineGlobal);
            if (engine < kMinPointer || !mem::IsWritable(engine + kOff_Tod_Manager, 8)) return 0;
            const uintptr_t mgr = *reinterpret_cast<volatile const uintptr_t*>(engine + kOff_Tod_Manager);
            if (mgr < kMinPointer || !mem::IsWritable(mgr + kOff_Tod_CurrentHour, 12)) return 0;
            return mgr;
        }

        void ReleaseSun(uintptr_t mgr)
        {
            // Restore only onto the manager we captured from; a different one
            // means the old was torn down and the new has its own limits.
            if (g_clampApplied && mgr && mgr == g_clampMgr)
            {
                WriteF32(mgr + kOff_Tod_LowerLimit, g_origLower);
                WriteF32(mgr + kOff_Tod_UpperLimit, g_origUpper);
            }
            g_clampApplied = false;
            g_clampMgr = 0;
        }

        // Pin the visible sun to the field clock: lower == upper == clock hour,
        // which the render manager clamps currentTimeOfDay into every frame.
        void SyncSun(bool haveClock, float hour)
        {
            const uintptr_t mgr = TodManager();
            if (!g_enabled || !g_cfg.syncSun || !haveClock || !mgr)
            {
                if (g_clampApplied) ReleaseSun(mgr);
                return;
            }
            if (!g_clampApplied || mgr != g_clampMgr)
            {
                g_origLower    = ReadF32(mgr + kOff_Tod_LowerLimit);
                g_origUpper    = ReadF32(mgr + kOff_Tod_UpperLimit);
                g_clampMgr     = mgr;
                g_clampApplied = true;
                LOG("Sun sync engaged (limits were %.2f..%.2f).", g_origLower, g_origUpper);
            }
            WriteF32(mgr + kOff_Tod_LowerLimit, hour);
            WriteF32(mgr + kOff_Tod_UpperLimit, hour);
        }

        void LogConfig(const char* what)
        {
            const Config& c = g_cfg;
            LOG("%s: enabled=%d | day x%.2f night x%.2f, day %.1fh-%.1fh, blend %.1fh | "
                "golden hour x%.2f over %.1fh | real minutes day=%.0f night=%.0f | "
                "seasons %.0f%% over %.0f days (peak day %.0f) | syncSun=%d toggleKey=0x%X",
                what, c.enabled, c.dayLength, c.nightLength, c.dayStartHour, c.dayEndHour,
                c.transitionHours, c.goldenHourMultiplier, c.goldenHourLength, c.dayRealMinutes,
                c.nightRealMinutes, c.seasonStrength * 100.0f, c.yearLengthDays, c.summerPeakDay,
                c.syncSun, c.toggleKey);
        }

        void PollIni()
        {
            const ULONGLONG now = GetTickCount64();
            if (now < g_nextIniCheck) return;
            g_nextIniCheck = now + 1000;

            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (!GetFileAttributesExW(g_iniPath.c_str(), GetFileExInfoStandard, &fad)) return;
            if (CompareFileTime(&fad.ftLastWriteTime, &g_iniStamp) == 0) return;
            g_iniStamp = fad.ftLastWriteTime;

            g_cfg = Config::Load(g_iniPath);
            g_enabled = g_cfg.enabled;
            LogConfig("Config reloaded");
        }

        void PollHotkey()
        {
            if (!g_cfg.toggleKey) return;
            DWORD pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(g_cfg.toggleKey) & 0x8000);
            if (down && !g_keyWasDown)
            {
                g_enabled = !g_enabled;
                LOG("Hotkey: Longer Days %s.", g_enabled ? "ON" : "OFF (vanilla day length)");
                MessageBeep(g_enabled ? MB_OK : MB_ICONHAND); // async, never stalls the game
            }
            g_keyWasDown = down;
        }

        // One line per in-game hour, so a player (or bug report) can see what
        // the mod is doing without an overlay.
        void LogHourly(const Clock& c, const SpeedSample& s)
        {
            if (c.hour == g_lastLoggedHour) return;
            g_lastLoggedHour = c.hour;
            const char* phase = s.golden > 0.5f ? "golden hour"
                              : s.dayness >= 0.99f ? "day"
                              : s.dayness <= 0.01f ? "night" : (c.hour < 12 ? "dawn" : "dusk");
            LOG("Day %d %02d:%02d %-11s time x%.2f slower%s | vanilla clock %.1fx real time (%s)",
                c.day, c.hour, c.minute, phase, g_enabled ? s.length : 1.0f,
                g_enabled ? "" : " [OFF]", g_meter.Rate(),
                g_meter.Calibrated() ? "measured" : "assumed");
        }

        // Housekeeping after each clock tick. Only one thread runs it at a
        // time; a thread that finds it busy just skips this frame.
        void AfterTick()
        {
            if (g_tickBusy.test_and_set(std::memory_order_acquire)) return;
            PollIni();
            PollHotkey();

            Clock clock;
            const bool haveClock = ReadClock(clock);
            if (haveClock)
            {
                g_meter.Feed(g_appliedSeconds.load(std::memory_order_relaxed), clock.TotalSeconds());
                if (g_meter.Calibrated() && !g_reportedCalibration)
                {
                    g_reportedCalibration = true;
                    LOG("Measured vanilla clock speed: %.2f in-game seconds per real second "
                        "(a full vanilla day = %.0f real minutes).",
                        g_meter.Rate(), 24.0f * 60.0f / g_meter.Rate());
                }
            }

            float length = 1.0f;
            if (haveClock)
            {
                const SpeedSample s = LengthAt(g_cfg, clock.Hours(), clock.day, g_meter.Rate());
                if (g_enabled) length = s.length;
                LogHourly(clock, s);
            }
            else if (g_enabled)
            {
                // No clock, no day/night: apply the day multiplier throughout.
                length = g_cfg.dayLength;
            }
            g_length.store(length, std::memory_order_relaxed);

            SyncSun(haveClock, haveClock ? clock.Hours() : 0.0f);
            g_tickBusy.clear(std::memory_order_release);
        }

        void HookedFieldTimeTick(void* mgr, float delta, float unused)
        {
            delta /= g_length.load(std::memory_order_relaxed);

            if (mgr == g_meterMgr.load(std::memory_order_relaxed))
            {
                g_appliedSeconds.store(g_appliedSeconds.load(std::memory_order_relaxed) + delta,
                                       std::memory_order_relaxed);
                g_meterMisses.store(0, std::memory_order_relaxed);
            }
            else if (g_meterMgr.load(std::memory_order_relaxed) == nullptr ||
                     g_meterMisses.fetch_add(1, std::memory_order_relaxed) > kAdoptAfterMisses)
            {
                g_meterMgr.store(mgr, std::memory_order_relaxed);
                g_meterMisses.store(0, std::memory_order_relaxed);
            }

            g_origTick(mgr, delta, unused);
            AfterTick();
        }

        uintptr_t ResolveInModule(uintptr_t match, uintptr_t off, int len)
        {
            const uintptr_t a = mem::ResolveRipAt(match + off, len);
            return mem::GameModule().Contains(a) ? a : 0;
        }

        // The game is packed: its code may not be readable the instant we are
        // loaded. Retry the essential signature for a while before giving up.
        uintptr_t WaitForPattern(const char* sig, int timeoutMs)
        {
            for (int waited = 0;; waited += 500)
            {
                if (const uintptr_t a = mem::FindPattern(sig)) return a;
                if (waited >= timeoutMs) return 0;
                Sleep(500);
            }
        }

        std::wstring SiblingPath(HMODULE self, const wchar_t* ext)
        {
            wchar_t buf[MAX_PATH] = {};
            GetModuleFileNameW(self, buf, MAX_PATH);
            std::wstring p = buf;
            const size_t dot = p.find_last_of(L'.');
            return (dot == std::wstring::npos ? p : p.substr(0, dot)) + ext;
        }
    }

    void Initialize(HMODULE self)
    {
        // The ASI loader in bin64 is also picked up by the game's helper
        // processes (crashpad_handler.exe, ...). Stay out of those entirely:
        // opening the log there would truncate the game's own log.
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const wchar_t* exeName = std::wcsrchr(exe, L'\\');
        exeName = exeName ? exeName + 1 : exe;
        if (_wcsicmp(exeName, L"CrimsonDesert.exe") != 0)
            return;

        g_iniPath = SiblingPath(self, L".ini");
        log::Open(SiblingPath(self, L".log"));
        LOG("Crimson Desert Longer Days v%s", LONGERDAYS_VERSION);
        LOG("Host process: %ls", exe);

        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(g_iniPath.c_str(), GetFileExInfoStandard, &fad))
            g_iniStamp = fad.ftLastWriteTime;
        else
            LOG("No INI found next to the .asi - using defaults.");
        g_cfg = Config::Load(g_iniPath);
        g_enabled = g_cfg.enabled;
        LogConfig("Config");

        const uintptr_t tick = WaitForPattern(kSig_FieldTimeTick, 60000);
        if (!tick)
        {
            LOG("ERROR: field-time tick signature NOT FOUND. This game version is not supported. "
                "Mod inactive.");
            return;
        }
        if (mem::CountMatches(kSig_FieldTimeTick) != 1)
            LOG("WARNING: field-time tick signature is ambiguous; hooking the first match.");

        if (const uintptr_t m = mem::FindPattern(kSig_FieldTimeRealm))
            g_clock = ResolveInModule(m, kOff_FieldTime_ClientVmovups, kLen_FieldTime_Vmovups);
        if (!g_clock)
            LOG("WARNING: field clock signature NOT FOUND - dawn/dusk, golden hour, seasons, "
                "real-time targets and sun sync are off; DayLengthMultiplier applies around the clock.");

        if (const uintptr_t m = mem::FindPattern(kSig_TodEngineGlobal))
            g_todEngineGlobal = ResolveInModule(m, kOff_TodEngineGlobal_Mov, kLen_TodEngineGlobal_Mov);
        if (!g_todEngineGlobal)
            LOG("WARNING: time-of-day engine signature NOT FOUND - the sun will keep vanilla speed "
                "while the clock is slowed.");

        if (MH_Initialize() != MH_OK)
        {
            LOG("ERROR: MinHook failed to initialize. Mod inactive.");
            return;
        }
        void* target = reinterpret_cast<void*>(tick);
        if (MH_CreateHook(target, reinterpret_cast<void*>(&HookedFieldTimeTick),
                          reinterpret_cast<void**>(&g_origTick)) != MH_OK ||
            MH_EnableHook(target) != MH_OK)
        {
            LOG("ERROR: failed to hook the field-time tick. Mod inactive.");
            MH_Uninitialize();
            return;
        }
        g_tickTarget = target;
        LOG("Ready. Clock tick hooked at exe+0x%llX.",
            static_cast<unsigned long long>(tick - mem::GameModule().base));
    }

    void Shutdown()
    {
        if (!g_tickTarget) return;
        MH_DisableHook(g_tickTarget);
        MH_RemoveHook(g_tickTarget);
        g_tickTarget = nullptr;
        MH_Uninitialize();

        while (g_tickBusy.test_and_set(std::memory_order_acquire)) Sleep(1);
        ReleaseSun(TodManager());
        g_tickBusy.clear(std::memory_order_release);
        LOG("Unloaded.");
        log::Close();
    }
}
