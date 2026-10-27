# Crimson Desert – Longer Days

Makes the day in Crimson Desert last longer. Instead of one fixed speed for the
whole clock, it shapes the day:

- **Separate day and night speeds.** By default daytime lasts 2× as long and
  night is vanilla.
- **Smooth dawn and dusk.** The speed eases between night and day over a
  configurable window instead of jumping at a set hour.
- **Golden hours.** Sunrise and sunset last longer, slowing most at the exact
  moment and less on either side.
- **Real-minute targets.** Write "daytime lasts 90 real minutes" instead of
  working out a multiplier. The mod reads the clock speed from the game itself,
  so the target stays accurate after a patch.
- **Seasons (optional).** Days are longer in summer and shorter in winter
  across the in-game year. The defaults roughly line up with
  *Seasons of Pywel*.
- **Sky stays in sync.** The sun, moon and sky follow the slowed clock.
- **Live tuning.** Save the INI in-game and changes apply within a second. F8
  switches between this mod and vanilla, with a short on-screen note saying
  which one is active (borderless or windowed mode). Hold F7 to fast-forward
  time (x30), for example to watch a whole sunset in seconds.
- **Hourly log.** The log shows how much time is slowed each in-game hour and
  why.
- **Built to survive game updates.** Each piece of game code is found by
  several signatures, from exact to loose, and every match is checked before
  it's used. The mod then keeps checking itself while you play. If anything
  stops making sense after a patch, it drops back to vanilla speed and tells
  you on screen, instead of misbehaving. See
  [Game updates](#game-updates).

Only the clock slows down. Combat, physics, AI and animation run at normal
speed.

Nexus Mods page materials (description, summary, images, publishing checklist)
are in [`nexus/`](nexus/PUBLISHING.md).

![How fast time passes, hour by hour](nexus/images/time-of-day-curve.png)

> **Status: untested in-game.** The game logic is unit-tested, but the hook has
> not been run in the game yet. The signatures come from game build **2.03.00**,
> and the clock-tick lookup has also been tested against the bytes of build
> **1.0.0.2976** (2.03.02+). The log names your build and which signature
> matched. Single-player only.

## How it compares

Other Crimson Desert mods also touch the clock. Longer Days is the one built
around *shaping* the day rather than setting one speed:

| | Longer Days | [Timescale](https://www.nexusmods.com/crimsondesert/mods/3399) | [Crimson Weather](https://www.nexusmods.com/crimsondesert/mods/632) | [DesertLink Time & Weather](https://www.nexusmods.com/crimsondesert/mods/3557) |
| --- | --- | --- | --- | --- |
| Focus | How the day *feels* | One day length + fast-forward | Full weather/sky suite | Menu: set, freeze, fast-forward |
| Separate day / night speed | ✓ | – | ✓ | – |
| Smooth dawn/dusk blend | ✓ | – | – | – |
| Longer golden hours | ✓ | – | – | – |
| Seasonal day length | ✓ | – | – | – |
| Targets in real time, self-measured | ✓ (minutes) | real hours per day | – | – |
| Live INI reload | automatic on save | F10 | in-menu | in-menu |
| Requirements | ASI loader | ASI loader | ReShade (add-on) | DesertLink |

(Based on each mod's Nexus page, October 2026.) Don't run two of them that
change the clock speed at once.

## Install

1. Install **Ultimate ASI Loader** (x64) by putting its `winmm.dll` in
   `Crimson Desert\bin64\`. Skip this if your mod manager already includes an
   ASI loader.
2. Copy `CrimsonDesertLongerDays.asi` and `CrimsonDesertLongerDays.ini` into
   `Crimson Desert\bin64\`.
3. Start the game. `CrimsonDesertLongerDays.log` appears next to the `.asi` and
   should contain `Ready.`

**Getting the files:** download the zip from the
[Releases](https://github.com/La2o/CrimsonDesert-LongerDays/releases) page (or
the Nexus page, once it's up) and extract it into the Crimson Desert folder.
Development builds are under **Actions → Build → latest run → Artifacts**.

**Uninstall:** delete the `.asi` and `.ini`. Game files and saves are never
touched.

## Settings

All settings are in `CrimsonDesertLongerDays.ini`, and every key is explained in
the file. Some combinations:

| You want… | Set |
| --- | --- |
| Days twice as long (default) | `DayLengthMultiplier=2` |
| Whole cycle 3× slower | `DayLengthMultiplier=3`, `NightLengthMultiplier=3` |
| Long days, quick nights | `DayLengthMultiplier=2.5`, `NightLengthMultiplier=0.5` |
| Day lasts 2 real hours, night 20 min | `[RealTime]` `DayRealMinutes=120`, `NightRealMinutes=20` |
| Long sunsets for screenshots | `[GoldenHour]` `Multiplier=4`, `LengthHours=1.5` |
| Long summer days, short winter days | `[Seasons]` `Strength=0.4` |
| Vanilla, but with smooth golden hours | `DayLengthMultiplier=1`, `[GoldenHour]` `Multiplier=2` |

## How it works

At every moment the mod works out how much slower the clock should run:

```
speed = blend(night, day × season, dawn/dusk curve) × golden-hour bump
```

- **The day/night blend** uses a smoothstep curve centred on `DayStartHour` and
  `DayEndHour`.
- **The golden hour** is a raised-cosine bump at sunrise and sunset.
- **The season** is a cosine over the in-game year.

This math is in [`src/schedule.cpp`](src/schedule.cpp). It doesn't depend on the
game, and [`tests/tests.cpp`](tests/tests.cpp) tests it.

To apply the speed, the mod changes two parts of the game:

1. **The field clock.** The game doesn't add up frame times. It keeps a
   snapshot (`{day, hour, minute, second, ms}`, a rate and a timestamp) and
   works out the current time on every read: snapshot + (game milliseconds
   since the timestamp) × rate. Each frame the mod moves the timestamp forward
   by part of the time that just passed, so only 1/speed of it counts. Moving
   it back fast-forwards. The snapshot always stays a valid game value, so
   saves and uninstalling are unaffected. The math mirrors the game's own and
   is tested in [`src/game_time.h`](src/game_time.h). The hooked clock tick
   is just the mod's once-per-frame callback.
2. **The visible sun.** The render-side TimeOfDay manager moves the sun on its
   own separate accumulator. The mod pins its `[lower, upper]` time limit to the
   field clock's hour every frame, so the sun follows the slowed clock. The
   original limits are restored when the mod is turned off.

**The clock speed.** The vanilla speed is read from the snapshot's own rate
(12: daytime runs 12× real time, and the game counts 21:00–03:00 at half
weight, so night runs 24×). Each hourly log line also reports how fast the
last hour actually ran.

**Finding the game code.** All game-specific knowledge is in
[`src/offsets.h`](src/offsets.h), as byte signatures that are scanned when the
game loads. There are no fixed addresses. The scanner
([`src/pattern.h`](src/pattern.h)) finds each signature's longest fixed run with
Boyer-Moore-Horspool, then checks the wildcards only where that run matches.

## Game updates

No hook can promise to survive every patch, but this one is built to keep
working through the usual ones, and to fail safe when it can't.

**Layered signatures** ([`src/locate.cpp`](src/locate.cpp)). Each target has an
exact signature, then looser ones that anchor on the instructions that *are*
the feature and wildcard what a recompile tends to shuffle:

- **Clock tick:** the exact prologue, then the body (`mov rdi, rcx;
  vaddss xmm0, xmm1, [rcx+2Ch]; vmovss [rcx+2Ch], xmm0`) with the function
  start found from the unwind table or the alignment padding, then the same
  add/store pair at any offset. Body matches must be unique. Patch 2.03.02
  added one saved register to this prologue, which broke the exact signature
  but not the body one.
- **Field clock:** the realm-select read, with and without the TLS slot and
  branch lengths.
- **Sun:** the layout is read from the game's own `/settimeofdaylowerlimit`
  and `/settimeofdayupperlimit` console handlers: which engine virtual returns
  the TimeOfDay manager, and where its limits are. The getter that virtual
  points at is decoded for the manager's field. In 2.03.02 that is vtable
  `+0x60` and limits at `+0x3B4`, where 2.03.00 used `+0x2F8` and `+0x3D4`.
  The engine global comes from the console registrar's init store. Every
  other static-init store is a fallback candidate. A background thread waits until you're in the
  world. It then keeps the candidates whose time of day agrees with the field
  clock and, 4 seconds later, has moved forward like a running sun while its
  limits stayed put. The known struct offsets are tried first; if they stop
  matching, the engine and manager objects are searched, and a searched
  answer must be unique. Nothing is written until a candidate passes.

**Runtime self-checks** ([`src/longer_days.cpp`](src/longer_days.cpp)):

- The hook checks that it receives real frame times. If most of them aren't,
  it enters **safe mode**: the hook stays in but changes nothing, and a
  warning appears on screen.
- The clock must advance while game time passes. If it stops, the mod falls
  back to the plain day multiplier until it moves again.
- After pinning the sun, the sky must follow within 5 seconds, or sun sync is
  released for the session.

**On-screen notice.** If no signature finds the clock tick at all, nothing is
hooked, and a toast says the update isn't supported yet.

The log names the game build and which signature matched each target, so a
report from a new patch contains what's needed to update
[`src/offsets.h`](src/offsets.h).

## Troubleshooting

Check `bin64\CrimsonDesertLongerDays.log`:

- **No log:** the ASI loader isn't loading the mod. Check the loader DLL and
  that both files are in `bin64`.
- **`field-time tick was NOT FOUND`** or **`SAFE MODE`:** a game update changed
  the code beyond what the fallbacks cover. The game runs as vanilla. The
  signatures in `src/offsets.h` need updating; please send the log.
- **`could not find the sun`** or **`the sun did not follow`:** the clock still
  slows down, but the sky moves at vanilla speed. Another mod that controls
  the sun's time of day (for example a visual time override) can also cause
  this.
- **Conflicts:** don't use it with other mods that change day/night speed or
  freeze time of day, such as Timescale or trainer "freeze time" options. They
  change the same values.

## Building

```sh
# The mod (Windows, Visual Studio 2022 x64 or MinGW-w64). MinHook is fetched automatically.
cmake -S . -B build -A x64
cmake --build build --config Release        # -> build/Release/CrimsonDesertLongerDays.asi

# Unit tests (any OS)
cmake -S . -B build-tests -DLONGERDAYS_TESTS=ON
cmake --build build-tests && ctest --test-dir build-tests
```

## Credits

- The signatures and offsets for the game's clock and time of day come from the
  open-source [Trinity](https://github.com/XeTrinityz/Trinity) by XeTrinityz
  (MIT).
- The 2.03.02 sun layout was located with the help of the research notes in
  [crimson-workbench](https://github.com/ArasHueseyin/crimson-workbench)
  (`docs/research/LIVE_TIME_WEATHER.md`), which identify the console handlers
  and their new limit offsets. No code was taken from it.
- Everything else here was written for this mod.
- Hooking: [MinHook](https://github.com/TsudaKageyu/minhook).

Not affiliated with or endorsed by Pearl Abyss.
