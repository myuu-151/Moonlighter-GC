"""The game's sound for the disc: the clips Moonlighter-Recomp's tools/extract/audio.py decoded
(build/game/audio: audio.json and pcm/*.wav) as DSP-ADPCM (tools/dspadpcm.py), in the
project's Scripts/Data/audio (which git ignores), as Source/AudioGc.cpp plays them.

    python tools/make_audio.py [--work build/game] [--out MLGC/Scripts/Data]

- Sound effects: mono, at their own rate up to 32 kHz, one bank a sound group creator
  ("Sounds - Will", "Sounds - GolemDungeon", ...: audio/banks/<bank>.bin), read into ARAM when
  its scene needs it. A clip two creators use is in the bank that is loaded whenever the other
  is (the first scene's Will and interface, then the dungeons' GeneralDungeons), else in each.
- Streamed from the disc (audio/streams/<clip>.dsp): the playlists' songs, the long ambient
  loops (over 8 s) and anything over 30 s; their own channels, up to 32 kHz.
- audio/groups/<bank>.json: the creator's groups as sound_groups.json has them (what the
  port's game::MasterAudio loads: the same data as the PC's, so the same draws), each variation
  with what plays it: "clip", the AudioSource's "volume", "pitch" and "loop", the random modes,
  "mixerGain" (the AudioMixer's attenuation on its way out: below), and the group's
  "groupMasterVolume".
- The mixer: the game sends its sounds through one AudioMixer, MasterAudio's buses to its groups
  (Master/SoundWrapper/Sounds/steps, Master/MusicWrapper/Music/GolemMusic/Dungeon, ...), each
  with a volume in dB (the start snapshot's); the options' volumes are the wrappers' (exposed
  musicVol, sfxVol, uiVol: GlobalAudioManager.SetVolume, from its min to its max dB as the option
  goes from 0 to 8; 4 when it has never been set). A sound's gain is its group's dB and its
  parents', with the options at 4.
- audio/index.json: {"rate": 32000, "blockFrames": 512,
    "banks": {bank: {"file", "bytes", "creators"}},
    "creators": {creator: {"bank", "groups"}},
    "clips": {key: {"bank", "offset", "bytes", "rate", "channels", "samples"} or
                   {"stream", "bytes", "rate", "channels", "samples"}},
    "playlists": {name: {"crossFadeTime", "songs": [{"clip", "volume", "pitch", "loop"}]}},
    "controllers": {name: {"startPlaylistName", "playlistVolume", "loopPlaylist", "mixerGain"}},
    "playlistManager": {PlaylistManager's field: its controller's name}}

The encoded clips are kept in build/game/audio/gc/ and made again only when their PCM or this
tool's settings change. Numbers for the budget go to build/game/audio/gc/budget.json.
"""
import argparse
import json
import multiprocessing
import os
import re
import shutil
import sys
import time
import wave
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dspadpcm  # noqa: E402

HERE = Path(__file__).resolve().parents[1]
RATE = 32000
VERSION = 1  # of the encoding: a change makes every clip again
STREAM_AMBIENT_OVER = 8.0   # seconds: an ambient loop longer streams
STREAM_ANY_OVER = 30.0      # anything longer streams
# Banks loaded with every other (the first scene's), then with every dungeon's.
PERMANENT = ('Sounds - Will', 'Sounds - interface')
DUNGEON_COMMON = 'Sounds - GeneralDungeons'
DUNGEONS = ('Sounds - GolemDungeon', 'Sounds - ForestDungeon', 'Sounds - DesertDungeon', 'Sounds - TeslaDungeon',
            'Sounds - UnknownDungeon', 'Sounds - Wanderer')
BATCH_BLOCKS = 1200
# The options' sliders, 0-8, at their default (OptionsPanel: OptionsManager.Get("MUSIC", ..., 4) / 8),
# and the exposed mixer parameters they set (GlobalAudioManager), with the ranges' fields.
OPTION_DEFAULT = 4 / 8
EXPOSED = {'musicVol': ('minMusicVolume', 'maxMusicVolume'), 'sfxVol': ('minSoundsVolume', 'maxSoundsVolume'),
           'uiVol': ('minUIVolume', 'maxUIVolume')}


def mixer_gains(audio):
    """Each mixer group's gain (a factor), its parents' included, the options at their default."""
    import zlib
    mixer = audio.get('mixer')
    if not mixer:
        return []
    groups = mixer['groups']
    db = [g['volume'] for g in groups]
    options = audio.get('options', {})
    for name, (lo, hi) in EXPOSED.items():
        group = mixer['exposed'].get(str(zlib.crc32(name.encode())))
        if group is not None and lo in options and hi in options:
            db[group] = options[lo] + (options[hi] - options[lo]) * OPTION_DEFAULT
    out = []
    for i in range(len(groups)):
        total, at = 0.0, i
        while at >= 0:
            total += db[at]
            at = groups[at]['parent']
        out.append(10.0 ** (total / 20.0))
    return out


