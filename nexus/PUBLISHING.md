# Publishing to Nexus Mods

Everything for the mod page is in this folder:

| File | Where it goes on Nexus |
| --- | --- |
| `summary.txt` | **Brief overview** (≤ 350 characters) |
| `description.bbcode` | **Detailed description**. Switch the editor to BBCode mode and paste it in. |
| `images/time-of-day-curve.png` | **Images**: an explanatory chart. Use it as a secondary image. |
| `../CHANGELOG.md` | The **Changelog** tab for each version |

## 1. Test in-game before the first upload

Do this for every release, on the current game build.

1. Download the build from **GitHub → Actions → Build → latest `main` run →
   Artifacts** and install it as the description explains.
2. Load a save and play for about 15 real minutes. Then check
   `bin64\CrimsonDesertLongerDays.log`:
   - It should say `Ready.`, with no `NOT FOUND` lines.
   - It should say `Sun sync verified: the sky follows the slowed clock.`
   - It should have hourly lines such as
     `Day 42 13:00 day time x2.00 slower | vanilla clock 12x | ran 6.0x real time`.
3. Watch the sky: the sun should move at about half its usual speed during
   the day. Press **F8** and it should speed back up to vanilla. Hold **F7**
   and the sun should race across the sky.
4. Sleep or wait once, and check that the time skips normally and the sky
   follows.
5. Take 2–4 in-game screenshots, such as a long golden sunset or a midday
   vista. The first one becomes the mod's thumbnail. Nexus prefers 16:9.

If something goes wrong, send the log to the developer (or to Claude).

## 2. Make the release zip

Bump the version in `src/version.h` if needed and add a section for it to
`CHANGELOG.md`. Then tag the release:

```sh
git tag v0.2.0
git push origin v0.2.0
```

CI does the rest:
- builds the mod and runs the tests,
- checks that the tag matches `src/version.h`,
- publishes a GitHub Release with `CrimsonDesertLongerDays-0.2.0.zip` and its
  SHA-256 checksum.

The zip mirrors the game folder:

```
bin64/CrimsonDesertLongerDays.asi
bin64/CrimsonDesertLongerDays.ini
bin64/CrimsonDesertLongerDays-LICENSE.txt
```

## 3. Create the mod page

- **Game:** Crimson Desert
- **Name:** Longer Days
- **Category:** Gameplay. If there's an Environment, Weather or Time category,
  that fits better.
- **Version:** `0.2.0` (must match the file version)
- **Author:** your Nexus name
- **Tags:** day night cycle, time, immersion, lighting, utility
- **Requirements:** Ultimate ASI Loader. Link the Nexus page if one exists for
  Crimson Desert, otherwise https://github.com/ThirteenAG/Ultimate-ASI-Loader.
- **Permissions and credits:**
  - Credits: XeTrinityz (Trinity) for the clock research, Tsuda Kageyu
    (MinHook), ThirteenAG (Ultimate ASI Loader).
  - Assets: open source under MIT. Others may modify and re-upload with
    credit. Say so on the permissions tab and link the GitHub repo.
- **Files tab:** upload the zip from the GitHub Release as a **Main file**,
  version `0.2.0`, description "Longer Days 0.2.0. Extract into the Crimson
  Desert folder."
- **Mod manager download:** leave it on if Vortex or other managers support
  Crimson Desert. The zip's `bin64/` layout is the standard one.

## 4. After publishing

- Check the Posts and Bugs tabs regularly. The most likely report after a game
  patch is `signature NOT FOUND` in the log. That means the signatures in
  `src/offsets.h` need finding again. To ship a fix, bump `version.h`, tag,
  and upload the new zip as an update.
- Expect some antivirus false positives. ASI files are unsigned DLLs that
  hook a game. The version info built into the DLL reduces them. Link the
  source code when someone asks.
