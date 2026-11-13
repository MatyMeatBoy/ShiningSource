#!/usr/bin/env python3
"""Static description of every song of an m4a (MusicPlayer2000) song table, read from the ROM.

  m4a_songs.py <rom> <song table offset> <count> [out.json]

For each song: music player, track count, priority, reverb, whether it loops (a GOTO backwards), its length in
ticks when it does not, the voices its notes really use by type, and the commands of its tracks. The audit
(m4a_audit.py) joins this with what the port renders. Nothing of the ROM is written out except these numbers.
"""
import json
import struct
import sys

CLOCK = list(range(25)) + [28, 30, 32, 36, 40, 42, 44, 48, 52, 54, 56, 60, 64, 66, 68, 72, 76, 78, 80, 84, 88, 90, 92, 96]
NAMES = {0xB1: 'FINE', 0xB2: 'GOTO', 0xB3: 'PATT', 0xB4: 'PEND', 0xB5: 'REPT', 0xB9: 'MEMACC', 0xBA: 'PRIO',
         0xBB: 'TEMPO', 0xBC: 'KEYSH', 0xBD: 'VOICE', 0xBE: 'VOL', 0xBF: 'PAN', 0xC0: 'BEND', 0xC1: 'BENDR',
         0xC2: 'LFOS', 0xC3: 'LFODL', 0xC4: 'MOD', 0xC5: 'MODT', 0xC8: 'TUNE', 0xCC: 'PORT', 0xCD: 'XCMD',
         0xCE: 'EOT', 0xCF: 'TIE'}
ARGS = {0xBA: 1, 0xBB: 1, 0xBC: 1, 0xBD: 1, 0xBE: 1, 0xBF: 1, 0xC0: 1, 0xC1: 1, 0xC2: 1, 0xC3: 1, 0xC4: 1,
        0xC5: 1, 0xC8: 1, 0xCC: 2}
# gXcmdTable of the driver: 1 xWAVE (a pointer), 2 xTYPE, 4..11 xATTA xDECA xSUST xRELE xIECV xIECL xLENG xSWEE;
# 0 and 3 end the track, and the table has nothing after 11
XCMD_ARGS = {1: 4, 2: 1, 4: 1, 5: 1, 6: 1, 7: 1, 8: 1, 9: 1, 10: 1, 11: 1}
TYPES = {0: 'pcm', 8: 'pcm-fixed', 1: 'sq1', 2: 'sq2', 3: 'wave', 4: 'noise', 9: 'sq1', 10: 'sq2', 11: 'wave', 12: 'noise'}


class Rom:
    def __init__(self, path):
        self.d = open(path, 'rb').read()

    def u32(self, off):
        return struct.unpack_from('<I', self.d, off)[0]

    def ptr(self, off):
        v = self.u32(off)
        return v - 0x08000000 if 0x08000000 <= v < 0x08000000 + len(self.d) else None


def voice_type(rom, group, program, key):
    """Type name of the voice a note really plays (through a key split or a rhythm set)."""
    tone = group + program * 12
    if tone + 12 > len(rom.d):
        return 'bad'
    t = rom.d[tone]
    prefix = ''
    if t & 0xC0:
        sub = rom.ptr(tone + 4)
        if sub is None:
            return 'bad-sub'
        if t & 0x40:
            table = rom.ptr(tone + 8)
            if table is None:
                return 'bad-split'
            tone = sub + rom.d[table + key] * 12
            prefix = 'split:'
        else:
            tone = sub + key * 12
            prefix = 'drum:'
        t = rom.d[tone]
        if t & 0xC0:
            return prefix + 'nested'
    name = TYPES.get(t & 0x0F if not (t & 0x30) else -1)
    if name is None:
        return prefix + 'type%02X' % t
    if name.startswith('pcm'):
        wave = rom.ptr(tone + 4)
        if wave is None:
            return prefix + name + '-nowave'
    return prefix + name


