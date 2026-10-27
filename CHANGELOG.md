# Changelog

## Unreleased

- Pressing F8 briefly shows "Longer Days: ON" or "OFF" near the top of the
  screen (borderless and windowed mode). Turn it off with `ShowToast=0`.
- The mod no longer loads into the game's crash reporter
  (`crashpad_handler.exe`), which was overwriting the log.
- Works with game build 1.0.0.2976 (2.03.02+), whose clock-tick prologue
  broke the 2.03.00 signature.
- Update resilience: each target now has several signatures, from exact to
  loose, and every match is checked before use. The sun's time of day is
  matched live against the game clock, and searched for if a patch moved it.
- Runtime self-checks: safe mode (vanilla speed plus an on-screen notice) if
  the hook sees values that aren't frame times. The mod also falls back
  automatically when the clock stops moving or the sky doesn't follow.
- The log names the game build and which signature matched each target.
- Fixed periodic stutter: the sun search now runs on a background thread,
  reads memory in blocks and only starts once you're in the world.
- Sun layout for 2.03.02 is read from the game's own time-of-day limit
  console handlers (manager getter at vtable+0x60, limits at +0x3B4), and the
  engine global from the console registrar's init store. 2.03.00's
  +0x2F8/+0x3D4 layout stays as a fallback.
- Sun finding fallback: every other static-init store is a candidate. A
  candidate is used only if its time of day agrees with the clock and keeps
  moving like a sun a few seconds later. If none qualifies, the log gets a
  diagnostic block.

## 0.2.0

First public release.

- Day and night lengths can be set separately (default: days 2× longer,
  nights vanilla).
- Smooth dawn and dusk: the clock speed eases between night and day instead of
  switching at a fixed hour.
- Golden hours: sunrise and sunset last longer.
- `[RealTime]`: set day or night length in real minutes. The game's clock speed
  is measured while you play, so the setting stays accurate after patches.
- Optional `[Seasons]`: daytime is longer in summer and shorter in winter
  across the in-game year.
- The sun, moon and sky stay in sync with the slowed clock.
- Changes to the INI apply in-game within a second, and F8 toggles the mod.
- The log has one line per in-game hour showing the current speed and why.

## 0.1.0

Internal prototype (not released).
