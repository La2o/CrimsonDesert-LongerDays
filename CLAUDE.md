# CLAUDE.md

Longer Days is an ASI plugin (a DLL renamed `.asi`) for Crimson Desert that
slows the in-game clock, with separate day and night speeds, smooth dawn and
dusk, longer golden hours and optional seasonal day length. Players and the
README come first; this file is for working on the code.

## Working rules

- Commit directly on `main`. No feature branches.
- Ask before pushing, tagging or publishing anything (GitHub, Nexus).
- Never type the user's passwords. The user signs in to Nexus themselves.
- Keep docs in step with code: `README.md`, `CHANGELOG.md` (`## Unreleased`
  at the top), `CrimsonDesertLongerDays.ini` comments and
  `nexus/description.bbcode` all describe the same behaviour.

## Build and test

CMake isn't on PATH: use `C:\Program Files\CMake\bin\cmake.exe`. The mod
builds with MSVC (Visual Studio 2022, x64); MinHook is fetched by CMake.

```sh
cmake -S . -B build -A x64
cmake --build build --config Release        # -> build/Release/CrimsonDesertLongerDays.asi

cmake -S . -B build-tests -DLONGERDAYS_TESTS=ON
cmake --build build-tests && ctest --test-dir build-tests --output-on-failure
```

The tests (`tests/tests.cpp`) cover only game-independent logic:
`src/schedule.*` and the header-only `src/game_time.h`. CI builds them on
Linux with `-Wall -Wextra -Werror` and ASan/UBSan, so keep them portable and
warning-free.

## Installing for an in-game test

Game folder: `D:\SteamLibrary\steamapps\common\Crimson Desert\bin64`. Copy
`build/Release/CrimsonDesertLongerDays.asi` there (and the INI only if its
keys changed; the user's copy may have their settings). Ultimate ASI Loader is
installed as `winmm.dll`. The copy fails while the game is running: check
with `cmp` after copying, or wait for the game to exit first.

The log is `bin64\CrimsonDesertLongerDays.log`. A healthy run says `Ready.`,
`Sun sync verified`, and one line per in-game hour ending in
`ran N.Nx real time` (about 6x for vanilla rate 12 with days 2× longer).

## How the code fits together

- `src/dllmain.cpp`: starts `longer_days` on a thread.
- `src/longer_days.cpp`: the core. Hooks the field-time tick
  (`exe+0xA384A0` on 1.0.0.2976) purely as a per-frame callback, then steers
  time, keeps the sun pinned, handles F8 (toggle) and F7 (fast-forward), INI
  hot-reload and the hourly log.
- `src/game_time.h`: the game's clock snapshot (`day, hour, minute, second,
  ms`, weekday, `u16 rate` at +0x16, `u64 stamp` at +0x18) and conversion to
  and from units. Live time = `Units(snapshot) + (now − stamp) × rate`; a day
  is 75,600,000 units and 21:00–03:00 counts half.
- **Steering:** each frame the mod moves the snapshot's `stamp` forward by
  `dt × (1 − 1/length)` (CAS), on both the client and server copies.
  Fast-forward moves it back. It never writes the time fields directly except
  to re-anchor (copy a fresh snapshot) when the stamp would go below 0, so
  saves always hold valid game values.
- `src/schedule.*`: the speed curve (day/night lengths, dawn/dusk blend,
  golden hours, seasons, `[RealTime]` targets).
- `src/locate.*`, `src/scanner.*`, `src/pattern.h`, `src/sigtools.h`: find
  code and data by signature. `src/offsets.h` holds the signatures (several
  per target, exact to loose). Every match must be unique and is checked
  before use.
- **Sun:** a background SunFinder thread finds the TimeOfDay manager and pins
  its lower and upper limits to the clock hour. It restarts when a save loads.
- `src/config.*`: INI parsing with clamped values. `src/toast.*`: the on-screen
  note. `src/log.*`: the log file.

Fail safe: when a self-check fails (a missing signature, implausible values,
a stalled clock, a sky that doesn't follow), that part switches off with a
log line and a toast, and the game runs at vanilla speed.

## Releasing

1. Bump `src/version.h`, and turn `## Unreleased` in `CHANGELOG.md` into
   `## X.Y.Z`.
2. Commit, then tag `vX.Y.Z` and push both (with the user's OK). CI checks
   that the tag matches `version.h`, builds, and publishes a GitHub Release
   with a `bin64/` zip, a `.sha256` and that version's changelog section.
3. Nexus (mod 3897): follow `nexus/PUBLISHING.md`. Upload the CI zip, not a
   local build.
