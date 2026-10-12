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

By hand, the data:

```
python tools/make_data.py [--game <the game's folder>] [--recomp <Moonlighter-Recomp>]
```

finds the game (`--game`, `ML_GAME`, else Steam app 606150), checks some of its files
are the Steam copy's, and runs Moonlighter-Recomp's `tools/extract/` into `build/game/`
(git-ignored): the asset bundles' index, the dungeons' details registers, the Wanderer
DLC's rooms.

## Layout

| Path | Contents |
|---|---|
| `Build MLGC.bat` | Starts the builder |
| `tools/builder.py` | The builder window |
| `tools/make_data.py` | Makes the data from your Steam copy, with Moonlighter-Recomp's extractors |
| `tools/make_art.py` | The disc banner (`MLGC/opening.bnr`) and the memory card save's icon and banner, from `art/` |
| `art/` | The disc banner and the memory card banner (96 x 32), the memory card icon (32 x 32), and this page's banner |
| `MLGC/` | The Octave project (to come) |
