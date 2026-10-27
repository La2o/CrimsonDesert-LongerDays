# Changelog

## Unreleased

- Pressing F8 briefly shows "Longer Days: ON" or "OFF" near the top of the
  screen (borderless and windowed mode). Turn it off with `ShowToast=0`.
- The mod no longer loads into the game's crash reporter
  (`crashpad_handler.exe`), which was overwriting the log.

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
