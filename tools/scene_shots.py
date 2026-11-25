#!/usr/bin/env python3
"""Pictures of the game's scenes from the Windows 64-bit build, at fixed frames, for comparing views and canvases.

  scene_shots.py [--canvas WxH] [--exe EXE] [--rom ROM] [--out DIR] [--only NAME,...] [--window]
  scene_shots.py --explore SCRIPT:FROM:TO:STEP [--canvas WxH] [--out DIR]

The game is played by the recorded button scripts tools/tests/play_input.txt and tools/tests/town_input.txt
(SS_INPUT_FILE, SS_SKIP=0 so the scenes are not skipped, SS_FRAMESKIP=0 so every frame is shown and the window's
picture count is the game's frame count) in a window, and SS_WINDOW_SHOT (runtime/win32/plat_sdl.c) saves what
the window shows at the listed pictures. A few scenes need the port's test controls: SS_TEST_WARP (the town),
SS_TEST_SHOP (a shop's menu), SS_SKIP_AT (the skip-scene overlay, with the skip feature on) and SS_TEST_KEYS
(the options menu, opened with Escape). See SCENES below; frames were found by looking at --explore pictures.

--canvas WxH sets SS_CANVAS=WxH, SS_VIEW=W and SS_VIEW_H=H (a build that does not know a variable ignores it).
Without --window the SDL offscreen video driver is used, so no window opens (the picture is the same renderer's).

Output in --out (default build/shots/WxH): one PNG per scene (NN-name.png), contact.png (all of them with their
names), index.json (scene, frame, run, size, file, environment) and the runs' logs. The ROM is only read.
Needs Pillow.
"""
import argparse, concurrent.futures, json, os, shutil, subprocess, sys, tempfile, time
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:                                  # pragma: no cover
    sys.exit("scene_shots.py needs Pillow (py -3.12 -m pip install pillow)")

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ROM = "D:/01Claude/GBA/Shining Soul II (Europe) (En,Fr,De,Es,It).gba"
DEFAULT_EXE = ROOT / "build/win64/ss2/ss2_win64.exe"
SCRIPTS = {"play": ROOT / "tools/tests/play_input.txt", "town": ROOT / "tools/tests/town_input.txt"}

# Runs: the script and the test controls of each. Every scene names its run and the picture it is taken at.
RUNS = {
    "play": {"script": "play", "env": {}},
    "town": {"script": "town", "env": {"SS_TEST_WARP": "9000:0:0"}},
    "shop": {"script": "town", "env": {"SS_TEST_WARP": "9000:0:0", "SS_TEST_SHOP": "9300", "SS_TEST_SHOP_KIND": "1"}},
    "skip": {"script": "play", "env": {"SS_SKIP": "1", "SS_SKIP_AT": "@skip_at"}},
    "options": {"script": "play", "env": {"SS_TEST_KEYS": "@options_keys"}},
}

# (name, run, picture). Kept in the order of the game.
# Found so far (play_input.txt, 240x160 explore): SEGA logo 60, language flags 300, title 540. Still to find with
# --explore: character creation, first cut-scene and the frames after it, town, shop, Items/Status menus, field
# with enemies, fight, text cut-scene; then set SKIP_AT and OPTIONS_AT and add the skip/options/shop scenes.
SCENES = [
    ("sega", "play", 60),
    ("language", "play", 300),
    ("title", "play", 540),
]
SKIP_AT = 0
OPTIONS_AT = 0


def parse_canvas(text):
    w, h = text.lower().split("x")
    return int(w), int(h)


def base_env(canvas, window):
    env = {k: v for k, v in os.environ.items() if not k.startswith("SS_")}
    env.update({"SS_SKIP": "0", "SS_NET": "", "SS_FRAMESKIP": "0", "SDL_AUDIO_DRIVER": "dummy", "SS_WINDOW_MODE": "windowed"})
    if not window:
        env["SDL_VIDEO_DRIVER"] = "offscreen"
    if canvas:
        w, h = canvas
        env.update({"SS_CANVAS": "%dx%d" % (w, h), "SS_VIEW": str(w), "SS_VIEW_H": str(h)})
    return env


