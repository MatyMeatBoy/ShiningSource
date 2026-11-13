#!/usr/bin/env python3
"""Survey of every song of the game's song table as the port's sound engine plays it.

  m4a_survey.py run <rom> <program> <out.txt> [--frames N]     play every song (SS_SONG_SURVEY), keep the lines
  m4a_survey.py table <out.txt> [--compare before.txt] [--all] the table and the summary

`run` starts the program once: the engine's survey mode (runtime/core/m4a.c, survey_run) plays each entry of the
song table alone for N frames (default 7200, two minutes) without opening a window or a sound device, and prints
one line of numbers per song. `table` shows the songs that need attention (silent, tracks that ended on anything
but FINE, tracks that put nothing into the mix, notes that found no voice, commands the engine skips), or every
song with --all, and with --compare what changed against an earlier run. Songs appear by number only.
"""
import argparse
import collections
import math
import os
import subprocess
import sys

DROPS = ('no free channel', 'split inside a split', 'voice type the engine lacks', 'no sample')
NAMES = {0xB1: 'FINE', 0xB2: 'GOTO', 0xB3: 'PATT', 0xB4: 'PEND', 0xB5: 'REPT', 0xB9: 'MEMACC', 0xBA: 'PRIO',
         0xBB: 'TEMPO', 0xBC: 'KEYSH', 0xBD: 'VOICE', 0xBE: 'VOL', 0xBF: 'PAN', 0xC0: 'BEND', 0xC1: 'BENDR',
         0xC2: 'LFOS', 0xC3: 'LFODL', 0xC4: 'MOD', 0xC5: 'MODT', 0xC8: 'TUNE', 0xCC: 'PORT', 0xCD: 'XCMD',
         0xCE: 'EOT'}
XCMD = {1: 'xWAVE', 2: 'xTYPE', 4: 'xATTA', 5: 'xDECA', 6: 'xSUST', 7: 'xRELE', 8: 'xIECV', 9: 'xIECL',
        10: 'xLENG', 11: 'xSWEE'}


def counts(text):
    if text == '-':
        return {}
    return {k: int(v) for k, v in (item.split(':') for item in text.split(','))}


def load(path):
    rows = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        w = line.split()
        if not w or w[0] != 'survey':
            continue
        f = {w[i]: w[i + 1] for i in range(1, len(w) - 1, 2)}
        got, header = (int(x) for x in f['tracks'].split('/'))
        r = {'song': int(f['song']), 'player': int(f['player']), 'tracks': got, 'header_tracks': header,
             'frames': int(f['frames']), 'stopped': int(f['stopped']), 'peak': int(f['peak']),
             'rms': math.sqrt(float(f['power'])), 'loud': int(f['loud']), 'notes': int(f['notes']),
             'dropped': [int(x) for x in f['dropped'].split('/')], 'flagged': int(f['flagged']),
             'notdone': counts(f['notdone']), 'xcmd': counts(f['xcmd']), 'cmds': counts(f['cmds']),
             'early': [] if f['early'] == '-' else f['early'].split(','),
             'mute': [] if f['mute'] == '-' else f['mute'].split(',')}
        rows[r['song']] = r
    return rows


def verdict(r):
    if r['header_tracks'] == 0:
        return 'empty'
    if r['peak'] < 64:
        return 'SILENT'
    return 'sounds'


def problems(r):
    out = []
    if r['header_tracks'] and r['peak'] < 64:
        out.append('silent')
    if r['tracks'] < r['header_tracks']:
        out.append('player has %d of %d tracks' % (r['tracks'], r['header_tracks']))
    if r['early']:
        out.append('ended early ' + ' '.join(r['early']))
    if r['mute']:
        out.append('mute tracks ' + ' '.join(r['mute']))
    for n, name in zip(r['dropped'], DROPS):
        if n:
            out.append('%d notes dropped: %s' % (n, name))
    if r['flagged']:
        out.append('%d notes on voices with type bits 0x30' % r['flagged'])
    if r['notdone']:
        out.append('skipped ' + ' '.join('%s x%d' % (NAMES.get(int(k, 16), k), v) for k, v in sorted(r['notdone'].items())))
    return out


