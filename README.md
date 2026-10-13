![MLGC](art/readme.png)

# Moonlighter GC

![ported](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/myuu-151/Moonlighter-Recomp/main/progress/badge.json)
![verified](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/myuu-151/Moonlighter-Recomp/main/progress/badge_verified.json)

(How much of the game's code Moonlighter-Recomp has ported, and verified against the real game: its README has the breakdown.)

Moonlighter (Moonlighter-Recomp, the native reimplementation) on the Nintendo GameCube,
built on the [Octave](https://github.com/myuu-151/Octave-libogc) engine.


## What you need

- **Moonlighter, from Steam** (v1.15.8; the Wanderer DLC is optional). The game's data
  is made only from your own copy; none of it is in this repository. Installed through
  Steam, it's found by itself; downloaded another way (a depot download, which Steam
  doesn't list), choose its folder, the one with `Moonlighter.exe`.
- **[devkitPro](https://devkitpro.org/)** with devkitPPC and libogc, or
  [gekko-toolchain](https://github.com/myuu-151/gekko-toolchain).
- **[Octave-libogc](https://github.com/myuu-151/Octave-libogc)** v2.2 or later.
- **Moonlighter-Recomp**: the reimplementation's source (and its data extractors), which
  the GameCube build compiles.
- **Python 3** with [Pillow](https://python-pillow.org/) and
  [UnityPy](https://github.com/K0lb3/UnityPy) (`py -m pip install pillow UnityPy`).

By default they're found next to this repository:

```
Documents/
  MLGC/            this repository
  octave-libogc/   Octave-libogc
  moonlighter/     Moonlighter-Recomp
```

## Building

Double-click **`Build MLGC.bat`**: it checks each thing above, says how
to fix what's missing, and builds with one button, in the background, at low priority.

### By hand

**1. Make the data** from your copy of the game:

```
python tools/make_data.py [--game <the game's folder>] [--recomp <Moonlighter-Recomp>]
```

finds the game (`--game`, `ML_GAME`, else Steam app 606150), checks some of its files
are the Steam copy's, runs Moonlighter-Recomp's `tools/extract/` into `build/game/`, and
copies what the disc needs into `MLGC/Scripts/Data/` (both git-ignored): the Constants
table, each dungeon's rooms and prefabs, its sprites (GameCube textures, CI8 where 256
colours do), the tiles the generator paints holes, slime and floor details from, and the
disc banner and memory card art.

**2. Build the disc** from the `octave-libogc` folder:

```
Octave.exe -headless -project <path to this repo>/MLGC/MLGC.octp -build GameCube
```

This compiles `MLGC/Source/` and Moonlighter-Recomp's `recomp/src/` with `MLGC/Makefile_GCN`
and packs it with the data: `MLGC/Packaged/GameCube/MLGC.iso`. `make -f Makefile_GCN` in
`MLGC/` compiles just the DOL.

### The translated game code

With `TRANSLATED=1` in the environment (the builder: "Translated game code"), the build first
runs Moonlighter-Recomp's translator against your copy of the game (`ML_GAME`: its folder; needs
the [.NET 9 SDK](https://dotnet.microsoft.com/download)) into `MLGC/Intermediate/gen/`, and
compiles the game's own code, translated, with the scene facade it runs on (`recomp/src/bind`).
The game itself still runs the hand port; the translated code is what's coming
(Moonlighter-Recomp's `docs/translation_strategy.md`).

### The GameCube test of the translated code

A test disc runs the BabySlime's oracle recording against the translated code and then the hand
port, compares every row with the game's, and times both on the Gekko's timebase (Moonlighter-Recomp's
`docs/gamecube_translated.md` has the results):

```
py tools/make_test_data.py --recomp <Moonlighter-Recomp>      (the recording and its data, on the disc)
TTEST=1 in the environment, then Octave.exe -headless ... -build GameCube   (as above)
powershell -File tools\dolphin_test.ps1 -Out build\ttest.log    (Dolphin, on a hidden desktop)
```

`TTEST_PROFILE=1` as well samples where the translated replay's time goes;
`py tools/ttest_profile.py build\ttest.log` reads the samples. Test builds run in Dolphin only:
they print through OSReport, which overwrites a console's clock.

## What it does so far

Dungeon floors, generated on the console by Moonlighter-Recomp's generator (the code its
tests check against the real game, object by object and texture by texture), a room at a
time, with Will walking, rolling, falling into holes and attacking with the starting Broom
Spear (his controller, his and the weapon's Animators checked frame by frame against the game
on the PC). The Golem dungeon's BabySlimes, GolemSoldiers and Tangles chase him through the
rooms' PolyNav maps and fight him (the soldiers' sword swings from their clips' colliders)
and die to his spear; he blinks and flashes when hurt, and a bar at the top left
shows his health (a stand-in until the game's HUD art is extracted). The other enemies are
still their rooms' still sprites.

The game's controls on a GameCube pad (what's ported so far responds; the rest is mapped
for when it is):

| Button | Game action |
|---|---|
| Control stick / D-pad | Walk (push into a door to go through it) |
| A | Attack |
| R | Roll |
| X | Secondary attack |
| B (hold) | Merchant pendant |
| Y | Inventory |
| L | Potion |
| Z | Swap weapons |
| START | Pause |
| C-stick | Map |

Debug, holding START:

| Button | |
|---|---|
| A | Down to the next floor (after the third, a new run) |
| L / R | The dungeon before / after (Golem, Forest, Desert, Tech) |
| Y | A new run (a new seed) |
| Z | Full widescreen (16:9 TV) / letterboxed (4:3 TV) |

## Layout

| Path | Contents |
|---|---|
| `Build MLGC.bat` | Starts the builder |
| `tools/builder.py` | The builder window |
| `tools/make_data.py` | Makes the data from your Steam copy, with Moonlighter-Recomp's extractors |
| `tools/make_art.py` | The disc banner (`MLGC/opening.bnr`) and the memory card save's icon and banner, from `art/` |
| `tools/make_test_data.py`, `tools/dolphin_test.ps1`, `tools/ttest_profile.py` | The GameCube test of the translated code: its data, Dolphin on a hidden desktop, the profile |
| `art/` | The disc banner and the memory card banner (96 x 32), the memory card icon (32 x 32), and this page's banner |
| `MLGC/Source/` | The GameCube side: `MoonGame` (floors, rooms, the pad, Will, the HUD), `RoomEnemies` (the room's live enemies), `DungeonView` (draws a room with GX), `DiscData` (the data on the disc), `Main.cpp`; `TranslatedTest` (the test builds' replays) |
| `MLGC/Makefile_GCN` | Compiles these and Moonlighter-Recomp's `recomp/src` (`cs`, `unity`, `util`, `game`, `render`; with `TRANSLATED=1`, the translated code and `bind`) |