def run_shots(exe, rom, env, pictures, work, log, timeout=1200):
    """One run that saves the window at each of `pictures`; returns {picture: Path of the BMP} for those made."""
    pictures = sorted(set(pictures))
    work.mkdir(parents=True, exist_ok=True)
    env = dict(env)
    env["SS_WINDOW_SHOT"] = ",".join(str(p) for p in pictures)
    env.setdefault("SS_QUIT", str(pictures[-1] + 20))
    with open(log, "w", encoding="utf-8", errors="replace") as out:
        out.write("env: %s\n" % " ".join("%s=%s" % (k, v) for k, v in sorted(env.items()) if k.startswith(("SS_", "SDL_"))))
        out.flush()
        done = subprocess.run([str(exe), str(rom)], cwd=work, env=env, stdout=out, stderr=subprocess.STDOUT, timeout=timeout)
        out.write("exit: %d\n" % done.returncode)
    made = {}
    for i, p in enumerate(pictures):
        f = work / ("ss_window.bmp" if i == 0 else "ss_window_%d.bmp" % p)
        if f.exists():
            made[p] = f
    return made, done.returncode


def label_font(size):
    for name in ("consola.ttf", "DejaVuSansMono.ttf", "arial.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def contact_sheet(items, path, columns=4, thumb_w=480):
    """items: [(label, png path)]"""
    if not items:
        return
    images = [(label, Image.open(p).convert("RGB")) for label, p in items]
    thumbs = []
    for label, im in images:
        scale = thumb_w / im.width
        thumbs.append((label, im.resize((thumb_w, max(1, round(im.height * scale))), Image.NEAREST)))
    cell_h = max(t.height for _, t in thumbs) + 28
    rows = (len(thumbs) + columns - 1) // columns
    sheet = Image.new("RGB", (columns * (thumb_w + 8) + 8, rows * (cell_h + 8) + 8), (32, 32, 40))
    draw = ImageDraw.Draw(sheet)
    font = label_font(18)
    for i, (label, t) in enumerate(thumbs):
        x = 8 + (i % columns) * (thumb_w + 8)
        y = 8 + (i // columns) * (cell_h + 8)
        sheet.paste(t, (x, y + 26))
        draw.text((x + 2, y + 2), label, fill=(240, 240, 240), font=font)
    sheet.save(path)


def resolve(value):
    if value == "@skip_at":
        return str(SKIP_AT)
    if value == "@options_keys":
        return "%d:esc" % OPTIONS_AT
    return value


def scripts_dir(out):
    """The button scripts are copied next to the runs (the game reads SS_INPUT_FILE relative to its folder too)."""
    return {name: path.resolve() for name, path in SCRIPTS.items()}


def take(args):
    canvas = parse_canvas(args.canvas) if args.canvas else None
    out = Path(args.out or ROOT / "build/shots" / (args.canvas or "default"))
    if out.exists():
        shutil.rmtree(out)
    (out / "logs").mkdir(parents=True)
    scripts = scripts_dir(out)
    wanted = [s for s in SCENES if not args.only or s[0] in args.only.split(",")]
    by_run = {}
    for name, run, picture in wanted:
        by_run.setdefault(run, []).append(picture)
    env0 = base_env(canvas, args.window)
    results, index = {}, []
    with tempfile.TemporaryDirectory(prefix="ss_shots_") as tmp:
        def one(run):
            spec = RUNS[run]
            env = dict(env0)
            env["SS_INPUT_FILE"] = str(scripts[spec["script"]])
            env.update({k: resolve(v) for k, v in spec["env"].items()})
            t0 = time.time()
            made, code = run_shots(args.exe, args.rom, env, by_run[run], Path(tmp) / run, out / "logs" / (run + ".log"))
            return run, made, code, time.time() - t0, env
        with concurrent.futures.ThreadPoolExecutor(max(1, args.jobs)) as pool:
            for run, made, code, seconds, env in pool.map(one, list(by_run)):
                results[run] = (made, code, seconds, env)
                print("run %-8s exit %d, %d of %d pictures, %.0f s" % (run, code, len(made), len(set(by_run[run])), seconds))
        sheet = []
        for number, (name, run, picture) in enumerate(wanted, 1):
            made, code, seconds, env = results[run]
            entry = {"scene": name, "run": run, "picture": picture, "script": RUNS[run]["script"],
                     "env": {k: v for k, v in sorted(env.items()) if k.startswith(("SS_", "SDL_")) and k != "SS_WINDOW_SHOT"}}
            if picture in made:
                png = out / ("%02d-%s.png" % (number, name))
                im = Image.open(made[picture])
                im.save(png)
                entry.update(file=png.name, width=im.width, height=im.height)
                sheet.append(("%02d %s (%s %d)" % (number, name, run, picture), png))
            else:
                entry["missing"] = "no picture (run exit %d)" % code
            index.append(entry)
    contact_sheet(sheet, out / "contact.png")
    exe = Path(args.exe)
    meta = {"canvas": args.canvas, "exe": str(exe), "exe_mtime": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(exe.stat().st_mtime)),
            "git": git_head(), "video": "window" if args.window else "offscreen", "scenes": index}
    (out / "index.json").write_text(json.dumps(meta, indent=1), encoding="utf-8")
    missing = [e["scene"] for e in index if "missing" in e]
    print("%s: %d pictures%s" % (out, len(index) - len(missing), (", missing: " + ", ".join(missing)) if missing else ""))
    return 1 if missing else 0


def git_head():
    try:
        return subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
    except OSError:
        return None


def explore(args):
    """Pictures every STEP frames of one script (with a run's test controls), for finding the frames of SCENES."""
    spec, a, b, step = args.explore.split(":")
    run = spec if spec in RUNS else None
    script = RUNS[run]["script"] if run else spec
    canvas = parse_canvas(args.canvas) if args.canvas else None
    out = Path(args.out or ROOT / "build/shots/explore" / ("%s-%s-%s" % (spec, a, b)))
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    env = base_env(canvas, args.window)
    env["SS_INPUT_FILE"] = str(SCRIPTS[script].resolve())
    if run:
        env.update({k: resolve(v) for k, v in RUNS[run]["env"].items()})
    for extra in args.env or []:
        k, v = extra.split("=", 1)
        env[k] = v
    pictures = list(range(int(a), int(b) + 1, int(step)))
    with tempfile.TemporaryDirectory(prefix="ss_explore_") as tmp:
        made, code = run_shots(args.exe, args.rom, env, pictures, Path(tmp), out / "run.log")
        items = []
        for p in pictures:
            if p in made:
                png = out / ("%06d.png" % p)
                Image.open(made[p]).save(png)
                items.append(("%d" % p, png))
    for i in range(0, len(items), 24):
        contact_sheet(items[i:i + 24], out / ("sheet-%02d.png" % (i // 24)), columns=6, thumb_w=320)
    print("%s: %d of %d pictures, exit %d" % (out, len(items), len(pictures), code))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--canvas")
    parser.add_argument("--exe", default=str(DEFAULT_EXE))
    parser.add_argument("--rom", default=os.environ.get("SS_ROM", DEFAULT_ROM))
    parser.add_argument("--out")
    parser.add_argument("--only")
    parser.add_argument("--window", action="store_true", help="a real window instead of the offscreen driver")
    parser.add_argument("--jobs", type=int, default=3, help="runs at once (each plays in real time)")
    parser.add_argument("--explore")
    parser.add_argument("--env", action="append", help="explore: extra VAR=VALUE")
    args = parser.parse_args()
    if not Path(args.exe).is_file():
        sys.exit("no program %s: build it with tools/build_win64.py" % args.exe)
    if not Path(args.rom).is_file():
        sys.exit("no ROM %s (--rom or SS_ROM)" % args.rom)
    return explore(args) if args.explore else take(args)


if __name__ == "__main__":
    sys.exit(main())
