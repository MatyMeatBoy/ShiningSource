#!/usr/bin/env python3
"""Build the launcher (Windows x64) into build/win64/ss2/ShiningSource.exe, next to the game host it starts.

  build_launcher.py

Dear ImGui (third_party/imgui) is compiled once into build/launcher/ and reused.
"""
import shutil, subprocess, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build, link

ROOT = build.ROOT


def main():
    cxx = str(Path(build.clang()).with_name("clang++.exe"))
    sdl = next((Path(h) / "x86_64-w64-mingw32" for h in link.SDL_HINTS if h and (Path(h) / "x86_64-w64-mingw32/lib").is_dir()), None)
    if sdl is None:
        sys.exit("SDL3 development files not found: set SHININGSOURCE_SDL3")
    work = ROOT / "build/launcher"
    out = ROOT / "build/win64/ss2"
    work.mkdir(parents=True, exist_ok=True)
    out.mkdir(parents=True, exist_ok=True)
    imgui = ROOT / "third_party/imgui"
    flags = ["--target=x86_64-w64-mingw32", "-O2", "-std=c++17", "-I", str(imgui), "-I", str(sdl / "include")]
    objects = []
    sources = [imgui / n for n in ("imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp",
                                   "backends/imgui_impl_sdl3.cpp", "backends/imgui_impl_opengl3.cpp")] + [ROOT / "launcher/launcher.cpp"]
    for source in sources:
        target = work / (source.stem + ".o")
        if not target.exists() or target.stat().st_mtime < source.stat().st_mtime:
            done = subprocess.run([cxx, *flags, "-c", str(source), "-o", str(target)], capture_output=True, text=True)
            if done.returncode:
                print(done.stderr[-3000:])
                sys.exit("failed: " + source.name)
        objects.append(str(target))
    exe = out / "ShiningSource.exe"
    done = subprocess.run([cxx, "--target=x86_64-w64-mingw32", *objects, "-o", str(exe), "-static-libstdc++", "-static-libgcc",
                           "-L", str(sdl / "lib"), "-lSDL3", "-lopengl32", "-lcomdlg32", "-mwindows",
                           "-Wl,-Bstatic", "-lc++", "-lunwind", "-Wl,-Bdynamic"], capture_output=True, text=True)
    if done.returncode:
        print(done.stderr[-3000:])
        sys.exit("launcher link failed")
    shutil.copy2(sdl / "bin/SDL3.dll", out / "SDL3.dll")
    print(exe.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
