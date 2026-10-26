#include "longer_days.h"

#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include "config.h"
#include "log.h"
#include "offsets.h"
#include "scanner.h"
#include "version.h"

namespace longerdays
{
    using namespace offsets;

    namespace
    {
        std::wstring g_iniPath;

        // Double-buffered so the clock hook (possibly on several game threads)
        // never reads a config while a reload is writing it. Reloads are at
        // least a second apart, so a reader always finishes first.
        Config            g_cfgSlots[2];
        std::atomic<int>  g_cfgIndex{0};
        std::atomic<bool> g_enabled{true}; // INI Enabled, flipped by the hotkey

        const Config& Cfg() { return g_cfgSlots[g_cfgIndex.load(std::memory_order_acquire)]; }

        void PublishConfig(const Config& c)
        {
            const int next = 1 - g_cfgIndex.load(std::memory_order_relaxed);
            g_cfgSlots[next] = c;
            g_cfgIndex.store(next, std::memory_order_release);
            g_enabled.store(c.enabled);
        }

        // Resolved at load. 0 means "not found": that sub-feature is inert.
        uintptr_t g_clock           = 0; // client-realm {day, hour, min, sec}
        uintptr_t g_todEngineGlobal = 0;

        // Sun clamp state. Only touched from AfterTick, under g_tickBusy.
        bool      g_clampApplied = false;
        uintptr_t g_clampMgr     = 0;
        float     g_origLower    = 0.0f;
        float     g_origUpper    = 24.0f;

        std::atomic_flag g_tickBusy = ATOMIC_FLAG_INIT;
        ULONGLONG        g_nextIniCheck = 0;
        FILETIME         g_iniStamp{};
        bool             g_keyWasDown = false;

        using FieldTimeTick_t = void (*)(void* mgr, float delta, float unused);
        FieldTimeTick_t g_origTick   = nullptr;
        void*           g_tickTarget = nullptr;

        float ReadF32(uintptr_t a)          { float v; std::memcpy(&v, reinterpret_cast<const void*>(a), 4); return v; }
        void  WriteF32(uintptr_t a, float v) { std::memcpy(reinterpret_cast<void*>(a), &v, 4); }
        int   ReadI32(uintptr_t a)          { return *reinterpret_cast<volatile const int32_t*>(a); }

        // Current in-game hour as a float (0..24), or -1 if unavailable (clock
        // not resolved, or not in-world yet and still zero/garbage).
        float ClockHour()
        {
            if (!g_clock) return -1.0f;
            const int h = ReadI32(g_clock + kOff_FieldTime_Hour);
            const int m = ReadI32(g_clock + kOff_FieldTime_Min);
            const int s = ReadI32(g_clock + kOff_FieldTime_Sec);
            if (h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59) return -1.0f;
            return h + m / 60.0f + s / 3600.0f;
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
            // Only restore onto the manager we captured from; a different one
            // means the old was torn down and has its own fresh limits.
            if (g_clampApplied && mgr && mgr == g_clampMgr)
            {
                WriteF32(mgr + kOff_Tod_LowerLimit, g_origLower);
                WriteF32(mgr + kOff_Tod_UpperLimit, g_origUpper);
            }
            g_clampApplied = false;
            g_clampMgr = 0;
        }

        // Pin the visible sun to our (slowed) field clock: lower == upper ==
        // clock hour, which the render manager clamps currentTimeOfDay into.
        void SyncSun(const Config& cfg)
        {
            const uintptr_t mgr  = TodManager();
            const float     hour = ClockHour();
            const bool want = g_enabled.load() && cfg.syncSun && cfg.ChangesAnything() && hour >= 0.0f;

            if (!want || !mgr)
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
                LOG("Sun sync engaged (manager %p, limits %.2f..%.2f, clock %.2fh).",
                    reinterpret_cast<void*>(mgr), g_origLower, g_origUpper, hour);
            }
            WriteF32(mgr + kOff_Tod_LowerLimit, hour);
            WriteF32(mgr + kOff_Tod_UpperLimit, hour);
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

            const Config c = Config::Load(g_iniPath);
            PublishConfig(c);
            LOG("Config reloaded: enabled=%d day x%.2f night x%.2f, day %.1fh-%.1fh, syncSun=%d.",
                c.enabled, c.dayLength, c.nightLength, c.dayStartHour, c.dayEndHour, c.syncSun);
        }

        void PollHotkey(const Config& cfg)
        {
            if (!cfg.toggleKey) return;
            DWORD pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(cfg.toggleKey) & 0x8000);
            if (down && !g_keyWasDown)
            {
                const bool on = !g_enabled.load();
                g_enabled.store(on);
                LOG("Hotkey: Longer Days %s.", on ? "ON" : "OFF (vanilla day length)");
                MessageBeep(on ? MB_OK : MB_ICONHAND); // async, never stalls the game thread
            }
            g_keyWasDown = down;
        }

        // Housekeeping after each clock tick. The tick may run on more than one
        // game thread (client / server realm), so only one runs this at a time.
        void AfterTick()
        {
            if (g_tickBusy.test_and_set(std::memory_order_acquire)) return;
            PollIni();
            const Config& cfg = Cfg();
            PollHotkey(cfg);
            SyncSun(cfg);
            g_tickBusy.clear(std::memory_order_release);
        }

        void HookedFieldTimeTick(void* mgr, float delta, float unused)
        {
            if (g_enabled.load(std::memory_order_relaxed))
            {
                const float hour = ClockHour();
                const Config& cfg = Cfg();
                // Without the clock we can't tell day from night: apply the day
                // multiplier around the clock.
                delta /= hour >= 0.0f ? cfg.LengthAt(hour) : cfg.dayLength;
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
        g_iniPath = SiblingPath(self, L".ini");
        log::Open(SiblingPath(self, L".log"));
        LOG("Crimson Desert Longer Days v%s", LONGERDAYS_VERSION);

        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        LOG("Host process: %ls", exe);

        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(g_iniPath.c_str(), GetFileExInfoStandard, &fad))
            g_iniStamp = fad.ftLastWriteTime;
        else
            LOG("No INI found next to the .asi - using defaults (day x2, night x1).");
        const Config cfg = Config::Load(g_iniPath);
        PublishConfig(cfg);
        LOG("Config: enabled=%d day x%.2f night x%.2f, day %.1fh-%.1fh, syncSun=%d, toggleKey=0x%X.",
            cfg.enabled, cfg.dayLength, cfg.nightLength, cfg.dayStartHour, cfg.dayEndHour,
            cfg.syncSun, cfg.toggleKey);

        const uintptr_t tick = WaitForPattern(kSig_FieldTimeTick, 60000);
        if (!tick)
        {
            LOG("ERROR: field-time tick signature NOT FOUND. This game version is not supported "
                "(or this is the launcher process, which is harmless). Mod inactive.");
            return;
        }
        if (mem::CountMatches(kSig_FieldTimeTick) != 1)
            LOG("WARNING: field-time tick signature is ambiguous; hooking the first match.");

        if (const uintptr_t m = mem::FindPattern(kSig_FieldTimeRealm))
            g_clock = ResolveInModule(m, kOff_FieldTime_ClientVmovups, kLen_FieldTime_Vmovups);
        if (!g_clock)
            LOG("WARNING: field clock signature NOT FOUND - day/night split and sun sync disabled; "
                "DayLengthMultiplier applies around the clock.");

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
