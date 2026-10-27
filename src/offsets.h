#pragma once
#include <cstdint>

// Everything this mod knows about the game binary lives here: byte signatures
// (re-scanned at load, so a patch that only shifts code keeps working) and the
// struct offsets reached through them. Never an absolute address.
//
// Each target has several signatures, tried in order: the exact one from the
// build it was derived on, then looser ones that anchor on the few instructions
// that ARE the feature (the clock add, the realm select) and wildcard what a
// recompile tends to shuffle (saved registers, stack size, jump lengths, TLS
// slots). Every candidate is then checked before it is trusted - see
// locate.cpp - and checked again at runtime against the live game.
//
// The original signatures were reverse-engineered and live-verified by the
// Trinity project (https://github.com/XeTrinityz/Trinity, MIT,
// src/game/offsets.h) for game build 2.03.00.
namespace longerdays::offsets
{
    // Anything below this is not a plausible game pointer.
    inline constexpr uintptr_t kMinPointer = 0x10000000;

    // --- Field-time tick: the day/night clock's per-frame advance ------------
    // __fastcall(rcx = mgr, xmm1 = frameDeltaSeconds, xmm2 = unused). Its first
    // act is `[mgr+0x2C] += delta`, a float accumulator that only times the
    // periodic time sync; it then reads the live field time and hands it to
    // the sky. Runs once per frame on the game thread, which is what the mod
    // hooks it for: the delta is passed through untouched (see the field
    // clock below for how time is actually slowed).

    // Whole prologue, build 2.03.00. The match is the function start.
    inline constexpr const char* kSig_FieldTimeTick_Exact =
        "48 89 5C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 55 41 56 41 57 48 8B EC "
        "48 83 EC 70 48 8B F9 C5 F2 58 41 2C";

    // The body: mov rdi, rcx ; vaddss xmm0, xmm1, [rcx+2Ch] ;
    // vmovss [rcx+2Ch], xmm0. Later builds save an extra register in the
    // prologue (2.03.02+), which breaks the exact signature but not this. The
    // function start is found by walking back to its alignment padding.
    inline constexpr const char* kSig_FieldTimeTick_Body =
        "48 8B F9 C5 F2 58 41 2C C5 FA 11 41 2C";

    // Looser still: the same add/store pair at any accumulator offset and
    // followed by the flag test the function opens with. Only accepted when
    // the add and the store use the same offset and the hit is unique.
    inline constexpr const char* kSig_FieldTimeTick_Loose =
        "C5 F2 58 41 ?? C5 FA 11 41 ?? 80 79";
    inline constexpr int kOff_Loose_AddDisp   = 4;
    inline constexpr int kOff_Loose_StoreDisp = 9;

    // How far back from a body hit the function may start.
    inline constexpr int kMaxPrologueBytes = 0x60;

    // --- Field clock globals (client / server realm) -------------------------
    // Two BSS globals, each a 32-byte gametime::Snapshot (game_time.h). They
    // are NOT the live time: the game's getter adds the game milliseconds
    // elapsed since the snapshot's stamp, times its rate. In 2.03.02 the
    // field-time tick above only reads that getter (to hand the time to the
    // sky and to send periodic syncs), so the time is steered through the
    // snapshot's stamp, not through the tick's delta.
    // Located via the realm-select read: `mov edx, <tls slot>` TLS probe
    // followed by two `vmovups ymm0, cs:<global>` (server, then client).
    struct RealmSig
    {
        const char* pattern;
        int         clientVmovups; // offset of the client-realm vmovups in the match
    };
    inline constexpr RealmSig kSig_FieldTimeRealm[] = {
        // Exact, build 2.03.00.
        {"BA F2 01 00 00 48 8B 08 0F B6 04 0A 84 C0 74 0A "
         "C5 FC 10 05 ?? ?? ?? ?? EB 08 C5 FC 10 05 ?? ?? ?? ??", 0x1A},
        // Any TLS slot, any branch lengths.
        {"BA ?? ?? 00 00 48 8B 08 0F B6 04 0A 84 C0 74 ?? "
         "C5 FC 10 05 ?? ?? ?? ?? EB ?? C5 FC 10 05 ?? ?? ?? ??", 0x1A},
        // Just the realm test and the two loads.
        {"0F B6 04 0A 84 C0 74 ?? C5 FC 10 05 ?? ?? ?? ?? EB ?? C5 FC 10 05 ?? ?? ?? ??", 0x12},
    };
    inline constexpr int kLen_FieldTime_Vmovups = 8;

    // The server-realm load sits just before the client one: `vmovups` (8
    // bytes) then a short `jmp` (2 bytes).
    inline constexpr int kOff_ServerVmovups = -10;

