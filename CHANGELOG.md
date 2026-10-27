# Changelog

## Unreleased

- `[Seasons]` follows Seasons of Pywel: with the new `SeasonLengthDays=0`
  (default), the season length picked in its F5 menu (3, 7, 14, 30, 45, 60 or
  90 days) and a season locked there are read from `bin64\Seasons.ini`, and
  menu changes apply within a second. Set `SeasonLengthDays` to 1–365 to choose
  your own. It replaces `YearLengthDays` and `SummerPeakDay`.
- Docs: a Compatibility section in the README, with notes for running
  alongside Seasons of Pywel.

## 0.2.0

First public release. Tested on game build 1.0.0.2976 (2.03.02).

- Day and night lengths can be set separately (default: days 2× longer,
  nights vanilla).
- Smooth dawn and dusk: the clock speed eases between night and day instead of
  switching at a fixed hour.
- Golden hours: sunrise and sunset last longer.
- `[RealTime]`: set day or night length in real minutes. The clock speed is
  read from the game itself, so the setting stays accurate after patches.
- Optional `[Seasons]`: daytime is longer in summer and shorter in winter
  across the in-game year.
- The sun, moon and sky stay in sync with the slowed clock.
- **F8** toggles the mod, with a short on-screen "Longer Days: ON/OFF" note
  (`ShowToast=0` turns it off). Hold **F7** to fast-forward time (x30 by
  default; `FastForwardKey`, `FastForwardSpeed`).
- Changes to the INI apply in-game within a second.
- The log has one line per in-game hour showing the current speed, why, and
  how fast the last hour really ran.
- Built to survive game updates: several signatures per target, every match
  checked before use, the sun's layout read from the game's own console
  commands, and self-checks that fall back to vanilla speed with an on-screen
  notice instead of misbehaving.
- How it works on 2.03.02: the game derives its clock from a snapshot plus
  elapsed time × rate; the mod steers that snapshot's timestamp each frame.
  Saves stay valid game values, so removing the mod leaves nothing behind.

## 0.1.0

Internal prototype (not released).
