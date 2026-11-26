#!/usr/bin/env python3
"""Pretend RetroAchievements for the game's own tests (SS_RA_TEST_SERVER): no network, no real account.

    ra_fixture.py SERVER_DIR RA_DIR [--shared AUTH_FILE] [--offline]

SERVER_DIR gets the canned answers the game reads instead of the site (login2, achievementsets, startsession,
awardachievement, unlocks, ping); with --offline also the file `offline`, which makes every request fail as without
a network. RA_DIR (the game's SS_RA_DIR) gets the same catalog as ra_set_<hash>.json for the offline evaluation and,
unless --shared names another file for it (another game's folder on the same card), a pretend sign-in:
retroachievements.auth (128-byte name + 128-byte token, the card format) and retroachievements.ini (enabled=1).

The catalog holds one test achievement, TEST_ID, that the real game memory triggers early with
tools/tests/play_input.txt: EWRAM byte 0x02034E3C (RA address 0x034E3C) becomes 12 between frames 3000 and 3300 of
that input, on Windows and on the GA36 alike (SS_RA_TEST_PEEK), so an unlock proves the host's view of the game's RAM.
Like the official set it also reads IWRAM 0x0300003C against itself: that byte differs between builds (0xE0 on Windows,
0x25 on the GA36), so neither set compares it with a number. Nothing here is a real token or password.
"""
import argparse
import json
from pathlib import Path

ROM_HASH = '49aa688745111aaa51076a345f8f18c3'
TEST_ID = 900001
TEST_USER = 'tester'
TEST_TOKEN = 'fixture-token-not-real'


def catalog():
    entry = dict(ID=TEST_ID, Title='Into the game (test)', Description='The play script reaches the field',
                 Flags=3, Points=1, MemAddr='0xH00003c=0xH00003c_0xH034e3c=12', Author='fixture', BadgeName='72483',
                 Created=0, Modified=0, Type='')
    return dict(Success=True, GameId=7095, Title='Shining Soul II (test catalog)', ConsoleId=5,
                ImageIconUrl='/Images/000001.png', RichPresenceGameId=7095, RichPresencePatch='',
                Sets=[dict(AchievementSetId=1, GameId=7095, Title=None, Type='core', ImageIconUrl='/Images/000001.png',
                           Achievements=[entry], Leaderboards=[])])


def write_server(server, offline=False):
    server.mkdir(parents=True, exist_ok=True)
    answers = {
        'login2': dict(Success=True, User=TEST_USER, AvatarUrl='', Token=TEST_TOKEN, Score=0, SoftcoreScore=0,
                       Messages=0, Permissions=1, AccountType='Registered'),
        'achievementsets': catalog(),
        'startsession': dict(Success=True, HardcoreUnlocks=[], Unlocks=[], ServerNow=1767225600),
        'awardachievement': dict(Success=True, Score=1, SoftcoreScore=1, AchievementID=TEST_ID, AchievementsRemaining=0),
        'unlocks': dict(Success=True, UserUnlocks=[], GameID=7095, HardcoreMode=False),
        'ping': dict(Success=True),
    }
    for name, answer in answers.items():
        (server / f'{name}.json').write_text(json.dumps(answer), encoding='utf-8', newline='\n')
    marker = server / 'offline'
    if offline:
        marker.write_text('every request fails as without a network\n', newline='\n')
    elif marker.exists():
        marker.unlink()


def auth_bytes():
    return TEST_USER.encode().ljust(128, b'\0') + TEST_TOKEN.encode().ljust(128, b'\0')


def write_ra_dir(folder, shared=None):
    folder.mkdir(parents=True, exist_ok=True)
    (folder / f'ra_set_{ROM_HASH}.json').write_text(json.dumps(catalog()), encoding='utf-8', newline='\n')
    if shared is None:
        (folder / 'retroachievements.auth').write_bytes(auth_bytes())
        (folder / 'retroachievements.ini').write_text('enabled=1\n', newline='\n')
    else:
        shared.parent.mkdir(parents=True, exist_ok=True)
        shared.write_bytes(auth_bytes())
        (shared.parent / 'retroachievements.ini').write_text('enabled=1\n', newline='\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('server', type=Path)
    parser.add_argument('ra_dir', type=Path)
    parser.add_argument('--shared', type=Path, help='write the pretend sign-in here instead (another game folder)')
    parser.add_argument('--offline', action='store_true')
    args = parser.parse_args()
    write_server(args.server, args.offline)
    write_ra_dir(args.ra_dir, args.shared)


if __name__ == '__main__':
    main()
