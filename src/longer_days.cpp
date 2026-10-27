#include "longer_days.h"

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "config.h"
#include "game_time.h"
#include "locate.h"
#include "log.h"
#include "offsets.h"
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
        // Vanilla clock speed if the snapshot can't tell us (one real hour =
        // 12 in-game hours of daytime).
        constexpr float kAssumedVanillaRate = 12.0f;

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
        std::wstring     g_seasonsIniPath; // Seasons of Pywel's, read for its season length
        Config           g_cfg;
        bool             g_enabled = true; // INI Enabled, flipped by the hotkey
        float            g_vanillaRate = kAssumedVanillaRate; // the snapshot's own rate
        ULONGLONG        g_nextIniCheck = 0;
        FILETIME         g_iniStamp{};
        FILETIME         g_seasonsIniStamp{};
        bool             g_keyWasDown = false;
        bool             g_ffWasDown  = false;
        bool             g_fastForward = false; // fast-forward key held
        int              g_lastLoggedHour = -1;
        bool             g_tickChecked = false;

        // Time steering (SteerTime): the game-millisecond counter as last
        // seen, the sub-millisecond remainder of the stamp shift, and the
        // client stamp we left behind, to notice when the game re-anchors.
        bool      g_haveNow     = false;
        uint64_t  g_lastNow     = 0;
        double    g_carry       = 0.0;
        double    g_realSeconds = 0.0; // unpaused game time seen, in seconds
        uint64_t  g_expectStamp = 0;
        uint32_t  g_reanchors   = 0;
        constexpr uint64_t kMaxSteerStepMs = 2000; // longer gaps are hitches; not steered

        // Measured clock speed between hourly log lines.
        double    g_hourRealSeconds  = 0.0;
        int64_t   g_hourClockSeconds = -1;

        // Clock watchdog: the clock must move while game time passes.
        int64_t   g_lastClockSeconds = -1;
        double    g_realAtClockMove = 0.0;
        bool      g_clockStale = false;
        constexpr double kStaleAfterRealSeconds = 90.0;

        // Sun. Found by a background thread (SunFinderThread) so the search
        // never costs the game a frame, and published once via g_todReady.
        std::vector<uintptr_t> g_todGlobals;       // set before the finder starts, then read-only
        locate::TodLayout      g_todLayout{};      // likewise
        bool                   g_todRegistrar = false; // g_todGlobals[0] is the game's own global
        locate::TodBinding     g_tod{};            // valid once g_todReady
        std::atomic<bool>      g_todReady{false};
        std::atomic<bool>      g_sunWanted{false};
        std::atomic<float>     g_sunClockHour{0.0f};
        std::atomic<uint32_t>  g_clockMoves{0};    // clock changes seen; in-world once it climbs
        std::atomic<uint32_t>  g_reanchorEpoch{0}; // bumped when the game re-anchors (save load, sleep)
        bool                   g_sunBroken = false; // pinning didn't take; off for the session

        bool      g_clampApplied = false;
        uintptr_t g_clampMgr     = 0;
        float     g_origLower    = 0.0f;
        float     g_origUpper    = 24.0f;
        ULONGLONG g_clampSince   = 0;
        bool      g_clampVerified = false;
        constexpr ULONGLONG kSunVerifyMs = 5000;

        // --- Resolved at load; 0 means that sub-feature is unavailable -------
        locate::FieldTime g_ft{}; // the field clock's snapshots and counter

        using FieldTimeTick_t = void (*)(void* mgr, float delta, float unused);
        FieldTimeTick_t g_origTick   = nullptr;
        void*           g_tickTarget = nullptr;

        float ReadF32(uintptr_t a)           { float v; std::memcpy(&v, reinterpret_cast<const void*>(a), 4); return v; }
        void  WriteF32(uintptr_t a, float v) { std::memcpy(reinterpret_cast<void*>(a), &v, 4); }

        using gametime::Snapshot;

        Snapshot ReadSnapshot(uintptr_t a)
        {
            Snapshot s;
            std::memcpy(&s, reinterpret_cast<const void*>(a), sizeof(s));
            return s;
        }

        volatile LONG64* StampOf(uintptr_t snapshot)
        {
            return reinterpret_cast<volatile LONG64*>(snapshot + offsetof(Snapshot, stamp));
        }

        // The game's own millisecond counter (stops while the game is paused),
        // and what the client realm adds to it.
        uint64_t GameNow()
        {
            return reinterpret_cast<uint64_t (*)()>(g_ft.nowFn)();
        }
        int64_t ClientNowOffset()
        {
            return *reinterpret_cast<volatile const int64_t*>(g_ft.nowOffset);
        }

        struct Clock
        {
            int day = 0, hour = 0, minute = 0, second = 0;
            float   Hours() const        { return hour + minute / 60.0f + second / 3600.0f; }
            int64_t TotalSeconds() const { return ((int64_t{day} * 24 + hour) * 60 + minute) * 60 + second; }
        };

        // The live time, computed the way the game's getter does. False when
        // the snapshot holds no sane time yet (main menu, still loading).
        bool ReadClock(Clock& c)
        {
            if (!g_ft) return false;
            const Snapshot snap = ReadSnapshot(g_ft.client);
            if (!gametime::Plausible(snap)) return false;
            const Snapshot live = gametime::At(snap, GameNow() + ClientNowOffset());
            c.day    = static_cast<int>(live.day);
            c.hour   = static_cast<int>(live.hour);
            c.minute = static_cast<int>(live.minute);
            c.second = static_cast<int>(live.second);
            g_vanillaRate = static_cast<float>(snap.rate);
            return true;
        }

        // Moves one snapshot's stamp by `shift` game milliseconds: forward
        // slows the clock, back speeds it up. A stamp that would go below
        // zero is first re-anchored at `now` (same time, fresh stamp).
        void Steer(uintptr_t snapshot, uint64_t now, int64_t shift)
        {
            Snapshot s = ReadSnapshot(snapshot);
            if (!gametime::Plausible(s)) return;
            int64_t want = static_cast<int64_t>(s.stamp) + shift;
            if (want < 0)
            {
                const Snapshot anchored = gametime::At(s, now);
                std::memcpy(reinterpret_cast<void*>(snapshot), &anchored, sizeof(anchored));
                s = anchored;
                want = static_cast<int64_t>(now) + shift;
                if (want < 0) return;
            }
            // Never anchor in the future: the clock would stop dead.
            if (shift > 0 && want > static_cast<int64_t>(now))
                want = std::max(static_cast<int64_t>(now), static_cast<int64_t>(s.stamp));
            InterlockedCompareExchange64(StampOf(snapshot), want, static_cast<LONG64>(s.stamp));
        }

        // Once per frame: let 1/length of the game time that just passed
        // count on the clock. Both realms get the same shift, so they agree.
        void SteerTime(float length)
        {
            const uint64_t raw = GameNow();
            if (!g_haveNow || raw < g_lastNow)
            {
                g_haveNow = true;
                g_lastNow = raw;
                g_carry   = 0.0;
                return;
            }
            const uint64_t dt = raw - g_lastNow;
            g_lastNow = raw;

            // Did the game re-anchor the clock itself (load, sleep, sync)?
            // Mid-load the snapshot briefly holds junk; that is not one.
            const uint64_t stamp = static_cast<uint64_t>(*StampOf(g_ft.client));
            if (g_expectStamp && stamp != g_expectStamp)
            {
                const Snapshot s = ReadSnapshot(g_ft.client);
                if (gametime::Plausible(s))
                {
                    g_reanchorEpoch.fetch_add(1, std::memory_order_relaxed);
                    if (++g_reanchors <= 5)
                        LOG("The game re-anchored the clock at Day %u %02u:%02u:%02u (rate %u)%s.", s.day,
                            s.hour, s.minute, s.second, s.rate,
                            g_reanchors == 5 ? "; not logging further ones" : "");
                }
            }

            if (dt > 0 && dt <= kMaxSteerStepMs)
            {
                g_realSeconds += dt / 1000.0;
                const double want = dt * (1.0 - 1.0 / length) + g_carry;
                const int64_t shift = static_cast<int64_t>(std::floor(want));
                g_carry = want - static_cast<double>(shift);
                if (shift != 0)
                {
                    Steer(g_ft.client, raw + ClientNowOffset(), shift);
                    if (g_ft.server) Steer(g_ft.server, raw, shift);
                }
            }
            g_expectStamp = static_cast<uint64_t>(*StampOf(g_ft.client));
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

        // The clock must advance while game time passes. If it doesn't (a
        // game update changed it, or another mod froze time), stop steering by
        // it until it moves again.
        bool ClockTrustworthy(const Clock& c)
        {
            const double applied = g_realSeconds;
            const int64_t secs = c.TotalSeconds();
            if (secs != g_lastClockSeconds)
            {
                if (g_lastClockSeconds >= 0) g_clockMoves.fetch_add(1, std::memory_order_relaxed);
                if (g_clockStale)
                    LOG("The clock is moving again; dawn/dusk, golden hour and seasons are back on.");
                g_lastClockSeconds = secs;
                g_realAtClockMove = applied;
                g_clockStale = false;
                return true;
            }
            if (!g_clockStale && applied - g_realAtClockMove > kStaleAfterRealSeconds)
            {
                g_clockStale = true;
                LOG("WARNING: the in-game clock has not moved for a while although time is "
                    "passing (another mod freezing time, or a game update). Using "
                    "DayLengthMultiplier around the clock until it moves.");
            }
            return !g_clockStale;
        }

        float VanillaRate() { return g_vanillaRate; }

        const char* LockedSeason(int season)
        {
            static const char* const kLocked[] = {", locked to summer", ", locked to autumn",
                                                  ", locked to winter", ", locked to spring"};
            return season >= 0 && season < 4 ? kLocked[season] : "";
        }

        void LogConfig(const char* what)
        {
            const Config& c = g_cfg;
            LOG("%s: enabled=%d | day x%.2f night x%.2f, day %.1fh-%.1fh, blend %.1fh | "
                "golden hour x%.2f over %.1fh | real minutes day=%.0f night=%.0f | "
                "seasons %.0f%%, %.0f days each%s%s | syncSun=%d toggleKey=0x%X showToast=%d "
                "fastForward=0x%X x%.0f",
                what, c.enabled, c.dayLength, c.nightLength, c.dayStartHour, c.dayEndHour,
                c.transitionHours, c.goldenHourMultiplier, c.goldenHourLength, c.dayRealMinutes,
                c.nightRealMinutes, c.seasonStrength * 100.0f, c.seasonLengthDays,
                c.seasonsFromPywel ? " (from Seasons of Pywel)" : "", LockedSeason(c.fixedSeason),
                c.syncSun, c.toggleKey, c.showToast, c.fastForwardKey, c.fastForwardSpeed);
        }

        // A file's last write time, or zero when it doesn't exist.
        FILETIME WriteTime(const std::wstring& path)
        {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) ? fad.ftLastWriteTime
                                                                                   : FILETIME{};
        }

        void PollIni()
        {
            const ULONGLONG now = GetTickCount64();
            if (now < g_nextIniCheck) return;
            g_nextIniCheck = now + 1000;

            // Either INI: Seasons of Pywel saves its own when its menu changes.
            const FILETIME ours = WriteTime(g_iniPath), seasons = WriteTime(g_seasonsIniPath);
            if (CompareFileTime(&ours, &g_iniStamp) == 0 &&
                CompareFileTime(&seasons, &g_seasonsIniStamp) == 0)
                return;
            g_iniStamp = ours;
            g_seasonsIniStamp = seasons;

            g_cfg = Config::Load(g_iniPath, g_seasonsIniPath);
            g_enabled = g_cfg.enabled;
            LogConfig("Config reloaded");
        }

        // Hold to fast-forward. A test aid: with sun sync working, the sky
        // races along with the clock.
        void PollFastForward(HWND game)
        {
            const bool down = game && g_cfg.fastForwardKey && !g_safeMode &&
                              (GetAsyncKeyState(g_cfg.fastForwardKey) & 0x8000);
            if (down && !g_ffWasDown)
            {
                LOG("Fast-forward x%.0f (key held).", g_cfg.fastForwardSpeed);
                if (g_cfg.showToast)
                {
                    wchar_t text[48];
                    swprintf(text, 48, L"fast-forward x%.0f", g_cfg.fastForwardSpeed);
                    toast::Show(game, text, toast::Tone::On);
                }
            }
            else if (!down && g_ffWasDown)
            {
                LOG("Fast-forward released.");
            }
            g_fastForward = down;
            g_ffWasDown = down;
        }

        void PollHotkey()
        {
            const HWND game = GameWindowIfFocused();
            PollFastForward(game);
            if (!g_cfg.toggleKey) return;
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

            // How fast the clock really ran since the previous line: the
            // proof that the steering works (vanilla: 12x by day, 24x at night).
            char ran[48] = "";
            const double real = g_realSeconds - g_hourRealSeconds;
            const int64_t game = c.TotalSeconds() - g_hourClockSeconds;
            if (g_hourClockSeconds >= 0 && real > 5.0 && game > 0 && game < 6 * 3600)
                std::snprintf(ran, sizeof(ran), " | ran %.1fx real time", game / real);
            g_hourRealSeconds  = g_realSeconds;
            g_hourClockSeconds = c.TotalSeconds();

            LOG("Day %d %02d:%02d %-11s time x%.2f slower%s | vanilla clock %.0fx%s",
                c.day, c.hour, c.minute, phase, g_enabled ? s.length : 1.0f,
                g_enabled ? "" : " [OFF]", VanillaRate(), ran);
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
            if (g_fastForward) length = 1.0f / g_cfg.fastForwardSpeed;
            SteerTime(length);

            SyncSun(haveClock, haveClock ? clock.Hours() : 0.0f);
            g_tickBusy.clear(std::memory_order_release);
        }

        // The tick is only our once-per-frame callback on the game thread;
        // its delta goes through untouched.
        void HookedFieldTimeTick(void* mgr, float delta, float unused)
        {
            g_tickCalls.fetch_add(1, std::memory_order_relaxed);
            if (!(delta >= 0.0f && delta < kMaxFrameStep)) // also catches NaN
                g_tickOdd.fetch_add(1, std::memory_order_relaxed);
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
            constexpr int kWarnInMenu    = 20; // ~5 more minutes before any save has loaded
            int attempts = 0;
            bool saveLoaded = false; // the main menu's sky stands still: no warning there
            bool reportedAmbiguous = false;
            uint32_t epoch = g_reanchorEpoch.load(std::memory_order_relaxed);
            for (;;)
            {
                // Wait in short steps so a save load is picked up at once.
                for (int waited = 0; waited < (attempts >= kGiveUpAfter ? 30000 : 2000); waited += 500)
                {
                    if (g_reanchorEpoch.load(std::memory_order_relaxed) != epoch) break;
                    Sleep(500);
                }
                if (g_safeMode.load()) break;

                // A save just loaded (often after tries in the main menu, whose
                // sky stands still): start over with a fresh count.
                const uint32_t nowEpoch = g_reanchorEpoch.load(std::memory_order_relaxed);
                if (nowEpoch != epoch)
                {
                    epoch = nowEpoch;
                    saveLoaded = true;
                    if (attempts > 0)
                        LOG("Sun search restarted: the game re-anchored the clock (save loaded).");
                    attempts = 0;
                    reportedAmbiguous = false;
                    Sleep(2000); // let the world settle
                }
                if (!g_sunWanted.load(std::memory_order_relaxed) ||
                    g_clockMoves.load(std::memory_order_relaxed) < 3)
                    continue; // menu, loading, paused, or sun sync off

                const float hour = g_sunClockHour.load(std::memory_order_relaxed);
                if (attempts == 0) LOG("Sun search started (in-world, clock at %.2f).", hour);
                const auto first = locate::TodCandidates(g_todGlobals, g_todRegistrar, g_todLayout, hour,
                                                         attempts >= 2);
                std::vector<locate::TodBinding> moving;
                if (!first.empty())
                {
                    Sleep(4000);
                    moving = locate::StillMoving(first, g_sunClockHour.load(std::memory_order_relaxed));
                }

                // The game's own path wins; otherwise the known offsets; a
                // searched answer must be unique.
                const locate::TodBinding* pick = nullptr;
                for (const auto& b : moving) if (b.viaGame) { pick = &b; break; }
                if (!pick)
                {
                    size_t known = 0;
                    for (const auto& b : moving) if (!b.searched) { ++known; pick = &b; }
                    if (known != 1) pick = (known == 0 && moving.size() == 1) ? &moving[0] : nullptr;
                }

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
                // A save that loaded during this try restarts the search
                // instead: no warning for the main menu's sky.
                if (attempts == (saveLoaded ? kGiveUpAfter : kWarnInMenu) &&
                    g_reanchorEpoch.load(std::memory_order_relaxed) == epoch)
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

        g_seasonsIniPath = g_iniPath.substr(0, g_iniPath.find_last_of(L'\\') + 1) + L"Seasons\\Seasons.ini";
        g_iniStamp = WriteTime(g_iniPath);
        g_seasonsIniStamp = WriteTime(g_seasonsIniPath);
        if (g_iniStamp.dwLowDateTime == 0 && g_iniStamp.dwHighDateTime == 0)
            LOG("No INI found next to the .asi - using defaults.");
        g_cfg = Config::Load(g_iniPath, g_seasonsIniPath);
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

        g_ft = locate::FieldClock();
        if (!g_ft)
        {
            LOG("ERROR: the field clock was NOT FOUND (snapshot %s, counter %s). This game version "
                "is not supported yet. Mod inactive; the game is untouched.",
                g_ft.client ? "found" : "missing", g_ft.nowFn ? "found" : "missing");
            ShowStartupFailure();
            return;
        }
        const uintptr_t exeBase = mem::GameModule().base;
        LOG("Field clock found via %s: snapshot exe+0x%llX%s, counter exe+0x%llX.", g_ft.how,
            static_cast<unsigned long long>(g_ft.client - exeBase), g_ft.server ? " (+ server copy)" : "",
            static_cast<unsigned long long>(g_ft.nowFn - exeBase));

        g_todLayout = locate::LocateTodLayout();
        if (g_todLayout)
            LOG("Sun layout read from the game's console handlers: manager getter at vtable+0x%X, "
                "limits at +0x%llX.", g_todLayout.slot, static_cast<unsigned long long>(g_todLayout.lowerOff));
        else
            LOG("Sun layout: console handlers not found; trying the 2.03.00 layout and a search.");
        g_todGlobals = locate::TodEngineGlobals(g_todRegistrar);
        if (g_todGlobals.empty())
            LOG("WARNING: time-of-day engine NOT FOUND - the sun will keep vanilla speed "
                "while the clock is slowed.");
        else
            LOG("Time-of-day engine: %zu candidate(s)%s; the right one is picked in-world.",
                g_todGlobals.size(), g_todRegistrar ? ", the console registrar's first" : "");
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
