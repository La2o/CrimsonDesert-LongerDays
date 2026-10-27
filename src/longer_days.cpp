#include "longer_days.h"

#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "config.h"
#include "locate.h"
#include "log.h"
#include "offsets.h"
#include "rate_meter.h"
#include "scanner.h"
#include "schedule.h"
#include "sigtools.h"
#include "toast.h"
#include "version.h"

namespace longerdays
{
    using namespace offsets;

    namespace
    {
        // Community measurement of the vanilla clock (one real hour = 12
        // in-game hours). Only used until the RateMeter has measured it.
        constexpr float kAssumedVanillaRate = 12.0f;

        // A measured vanilla rate outside this range means we are reading the
        // wrong thing, not that the game changed its day length that much.
        constexpr float kMinPlausibleRate = 2.0f;
        constexpr float kMaxPlausibleRate = 120.0f;

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

        // Self-check of the hook: a real frame step is a small positive number
        // of seconds. Anything else is passed through untouched and counted.
        std::atomic<uint32_t> g_tickCalls{0};
        std::atomic<uint32_t> g_tickOdd{0};
        std::atomic<bool>     g_safeMode{false}; // hook stays, changes nothing
        constexpr uint32_t    kTickCheckAfter = 600;
        constexpr float       kMaxFrameStep   = 2.0f;

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
        bool             g_tickChecked = false;

        // Clock watchdog: the clock must move while ticks are applied.
        int64_t   g_lastClockSeconds = -1;
        double    g_appliedAtClockMove = 0.0;
        bool      g_clockStale = false;
        constexpr double kStaleAfterAppliedSeconds = 90.0; // ~18 vanilla game minutes

        // Sun. Found by a background thread (SunFinderThread) so the search
        // never costs the game a frame, and published once via g_todReady.
        std::vector<uintptr_t> g_todGlobals;       // set before the finder starts, then read-only
        locate::TodLayout      g_todLayout{};      // likewise
        locate::TodBinding     g_tod{};            // valid once g_todReady
        std::atomic<bool>      g_todReady{false};
        std::atomic<bool>      g_sunWanted{false};
        std::atomic<float>     g_sunClockHour{0.0f};
        std::atomic<uint32_t>  g_clockMoves{0};    // clock changes seen; in-world once it climbs
        bool                   g_sunBroken = false; // pinning didn't take; off for the session

        bool      g_clampApplied = false;
        uintptr_t g_clampMgr     = 0;
        float     g_origLower    = 0.0f;
        float     g_origUpper    = 24.0f;
        ULONGLONG g_clampSince   = 0;
        bool      g_clampVerified = false;
        constexpr ULONGLONG kSunVerifyMs = 5000;

        // --- Resolved at load; 0 means that sub-feature is unavailable -------
        uintptr_t g_clock = 0; // client-realm {day, hour, min, sec}

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

        HWND GameWindowIfFocused()
        {
            DWORD pid = 0;
            const HWND fg = GetForegroundWindow();
            GetWindowThreadProcessId(fg, &pid);
            return pid == GetCurrentProcessId() ? fg : nullptr;
        }

        void Warn(const wchar_t* toastText)
        {
            if (g_cfg.showToast)
                if (const HWND w = GameWindowIfFocused())
                    toast::Show(w, toastText, toast::Tone::Warning);
        }

        uintptr_t LowerLimit(uintptr_t mgr) { return mgr + g_tod.lowerOff; }
        uintptr_t UpperLimit(uintptr_t mgr) { return mgr + g_tod.lowerOff + 4; }

        void ReleaseSun(uintptr_t mgr)
        {
            // Restore only onto the manager we captured from; a different one
            // means the old was torn down and the new has its own limits.
            if (g_clampApplied && mgr && mgr == g_clampMgr)
            {
                WriteF32(LowerLimit(mgr), g_origLower);
                WriteF32(UpperLimit(mgr), g_origUpper);
            }
            g_clampApplied = false;
            g_clampMgr = 0;
        }

