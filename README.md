# Crimson Desert – Longer Days

Makes the day in Crimson Desert last longer. By default, daytime lasts **2×** as
long and night stays vanilla. Both can be set separately, and the sun, moon and
sky move in step with the slowed clock.

> **Status: untested in-game (v0.1.0).** It is built on signatures that the
> Trinity mod confirmed in game build **2.03.00**. If your build differs, the log
> will say so (see [Troubleshooting](#troubleshooting)). Single-player only.

## Install

1. Install **Ultimate ASI Loader** (x64): put its `winmm.dll` (or
   `dinput8.dll`) in `Crimson Desert\bin64\`. Skip this step if your mod manager
   already provides an ASI loader.
2. Copy `CrimsonDesertLongerDays.asi` and `CrimsonDesertLongerDays.ini` into
   `Crimson Desert\bin64\`.
3. Start the game. A `CrimsonDesertLongerDays.log` file appears next to the
   `.asi`, and its last line should read `Ready.`

**Getting the files:** each push to this repo builds the mod on GitHub Actions.
Open **Actions → Build → latest run → Artifacts** and download
`CrimsonDesertLongerDays`. To build it yourself, see [Building](#building).

**Uninstall:** delete the `.asi` and `.ini`. The mod never touches game files or
saves.

## Settings (`CrimsonDesertLongerDays.ini`)

| Key | Default | Meaning |
| --- | --- | --- |
| `Enabled` | `1` | Master switch. |
| `DayLengthMultiplier` | `2.0` | How many times longer daytime lasts (0.1–20). |
| `NightLengthMultiplier` | `1.0` | Same for night. |
| `DayStartHour` / `DayEndHour` | `6` / `20` | In-game hours that count as day. A window that wraps past midnight also works. |
| `SyncSun` | `1` | Keep the sun and sky in step with the slowed clock. |
| `ToggleKey` | `0x77` (F8) | Turns the mod on/off in-game, for comparing against vanilla. `0` disables the key. |

Save the INI while the game is running and the new settings apply within a
second.

Examples:
- Whole cycle 3× slower: `DayLengthMultiplier=3`, `NightLengthMultiplier=3`
- Long days, short nights: `DayLengthMultiplier=2.5`, `NightLengthMultiplier=0.5`

## How it works

Crimson Desert's clock has two parts:

1. **The field clock.** Each frame, a tick function adds the frame time to an
   accumulator, and the game's `{day, hour, minute, second}` clock comes from
   that accumulator. The mod hooks the tick and divides the frame time by the
   multiplier for the current hour. Only the clock slows down: physics, AI,
   combat and animation keep their normal speed.
2. **The visible sun.** The render-side TimeOfDay manager moves the sun on a
   separate accumulator, so it would keep vanilla speed. Its
   `[lowerLimit, upperLimit]` clamp is the one the engine's
   `/settimeofday…limit` console commands use. The mod sets both limits to the
   field clock's hour every frame, which holds the sun to the slowed clock. The
   original limits are restored when the mod is switched off.

Sleeping, waiting and other time skips change the field clock directly, so they
still work and the sun follows.

All game-specific knowledge is in [`src/offsets.h`](src/offsets.h), as byte
signatures that are scanned when the game loads. There are no hard-coded
addresses.

## Troubleshooting

Check `bin64\CrimsonDesertLongerDays.log`:

- **No log file:** the ASI loader isn't loading the mod. Check the loader DLL
  name and that both files are in `bin64`.
- **`field-time tick signature NOT FOUND`:** a game update changed the code. The
  signatures in `src/offsets.h` need to be found again. A second log from the
  launcher process with this error is harmless.
- **`time-of-day engine signature NOT FOUND`:** the clock still slows down, but
  the sky moves at vanilla speed.
- **Conflicts:** don't use this with another mod that changes the day/night
  speed or clamps time of day (for example Timescale, or freeze-time features
  in trainers). They change the same values.

## Building

Requires CMake ≥ 3.20 and either Visual Studio 2022 (x64) or MinGW-w64.
MinHook is downloaded automatically.

```sh
cmake -S . -B build -A x64
cmake --build build --config Release
# -> build/Release/CrimsonDesertLongerDays.asi
```

## Credits

- Clock and time-of-day signatures: [Trinity](https://github.com/XeTrinityz/Trinity)
  by XeTrinityz (MIT).
- Hooking: [MinHook](https://github.com/TsudaKageyu/minhook).

Not affiliated with or endorsed by Pearl Abyss.
