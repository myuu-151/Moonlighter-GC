"""Makes the game's data from your Steam copy of Moonlighter, into build/game/ (which git
ignores), for the disc.

    python tools/make_data.py [--game <the game's folder>] [--recomp <Moonlighter-Recomp>]

1. Finds the game: the folder given (--game, or ML_GAME in the environment: a depot
   download, say, which Steam doesn't list), else through Steam (app 606150); and checks
   some of its files are the Steam copy's. (Moonlighter-Recomp's tools/make_data.py does
   both; this uses it.)
2. Extracts from it with Moonlighter-Recomp's tools/extract/, into build/game/: the
   Constants table, the room database, each dungeon's generation data and details
   registers, the asset bundles' index, the Wanderer DLC's rooms (if the DLC is installed).

Nothing from the game is stored in this repository.
"""
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
WORK = HERE / 'build' / 'game'


def recomp_folder(chosen=None):
    """Moonlighter-Recomp's checkout: the one chosen (an argument, or ML_RECOMP), else next
    to this repository. Exits, saying why, if it isn't one."""
    folder = Path(chosen or os.environ.get('ML_RECOMP') or HERE.parent / 'moonlighter')
    if not (folder / 'tools' / 'make_data.py').exists() or not (folder / 'recomp' / 'src').is_dir():
        sys.exit(f"{folder} isn't a Moonlighter-Recomp checkout (no tools/make_data.py and recomp/src in it).")
    return folder


def recomp_tools(recomp):
    """Moonlighter-Recomp's tools/make_data.py, as a module of its own (this one has the same name)."""
    import importlib.util
    spec = importlib.util.spec_from_file_location('recomp_make_data', recomp / 'tools' / 'make_data.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def find_game(chosen=None, recomp=None):
    return recomp_tools(recomp_folder(recomp)).find_game(chosen)


def step(title, args):
    print(f'-- {title}', flush=True)
    # No console window of its own; its output into ours.
    out = sys.stdout if sys.stdout is not None else None
    subprocess.run([sys.executable, '-u'] + [str(a) for a in args], check=True, stdout=out, stderr=out,
                   creationflags=0x08000000 if sys.platform == 'win32' else 0)


def main():
    import argparse
    parser = argparse.ArgumentParser(description='Makes the data from your copy of Moonlighter.')
    parser.add_argument('--game', help="the game's folder (else ML_GAME, else Steam's)")
    parser.add_argument('--recomp', help='the Moonlighter-Recomp checkout (else ML_RECOMP, else ../moonlighter)')
    args = parser.parse_args()
    recomp = recomp_folder(args.recomp)
    game = recomp_tools(recomp).find_game(args.game)
    print(f'The game: {game}', flush=True)
    try:
        import UnityPy  # noqa: F401
    except ImportError:
        sys.exit("UnityPy is needed to read the game's asset bundles: py -m pip install UnityPy")
    WORK.mkdir(parents=True, exist_ok=True)
    extract = recomp / 'tools' / 'extract'
    step("the Resources texts", [extract / 'resources_text.py', '--game', game, '--out', WORK])
    step('the room database', [extract / 'room_database.py', '--game', game, '--out', WORK])
    step("the dungeons' generation data", [extract / 'generation_data.py', '--game', game, '--out', WORK])
    step('indexing the asset bundles', [extract / 'bundle_index.py', '--game', game, '--out', WORK])
    step("the dungeons' details", [extract / 'details.py', '--game', game, '--out', WORK])
    step("the Wanderer DLC's rooms", [extract / 'wanderer_rooms.py', '--game', game, '--out', WORK])
    print(f'-- done: {WORK}', flush=True)


if __name__ == '__main__':
    main()
