![MLGC](art/readme.png)

# Moonlighter GC

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

## What it does so far

Dungeon floors, generated on the console by Moonlighter-Recomp's generator (the code its
tests check against the real game, object by object and texture by texture), a room at a
time, with Will walking, rolling, falling into holes and attacking with the starting Broom
Spear (his controller, his and the weapon's Animators checked frame by frame against the game
on the PC). The Golem dungeon's BabySlimes chase him through the rooms' PolyNav maps, hurt
him on contact and die to his spear; he blinks and flashes when hurt. The other enemies are
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
| `art/` | The disc banner and the memory card banner (96 x 32), the memory card icon (32 x 32), and this page's banner |
| `MLGC/Source/` | The GameCube side: `MoonGame` (floors, rooms, the pad), `DungeonView` (draws a room with GX), `DiscData` (the data on the disc), `Main.cpp` |
| `MLGC/Makefile_GCN` | Compiles these and Moonlighter-Recomp's `recomp/src` (`cs`, `unity`, `util`, `game`, `render`) |