        // Pin the visible sun to the field clock: lower == upper == clock hour,
        // which the render manager clamps currentTimeOfDay into every frame.
        // If the sun doesn't follow within a few seconds, let go for good.
        void SyncSun(bool haveClock, float hour)
        {
            const bool want = !g_safeMode && g_enabled && g_cfg.syncSun && !g_sunBroken;
            g_sunWanted.store(want, std::memory_order_relaxed);

            const uintptr_t mgr = g_todReady.load(std::memory_order_acquire) ? locate::TodManager(g_tod) : 0;
            if (!want || !haveClock || !mgr)
            {
                if (g_clampApplied) ReleaseSun(mgr);
                return;
            }
            if (!g_clampApplied || mgr != g_clampMgr)
            {
                g_origLower     = ReadF32(LowerLimit(mgr));
                g_origUpper     = ReadF32(UpperLimit(mgr));
                g_clampMgr      = mgr;
                g_clampApplied  = true;
                g_clampSince    = GetTickCount64();
                g_clampVerified = false;
                LOG("Sun sync engaged (limits were %.2f..%.2f).", g_origLower, g_origUpper);
            }
            WriteF32(LowerLimit(mgr), hour);
            WriteF32(UpperLimit(mgr), hour);

            if (!g_clampVerified)
            {
                const float cur = ReadF32(mgr + g_tod.hourOff);
                if (std::isfinite(cur) && mem::HourDistance(cur, hour) < 0.1f)
                {
                    g_clampVerified = true;
                    LOG("Sun sync verified: the sky follows the slowed clock.");
                }
                else if (GetTickCount64() - g_clampSince > kSunVerifyMs)
                {
                    ReleaseSun(mgr);
                    g_sunBroken = true;
                    LOG("WARNING: the sun did not follow the clock (sky at %.2f, clock at %.2f). "
                        "Sun sync is off for this session; the clock still slows down.", cur, hour);
                }
            }
        }

        void EnterSafeMode(const char* why)
        {
            g_safeMode.store(true);
            g_length.store(1.0f, std::memory_order_relaxed);
            if (g_todReady.load()) ReleaseSun(locate::TodManager(g_tod));
            LOG("SAFE MODE: %s. Longer Days has stopped changing anything; the game runs at "
                "vanilla speed. A game update probably changed this code - please report the log.", why);
            Warn(L"paused: game update not supported yet (see log)");
        }

        // The hook watches its own inputs: if most "frame steps" are not small
        // positive seconds, we hooked something we don't understand.
        void CheckTickSanity()
        {
            if (g_tickChecked) return;
            const uint32_t calls = g_tickCalls.load(std::memory_order_relaxed);
            if (calls < kTickCheckAfter) return;
            g_tickChecked = true;
            const uint32_t odd = g_tickOdd.load(std::memory_order_relaxed);
            if (odd * 2 > calls)
                EnterSafeMode("the hooked clock tick receives values that are not frame times");
            else
                LOG("Self-check passed: the hooked clock tick receives normal frame times.");
        }

        // The clock must advance while time is being applied. If it doesn't
        // (a game update moved it, or another mod froze time), stop steering by
        // it until it moves again.
        bool ClockTrustworthy(const Clock& c)
        {
            const double applied = g_appliedSeconds.load(std::memory_order_relaxed);
            const int64_t secs = c.TotalSeconds();
            if (secs != g_lastClockSeconds)
            {
                if (g_lastClockSeconds >= 0) g_clockMoves.fetch_add(1, std::memory_order_relaxed);
                if (g_clockStale)
                    LOG("The clock is moving again; dawn/dusk, golden hour and seasons are back on.");
                g_lastClockSeconds = secs;
                g_appliedAtClockMove = applied;
                g_clockStale = false;
                return true;
            }
            if (!g_clockStale && applied - g_appliedAtClockMove > kStaleAfterAppliedSeconds)
            {
                g_clockStale = true;
                LOG("WARNING: the in-game clock has not moved for a while although time is "
                    "passing (another mod freezing time, or a game update). Using "
                    "DayLengthMultiplier around the clock until it moves.");
            }
            return !g_clockStale;
        }

        float VanillaRate()
        {
            const float r = g_meter.Rate();
            return r >= kMinPlausibleRate && r <= kMaxPlausibleRate ? r : kAssumedVanillaRate;
        }

