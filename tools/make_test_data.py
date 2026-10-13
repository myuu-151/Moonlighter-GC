"""The data the test builds' replay reads (TTEST=1: MLGC/Source/TranslatedTest.cpp runs the
BabySlime's oracle replay on the GameCube), into MLGC/Scripts/Data/ttest/ (which git ignores; Octave
puts it on the disc with the rest of Scripts/):

- the recording: Moonlighter-Recomp's data/oracle/slime.jsonl (the oracle's, from your copy of the
  game: `-oracle-slime`), or the one given;
- what the replays read of the game's data, from build/game (tools/make_data.py makes it from your
  copy): constants, enemy_stats, physics2d, the BabySlime's animator; the prefabs and MasterAudio's
  sound groups cut to what the replay uses (the BabySlime's prefab; the Golem dungeon's and the
  general dungeon sounds), so that their parse fits the GameCube's memory. The sound groups are
  extracted first if build/game doesn't have them (Moonlighter-Recomp's tools/extract/sound_groups.py).

    py tools/make_test_data.py [--recomp <Moonlighter-Recomp>] [--game <the game's folder>] [--recording <slime.jsonl>]

Nothing from the game is stored in this repository.
"""
import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
WORK = HERE / 'build' / 'game'
OUT = HERE / 'MLGC' / 'Scripts' / 'Data' / 'ttest'
PREFAB, BUNDLE = 'BabySlime', 'enemies_golem'
SOUND_GROUPS = ('Sounds - GolemDungeon', 'Sounds - GeneralDungeons')  # (the replays' MasterAudio.Load)


def main():
    sys.path.insert(0, str(HERE / 'tools'))
    import make_data
    parser = argparse.ArgumentParser(description="The test builds' replay data (TTEST=1).")
    parser.add_argument('--recomp', help='the Moonlighter-Recomp checkout (else ML_RECOMP, else ../moonlighter)')
    parser.add_argument('--game', help="the game's folder (else ML_GAME, else Steam's)")
    parser.add_argument('--recording', help="the recording (default: Moonlighter-Recomp's data/oracle/slime.jsonl)")
    args = parser.parse_args()
    recomp = make_data.recomp_folder(args.recomp)
    recording = Path(args.recording) if args.recording else recomp / 'data' / 'oracle' / 'slime.jsonl'
    if not recording.exists():
        sys.exit(f'no recording: {recording} (record it with the oracle: -oracle-slime)')
    for name in ('constants.json', 'enemy_stats.json', 'physics2d.json', 'prefabs.json', f'animators/{PREFAB}.json'):
        if not (WORK / name).exists():
            sys.exit(f'no {WORK / name}: make the data first (tools/make_data.py)')
    if not (WORK / 'sound_groups.json').exists():
        game = make_data.find_game(args.game, str(recomp))
        print('-- the sound groups', flush=True)
        subprocess.run([sys.executable, '-u', str(recomp / 'tools' / 'extract' / 'sound_groups.py'), '--game', str(game),
                        '--out', str(WORK)], check=True)

    if OUT.exists():
        shutil.rmtree(OUT)
    (OUT / 'animators').mkdir(parents=True)
    shutil.copy2(recording, OUT / 'slime.jsonl')
    for name in ('constants.json', 'enemy_stats.json', 'physics2d.json', f'animators/{PREFAB}.json'):
        shutil.copy2(WORK / name, OUT / name)
    prefabs = json.loads((WORK / 'prefabs.json').read_text(encoding='utf-8'))
    cut = {key: value for key, value in prefabs.items() if key != 'bundles'}
    cut['bundles'] = {BUNDLE: {PREFAB: prefabs['bundles'][BUNDLE][PREFAB]}}
    (OUT / 'prefabs.json').write_text(json.dumps(cut, separators=(',', ':')), encoding='utf-8')
    sounds = json.loads((WORK / 'sound_groups.json').read_text(encoding='utf-8'))
    (OUT / 'sound_groups.json').write_text(json.dumps({k: sounds[k] for k in SOUND_GROUPS}, separators=(',', ':')),
                                           encoding='utf-8')
    total = sum(f.stat().st_size for f in OUT.rglob('*') if f.is_file())
    print(f'-- done: {OUT} ({total // 1024} KB; the recording {recording})', flush=True)


if __name__ == '__main__':
    main()
