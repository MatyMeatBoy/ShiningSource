#!/usr/bin/env python3
"""render_dump.py DUMP.bin OUT.png : draw a video-state dump (SS_DUMP=N) as a 240x160 picture.

A reference renderer for checking the port before its real PPU exists: mode 0 text backgrounds and
regular sprites, priorities, no blending, windows or affine. Not fast and not part of the runtime.
"""
import struct, sys, zlib

data = open(sys.argv[1], "rb").read()
io, pal, vram, oam = data[:0x400], data[0x400:0x800], data[0x800:0x18800], data[0x18800:0x18C00]
r16 = lambda buf, o: buf[o] | buf[o + 1] << 8
colour = lambda i: (lambda v: ((v & 31) * 255 // 31, (v >> 5 & 31) * 255 // 31, (v >> 10 & 31) * 255 // 31))(r16(pal, i * 2))
dispcnt = r16(io, 0)
W, H = 240, 160
screen = [[colour(0)] * W for _ in range(H)]
order = [[(4, 0)] * W for _ in range(H)]      # (priority, layer) of what is drawn; lower wins


def tile_pixel(base, tile, x, y, bpp8, palbank, obj=False):
    if bpp8:
        index = vram[base + tile * 64 + y * 8 + x]
        return (0x100 if obj else 0) + index if index else None
    byte = vram[base + tile * 32 + y * 4 + x // 2]
    index = byte >> 4 if x & 1 else byte & 15
    return (0x100 if obj else 0) + palbank * 16 + index if index else None


def background(n):
    cnt = r16(io, 8 + n * 2)
    prio, char, bpp8, scr, size = cnt & 3, (cnt >> 2 & 3) * 0x4000, cnt >> 7 & 1, (cnt >> 8 & 31) * 0x800, cnt >> 14
    sx, sy = r16(io, 0x10 + n * 4) & 511, r16(io, 0x12 + n * 4) & 511
    mw, mh = (256, 512)[size & 1], (256, 512)[size >> 1]
    for y in range(H):
        for x in range(W):
            px, py = (x + sx) % mw, (y + sy) % mh
            block = (px // 256) + (py // 256) * (2 if mw == 512 else 1)
            entry = r16(vram, scr + block * 0x800 + ((py % 256) // 8) * 64 + ((px % 256) // 8) * 2)
            tx, ty = px & 7, py & 7
            if entry & 0x400: tx = 7 - tx
            if entry & 0x800: ty = 7 - ty
            index = tile_pixel(char, entry & 0x3FF, tx, ty, bpp8, entry >> 12)
            if index is not None and (prio, n) < order[y][x]:
                screen[y][x], order[y][x] = colour(index), (prio, n)


def sprites():
    sizes = {0: [(8, 8), (16, 16), (32, 32), (64, 64)], 1: [(16, 8), (32, 8), (32, 16), (64, 32)], 2: [(8, 16), (8, 32), (16, 32), (32, 64)]}
    for i in range(127, -1, -1):
        a0, a1, a2 = r16(oam, i * 8), r16(oam, i * 8 + 2), r16(oam, i * 8 + 4)
        if (a0 >> 8 & 3) == 2 or (a0 >> 14) == 3:
            continue
        w, h = sizes[a0 >> 14][a1 >> 14]
        y0, x0 = a0 & 255, a1 & 511
        if y0 >= 160: y0 -= 256
        if x0 >= 240: x0 -= 512
        bpp8, tile, prio, bank = a0 >> 13 & 1, a2 & 0x3FF, a2 >> 10 & 3, a2 >> 12
        for y in range(h):
            for x in range(w):
                sx, sy = x0 + x, y0 + y
                if not (0 <= sx < W and 0 <= sy < H):
                    continue
                tx, ty = (w - 1 - x if a1 & 0x1000 else x), (h - 1 - y if a1 & 0x2000 else y)
                number = tile + (ty // 8) * (32 if not dispcnt & 0x40 else w // 8) * (2 if bpp8 and not dispcnt & 0x40 else 1) + (tx // 8) * (2 if bpp8 else 1)
                index = tile_pixel(0x10000, number & 0x3FF, tx & 7, ty & 7, bpp8, bank, obj=True)
                if index is not None and (prio, -1) <= order[sy][sx]:
                    screen[sy][sx], order[sy][sx] = colour(index), (prio, -1)


for n in (3, 2, 1, 0):
    if dispcnt & (0x100 << n) and (dispcnt & 7) == 0:
        background(n)
if dispcnt & 0x1000:
    sprites()
if dispcnt & 0x80:
    screen = [[(255, 255, 255)] * W for _ in range(H)]
rows = b"".join(b"\0" + bytes(c for px in row for c in px) for row in screen)
chunk = lambda tag, body: struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)
open(sys.argv[2], "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
print("DISPCNT %04X, mode %d, layers %s" % (dispcnt, dispcnt & 7, "".join(str(n) for n in range(4) if dispcnt & (0x100 << n)) + ("+OBJ" if dispcnt & 0x1000 else "")))