        void LogConfig(const char* what)
        {
            const Config& c = g_cfg;
            LOG("%s: enabled=%d | day x%.2f night x%.2f, day %.1fh-%.1fh, blend %.1fh | "
                "golden hour x%.2f over %.1fh | real minutes day=%.0f night=%.0f | "
                "seasons %.0f%% over %.0f days (peak day %.0f) | syncSun=%d toggleKey=0x%X showToast=%d",
                what, c.enabled, c.dayLength, c.nightLength, c.dayStartHour, c.dayEndHour,
                c.transitionHours, c.goldenHourMultiplier, c.goldenHourLength, c.dayRealMinutes,
                c.nightRealMinutes, c.seasonStrength * 100.0f, c.yearLengthDays, c.summerPeakDay,
                c.syncSun, c.toggleKey, c.showToast);
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
            const HWND game = GameWindowIfFocused();
            const bool down = game && (GetAsyncKeyState(g_cfg.toggleKey) & 0x8000);
            if (down && !g_keyWasDown)
            {
                if (g_safeMode)
                {
                    if (g_cfg.showToast) toast::Show(game, L"paused: game update not supported yet", toast::Tone::Warning);
                }
                else
                {
                    g_enabled = !g_enabled;
                    LOG("Hotkey: Longer Days %s.", g_enabled ? "ON" : "OFF (vanilla day length)");
                    MessageBeep(g_enabled ? MB_OK : MB_ICONHAND); // async, never stalls the game
                    if (g_cfg.showToast) toast::Show(game, g_enabled);
                }
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
                g_enabled ? "" : " [OFF]", VanillaRate(),
                g_meter.Calibrated() ? "measured" : "assumed");
        }

