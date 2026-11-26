"""The achievements in the Linux game hosts (GA36, R36S, Linux x64): rcheevos (third_party, MIT), the client the
Windows launcher and host also use (launcher/achievements/ra_client.c, with ra_https_posix.inc for HTTPS through the
system's or the package's OpenSSL, opened at run time) and the host's part (runtime/achievements/ach_host.c).

    sources()          the C files, compiled with the host
    flags(handheld)    their defines and include folders; handheld: everything stays on the card (SS_RA_HANDHELD:
                       offline unlocks are queued for the card's account, no sign-in copy in the home folder)
    LIBRARIES          dlopen lives in libdl before glibc 2.34
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RC = ROOT / 'third_party/rcheevos'
LIBRARIES = ['-ldl', '-lpthread']


def sources():
    files = sorted((RC / 'src/rcheevos').glob('*.c')) + sorted((RC / 'src/rapi').glob('*.c'))
    files += sorted((RC / 'src/rhash').glob('*.c'))
    files += [RC / 'src' / name for name in ('rc_client.c', 'rc_compat.c', 'rc_util.c', 'rc_version.c')]
    return files + [ROOT / 'launcher/achievements/ra_client.c', ROOT / 'runtime/achievements/ach_host.c']


def flags(handheld):
    defines = ['-DRC_CLIENT_SUPPORTS_HASH', '-DSS_ACHIEVEMENTS=1', '-DSS_RA_ONLINE=1']
    if handheld:
        defines.append('-DSS_RA_HANDHELD=1')
    return [*defines, '-I', str(RC / 'include'), '-I', str(RC / 'src'), '-I', str(ROOT / 'launcher/achievements')]


def package_files():
    """Package member (relative to the game folder) -> source: the set as the site answered it and its badges,
    as the Windows package ships them, and a CA bundle for HTTPS on firmwares without one."""
    achievements = ROOT / 'assets/achievements'
    files = {p.name: p for p in achievements.glob('ra_set_*.json')}
    for badge in sorted((achievements / 'badges').glob('badge_*.png')):
        files[f'achievements/badges/{badge.name}'] = badge
    files['achievements/cacert.pem'] = ROOT / 'third_party/cacert/cacert.pem'
    files['achievements/cacert-LICENSE.txt'] = ROOT / 'third_party/cacert/LICENSE.txt'
    files['rcheevos-LICENSE.txt'] = RC / 'LICENSE'
    files['docs/RETROACHIEVEMENTS_SOURCE.md'] = achievements / 'SOURCE.md'
    return files
