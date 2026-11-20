"""Package a Windows release from current build artifacts with an explicit allowlist.
Usage: python tools/package_windows.py 0.9
"""
import hashlib,json,re,sys,zipfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import link
ROOT=Path(__file__).resolve().parents[1]
def main():
    version=sys.argv[1]
    if not re.fullmatch(r'\d+\.\d+(?:\.\d+)?',version): raise SystemExit('Expected a release version')
    out=ROOT/'build/releases'/version;out.mkdir(parents=True,exist_ok=True)
    game=ROOT/'build/win64/ss2'
    sdk=next(Path(h) for h in link.SDL_HINTS if h and (Path(h)/'x86_64-w64-mingw32/lib').is_dir())
    files={name:game/name for name in ('ShiningSource.exe','ss2_win64.exe','ss2_game.elf','SDL3.dll')}
    files.update({'ss_relay.exe':ROOT/'build/relay/ss_relay.exe','LICENSE.txt':ROOT/'LICENSE','DearImGui-LICENSE.txt':ROOT/'third_party/imgui/LICENSE.txt','rcheevos-LICENSE.txt':ROOT/'third_party/rcheevos/LICENSE','SDL3-LICENSE.txt':sdk/'LICENSE.txt','README.md':ROOT/'README.md',f'docs/RELEASE_{version}.md':ROOT/f'docs/RELEASE_{version}.md','docs/ACHIEVEMENTS_VERIFICATION.md':ROOT/'docs/ACHIEVEMENTS_VERIFICATION.md','docs/RETROACHIEVEMENTS_SOURCE.md':ROOT/'assets/achievements/SOURCE.md'})
    files['docs/MULTIPLAYER_AUDIO_AUDIT.md']=ROOT/'docs/MULTIPLAYER_AUDIO_AUDIT.md'
    files['recomplauncher.json']=ROOT/'recomplauncher.json'
    for image in ('launcher-achievements.png','achievement-popup.png'):
        files['docs/img/'+image]=ROOT/'docs/img'/image
    for asset in (ROOT/'assets/achievements').iterdir():
        if re.fullmatch(r'(ra_set_[0-9a-f]{32}\.json|badge_[0-9]+\.png)',asset.name):files[asset.name]=asset
    catalogs=[p for n,p in files.items() if n.startswith('ra_set_')]
    assert len(catalogs)==1
    body=json.loads(catalogs[0].read_text())
    official=[a for s in body['Sets'] for a in s['Achievements'] if a['Flags']==3 and 0<a['ID']<101000001]
    assert len(official)==52 and sum(a['Points'] for a in official)==454
    assert all('badge_'+a['BadgeName']+'.png' in files for a in official)
    assert all(path.is_file() for path in files.values()),[(n,str(p)) for n,p in files.items() if not p.is_file()]
    forbidden=('.gba','.sav','.auth','.ini','.log','.raw','.bmp')
    assert not any(name.endswith(forbidden) for name in files)
    manifest={name:hashlib.sha256(path.read_bytes()).hexdigest() for name,path in files.items()}
    archive=out/f'ShiningSource-{version}-windows-x64.zip';prefix=f'ShiningSource-{version}/'
    with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
        for name,path in sorted(files.items()):z.write(path,prefix+name)
        z.writestr(prefix+'MANIFEST-SHA256.json',json.dumps(manifest,indent=2)+'\n')
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None
        for name,digest in manifest.items():assert hashlib.sha256(z.read(prefix+name)).hexdigest()==digest
    digest=hashlib.sha256(archive.read_bytes()).hexdigest()
    (out/f'SHA256SUMS-{version}.txt').write_text(digest+'  '+archive.name+'\n')
    print(archive);print(digest);print(len(files),'verified files; 52 official achievements and badges')
if __name__=='__main__':main()