def route_gain(gains, *routes):
    """The gain of the first route that is a mixer group (else none: 1)."""
    for r in routes:
        if r is not None and 0 <= r < len(gains):
            return gains[r]
    return 1.0


def safe(name):
    return re.sub(r'[^A-Za-z0-9_.+-]', '_', name)


def bank_name(creator):
    return safe(creator.replace('Sounds - ', '').lower())


def progress(done, total):
    if os.environ.get('ML_PROGRESS'):
        print(f'@@ {done} {total}', flush=True)


def read_wav(path):
    with wave.open(str(path)) as w:
        data = np.frombuffer(w.readframes(w.getnframes()), '<i2').reshape(-1, w.getnchannels())
        return data.astype(np.float64), w.getframerate()


def resample(x, rate, to, loop):
    """x (n, channels) to `to` Hz: by the spectrum (a loop as the periodic signal it is; a sound
    played once with silence after it first, so its end doesn't ring into its start)."""
    if rate == to:
        return x
    n = len(x)
    pad = 0 if loop else 4096
    total = n + pad
    m = int(round(total * to / rate))
    spectrum = np.fft.rfft(x, total, axis=0)
    keep = min(spectrum.shape[0], m // 2 + 1)
    out = np.zeros((m // 2 + 1, x.shape[1]), complex)
    out[:keep] = spectrum[:keep]
    y = np.fft.irfft(out, m, axis=0) * (m / total)
    return y[:int(round(n * to / rate))]


def prepare(item):
    """A clip's channels as 16-bit samples at its target rate."""
    key, pcm, channels, loop = item
    x, rate = read_wav(pcm)
    if channels == 1 and x.shape[1] > 1:
        x = x.mean(axis=1, keepdims=True)
    target = min(rate, RATE)
    y = resample(x, rate, target, loop)
    y = np.clip(np.round(y), -32768, 32767).astype(np.int16)
    return key, target, [y[:, c].copy() for c in range(y.shape[1])]


def encode_batch(batch):
    x, hist = batch
    coefs, headers, residuals, _ = dspadpcm.encode_blocks(x.astype(np.int64), hist.astype(np.int64))
    return coefs.astype(np.int16), headers, residuals.astype(np.int8)


def low_priority():
    if os.name == 'nt':
        try:
            import ctypes
            ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x4000)
        except Exception:  # noqa: BLE001
            pass


def plan(audio, groups_json):
    """What each clip becomes: {key: {"stream": bool, "channels", "banks": [...]}}."""
    clips = audio['clips']
    users = {}  # key -> set of creators using it
    ambient, looped = set(), set()
    for creator, c in audio['creators'].items():
        for gname, g in c['groups'].items():
            for v in g['variations']:
                if not v['clip']:
                    continue
                users.setdefault(v['clip'], set()).add(creator)
                if v['loop']:
                    looped.add(v['clip'])
                if 'ambient' in gname.lower():
                    ambient.add(v['clip'])
    songs = set()
    for p in audio['playlists'].values():
        for s in p['songs']:
            if s['clip']:
                songs.add(s['clip'])
                if s['loop']:
                    looped.add(s['clip'])
    out = {}
    for key in set(users) | songs:
        c = clips[key]
        stream = key in songs or (key in ambient and c['length'] > STREAM_AMBIENT_OVER) or c['length'] > STREAM_ANY_OVER
        banks = []
        if not stream:
            who = users.get(key, set())
            permanent = [cr for cr in PERMANENT if cr in who]
            if permanent:
                banks = [permanent[0]]
            elif DUNGEON_COMMON in who and who <= set(DUNGEONS) | {DUNGEON_COMMON}:
                banks = [DUNGEON_COMMON]
            else:
                banks = sorted(who)
        out[key] = {'stream': stream, 'channels': c['channels'] if stream else 1, 'banks': banks,
                    'loop': key in looped}
    return out


def encode_clips(todo, pool, manifest):
    """Encodes clips (todo's items: key, pcm, plan, stamp, target), their blocks side by side."""
    prepared = pool.map(prepare, [(k, p, w['channels'], w['loop']) for k, p, w, _, _ in todo], chunksize=2)
    pieces = []  # a channel each: (clip, x (blocks, samples) int16, hist (blocks, 2) int16, frames, samples)
    for i, (_, rate, chans) in enumerate(prepared):
        for samples in chans:
            x, hist, frames = dspadpcm.split(samples)
            pieces.append((i, x.astype(np.int16), hist.astype(np.int16), frames, len(samples)))
    rows = [(p, b) for p in range(len(pieces)) for b in range(len(pieces[p][3]))]
    batches = [rows[k:k + BATCH_BLOCKS] for k in range(0, len(rows), BATCH_BLOCKS)]
    jobs = ((np.stack([pieces[p][1][b] for p, b in batch]), np.stack([pieces[p][2][b] for p, b in batch]))
            for batch in batches)
    encoded = {}  # piece -> {block: (coefs, headers, residuals)}
    for batch, (coefs, headers, residuals) in zip(batches, pool.imap(encode_batch, jobs)):
        for j, (p, b) in enumerate(batch):
            encoded.setdefault(p, {})[b] = (coefs[j], headers[j], residuals[j])
    for i, (key, pcm, w, stamp, target) in enumerate(todo):
        _, rate, chans = prepared[i]
        mine = [p for p in range(len(pieces)) if pieces[p][0] == i]
        count = len(pieces[mine[0]][3])
        out = bytearray()
        for b in range(count):
            for p in mine:
                coefs, headers, residuals = encoded[p][b]
                out += dspadpcm.block_bytes(coefs, pieces[p][2][b], pieces[p][3][b], headers, residuals,
                                            full=w['stream'] or b < count - 1)
        target.write_bytes(bytes(out))
        manifest[key] = {'stamp': stamp, 'rate': rate, 'channels': len(chans), 'samples': pieces[mine[0]][4],
                         'bytes': len(out)}


def main():
    parser = argparse.ArgumentParser(description="Converts the game's sound for the GameCube.")
    parser.add_argument('--work', default=str(HERE / 'build' / 'game'))
    parser.add_argument('--out', default=str(HERE / 'MLGC' / 'Scripts' / 'Data'))
    parser.add_argument('--jobs', type=int, default=max(1, min(8, (os.cpu_count() or 2) // 2)))
    args = parser.parse_args()
    low_priority()
    work = Path(args.work)
    audio = json.loads((work / 'audio' / 'audio.json').read_text(encoding='utf-8'))
    groups_json = json.loads((work / 'sound_groups.json').read_text(encoding='utf-8'))
    cache = work / 'audio' / 'gc'
    cache.mkdir(parents=True, exist_ok=True)
    manifest_path = cache / 'manifest.json'
    try:
        manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        manifest = {}

    wanted = plan(audio, groups_json)
    # What has to be encoded (its PCM or the settings changed since).
    todo = []
    for key, w in sorted(wanted.items()):
        c = audio['clips'][key]
        pcm = work / 'audio' / c['pcm']
        stamp = [VERSION, RATE, w['channels'], w['stream'], w['loop'], pcm.stat().st_size, int(pcm.stat().st_mtime)]
        target = cache / (safe(key) + '.dsp')
        if manifest.get(key, {}).get('stamp') == stamp and target.exists():
            continue
        todo.append((key, pcm, w, stamp, target))
    print(f'   {len(wanted)} clips for the GameCube ({sum(w["stream"] for w in wanted.values())} streamed), '
          f'{len(todo)} to encode', flush=True)

    if todo:
        started = time.time()
        with multiprocessing.Pool(args.jobs, initializer=low_priority) as pool:
            # A few hundred clips at a time (their samples and blocks in memory at once).
            chunk, chunks, blocks = [], [], 0
            for item in todo:
                chunk.append(item)
                blocks += max(1, int(audio['clips'][item[0]]['length'] * RATE / dspadpcm.BLOCK_SAMPLES) + 1) * item[2]['channels']
                if blocks >= 8 * BATCH_BLOCKS:
                    chunks.append(chunk)
                    chunk, blocks = [], 0
            if chunk:
                chunks.append(chunk)
            for n, chunk in enumerate(chunks):
                encode_clips(chunk, pool, manifest)
                manifest_path.write_text(json.dumps(manifest, indent=1), encoding='utf-8')
                progress(n + 1, len(chunks))
        print(f'   encoded in {time.time() - started:.0f} s', flush=True)

    pack(audio, groups_json, wanted, manifest, cache, Path(args.out))


def pack(audio, groups_json, wanted, manifest, cache, out_root):
    out = out_root / 'audio'
    if out.exists():
        shutil.rmtree(out)
    (out / 'banks').mkdir(parents=True)
    (out / 'streams').mkdir()
    (out / 'groups').mkdir()
    index = {'rate': RATE, 'blockFrames': dspadpcm.BLOCK_FRAMES, 'banks': {}, 'creators': {}, 'clips': {},
             'playlists': {}, 'controllers': {}, 'playlistManager': {}}
    budget = {'banks': {}, 'streams': 0}
    gains = mixer_gains(audio)
    # Banks: their clips, each 32-byte aligned.
    for creator in audio['creators']:
        if ':' in creator:
            continue  # (a copy: the tutorial's prefab of the town's)
        bank = bank_name(creator)
        data = bytearray()
        for key, w in sorted(wanted.items()):
            if w['stream'] or creator not in w['banks']:
                continue
            m = manifest[key]
            blob = (cache / (safe(key) + '.dsp')).read_bytes()
            entry = {'bank': bank, 'offset': len(data), 'bytes': len(blob), 'rate': m['rate'], 'channels': 1,
                     'samples': m['samples']}
            index['clips'].setdefault(key, entry)
            data += blob
        (out / 'banks' / f'{bank}.bin').write_bytes(bytes(data))
        index['banks'][bank] = {'file': f'audio/banks/{bank}.bin', 'bytes': len(data), 'creators': [creator]}
        budget['banks'][bank] = len(data)
        # Its groups: sound_groups.json's (the PC's) where it has them, with what plays them.
        source = audio['creators'][creator]['groups']
        groups = groups_json.get(creator)
        if groups is None:
            groups = {}
            for gname, g in source.items():
                gg = {k: v for k, v in g.items() if k not in ('variations', 'creator', 'busName', 'busIndex')}
                gg['variations'] = []
                for v in g['variations']:
                    vv = dict(v)
                    vv['clipLength'] = audio['clips'][v['clip']]['length'] if v['clip'] else None
                    gg['variations'].append(vv)
                groups[gname] = gg
        merged = {}
        for gname, g in groups.items():
            src = source[gname]
            if [v['name'] for v in g['variations']] != [v['name'] for v in src['variations']]:
                raise SystemExit(f'{creator} / {gname}: sound_groups.json and audio.json disagree on its variations')
            gg = dict(g)
            gg['groupMasterVolume'] = src.get('groupMasterVolume', 1.0)
            gg['variations'] = []
            for v, s in zip(g['variations'], src['variations']):
                vv = dict(v)
                # (The bus's mixer group, as MasterAudio routes it; else the AudioSource's own.)
                vv.update({'clip': s['clip'], 'volume': s['volume'], 'pitch': s['pitch'], 'loop': s['loop'],
                           'randomVolumeMode': s.get('randomVolumeMode', 0), 'randomPitchMode': s.get('randomPitchMode', 0),
                           'useCustomLooping': s.get('useCustomLooping', 0),
                           'mixerGain': route_gain(gains, src.get('mixerGroup'), s.get('mixerGroup'))})
                gg['variations'].append(vv)
            merged[gname] = gg
        (out / 'groups' / f'{bank}.json').write_text(json.dumps({creator: merged}, separators=(',', ':')), encoding='utf-8')
        index['creators'][creator] = {'bank': bank, 'groups': f'audio/groups/{bank}.json'}
    # Streams.
    for key, w in sorted(wanted.items()):
        if not w['stream']:
            continue
        m = manifest[key]
        name = f'{safe(key)}.dsp'
        shutil.copyfile(cache / name, out / 'streams' / name)
        index['clips'][key] = {'stream': f'audio/streams/{name}', 'bytes': m['bytes'], 'rate': m['rate'],
                               'channels': m['channels'], 'samples': m['samples']}
        budget['streams'] += m['bytes']
    for name, p in audio['playlists'].items():
        index['playlists'][name] = {'crossFadeTime': p['crossFadeTime'],
                                    'songs': [{'clip': s['clip'], 'volume': s['volume'], 'pitch': s['pitch'],
                                               'loop': s['loop']} for s in p['songs'] if s['clip'] in index['clips']]}
    for name, c in audio['controllers'].items():
        index['controllers'][name] = {'startPlaylistName': c['startPlaylistName'], 'playlistVolume': c['playlistVolume'],
                                      'loopPlaylist': c['loopPlaylist'], 'mixerGain': route_gain(gains, c.get('mixerGroup'))}
    index['playlistManager'] = audio.get('playlistManager', {})
    (out / 'index.json').write_text(json.dumps(index, separators=(',', ':')), encoding='utf-8')
    (cache / 'budget.json').write_text(json.dumps(budget, indent=1), encoding='utf-8')
    total = sum(f.stat().st_size for f in out.rglob('*') if f.is_file())
    print(f'   {out}: {len(index["banks"])} banks ({sum(budget["banks"].values()) / 1e6:.1f} MB), '
          f'{sum(1 for c in index["clips"].values() if "stream" in c)} streams ({budget["streams"] / 1e6:.1f} MB), '
          f'{total / 1e6:.1f} MB in all', flush=True)
    for bank, size in budget['banks'].items():
        print(f'   bank {bank}: {size / 1024:.0f} KB', flush=True)


if __name__ == '__main__':
    main()
