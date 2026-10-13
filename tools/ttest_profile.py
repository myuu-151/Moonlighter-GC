"""Where a TTEST_PROFILE=1 build's time went: the samples MLGC/Source/TranslatedTest.cpp writes to
Dolphin's log (tools/dolphin_test.ps1 copies them out), each the main thread's PC, its LR and the
return addresses up its stack's back chain, taken every 100 us of the Gekko's timebase inside the
replay's timed phases; mapped to functions with MLGC.elf's symbols (devkitPPC's nm).

    py tools/ttest_profile.py <log> [--elf MLGC/Build/GCN/MLGC.elf] [--top 25]

Prints, per phase and overall: the functions the samples were in (self) and the functions on the
stack (inclusive: each counted once per sample), by share of the samples.
"""
import argparse
import bisect
import collections
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
PHASES = ['physics', 'fixed steps', 'DOTween', 'Updates', 'Invoke/coroutines', 'Animators', 'LateUpdates', 'destroys',
          'collector']


def symbols(elf, nm):
    out = subprocess.run([nm, '-n', '-S', '-C', '--defined-only', str(elf)], capture_output=True, text=True,
                         errors='replace', check=True).stdout
    starts, ends, names = [], [], []
    for line in out.splitlines():
        m = re.match(r'^([0-9a-f]{8}) ([0-9a-f]{8}) [TtWw] (.*)$', line)
        if not m:
            continue
        start, size = int(m[1], 16), int(m[2], 16)
        if size == 0:
            continue
        starts.append(start)
        ends.append(start + size)
        names.append(m[3])
    return starts, ends, names


def short(name, width=110):
    # templates' arguments folded: what's left names the function
    depth, out = 0, []
    for ch in name:
        if ch == '<':
            depth += 1
            if depth == 1:
                out.append('<..>')
            continue
        if ch == '>':
            depth -= 1
            continue
        if depth == 0:
            out.append(ch)
    s = ''.join(out)
    return s if len(s) <= width else s[:width - 3] + '...'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('log')
    parser.add_argument('--elf', default=str(HERE / 'MLGC' / 'Build' / 'GCN' / 'MLGC.elf'))
    parser.add_argument('--top', type=int, default=25)
    parser.add_argument('--nm', default=os.path.join(os.environ.get('DEVKITPPC_WIN', r'C:\DolphinWorks\gekko-toolchain\devkitPPC'),
                                                     'bin', 'powerpc-eabi-nm.exe'))
    args = parser.parse_args()
    starts, ends, names = symbols(args.elf, args.nm)

    def where(addr):
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0 and addr < ends[i]:
            return names[i]
        return f'?{addr:08x}'

    samples = []
    with open(args.log, encoding='utf-8-sig', errors='replace') as f:
        for line in f:
            if line.startswith('S '):
                parts = line.split()
                samples.append((int(parts[1]), [int(a, 16) + 0x80000000 for a in parts[2:]]))
    if not samples:
        sys.exit(f'no samples in {args.log} (a TTEST_PROFILE=1 build?)')
    total = len(samples)
    by_phase = collections.Counter(p for p, _ in samples)
    print(f'{total} samples (every 100 us of the timebase: {total / 10:.0f} ms of the replay\'s timed phases)')
    for p, n in by_phase.most_common():
        print(f'  {PHASES[p] if p < len(PHASES) else p:<20} {n:6d}  {100 * n / total:5.1f}%')

    def report(title, chosen):
        n = len(chosen)
        if not n:
            return
        self_count = collections.Counter()
        incl = collections.Counter()
        for _, addrs in chosen:
            fns = [where(a) for a in addrs]
            self_count[fns[0]] += 1
            seen = set()
            for i, fn in enumerate(fns):
                if i == 1 and fn == fns[0]:
                    continue  # (the LR inside the function itself: a non-leaf's last call)
                if fn not in seen:
                    seen.add(fn)
                    incl[fn] += 1
        print(f'\n## {title}: {n} samples\n\nself:')
        for fn, c in self_count.most_common(args.top):
            print(f'  {100 * c / n:5.1f}%  {short(fn)}')
        print('inclusive:')
        for fn, c in incl.most_common(args.top):
            print(f'  {100 * c / n:5.1f}%  {short(fn)}')

    report('all phases', samples)
    for p, _ in by_phase.most_common():
        report(PHASES[p] if p < len(PHASES) else str(p), [s for s in samples if s[0] == p])


if __name__ == '__main__':
    main()
