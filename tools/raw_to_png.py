#!/usr/bin/env python3
"""raw_to_png.py ss_frame.raw OUT.png : convert the 240x160 ARGB frame written by SS_DUMP=N to PNG."""
import struct, sys, zlib
raw = open(sys.argv[1], "rb").read()
H = 160
W = len(raw) // 4 // H        # the picture is 240 wide, or wider with SS_VIEW
rows = b"".join(b"\0" + b"".join(raw[(y * W + x) * 4 + 2::-1][:3] if False else bytes((raw[(y * W + x) * 4 + 2], raw[(y * W + x) * 4 + 1], raw[(y * W + x) * 4])) for x in range(W)) for y in range(H))
chunk = lambda tag, body: struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)
open(sys.argv[2], "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
