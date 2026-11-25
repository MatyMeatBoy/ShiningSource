#!/usr/bin/env python3
"""Convert a raw GA36 framebuffer dump to PNG (no third-party modules).

    py -3.12 tools/ga36_fb_to_png.py panel.raw panel.png [--width 640 --height 480 --format auto]

The GA36 panel is 640x480 RGB565, little endian, 1280 bytes a row. A file of 640*480*2 bytes is read as RGB565, one
of 640*480*4 bytes as XRGB8888 (B, G, R, X in memory); any other size needs --format and --stride. Prints the
size, the format used and how many pixels are not black; exits 1 when the picture is all black and --require-content
is given.
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path

FORMATS = ('auto', 'rgb565', 'bgr565', 'xrgb8888', 'xbgr8888')


def _png(width, height, rows):
    def chunk(kind, data):
        body = kind + data
        return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body) & 0xFFFFFFFF)
    raw = b''.join(b'\0' + row for row in rows)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def _lut565(swap):
    """RGB565 word -> 3 RGB bytes, for all 65536 words (bit replication, as a panel shows them)."""
    out = bytearray(65536 * 3)
    for word in range(65536):
        r, g, b = word >> 11, (word >> 5) & 63, word & 31
        r, g, b = (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)
        if swap:
            r, b = b, r
        out[word * 3:word * 3 + 3] = bytes((r, g, b))
    return out


def convert(raw_path, png_path, width=640, height=480, fmt='auto', stride=0):
    """Write png_path from raw_path. Returns a dict: format, width, height, non_black (pixel count)."""
    data = Path(raw_path).read_bytes()
    if fmt == 'auto':
        if len(data) == width * height * 2:
            fmt = 'rgb565'
        elif len(data) == width * height * 4:
            fmt = 'xrgb8888'
        else:
            raise ValueError(f'{raw_path}: {len(data)} bytes is neither {width}x{height}x2 nor x4; give --format')
    depth = 2 if fmt.endswith('565') else 4
    stride = stride or width * depth
    if len(data) < stride * (height - 1) + width * depth:
        raise ValueError(f'{raw_path}: {len(data)} bytes is too short for {width}x{height} {fmt}, stride {stride}')
    rows, non_black = [], 0
    if depth == 2:
        lut = _lut565(fmt == 'bgr565')
        for y in range(height):
            words = struct.unpack_from(f'<{width}H', data, y * stride)
            non_black += width - words.count(0)
            rows.append(b''.join(lut[w * 3:w * 3 + 3] for w in words))
    else:
        for y in range(height):
            line = data[y * stride:y * stride + width * 4]
            if fmt == 'xrgb8888':       # memory order B G R X
                rgb = bytearray(width * 3)
                rgb[0::3], rgb[1::3], rgb[2::3] = line[2::4], line[1::4], line[0::4]
            else:                        # xbgr8888: memory order R G B X
                rgb = bytearray(width * 3)
                rgb[0::3], rgb[1::3], rgb[2::3] = line[0::4], line[1::4], line[2::4]
            non_black += sum(1 for i in range(0, len(rgb), 3) if rgb[i] or rgb[i + 1] or rgb[i + 2])
            rows.append(bytes(rgb))
    Path(png_path).write_bytes(_png(width, height, rows))
    return {'format': fmt, 'width': width, 'height': height, 'non_black': non_black}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('raw', type=Path)
    parser.add_argument('png', type=Path)
    parser.add_argument('--width', type=int, default=640)
    parser.add_argument('--height', type=int, default=480)
    parser.add_argument('--format', choices=FORMATS, default='auto')
    parser.add_argument('--stride', type=int, default=0, help='bytes per row (default: width times pixel size)')
    parser.add_argument('--require-content', action='store_true', help='exit 1 if every pixel is black')
    args = parser.parse_args()
    try:
        info = convert(args.raw, args.png, args.width, args.height, args.format, args.stride)
    except (OSError, ValueError) as error:
        sys.exit(f'ga36_fb_to_png: {error}')
    total = info['width'] * info['height']
    print(f"{args.png}: {info['width']}x{info['height']} {info['format']}, {info['non_black']} of {total} pixels not black")
    if args.require_content and not info['non_black']:
        sys.exit('ga36_fb_to_png: the framebuffer is all black')


if __name__ == '__main__':
    main()