def row(r):
    return '%4d %2d %2d/%-2d %7s %5d %6.0f %6d %s' % (
        r['song'], r['player'], r['tracks'], r['header_tracks'],
        'plays' if r['stopped'] < 0 else '%.1fs' % (r['stopped'] / 59.7275), r['peak'], r['rms'], r['notes'],
        verdict(r))


def summary(rows):
    real = [r for r in rows.values() if r['header_tracks']]
    print('%d songs, %d with tracks, %d empty entries' % (len(rows), len(real), len(rows) - len(real)))
    print('silent: %d %s' % (sum(r['peak'] < 64 for r in real), [r['song'] for r in real if r['peak'] < 64]))
    print('tracks that ended on anything but FINE: %d songs' % sum(bool(r['early']) for r in real))
    print('tracks that sounded nothing: %d songs' % sum(bool(r['mute']) for r in real))
    for i, name in enumerate(DROPS):
        print('notes dropped, %s: %d songs, %d notes' % (name, sum(r['dropped'][i] > 0 for r in real),
                                                         sum(r['dropped'][i] for r in real)))
    used = collections.Counter()
    skipped = collections.Counter()
    xcmd = collections.Counter()
    for r in real:
        used.update(int(k, 16) for k in r['cmds'])
        skipped.update(int(k, 16) for k in r['notdone'])
        xcmd.update(int(k) for k in r['xcmd'])
    print('commands by the number of songs that use them:')
    print('  ' + '  '.join('%s %d' % (NAMES.get(k, '%02X' % k), v) for k, v in sorted(used.items())))
    print('XCMD sub-commands: ' + ('  '.join('%s %d' % (XCMD.get(k, '#%d' % k), v) for k, v in sorted(xcmd.items())) or 'none'))
    print('commands the engine skips: ' + ('  '.join('%s %d' % (NAMES.get(k, '%02X' % k), v) for k, v in sorted(skipped.items())) or 'none'))
    print('highest peak %d (song %d)' % max((r['peak'], r['song']) for r in real))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='command', required=True)
    a = sub.add_parser('run')
    a.add_argument('rom'); a.add_argument('program'); a.add_argument('out')
    a.add_argument('--frames', type=int, default=7200)
    b = sub.add_parser('table')
    b.add_argument('out')
    b.add_argument('--compare', default=None)
    b.add_argument('--all', action='store_true')
    args = ap.parse_args()
    if args.command == 'run':
        env = dict(os.environ, SS_SKIP='0', SS_FRAMES='100000', SS_SONG_SURVEY=str(args.frames))
        got = subprocess.run([os.path.abspath(args.program), os.path.abspath(args.rom)], env=env, capture_output=True,
                             text=True, errors='replace')
        lines = [l for l in got.stderr.splitlines() if l.startswith('survey ')]
        open(args.out, 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
        print('%d songs surveyed' % len(lines))
        return 0 if lines else 1
    rows = load(args.out)
    before = load(args.compare) if args.compare else {}
    print('song pl trks    ends  peak    rms  notes verdict  remarks')
    for n in sorted(rows):
        r = rows[n]
        notes = problems(r)
        b = before.get(n)
        changed = b is not None and (b['peak'] != r['peak'] or round(b['rms']) != round(r['rms']) or problems(b) != notes
                                     or b['notes'] != r['notes'] or b['stopped'] != r['stopped'])
        if not (args.all or notes or changed):
            continue
        line = row(r) + '  ' + ('; '.join(notes) or '-')
        if changed:
            line += '   | before: peak %d rms %.0f notes %d %s %s' % (b['peak'], b['rms'], b['notes'], verdict(b),
                                                                    '; '.join(problems(b)) or '-')
        print(line)
    print()
    if before:
        print('--- before')
        summary(before)
        print('--- now')
    summary(rows)
    return 0


if __name__ == '__main__':
    sys.exit(main())