    // The game-millisecond counter the getter measures elapsed time with, in
    // the same getter: mov rbx, [rip+offset] ; xor ecx, ecx ; test al, al ;
    // cmovne rbx, rcx ; vzeroupper ; call now. `offset` is added for the
    // client realm only (it is dropped when the TLS realm flag is set).
    inline constexpr const char* kSig_FieldTimeNow =
        "48 8B 1D ?? ?? ?? ?? 33 C9 84 C0 48 0F 45 D9 C5 F8 77 E8 ?? ?? ?? ??";
    inline constexpr int kLen_NowOffsetMov = 7;
    inline constexpr int kOff_NowCall      = 18;
    inline constexpr int kLen_NowCall      = 5;

    // --- Render TimeOfDay manager: the visible sun / moon / sky --------------
    // The sun rides the render manager's OWN accumulator, not the field clock,
    // so slowing the field tick alone would leave the sun at vanilla speed. The
    // manager clamps currentTimeOfDay into [lower, upper] every frame (the
    // engine's /settimeofday{lower,upper}limit console commands), so pinning
    // lower == upper == the field clock's hour makes the sun follow our clock.
    //
    //   engine  = *engineGlobal
    //   manager = engine->vfunc[slot]()       (a plain field getter)
    //   manager + lower - 4  f32 currentTimeOfDay (hours 0..24)
    //   manager + lower      f32 lowerLimit
    //   manager + lower + 4  f32 upperLimit
    //
    //   build      slot   getter field   lower
    //   2.03.00    0x40   +0x2F8         0x3D4
    //   2.03.02    0x60   (read live)    0x3B4
    //
    // Rather than hard-code the 2.03.02 column, the mod reads it from the
    // game's own console handlers for those commands, which do exactly
    //   mov rcx,[rcx] ; mov rax,[rcx] ; call [rax+slot] ; ... ;
    //   vmovss [rax+lower], xmm1          (and +upper in the twin handler)
    // and decodes the getter the slot points at. Both handlers must agree.
    inline constexpr const char* kSig_TodLimitHandler =
        "48 8B 01 FF 50 ?? 48 8B 0F 48 8B D8 48 8B 49 08 FF 15 ?? ?? ?? ?? "
        "C5 FB 5A C8 C5 FA 11 8B ?? ?? ?? ??";
    inline constexpr int kOff_TodLimitHandler_Slot  = 5;  // disp8 of `call [rax+slot]`
    inline constexpr int kOff_TodLimitHandler_Limit = 30; // disp32 of the vmovss store

    // engineGlobal is set once by the console registrar's thread-safe static
    // init:
    //   cmp cs:<guard>, -1 ; jnz ; mov cs:<engineGlobal>, rsi ;
    //   mov cs:<..>, rbx ; mov rax, [rbx] ; mov rdx, [rax+28h]      (2.03.02)
    // 2.03.00 stored rbx/rdi instead. Trinity's exact shape now matches an
    // unrelated initializer, so the registrar shape goes first, then every
    // static-init store as a fallback candidate; the live game picks the one
    // whose sun agrees with - and moves like - the field clock.
    struct TodGlobalSig
    {
        const char* pattern;
        int         movOffset; // where the `mov cs:<global>, reg` starts in the match
    };
    inline constexpr TodGlobalSig kSig_TodEngineGlobal[] = {
        {"83 3D ?? ?? ?? ?? FF 75 ?? 48 89 ?? ?? ?? ?? ?? 48 89 ?? ?? ?? ?? ?? "
         "48 8B ?? 48 8B 50 28", 9},                                     // console registrar
        {"83 3D ?? ?? ?? ?? FF 75 ?? 48 89 1D ?? ?? ?? ?? 48 89 3D", 9}, // exact, 2.03.00
        {"83 3D ?? ?? ?? ?? FF 75 ??", 9},                               // short jnz, any store
        {"83 3D ?? ?? ?? ?? FF 0F 85 ?? ?? ?? ??", 13},                  // long jnz, any store
    };
    inline constexpr int kLen_TodEngineGlobal_Mov = 7; // REX + 89 + ModRM + disp32
    inline constexpr int kMaxTodCandidates        = 4096;
    inline constexpr int kMaxTodSearchGlobals     = 256; // deep search is costlier

    // 2.03.00 field offsets: the fallback when the handlers can't be read.
    // If nothing makes sense, the manager is searched (see locate.cpp).
    inline constexpr uintptr_t kOff_Tod_Manager     = 0x2F8;
    inline constexpr uintptr_t kOff_Tod_CurrentHour = 0x3D0;
    inline constexpr uintptr_t kOff_Tod_LowerLimit  = 0x3D4; // CurrentHour + 4
    inline constexpr uintptr_t kOff_Tod_UpperLimit  = 0x3D8; // CurrentHour + 8

    // Search windows around the known offsets, for when they move.
    inline constexpr uintptr_t kTodManagerSearchLo = 0x200, kTodManagerSearchHi = 0x500;
    inline constexpr uintptr_t kTodHourSearchLo    = 0x300, kTodHourSearchHi    = 0x600;
}
