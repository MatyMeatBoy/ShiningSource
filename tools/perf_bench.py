#!/usr/bin/env python3
"""Compare the picture and frame cost of two Windows 64-bit builds on the reference script.

  perf_bench.py --a DIR_A --b DIR_B [--frames 12000] [--repeat 3] [--views 240x160,320x160,320x240,480x160]

Each DIR holds ss2_win64.exe and ss2_game.elf. Every run is headless (SS_FRAMES), draws every frame at the given
view (SS_TEST_RENDER=W, SS_VIEW_H=H), times the stages (SS_PROF=1) and prints the picture hash (SS_TEST_RENDER's
final line) so that the two builds can be shown to draw the same pictures. Runs alternate A, B, A, B... to spread
the machine's own noise over both. The ROM is only read.
"""
import argparse, os, re, statistics, subprocess, tempfile, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ROM = Path("D:/01Claude/GBA/Shining Soul II (Europe) (En,Fr,De,Es,It).gba")


def run(exe, view, frames):
    width, height = view.split("x")
    env = dict(os.environ, SS_FRAMES=str(frames), SS_SKIP="0", SS_NET="", SS_PROF="1",
               SS_INPUT_FILE=str(ROOT / "tools/tests/play_input.txt"), SS_TEST_RENDER=width, SS_VIEW=width)
    if height != "160":
        env["SS_VIEW_H"] = height
    with tempfile.TemporaryDirectory() as work:
        start = time.perf_counter()
        done = subprocess.run([str(exe), str(ROM)], cwd=work, env=env, capture_output=True, text=True)
        wall = time.perf_counter() - start
    text = done.stdout + done.stderr
    stages = {}
    for name, mean in re.findall(r"^prof: ([^:\n]+): n \d+\s+mean ([0-9.]+)", text, re.M):
        stages[name.strip()] = float(mean)
    picture = re.findall(r"^picture: .*hash ([0-9A-F]{8})", text, re.M)
    return {"wall": wall, "stages": stages, "picture": picture[-1] if picture else None, "code": done.returncode,
            "tail": text[-1500:]}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--a", required=True, type=Path)
    ap.add_argument("--b", required=True, type=Path)
    ap.add_argument("--frames", type=int, default=12000)
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--views", default="240x160,320x160,320x240,480x160")
    args = ap.parse_args()
    for view in args.views.split(","):
        results = {"A": [], "B": []}
        for _ in range(args.repeat):
            for side, folder in (("A", args.a), ("B", args.b)):
                results[side].append(run(folder / "ss2_win64.exe", view, args.frames))
        # the fastest run of each: the machine's other work only ever adds time
        wa = min(r["wall"] for r in results["A"])
        wb = min(r["wall"] for r in results["B"])
        pa = {r["picture"] for r in results["A"]}
        pb = {r["picture"] for r in results["B"]}
        print("== view %s, %d frames x %d" % (view, args.frames, args.repeat))
        print("  wall  A %.2f s  B %.2f s  (%+.1f%%)" % (wa, wb, (wb - wa) / wa * 100))
        names = sorted(set(results["A"][0]["stages"]) | set(results["B"][0]["stages"]))
        for name in names:
            a = min(r["stages"].get(name, 0) for r in results["A"])
            b = min(r["stages"].get(name, 0) for r in results["B"])
            change = (b - a) / a * 100 if a else 0
            print("  %-34s A %8.1f  B %8.1f  (%+.1f%%)" % (name, a, b, change))
        print("  picture hash A %s  B %s  %s" % (sorted(map(str, pa)), sorted(map(str, pb)),
                                                 "SAME" if pa == pb and None not in pa else "DIFFERENT"))
        if any(r["code"] for r in results["A"] + results["B"]):
            print("  a run failed:\n" + results["A"][0]["tail"])


if __name__ == "__main__":
    main()
