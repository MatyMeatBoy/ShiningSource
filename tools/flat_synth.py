#!/usr/bin/env python3
"""Draft Flat art for a framed scene by patch synthesis (PixelBuddy's fill step, without a generative model).

  flat_synth.py --scene NAME [--masks assets/launcher/43-masks] [--patch 6] [--seed 1] [--out FILE]

Reads NAME/base.png and NAME/mascara.png from tools/flat_masks.py. Every pixel painted comes, unchanged, from the
approved 16:9 side art of the scene (the kept columns of base.png beside the console's picture) or from given
source rectangles of the console's own background (--source x,y,w,h in base.png coordinates, repeatable): the
missing area is filled square by square from the known edge outwards, each square the candidate whose border
matches what is already there best (image quilting with a minimum-error seam). Nothing is scaled or smeared.
Writes a 320 x 240 draft (default assets/launcher/43-masks/NAME/borrador.png) and a 2x preview; the draft is
only used after the user approves it (then it is copied to assets/launcher/NAME-flat.png).
"""
import argparse
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]


def candidates(sources, size):
    """Every size x size block of the source images (numpy RGB arrays), as one array."""
    blocks = []
    for image in sources:
        h, w, _ = image.shape
        for y in range(0, h - size + 1):
            for x in range(0, w - size + 1):
                blocks.append(image[y:y + size, x:x + size])
    return np.asarray(blocks, dtype=np.int32)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scene", required=True)
    ap.add_argument("--masks", type=Path, default=ROOT / "assets/launcher/43-masks")
    ap.add_argument("--patch", type=int, default=6)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--source", action="append", default=[], help="x,y,w,h of background to sample, base.png coordinates")
    ap.add_argument("--out", type=Path)
    ap.add_argument("--warm", action="store_true", help="only source blocks whose mean blue is below their mean red (no flat cool patches)")
    args = ap.parse_args()
    folder = args.masks / args.scene
    base = np.asarray(Image.open(folder / "base.png").convert("RGB"), dtype=np.int32)
    paint = np.asarray(Image.open(folder / "mascara.png").convert("L")) > 127
    H, W, _ = base.shape
    rng = np.random.default_rng(args.seed)
    size = args.patch
    # sources: the approved columns (left and right strips) and any given background rectangles
    sources = [base[40:200, 18:40], base[40:200, 280:302]]
    for text in args.source:
        x, y, w, h = map(int, text.split(","))
        sources.append(base[y:y + h, x:x + w])
    blocks = candidates(sources, size)
    if args.warm:
        means = blocks.mean(axis=(1, 2))
        blocks = blocks[means[:, 2] < means[:, 0]]
        print("warm blocks:", len(blocks))
    known = ~paint
    out = base.copy()
    out[paint] = 0
    # fill order: squares touching known pixels first, outwards (onion peel), the nearest to the picture first
    while not known.all():
        best_spot, best_count = None, -1
        ys, xs = np.nonzero(~known)
        order = rng.permutation(len(ys))
        for k in order[:400]:
            y0 = min(max(ys[k] - size // 2, 0), H - size)
            x0 = min(max(xs[k] - size // 2, 0), W - size)
            region = known[y0:y0 + size, x0:x0 + size]
            count = int(region.sum())
            if count > best_count and count < size * size:
                best_spot, best_count = (y0, x0), count
        y0, x0 = best_spot
        region_known = known[y0:y0 + size, x0:x0 + size]
        target = out[y0:y0 + size, x0:x0 + size]
        if region_known.any():
            diff = ((blocks - target) ** 2).sum(axis=3)
            errors = (diff * region_known).sum(axis=(1, 2)) / region_known.sum()
            limit = errors.min() * 1.1 + 1
            choice = rng.choice(np.nonzero(errors <= limit)[0])
        else:
            choice = rng.integers(len(blocks))
        patch = blocks[choice]
        fill = ~region_known
        target[fill] = patch[fill]
        known[y0:y0 + size, x0:x0 + size] = True
    image = Image.fromarray(out.astype(np.uint8), "RGB")
    path = args.out or folder / "borrador.png"
    image.save(path)
    image.resize((W * 2, H * 2), Image.NEAREST).save(path.with_name(path.stem + "-x2.png"))
    print(path)


if __name__ == "__main__":
    main()
