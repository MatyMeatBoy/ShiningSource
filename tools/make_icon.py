#!/usr/bin/env python3
"""Draw the port's icon: assets/icon.png (256 x 256), assets/icon.ico and the Android sizes.

  make_icon.py

The icon is the port's own "Soul" jewel (the motif of its border and launcher): a gold-rimmed diamond with
light facets on a dark teal tile. It is drawn here, by formula, so that no artwork of any game is involved and
the files can be regenerated at any size. Only the standard library is used.
"""
import struct, zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def png(pixels, size):
    rows = b"".join(b"\0" + bytes(c for x in range(size) for c in pixels[y * size + x]) for y in range(size))
    chunk = lambda kind, data: struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def mix(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def draw(size, tile=True):
    """One sample per sub-pixel on a 4 x 4 grid: smooth edges at every size."""
    out = []
    for py in range(size):
        for px in range(size):
            r = g = b = a = 0
            for sy in range(4):
                for sx in range(4):
                    x = (px + (sx + 0.5) / 4) / size * 2 - 1        # -1 .. 1
                    y = (py + (sy + 0.5) / 4) / size * 2 - 1
                    colour, alpha = sample(x, y, tile)
                    r += colour[0] * alpha; g += colour[1] * alpha; b += colour[2] * alpha; a += alpha
            out.append((int(r / a), int(g / a), int(b / a), int(a / 16 * 255)) if a else (0, 0, 0, 0))
    return out


def sample(x, y, tile):
    deep, teal, gold, pale = (10, 28, 38), (22, 70, 82), (232, 168, 56), (255, 232, 150)
    colour, alpha = (0, 0, 0), 0.0
    if tile:
        # rounded tile
        corner = 0.30
        dx, dy = max(abs(x) - (0.94 - corner), 0), max(abs(y) - (0.94 - corner), 0)
        if dx * dx + dy * dy <= corner * corner:
            colour, alpha = mix(teal, deep, (y + 1) / 2 * 0.9 + 0.1), 1.0
            # a few still stars, as in the port's field
            for sx, sy in ((-0.62, -0.58), (0.66, -0.44), (-0.70, 0.36), (0.58, 0.66), (0.10, -0.80), (-0.28, 0.76)):
                if abs(x - sx) + abs(y - sy) < 0.035:
                    colour = mix(colour, (190, 230, 235), 0.8)
    d = abs(x) / 0.56 + abs(y) / 0.74               # the jewel: a tall diamond
    if d <= 1.0:
        if d > 0.86:
            colour = mix(gold, pale, max(0.0, -(x + y)) * 0.9)          # rim, lit from the top left
        else:
            base = (36, 196, 190) if y < -0.05 * (1 if x < 0 else -1) else (20, 138, 150)
            if x < 0 and y < 0:
                base = (150, 240, 228)                                   # the bright facet
            elif x >= 0 and y < 0:
                base = (60, 206, 200)
            elif x < 0:
                base = (26, 150, 160)
            else:
                base = (14, 104, 122)
            if abs(x) < 0.025 or abs(y) < 0.02:
                base = mix(base, (220, 255, 250), 0.55)                  # the cut between facets
            colour = base
        alpha = 1.0
    return colour, alpha


def main():
    assets = ROOT / "assets"
    assets.mkdir(exist_ok=True)
    images = {size: png(draw(size), size) for size in (16, 32, 48, 64, 128, 256)}
    (assets / "icon.png").write_bytes(images[256])
    # .ico: a directory of PNG images (Windows Vista and later read them)
    entries, blobs, offset = b"", b"", 6 + 16 * len(images)
    for size, data in sorted(images.items()):
        entries += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset)
        blobs += data
        offset += len(data)
    (assets / "icon.ico").write_bytes(struct.pack("<HHH", 0, 1, len(images)) + entries + blobs)
    for name, size in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)):
        folder = ROOT / "android/app/src/main/res" / ("mipmap-" + name)
        folder.mkdir(parents=True, exist_ok=True)
        (folder / "ic_launcher.png").write_bytes(png(draw(size), size))
    (ROOT / "runtime/web/favicon.png").write_bytes(images[64])
    print("assets/icon.png, assets/icon.ico, android mipmaps, runtime/web/favicon.png")


if __name__ == "__main__":
    main()
