#!/usr/bin/env python3
"""List what stops the game from linking for WebAssembly: calls whose declaration and definition disagree.

  check_wasm.py [--game ss2]

WebAssembly checks every call against the function's real signature. A function declared with other parameters
or another return type in the calling file links with a warning and is replaced by a trap; on the console and on
the other processors the same call works by accident of the calling convention. This compiles the generated
sources for wasm32 and prints the linker's list, grouped by function: the work list for the web build, and the
places where the decompilation's declarations are wrong on every platform.

Needs emscripten's clang and wasm-ld (SHININGSOURCE_EMSDK, default D:/01Claude/ReKHCoM-build/toolchain/emsdk).
"""
import argparse, concurrent.futures, os, re, subprocess, sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build

ROOT = build.ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game", default="ss2", choices=sorted(build.GAMES))
    parser.add_argument("-j", type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()
    emsdk = Path(os.environ.get("SHININGSOURCE_EMSDK", "D:/01Claude/ReKHCoM-build/toolchain/emsdk"))
    cc, ld = emsdk / "upstream/bin/clang.exe", emsdk / "upstream/bin/wasm-ld.exe"
    if not cc.exists() or not ld.exists():
        sys.exit("emscripten's clang and wasm-ld not found under %s" % emsdk)
    out = ROOT / "build/wasm-check" / args.game
    (out / "obj").mkdir(parents=True, exist_ok=True)
    flags = [f for f in build.CFLAGS if not f.startswith("--target") and f != "-mno-ms-bitfields"] + ["--target=wasm32-unknown-unknown",
             "-ffreestanding", "-nostdlib"]
    gen = build.prepare(args.game, prefix=False)        # plain sources: no 64-bit adapters, wasm32 is a 32-bit target
    replaced = build.port_config(args.game)["runtime"]
    sources = [p for p in sorted((gen / "src").glob("*.c")) if p.stem not in replaced and p.stem != "abi_thunks"]

    def compile_one(path):
        target = out / "obj" / (path.stem + ".o")
        done = subprocess.run([str(cc), *flags, "-iquote", str(gen / "include"), "-I", str(gen / "include"), "-c", str(path),
                               "-o", str(target)], capture_output=True, text=True, cwd=gen)
        return path.stem, done.returncode, done.stderr, target
    objects, failed = [], []
    with concurrent.futures.ThreadPoolExecutor(args.j) as pool:
        for name, code, errors, target in pool.map(compile_one, sources):
            if code:
                failed.append((name, [l for l in errors.splitlines() if "error:" in l][:3]))
            else:
                objects.append(target)
    for name, errors in failed:
        print("does not compile: %s  %s" % (name, " | ".join(e.split("error:")[1].strip() for e in errors)))
    done = subprocess.run([str(ld), "--no-entry", "--allow-undefined", "--export-all", "--error-limit=0", "-o", str(out / "check.wasm"), *map(str, objects)],
                          capture_output=True, text=True)
    mismatches = defaultdict(list)
    current = None
    for line in done.stderr.splitlines():
        m = re.search(r'function signature mismatch: (\w+)', line)
        if m:
            current = m[1]
            continue
        m = re.search(r'>>> defined as (.*?) in (.*)', line)
        if m and current:
            mismatches[current].append((m[1].strip(), Path(m[2].strip()).name))
    other = [l for l in done.stderr.splitlines() if "error:" in l and "signature mismatch" not in l]
    for name in sorted(mismatches):
        print(name)
        for signature, where in mismatches[name]:
            print("    %-40s %s" % (signature, where))
    for line in other[:20]:
        print(line)
    print("%d of %d modules compile for wasm32; %d function(s) are called with a signature that is not theirs"
          % (len(objects), len(sources), len(mismatches)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