        // Housekeeping after each clock tick. Only one thread runs it at a
        // time; a thread that finds it busy just skips this frame.
        void AfterTick()
        {
            if (g_tickBusy.test_and_set(std::memory_order_acquire)) return;
            PollIni();
            PollHotkey();
            CheckTickSanity();
            if (g_safeMode)
            {
                g_tickBusy.clear(std::memory_order_release);
                return;
            }

            Clock clock;
            const bool haveClock = ReadClock(clock) && ClockTrustworthy(clock);
            if (haveClock)
            {
                g_meter.Feed(g_appliedSeconds.load(std::memory_order_relaxed), clock.TotalSeconds());
                if (g_meter.Calibrated() && !g_reportedCalibration)
                {
                    g_reportedCalibration = true;
                    const float r = g_meter.Rate();
                    if (r >= kMinPlausibleRate && r <= kMaxPlausibleRate)
                        LOG("Measured vanilla clock speed: %.2f in-game seconds per real second "
                            "(a full vanilla day = %.0f real minutes).", r, 24.0f * 60.0f / r);
                    else
                        LOG("WARNING: measured a vanilla clock speed of %.2f, which can't be right; "
                            "using the assumed %.0f for real-minute targets.", r, kAssumedVanillaRate);
                }
            }

            float length = 1.0f;
            if (haveClock)
            {
                g_sunClockHour.store(clock.Hours(), std::memory_order_relaxed);
                const SpeedSample s = LengthAt(g_cfg, clock.Hours(), clock.day, VanillaRate());
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
            g_tickCalls.fetch_add(1, std::memory_order_relaxed);
            if (!(delta >= 0.0f && delta < kMaxFrameStep)) // also catches NaN
            {
                g_tickOdd.fetch_add(1, std::memory_order_relaxed);
                g_origTick(mgr, delta, unused);
                AfterTick();
                return;
            }

            if (!g_safeMode.load(std::memory_order_relaxed))
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

        // Finds the sun off the game thread. Waits until the clock is running
        // in-world, then looks for a TimeOfDay that agrees with the clock and,
        // a few seconds later, has moved like one. Only then is it used.
        DWORD WINAPI SunFinderThread(LPVOID)
        {
            // Runs code in this DLL, so it keeps the DLL loaded while it runs.
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                               reinterpret_cast<LPCWSTR>(&SunFinderThread), &self);

            constexpr int kDiagnoseAfter = 3;  // in-world attempts before the diagnostic block
            constexpr int kGiveUpAfter   = 10; // ~1 minute, then retry every 30 s
            int attempts = 0;
            bool reportedAmbiguous = false;
            for (;;)
            {
                Sleep(attempts >= kGiveUpAfter ? 30000 : 2000);
                if (g_safeMode.load()) break;
                if (!g_sunWanted.load(std::memory_order_relaxed) ||
                    g_clockMoves.load(std::memory_order_relaxed) < 3)
                    continue; // menu, loading, paused, or sun sync off

                const float hour = g_sunClockHour.load(std::memory_order_relaxed);
                if (attempts == 0) LOG("Sun search started (in-world, clock at %.2f).", hour);
                const auto first = locate::TodCandidates(g_todGlobals, g_todLayout, hour, attempts >= 2);
                std::vector<locate::TodBinding> moving;
                if (!first.empty())
                {
                    Sleep(4000);
                    moving = locate::StillMoving(first, g_sunClockHour.load(std::memory_order_relaxed));
                }

                // Prefer the known offsets; a searched answer must be unique.
                const locate::TodBinding* pick = nullptr;
                size_t known = 0;
                for (const auto& b : moving) if (!b.searched) { ++known; pick = &b; }
                if (known != 1) pick = (known == 0 && moving.size() == 1) ? &moving[0] : nullptr;

                if (pick)
                {
                    g_tod = *pick;
                    g_todReady.store(true, std::memory_order_release);
                    const mem::Module& mod = mem::GameModule();
                    if (g_tod.searched)
                        LOG("Sun found by search (exe+0x%llX, manager at engine+0x%llX, time of day at "
                            "+0x%llX); the known offsets no longer match.",
                            static_cast<unsigned long long>(g_tod.global - mod.base),
                            static_cast<unsigned long long>(g_tod.managerOff),
                            static_cast<unsigned long long>(g_tod.hourOff));
                    else
                        LOG("Sun found (exe+0x%llX, manager at engine%s0x%llX, limits at +0x%llX): its "
                            "time of day agrees with the clock and is moving.",
                            static_cast<unsigned long long>(g_tod.global - mod.base),
                            g_tod.embedded ? "+" : "->+", static_cast<unsigned long long>(g_tod.managerOff),
                            static_cast<unsigned long long>(g_tod.lowerOff));
                    break;
                }
                if (moving.size() > 1 && !reportedAmbiguous)
                {
                    reportedAmbiguous = true;
                    LOG("Sun: %zu places look like the time of day; not guessing.", moving.size());
                }
                ++attempts;
                if (attempts == kDiagnoseAfter)
                {
                    LOG("Sun not found yet (%zu agreed with the clock, %zu kept moving).",
                        first.size(), moving.size());
                    locate::LogTodDiagnostics(g_todGlobals, g_todLayout, hour);
                }
                if (attempts == kGiveUpAfter)
                    LOG("WARNING: could not find the sun's time of day (or another mod is controlling "
                        "it). The clock still slows down; the sky keeps vanilla speed. Still looking "
                        "every 30 s.");
            }
            FreeLibraryAndExitThread(self, 0);
        }

        // The game is packed: its code may not be readable the instant we are
        // loaded. Retry the essential lookup for a while before giving up.
        locate::Hit WaitForTick(int timeoutMs)
        {
            for (int waited = 0;; waited += 1000)
            {
                if (const locate::Hit h = locate::FieldTimeTick()) return h;
                if (waited >= timeoutMs) return {};
                Sleep(1000);
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

        // Which game build this is, so a log from a new patch says so.
        void LogGameBuild(const wchar_t* exe)
        {
            DWORD ignored = 0;
            const DWORD size = GetFileVersionInfoSizeW(exe, &ignored);
            std::vector<uint8_t> info(size);
            VS_FIXEDFILEINFO* ffi = nullptr;
            UINT len = 0;
            if (size && GetFileVersionInfoW(exe, 0, size, info.data()) &&
                VerQueryValueW(info.data(), L"\\", reinterpret_cast<void**>(&ffi), &len) && ffi)
            {
                LOG("Game build: %u.%u.%u.%u", HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
                    HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
            }
            else
            {
                LOG("Game build: unknown (no version info).");
            }
        }

        // If the mod can't start, say so on screen once the game window is up,
        // instead of failing silently. Runs on the init thread; never blocks
        // the game.
        void ShowStartupFailure()
        {
            if (!g_cfg.showToast) return;
            for (int i = 0; i < 600; ++i) // up to ten minutes
            {
                if (HWND w = GameWindowIfFocused())
                {
                    Sleep(5000); // let the window settle past the splash
                    if (const HWND now = GameWindowIfFocused()) w = now;
                    toast::Show(w, L"not active: game update not supported yet (see log)",
                                toast::Tone::Warning);
                    return;
                }
                Sleep(1000);
            }
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
        LogGameBuild(exe);

        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(g_iniPath.c_str(), GetFileExInfoStandard, &fad))
            g_iniStamp = fad.ftLastWriteTime;
        else
            LOG("No INI found next to the .asi - using defaults.");
        g_cfg = Config::Load(g_iniPath);
        g_enabled = g_cfg.enabled;
        LogConfig("Config");

        const locate::Hit tick = WaitForTick(60000);
        if (!tick)
        {
            LOG("ERROR: the field-time tick was NOT FOUND by any signature. This game version "
                "is not supported yet. Mod inactive; the game is untouched.");
            ShowStartupFailure();
            return;
        }
        LOG("Clock tick found via %s at exe+0x%llX.", tick.how,
            static_cast<unsigned long long>(tick.addr - mem::GameModule().base));

        if (const locate::Hit clock = locate::FieldClock())
        {
            g_clock = clock.addr;
            LOG("Field clock found via %s.", clock.how);
        }
        else
        {
            LOG("WARNING: field clock NOT FOUND - dawn/dusk, golden hour, seasons, "
                "real-time targets and sun sync are off; DayLengthMultiplier applies around the clock.");
        }

        g_todLayout = locate::LocateTodLayout();
        if (g_todLayout)
            LOG("Sun layout read from the game's console handlers: manager getter at vtable+0x%X, "
                "limits at +0x%llX.", g_todLayout.slot, static_cast<unsigned long long>(g_todLayout.lowerOff));
        else
            LOG("Sun layout: console handlers not found; trying the 2.03.00 layout and a search.");
        g_todGlobals = locate::TodEngineGlobals();
        if (g_todGlobals.empty())
            LOG("WARNING: time-of-day engine NOT FOUND - the sun will keep vanilla speed "
                "while the clock is slowed.");
        else
            LOG("Time-of-day engine: %zu candidate(s); the right one is picked in-world.",
                g_todGlobals.size());
        const bool findSun = !g_todGlobals.empty();

        if (MH_Initialize() != MH_OK)
        {
            LOG("ERROR: MinHook failed to initialize. Mod inactive.");
            return;
        }
        void* target = reinterpret_cast<void*>(tick.addr);
        if (MH_CreateHook(target, reinterpret_cast<void*>(&HookedFieldTimeTick),
                          reinterpret_cast<void**>(&g_origTick)) != MH_OK ||
            MH_EnableHook(target) != MH_OK)
        {
            LOG("ERROR: failed to hook the field-time tick. Mod inactive.");
            MH_Uninitialize();
            return;
        }
        g_tickTarget = target;
        LOG("Ready.");

        if (findSun)
            if (HANDLE t = CreateThread(nullptr, 0, SunFinderThread, nullptr, 0, nullptr))
                CloseHandle(t);
    }

    void Shutdown()
    {
        if (!g_tickTarget) return;
        MH_DisableHook(g_tickTarget);
        MH_RemoveHook(g_tickTarget);
        g_tickTarget = nullptr;
        MH_Uninitialize();

        while (g_tickBusy.test_and_set(std::memory_order_acquire)) Sleep(1);
        if (g_todReady.load()) ReleaseSun(locate::TodManager(g_tod));
        g_tickBusy.clear(std::memory_order_release);
        LOG("Unloaded.");
        log::Close();
    }
}
