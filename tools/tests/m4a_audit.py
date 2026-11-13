#!/usr/bin/env python3
"""Audit of every song of the game's song table as the port renders it.

  m4a_audit.py <rom> <program> <out.json> [--frames N] [--songs a-b] [--jobs N] [--compare before.json]

Each song is started alone through the SS_TEST_SONG hook of runtime/core/m4a.c in a headless run of N frames
(default 1800, 30 seconds; music 7200, two minutes, because tracks may enter late), the mix is recorded (SS_AUDIO_FILE) and measured: peak, RMS over the whole run and
over the part that sounds, and per track the notes started and the energy it put into the mix. This is joined
with the static description of the song (m4a_songs.py). The recordings are deleted; only numbers are kept.
"""
import argparse
import array
import concurrent.futures
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import m4a_songs

SONG_TABLE = 0xBC60C        # gSongTable of Shining Soul II (EU): 428 entries of 8 bytes, after gMPlayTable (4 players)
SONG_COUNT = 428


def render(args):
    rom, program, song, frames, keep = args
    work = tempfile.mkdtemp(prefix='m4a%03d_' % song)
    env = dict(os.environ, SS_SKIP='0', SS_FRAMES=str(frames), SS_AUDIO_FILE='1', SS_TEST_SONG=str(song))
    out = subprocess.run([program, rom], cwd=work, env=env, capture_output=True, text=True, errors='replace')
    result = {'song': song, 'tracks': {}, 'peak': 0, 'rms': 0.0, 'rms_on': 0.0, 'seconds_on': 0.0, 'error': None}
    for line in out.stderr.splitlines():
        m = re.match(r'm4a: player (\d+) track (\d+) notes (\d+) dropped (\d+) pcm (\d+) sq1 (\d+) sq2 (\d+) wave (\d+) noise (\d+) energy (\d+)', line)
        if m:
            v = [int(x) for x in m.groups()]
            result['tracks'][v[1]] = {'player': v[0], 'notes': v[2], 'dropped': v[3], 'energy': v[9]}
    path = os.path.join(work, 'ss_audio.raw')
    if not os.path.exists(path):
        result['error'] = 'no audio (exit %d)' % out.returncode
    else:
        data = array.array('h')
        with open(path, 'rb') as f:
            data.frombytes(f.read())
        n = len(data)
        if n:
            peak = max(max(data), -min(data))
            total = 0
            on = 0
            window = 4800 * 2           # tenths of a second
            for at in range(0, n, window):
                chunk = data[at:at + window]
                s = sum(x * x for x in chunk)
                total += s
                if s / len(chunk) > 20 * 20:
                    on += len(chunk)
            result['peak'] = peak
            result['rms'] = math.sqrt(total / n)
            result['seconds_on'] = on / 2 / 48000.0
            result['seconds'] = n / 2 / 48000.0
        if keep:
            shutil.copy(path, os.path.join(keep, 'song%03d.raw' % song))
    shutil.rmtree(work, ignore_errors=True)
    return result


def verdict(r):
    if r.get('error'):
        return 'ERROR'
    if r['peak'] < 64:
        return 'silent'
    if r['peak'] < 1200:
        return 'very quiet'
    return 'normal'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rom'); ap.add_argument('program'); ap.add_argument('out')
    ap.add_argument('--frames', type=int, default=1800)
    ap.add_argument('--music-frames', type=int, default=7200, help='music is rendered longer: tracks may enter late')
    ap.add_argument('--songs', default='0-%d' % (SONG_COUNT - 1))
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--keep', default=None, help='folder to keep the recordings in (never inside the repository)')
    ap.add_argument('--compare', default=None)
    a = ap.parse_args()
    lo, _, hi = a.songs.partition('-')
    numbers = list(range(int(lo), int(hi or lo) + 1))
    rom = m4a_songs.Rom(a.rom)
    static = {s['song']: s for s in m4a_songs.describe(rom, SONG_TABLE, SONG_COUNT)}
    program = os.path.abspath(a.program)
    def is_music(s):
        return bool(s.get('loops') or s.get('ticks', 0) >= 96 * 8)
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        rendered = list(pool.map(render, [(os.path.abspath(a.rom), program, n,
                                           a.music_frames if is_music(static[n]) else a.frames, a.keep) for n in numbers]))
    before = {}
    if a.compare:
        before = {r['song']: r for r in json.load(open(a.compare))}
    rows = []
    for r in rendered:
        s = static[r['song']]
        r['player'] = s['player']
        r['track_count'] = s.get('track_count', 0)
        r['loops'] = s.get('loops', False)
        r['ticks'] = s.get('ticks', 0)
        r['kind'] = 'music' if is_music(s) else ('empty' if not s.get('track_count') else 'effect')
        voices = {}
        dead = []
        for i, t in enumerate(s['tracks']):
            for k, v in t['voices'].items():
                voices[k] = voices.get(k, 0) + v
            got = r['tracks'].get(i)
            if t['notes'] and (got is None or got['energy'] == 0):
                dead.append('%d(%s)' % (i, '+'.join(sorted(t['voices']))))
        r['voices'] = voices
        r['dead_tracks'] = dead
        r['verdict'] = verdict(r)
        rows.append(r)
    json.dump(rows, open(a.out, 'w'), indent=0)
    print('song pl trk kind   loop voices                                   peak    rms  on(s) verdict     dead tracks' + ('   | before: peak rms verdict dead' if before else ''))
    for r in rows:
        line = '%4d %2d %3d %-6s %-4s %-40s %5d %6.0f %5.1f %-11s %s' % (
            r['song'], r['player'], r['track_count'], r['kind'], 'yes' if r['loops'] else 'no',
            ' '.join('%s:%d' % kv for kv in sorted(r['voices'].items())), r['peak'], r['rms'], r['seconds_on'],
            r['verdict'], ' '.join(r['dead_tracks']) or '-')
        b = before.get(r['song'])
        if b:
            line += '   | %5d %6.0f %-10s %s' % (b['peak'], b['rms'], b['verdict'], ' '.join(b['dead_tracks']) or '-')
        print(line)
    for kind in ('music', 'effect', 'empty'):
        sel = [r for r in rows if r['kind'] == kind]
        print('%s: %d songs; silent %d, very quiet %d, normal %d; with dead tracks %d' % (
            kind, len(sel), sum(r['verdict'] == 'silent' for r in sel), sum(r['verdict'] == 'very quiet' for r in sel),
            sum(r['verdict'] == 'normal' for r in sel), sum(bool(r['dead_tracks']) for r in sel)))
    print('highest peak %d (song %d)' % max((r['peak'], r['song']) for r in rows))


if __name__ == '__main__':
    main()
