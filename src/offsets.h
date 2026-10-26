#pragma once
#include <cstdint>

// Everything this mod knows about the game binary lives here: byte signatures
// (re-scanned at load, so a patch that only shifts code keeps working) and the
// struct offsets reached through them. Never an absolute address.
//
// These signatures were reverse-engineered and live-verified by the Trinity
// project (https://github.com/XeTrinityz/Trinity, MIT, src/game/offsets.h) for
// game build 2.03.00. If the log reports a signature NOT FOUND after a game
// update, these are what need re-deriving.
namespace longerdays::offsets
{
    // Anything below this is not a plausible game pointer.
    inline constexpr uintptr_t kMinPointer = 0x10000000;

    // --- Field-time tick: the day/night clock's per-frame advance ------------
    // __fastcall(rcx = mgr, xmm1 = frameDeltaSeconds, xmm2 = unused). Its first
    // act is `[mgr+0x2C] += delta`, a float accumulator from which the realm
    // clock globals and the sun sync are derived. Scaling `delta` therefore
    // scales how fast in-game time passes, and nothing else - physics, AI and
    // combat advance on their own deltas.
    inline constexpr const char* kSig_FieldTimeTick =
        "48 89 5C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 55 41 56 41 57 48 8B EC "
        "48 83 EC 70 48 8B F9 C5 F2 58 41 2C";

    // --- Field clock globals (client / server realm) -------------------------
    // Two BSS globals, each a struct of int32 { day, hour, minute, second }.
    // Located via the realm-select read: `mov edx, 1F2h` TLS probe followed by
    // two `vmovups ymm0, cs:<global>` (server, then client).
    inline constexpr const char* kSig_FieldTimeRealm =
        "BA F2 01 00 00 48 8B 08 0F B6 04 0A 84 C0 74 0A "
        "C5 FC 10 05 ?? ?? ?? ?? EB 08 C5 FC 10 05 ?? ?? ?? ??";
    inline constexpr uintptr_t kOff_FieldTime_ServerVmovups = 0x10;
    inline constexpr uintptr_t kOff_FieldTime_ClientVmovups = 0x1A;
    inline constexpr int       kLen_FieldTime_Vmovups       = 8;

    inline constexpr uintptr_t kOff_FieldTime_Day  = 0x00;
    inline constexpr uintptr_t kOff_FieldTime_Hour = 0x04;
    inline constexpr uintptr_t kOff_FieldTime_Min  = 0x08;
    inline constexpr uintptr_t kOff_FieldTime_Sec  = 0x0C;

    // --- Render TimeOfDay manager: the visible sun / moon / sky --------------
    // The sun rides the render manager's OWN accumulator, not the field clock,
    // so slowing the field tick alone would leave the sun at vanilla speed. The
    // manager clamps currentTimeOfDay into [lower, upper] every frame (the
    // engine's /settimeofday{lower,upper}limit console commands), so pinning
    // lower == upper == the field clock's hour makes the sun follow our clock.
    //
    //   engine  = *engineGlobal
    //   manager = *(engine + 0x2F8)
    //   manager + 0x3D0  f32 currentTimeOfDay (hours 0..24)
    //   manager + 0x3D4  f32 lowerLimit
    //   manager + 0x3D8  f32 upperLimit
    //
    // engineGlobal is the RIP target of the one-time init store
    //   cmp cs:<guard>, -1 ; jnz ; mov cs:<engineGlobal>, rbx ; mov cs:<..>, rdi
    inline constexpr const char* kSig_TodEngineGlobal =
        "83 3D ?? ?? ?? ?? FF 75 ?? 48 89 1D ?? ?? ?? ?? 48 89 3D";
    inline constexpr uintptr_t kOff_TodEngineGlobal_Mov = 9;
    inline constexpr int       kLen_TodEngineGlobal_Mov = 7;
    inline constexpr uintptr_t kOff_Tod_Manager     = 0x2F8;
    inline constexpr uintptr_t kOff_Tod_CurrentHour = 0x3D0;
    inline constexpr uintptr_t kOff_Tod_LowerLimit  = 0x3D4;
    inline constexpr uintptr_t kOff_Tod_UpperLimit  = 0x3D8;
}
