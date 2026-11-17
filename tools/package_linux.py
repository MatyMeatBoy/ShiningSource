"""Package a Linux x64 release from an explicit allowlist and verify its manifest."""
import hashlib
import io
import json
import re
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    version = sys.argv[1]
    if not re.fullmatch(r'\d+\.\d+\.\d+', version):
        raise SystemExit('Expected a release version')
    out = ROOT/'build/releases'/version
    out.mkdir(parents=True, exist_ok=True)
    game = ROOT/'build/linux64/ss2'
    files = {name: game/name for name in ('ss2_linux64', 'ss2_game.elf', 'lib/libSDL3.so.0')}
    files.update({'LICENSE.txt': ROOT/'LICENSE',
                  'README.md': ROOT/'README.md',
                  'SDL3-LICENSE.txt': ROOT/'third_party/SDL3-LICENSE.txt',
                  f'docs/RELEASE_{version}.md': ROOT/f'docs/RELEASE_{version}.md',
                  'docs/MULTIPLAYER_AUDIO_AUDIT.md': ROOT/'docs/MULTIPLAYER_AUDIO_AUDIT.md'})
    # The cross-build's SDL SDK supplies its own upstream license.
    if not files['SDL3-LICENSE.txt'].is_file():
        import os
        sdk = Path(os.environ.get('SHININGSOURCE_LINUX_SYSROOT', 'D:/01Claude/ReKHCoM-build/linux/amd64'))
        candidates = list((sdk/'sdl/share/licenses').glob('**/LICENSE*')) + list((sdk/'sdl/share').glob('**/LICENSE.txt'))
        if not candidates:
            raise SystemExit('SDL3 license missing from the Linux SDK')
        files['SDL3-LICENSE.txt'] = candidates[0]
    assert all(path.is_file() for path in files.values())
    assert not any(name.lower().endswith(('.gba', '.sav', '.ini', '.log', '.raw', '.auth')) for name in files)
    data = {name: path.read_bytes() for name, path in files.items()}
    data['LINUX.txt'] = (f'ShiningSource {version} Linux x64\n\n'
        'Extract the complete folder. Supply your own European ROM:\n'
        '  LD_LIBRARY_PATH=./lib ./ss2_linux64 /path/to/your.gba\n\n'
        'No mmap_min_addr adjustment is required. Restored Multiplayer Audio is on\n'
        'by default in Options; SS_REMOTE_SFX=0 disables it. Online rules remain 7.\n'
        f'See docs/RELEASE_{version}.md for verification and platform limits.\n').encode()
    manifest = {name: hashlib.sha256(body).hexdigest() for name, body in data.items()}
    data['MANIFEST-SHA256.json'] = (json.dumps(manifest, indent=2)+'\n').encode()
    archive = out/f'ShiningSource-{version}-linux-x64.tar.gz'
    prefix = f'ShiningSource-{version}-linux-x64/'
    with tarfile.open(archive, 'w:gz') as tar:
        for name, body in sorted(data.items()):
            info = tarfile.TarInfo(prefix+name)
            info.size = len(body)
            info.mode = 0o755 if name in ('ss2_linux64', 'lib/libSDL3.so.0') else 0o644
            tar.addfile(info, io.BytesIO(body))
    with tarfile.open(archive) as tar:
        assert all(m.isfile() and m.name.startswith(prefix) for m in tar.getmembers())
        assert tar.getmember(prefix+'ss2_linux64').mode == 0o755
        for name, digest in manifest.items():
            assert hashlib.sha256(tar.extractfile(prefix+name).read()).hexdigest() == digest
    print(archive)
    print(len(manifest), 'files verified against the explicit allowlist and manifest')


if __name__ == '__main__':
    main()