def walk_track(rom, start, group):
    """Follow a track once (patterns entered, the final GOTO not taken)."""
    info = {'commands': {}, 'voices': {}, 'notes': 0, 'ticks': 0, 'loops': False, 'bad': None, 'programs': []}
    pc, stack, running, program, key, guard = start, [], 0, None, 60, 0
    repeat = {}
    while guard < 200000:
        guard += 1
        if pc is None or pc >= len(rom.d):
            info['bad'] = 'pointer'
            break
        c = rom.d[pc]
        if c < 0x80:
            c = running
            if c < 0x80:
                info['bad'] = 'data without status at %X' % pc
                break
        else:
            pc += 1
            if c >= 0xBD:
                running = c
        if c >= 0xCF:
            name = 'TIE' if c == 0xCF else 'NOTE'
            info['commands'][name] = info['commands'].get(name, 0) + 1
            if rom.d[pc] < 0x80:
                key = rom.d[pc]; pc += 1
                if rom.d[pc] < 0x80:
                    pc += 1
                    if rom.d[pc] < 0x80:
                        pc += 1
            info['notes'] += 1
            vt = 'no VOICE' if program is None else voice_type(rom, group, program, key)
            info['voices'][vt] = info['voices'].get(vt, 0) + 1
            continue
        if c <= 0xB0:
            info['ticks'] += CLOCK[c - 0x80]
            continue
        name = NAMES.get(c, 'cmd%02X' % c)
        info['commands'][name] = info['commands'].get(name, 0) + 1
        if c in (0xB1, 0xB6, 0xB7, 0xB8, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB):
            if c != 0xB1:
                info['bad'] = 'ends on %02X' % c
            break
        if c == 0xB2:
            target = rom.ptr(pc)
            info['loops'] = True
            break
        if c == 0xB3:
            if len(stack) >= 3:
                info['bad'] = 'pattern depth'
                break
            stack.append(pc + 4)
            pc = rom.ptr(pc)
        elif c == 0xB4:
            if stack:
                pc = stack.pop()
        elif c == 0xB5:
            count = rom.d[pc]
            if count == 0:
                info['loops'] = True
                break
            repeat[pc] = repeat.get(pc, 0) + 1
            if repeat[pc] < count:
                pc = rom.ptr(pc + 1)
            else:
                repeat[pc] = 0
                pc += 5
        elif c == 0xB9:
            op = rom.d[pc]
            info['commands']['MEMACC%d' % op] = info['commands'].get('MEMACC%d' % op, 0) + 1
            pc += 3
            if 6 <= op <= 17:
                pc += 4          # the jump is not taken here
        elif c == 0xCD:
            sub = rom.d[pc]
            info['commands']['XCMD%d' % sub] = info['commands'].get('XCMD%d' % sub, 0) + 1
            pc += 1 + XCMD_ARGS.get(sub, 0)
            if sub not in XCMD_ARGS:
                info['bad'] = 'xcmd %d' % sub
                break
        elif c == 0xCE:
            if rom.d[pc] < 0x80:
                pc += 1
        elif c in ARGS:
            if c == 0xBD:
                program = rom.d[pc]
                if program not in info['programs']:
                    info['programs'].append(program)
            pc += ARGS[c]
        else:
            info['bad'] = 'command %02X' % c
            break
    return info


def describe(rom, table, count):
    songs = []
    for n in range(count):
        header = rom.ptr(table + n * 8)
        player = struct.unpack_from('<H', rom.d, table + n * 8 + 4)[0]
        song = {'song': n, 'player': player, 'header': header, 'tracks': []}
        if header is None:
            song['bad'] = 'no header'
            songs.append(song)
            continue
        tracks, blocks, priority, reverb = rom.d[header:header + 4]
        group = rom.ptr(header + 4)
        song.update(track_count=tracks, priority=priority, reverb=reverb, group=group)
        for i in range(tracks):
            start = rom.ptr(header + 8 + 4 * i)
            song['tracks'].append(walk_track(rom, start, group) if start is not None and group is not None
                                  else {'bad': 'pointer', 'commands': {}, 'voices': {}, 'notes': 0, 'ticks': 0, 'loops': False, 'programs': []})
        song['loops'] = any(t['loops'] for t in song['tracks'])
        song['ticks'] = max([t['ticks'] for t in song['tracks']] or [0])
        songs.append(song)
    return songs


if __name__ == '__main__':
    rom = Rom(sys.argv[1])
    songs = describe(rom, int(sys.argv[2], 0), int(sys.argv[3], 0))
    if len(sys.argv) > 4:
        json.dump(songs, open(sys.argv[4], 'w'))
    commands, voices = {}, {}
    for s in songs:
        for t in s['tracks']:
            for k, v in t['commands'].items():
                commands[k] = commands.get(k, 0) + v
            for k, v in t['voices'].items():
                voices[k] = voices.get(k, 0) + v
            if t.get('bad'):
                print('song', s['song'], 'track bad:', t['bad'])
    print('songs', len(songs), 'looping', sum(1 for s in songs if s.get('loops')))
    print('commands', sorted(commands.items()))
    print('voices', sorted(voices.items()))
