#!/usr/bin/env python3
"""flicker_bench.py : measure how the renderer shows sprites the game makes "transparent" by alternating them.

    python3 tools/tests/flicker_bench.py ROM [--exe build/dev32/ss2/ss2_dev32.exe] [--input build/stats/input_play.txt]
                                         [--only TEXT] [--view 427] [--png DIR] [--jobs 8] [--time] [--frames]
                                         [--field 600-32000]
    --only real    only the game's own alternating sprite;  --only none --field A-B    only the long comparison

The cartridge shows a sprite one frame and hides it the next (shadows, ghosts, labels, a character that was hurt)
and counts on a slow screen to melt that into half transparency. The port draws such a sprite steadily at half
strength (runtime/core/ppu.c) and smooths other blinking per pixel (finish_picture in runtime/core/frame.c).
This bench checks that with numbers instead of by eye.

How. The game is run without a window along the reference input script up to a field scene. From there the test
mode SS_BENCH (frame.c) draws every frame and writes a run of them out, and a script (SS_BENCH_SCRIPT) puts test
sprites into the sprite table for the picture only: the game itself never sees them and plays the same in every
run. Each case is a pattern of test sprites over time. For each case the game is run several times:
    T    the pattern itself, as the game would produce it (sprite there one frame, gone the next ...)
    B    no test sprite at all (the background), sprite logic and smoothing off; and once more with them on
    Fk   for every thing that alternates: that thing solid in every frame where it "really" is, logic off
The ideal picture of a frame is the average of the Fk (and of B for a sprite alternating with nothing): what a slow
screen shows. From T, B and Fk, per frame:
    opacity   how strongly each thing shows on its own pixels, at its true place (0 absent, 1 solid; 0.5 wanted)
    stray     pixels that differ from the background outside the true place of every test sprite (a trail, a
              copy left behind), and how far from that place they are
    garbage   pixels at the true place that are neither background, nor the sprite, nor a mix of them
Per case the table gives, over the frames where the pattern has had time to be recognised: the mean opacity and
its range, the largest change of opacity from one frame to the next (steady or blinking), the mean error against
the ideal picture (0..255), the worst stray count with its distance, the worst garbage count, the number of
frames that fail ("bad"), how many frames the pattern took to settle, and for how many frames something was
still drawn after the sprite really went away ("left").
Two pictures are measured: "ppu" is the renderer's own output, "shown" is after the per-pixel smoothing, which
is what the player sees. Controls are things that must not be touched at all: their T run is compared pixel by
pixel with a run that has the sprite logic off (SS_FLICKER=0), column "vs off".
"""
import argparse, os, struct, subprocess, sys, zlib
from array import array
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
H = 160

# ---------------------------------------------------------------- test graphics

def gfx(rows):
    return [[int(ch, 16) for ch in row] for row in rows]

def disc(size=16, a=1, b=2, c=3):
    """A filled disc with a rim and a mark off centre: any shift or flip of it shows."""
    r = size / 2
    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            d = ((x + 0.5 - r) ** 2 + (y + 0.5 - r) ** 2) ** 0.5
            v = 0 if d > r else b if d > r - 2 else a
            if v == a and 3 <= x < 6 and 4 <= y < 7:
                v = c
            row.append(v)
        rows.append(row)
    return rows

def cross(size=16, a=1, b=2):
    rows = []
    for y in range(size):
        rows.append([a if abs(x - y) < 2 else b if abs(x + y - size + 1) < 2 else 0 for x in range(size)])
    return rows

def block(w=16, h=16, a=4, b=5):
    return [[b if x in (0, w - 1) or y in (0, h - 1) else a for x in range(w)] for y in range(h)]

def label(w=32, h=8):
    """Like a name label: thin strokes on nothing."""
    rows = [[0] * w for _ in range(h)]
    for x in range(1, w - 1):
        rows[1][x] = rows[h - 2][x] = 2
    for x in range(3, w - 3, 4):
        for y in range(2, h - 2):
            rows[y][x] = 1
            rows[y][x + 1] = 3 if y % 2 else 1
    return rows

def checker(size=32, a=1, b=6):
    return [[a if (x + y) & 1 else b for x in range(size)] for y in range(size)]

def junk(w=16, h=16):
    return [[9 + ((x * 7 + y * 3) % 4) for x in range(w)] for y in range(h)]

def frame_of(n, size=16):
    """Animation picture n: a bar that turns."""
    rows = [[0] * size for _ in range(size)]
    for i in range(size):
        x, y = [(i, 7), (i, i), (7, i), (size - 1 - i, i)][n % 4]
        for d in (0, 1):
            rows[min(size - 1, y + d)][x] = 1
            rows[y][min(size - 1, x + d)] = 2
    return rows

SIZES = [[(8, 8), (16, 16), (32, 32), (64, 64)], [(16, 8), (32, 8), (32, 16), (64, 32)], [(8, 16), (8, 32), (16, 32), (32, 64)]]
SHAPES = {(8, 8): (0, 0), (16, 16): (0, 1), (32, 32): (0, 2), (64, 64): (0, 3), (16, 8): (1, 0), (32, 8): (1, 1),
          (32, 16): (1, 2), (64, 32): (1, 3), (8, 16): (2, 0), (8, 32): (2, 1), (16, 32): (2, 2), (32, 64): (2, 3)}

# colours of the two test palettes (index: 15-bit colour), strong and unlike the scenery
PALETTES = [
    {1: 0x7C1F, 2: 0x03FF, 3: 0x7FFF, 4: 0x001F, 5: 0x0010, 6: 0x7FE0, 9: 0x03E0, 10: 0x02A0, 11: 0x0160, 12: 0x0BE2},
    {1: 0x7FE0, 2: 0x001F, 3: 0x2000, 4: 0x7C00, 5: 0x4000, 6: 0x7C1F, 9: 0x03E0, 10: 0x02A0, 11: 0x0160, 12: 0x0BE2},
]

class Spr:
    """One test sprite in one frame."""
    def __init__(self, g, x, y, pal=0, prio=2, slot=64, tile=600, mode=0, hflip=0):
        self.g, self.x, self.y, self.pal, self.prio, self.slot, self.tile, self.mode, self.hflip = g, int(x), int(y), pal, prio, slot, tile, mode, hflip

    def lines(self, ctx):
        h, w = len(self.g), len(self.g[0])
        shape, size = SHAPES[(w, h)]
        a0 = (self.y & 255) | self.mode << 10 | shape << 14
        a1 = (self.x & 511) | self.hflip << 12 | size << 14
        a2 = self.tile | self.prio << 10 | ctx.banks[self.pal] << 12
        out = ["m %x %x" % (0x07000000 + self.slot * 8 + i * 2, v) for i, v in enumerate((a0, a1, a2))]
        return out + tile_lines(self.g, self.tile)

def tile_lines(g, tile, base=0x06010000):
    h, w = len(g), len(g[0])
    out = []
    for ty in range(h // 8):
        for tx in range(w // 8):
            at = base + (tile + ty * (w // 8) + tx) * 32          # the game uses the one-dimensional tile layout
            for y in range(8):
                row = g[ty * 8 + y][tx * 8:tx * 8 + 8]
                for half in range(2):
                    v = sum(row[half * 4 + i] << (4 * i) for i in range(4))
                    out.append("m %x %x" % (at + y * 4 + half * 2, v))
    return out

class Raw:
    """Script lines as they are (registers, background tiles)."""
    def __init__(self, lines):
        self.text = list(lines)

    def lines(self, ctx):
        return self.text

# ---------------------------------------------------------------- running the game

class Ctx:
    pass

def run_game(args, tag, script, first, count, env=None):
    """Run to frame first+count with this script; returns the frames written: (frame, world, cam, oam, ppu picture, shown picture)."""
    work = os.path.join(args.work, tag)
    os.makedirs(work, exist_ok=True)
    with open(os.path.join(work, "script.txt"), "w") as f:
        f.write("\n".join(script) + "\n")
    e = dict(os.environ)
    e.update({"SS_SKIP": "0", "SS_FRAMES": str(first + count - 1), "SS_INPUT_FILE": args.input, "SS_BENCH": "%d:%d" % (first, count),
              "SS_BENCH_SCRIPT": os.path.join(work, "script.txt"), "SS_BENCH_OUT": os.path.join(work, "out.bin")})
    if args.view:
        e["SS_VIEW"] = str(args.view)
    e.update(env or {})
    done = subprocess.run([args.exe, args.rom], cwd=work, env=e, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    frames = []
    with open(os.path.join(work, "out.bin"), "rb") as f:
        while True:
            head = f.read(24)
            if len(head) < 24:
                break
            magic, frame, width, world, cx, cy = struct.unpack("<IIIIii", head)
            oam = f.read(0x400)
            frames.append((frame, world, (cx, cy), oam, f.read(width * H * 4), f.read(width * H * 4), width))
    os.remove(os.path.join(work, "out.bin"))
    if len(frames) != count:
        raise SystemExit("%s: %d frames written instead of %d\n%s" % (tag, len(frames), count, done.stderr.decode(errors="replace")[-400:]))
    return frames

def script_for(ctx, case, items_at, with_extra=True):
    lines = []
    for n, colours in enumerate(PALETTES):
        for index, colour in colours.items():
            lines.append("m %x %x" % (0x05000200 + (ctx.banks[n] * 16 + index) * 2, colour))
    for f in range(case.first - 40, case.first + case.count):
        lines.append("F %d" % f)
        if case.extra:
            lines += case.extra(f - case.first)
        for item in items_at(f - case.first):
            lines += item.lines(ctx)
    return lines

# ---------------------------------------------------------------- measuring

def differing(a, b, width):
    """Indexes of the pixels where two pictures differ."""
    out = []
    if a == b:
        return out
    ai, bi = memoryview(a).cast("I"), memoryview(b).cast("I")
    row = width * 4
    for y in range(H):
        if a[y * row:(y + 1) * row] != b[y * row:(y + 1) * row]:
            base = y * width
            out += [base + x for x in range(width) if ai[base + x] != bi[base + x]]
    return out

def rgb(v):
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255)

def to_segment(p, a, b):
    """Distance from colour p to the line of colours between a and b."""
    d = [b[i] - a[i] for i in range(3)]
    q = [p[i] - a[i] for i in range(3)]
    n = sum(x * x for x in d)
    t = 0 if not n else max(0.0, min(1.0, sum(q[i] * d[i] for i in range(3)) / n))
    return sum((q[i] - t * d[i]) ** 2 for i in range(3)) ** 0.5

def measure(pic, back, fulls, weight, width, same):
    """One frame: opacity of each alternating thing, error against the ideal, strays, garbage.
    `back` is the background drawn plainly; `same` is the background drawn the way the tested picture is (the
    game's own scene may hold things the renderer steadies), which is what strays are judged against."""
    P, B = memoryview(pic).cast("I"), memoryview(back).cast("I")
    F = [memoryview(f).cast("I") for f in fulls]
    places = [differing(f, back, width) for f in fulls]
    union = sorted(set().union(*places)) if places else []
    inside = set(union)
    stray = [i for i in differing(pic, same, width) if i not in inside]
    k = len(F)
    # least squares: picture - background = sum of opacity[k] * (full[k] - background) over the true places
    m = [[0.0] * k for _ in range(k)]
    v = [0.0] * k
    error = garbage = 0
    for i in union:
        b = rgb(B[i]); p = rgb(P[i]); f = [rgb(x[i]) for x in F]
        d = [[c[j] - b[j] for j in range(3)] for c in f]
        q = [p[j] - b[j] for j in range(3)]
        for x in range(k):
            v[x] += sum(d[x][j] * q[j] for j in range(3))
            for y in range(k):
                m[x][y] += sum(d[x][j] * d[y][j] for j in range(3))
        ideal = [b[j] + weight * sum(dx[j] for dx in d) for j in range(3)]
        error += sum(abs(p[j] - ideal[j]) for j in range(3)) / 3
        # a mix of background and sprite(s)? distance to the lines between them (and, for two things, their average)
        dist = min(to_segment(p, b, c) for c in f)
        if k == 2:
            mid = [(f[0][j] + f[1][j]) / 2 for j in range(3)]
            dist = min(dist, to_segment(p, f[0], f[1]), to_segment(p, b, mid), to_segment(p, f[0], mid), to_segment(p, f[1], mid))
            # anywhere inside the triangle of the three colours also counts as a mix
            det = m and (sum(a * a for a in d[0]) * sum(a * a for a in d[1]) - sum(d[0][j] * d[1][j] for j in range(3)) ** 2)
            if det > 1e-6:
                a11, a12, a22 = sum(a * a for a in d[0]), sum(d[0][j] * d[1][j] for j in range(3)), sum(a * a for a in d[1])
                b1, b2 = sum(d[0][j] * q[j] for j in range(3)), sum(d[1][j] * q[j] for j in range(3))
                s, t = (b1 * a22 - b2 * a12) / det, (b2 * a11 - b1 * a12) / det
                if s >= 0 and t >= 0 and s + t <= 1:
                    dist = min(dist, sum((q[j] - s * d[0][j] - t * d[1][j]) ** 2 for j in range(3)) ** 0.5)
        garbage += dist > 12
    opacity = [None] * k
    if k == 1 and m[0][0] > 0:
        opacity = [v[0] / m[0][0]]
    elif k == 2:
        det = m[0][0] * m[1][1] - m[0][1] * m[1][0]
        if abs(det) > 1e-3 * max(1.0, m[0][0] * m[1][1]):
            opacity = [(v[0] * m[1][1] - v[1] * m[0][1]) / det, (v[1] * m[0][0] - v[0] * m[1][0]) / det]
        elif m[0][0] > 0:                 # the two look the same on these pixels: one figure for both together
            opacity = [v[0] / m[0][0] / 2] * 2
    far = 0
    if stray and union:
        xs = [i % width for i in union]; ys = [i // width for i in union]
        x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
        for i in stray:
            x, y = i % width, i // width
            far = max(far, max(x0 - x, x - x1, y0 - y, y - y1, 1))
    return {"opacity": opacity, "error": error / len(union) if union else 0.0, "stray": len(stray), "far": far,
            "garbage": garbage, "area": len(union), "union": union}

# ---------------------------------------------------------------- cases

class Case:
    def __init__(self, name, kind, scene, show, ideal, extra=None, start=6, end=None, settle=4, left=1, note="", env=None, tol=0.06, allow=0, limit=""):
        self.name, self.kind, self.show, self.ideal, self.extra = name, kind, show, ideal, extra
        self.first, self.count = scene
        self.start, self.end = start, end if end is not None else self.count - 13
        self.settle, self.left, self.note, self.env, self.tol = settle, left, note, env or {}, tol
        # allow: frames that cannot be right whatever the renderer does (the sprite turned round while hidden);
        # limit: the case is beyond what can be told from the sprite table, a failure is reported as "limit"
        self.allow, self.limit = allow, limit
        self.back = lambda t: []        # what the background runs hold (for a real case: the game told to hide the sprite)

STILL = (9100, 72)          # the camera stands still (the reference script: frames 9059..9550)
SCROLL = (9732, 48)         # the camera scrolls by itself, unevenly (1, 1, 0, 1, 1, 1, 0 ... pixels a frame)

def forced(t):
    """A camera that the test moves (STILL scene): speeds of 1, 2, 3 and 4 pixels a frame, stops and a turn."""
    y = 0
    for i in range(max(0, t) + 1):
        step = 0 if i < 10 else 1 if i < 20 else 2 if i < 28 else 4 if i < 34 else 0 if i < 38 else -3 if i < 48 else -1 if i < 54 else 0
        y += step
    return y

def camera_lines(t):
    return ["v 0 %d" % forced(t)]

def bounce(t, speed, low=40, high=190, start=60):
    x = start + max(0, t) * speed
    span = high - low
    x = (x - low) % (2 * span)
    return low + (x if x < span else 2 * span - x)

def make_cases():
    D, X, LB = disc(), cross(), label()
    cases = []
    add = cases.append
    on = lambda t, c: c.start <= t < c.end and (t - c.start) % 2 == 0
    on22 = lambda t, c: c.start <= t < c.end and (t - c.start) % 4 < 2
    live = lambda t, c: c.start <= t < c.end

    def flicker(name, where, scene=STILL, extra=None, pattern=on, g=D, note="", **kw):
        """A sprite alternating with nothing; `where(t, ctx)` is its true place in every frame, shown or not."""
        c = Case(name, "flicker", scene, None, None, extra, note=note, **kw)
        c.show = lambda t, ctx: [Spr(g, *where(t, ctx))] if pattern(t, c) else []
        c.ideal = lambda t, ctx: [[Spr(g, *where(t, ctx))], []] if live(t, c) else [[]]
        add(c)
        return c

    world = lambda x, y: (lambda t, ctx: (x - (ctx.cam(t)[0] - ctx.cam(0)[0]), y - (ctx.cam(t)[1] - ctx.cam(0)[1])))

    # (a) on/off every frame, standing still
    flicker("a  on/off, still", lambda t, ctx: (120, 100))
    # (b) the same for a sprite fixed in the world while the camera moves
    flicker("b1 on/off, world, camera scrolls (game)", world(120, 100), scene=SCROLL)
    flicker("b2 on/off, world, camera 1-4 px (forced)", world(120, 80), extra=camera_lines)
    flicker("b3 on/off, fixed on screen, camera forced", lambda t, ctx: (120, 100), extra=camera_lines)
    # (c) moving across the screen
    for speed in (1, 2, 4):
        flicker("c%d on/off, moving %d px/frame" % (speed, speed), lambda t, ctx, s=speed: (60 + t * s, 100), g=LB)
    flicker("c5 on/off, moving 1.5 px/frame, diagonal", lambda t, ctx: (60 + t * 3 // 2, 70 + t), g=LB)
    flicker("c6 on/off, walks in the world, camera forced", lambda t, ctx: (60 + t, 90 - (ctx.cam(t)[1] - ctx.cam(0)[1])), g=LB, extra=camera_lines)
    flicker("c7 on/off, walks, camera scrolls (game)", lambda t, ctx: (60 + t * 2, 100 - (ctx.cam(t)[1] - ctx.cam(0)[1])), g=LB, scene=SCROLL)
    flicker("c8 on/off, 3 px/frame, turns round twice", lambda t, ctx: (bounce(t, 3, 60, 120, 70), 100), g=LB, allow=6,
            note="turning while hidden cannot be foreseen: two frames a turn (three turns) are allowed to be off")
    flicker("c9 on/off, walks and bobs 1 px every 3 frames", lambda t, ctx: (60 + t, 100 + (t // 3) % 2), g=LB,
            limit="steps that follow no rule cannot be foreseen: in some hidden frames the copy is 1 pixel off")

    # (d) two different things alternating in one place
    def pair(name, ga, gb, pala, palb, where, **kw):
        c = Case(name, "flicker", STILL, None, None, settle=6, **kw)
        def show(t, ctx):
            if not live(t, c):
                return []
            return [Spr(ga, *where(t), pal=pala, tile=600)] if (t - c.start) % 2 == 0 else [Spr(gb, *where(t), pal=palb, tile=608)]
        c.show = show
        c.ideal = lambda t, ctx: [[Spr(ga, *where(t), pal=pala, tile=600)], [Spr(gb, *where(t), pal=palb, tile=608)]] if live(t, c) else [[]]
        add(c)
    pair("d1 two alternate, different palettes", D, D, 0, 1, lambda t: (120, 100))
    pair("d2 two alternate, same palette, other tiles", D, X, 0, 0, lambda t: (120, 100))
    pair("d3 two alternate, other palette and tiles", D, X, 0, 1, lambda t: (120, 100))
    pair("d4 two alternate (same palette), moving 2 px", D, X, 0, 0, lambda t: (bounce(t, 2), 100))

    # (e) two frames on, two off
    flicker("e1 2 on/2 off, still", lambda t, ctx: (120, 100), pattern=on22, settle=7, left=2)
    flicker("e2 2 on/2 off, moving 2 px/frame", lambda t, ctx: (bounce(t, 2), 100), pattern=on22, g=LB, settle=7, left=2)
    flicker("e3 2 on/2 off, world, camera forced", world(120, 80), pattern=on22, extra=camera_lines, settle=7, left=2)

    # (f) the sprite's tiles are handed out anew every frame, and reused by another sprite while it is hidden
    def realloc(name, where):
        c = Case(name, "flicker", STILL, None, None)
        other = lambda t: Spr(junk(), 60, 120, tile=600 + 4 * ((t - 1) % 3), slot=70)       # solid, takes the tiles just freed
        def show(t, ctx):
            out = [other(t)]
            if on(t, c):
                out.append(Spr(D, *where(t), tile=600 + 4 * (t % 3)))
            return out
        c.show = show
        c.ideal = lambda t, ctx: [[other(t), Spr(D, *where(t), tile=640)], [other(t)]] if live(t, c) else [[other(t)]]
        add(c)
    realloc("f1 on/off, tiles reallocated and reused, still", lambda t: (120, 100))
    realloc("f2 on/off, tiles reallocated, moving 2 px", lambda t: (bounce(t, 2), 100))

    # (h) under and over a solid sprite, two that alternate overlapping
    def layered(name, slot_solid, slot_flicker, prio=2):
        c = Case(name, "flicker", STILL, None, None)
        solid = lambda t: Spr(block(), 128, 104, slot=slot_solid, tile=620)
        c.show = lambda t, ctx: [solid(t)] + ([Spr(D, 120, 100, slot=slot_flicker, prio=prio)] if on(t, c) else [])
        c.ideal = lambda t, ctx: [[solid(t), Spr(D, 120, 100, slot=slot_flicker, prio=prio)], [solid(t)]] if live(t, c) else [[solid(t)]]
        add(c)
    layered("h1 on/off under a solid sprite (shadow)", 64, 80)
    layered("h2 on/off over a solid sprite", 80, 64)
    layered("h3 on/off, lower priority than the solid one", 80, 64, prio=3)
    def two(name, same_phase, dx):
        c = Case(name, "flicker", STILL, None, None, settle=6)
        s1 = lambda t: Spr(D, 120, 100, slot=64, tile=600)
        s2 = lambda t: Spr(D, 120 + dx, 100, slot=65, tile=600)
        def show(t, ctx):
            if not live(t, c):
                return []
            even = (t - c.start) % 2 == 0
            return ([s1(t), s2(t)] if even else []) if same_phase else [s1(t)] if even else [s2(t)]
        c.show = show
        c.ideal = lambda t, ctx: [[]] if not live(t, c) else [[s1(t), s2(t)], []] if same_phase else [[s1(t)], [s2(t)]]
        add(c)
    two("h4 two on/off overlapping, same frames", True, 6)
    two("h5 two alike 6 px apart, opposite frames", False, 6)
    two("h6 two alike 20 px apart, opposite frames", False, 20)

    # on/off while its animation runs (a hurt character): the picture changes every 8 frames
    c = Case("k  on/off and animated (new picture every 8)", "flicker", STILL, None, None)
    c.show = (lambda c: lambda t, ctx: [Spr(frame_of(t // 8), 120, 100, tile=600 + 4 * (t // 8 % 4))] if on(t, c) else [])(c)
    c.ideal = (lambda c: lambda t, ctx: [[Spr(frame_of(t // 8), 120, 100, tile=600 + 4 * (t // 8 % 4))], []] if live(t, c) else [[]])(c)
    add(c)
    # on/off that ends with the sprite staying (the character has recovered): it must turn solid at once
    c = Case("a2 on/off, then stays solid", "flicker", STILL, None, None, left=0)
    c.show = (lambda c: lambda t, ctx: [Spr(D, 120, 100)] if on(t, c) or t >= c.end else [])(c)
    c.ideal = (lambda c: lambda t, ctx: [[Spr(D, 120, 100)], []] if live(t, c) else [[Spr(D, 120, 100)]] if t >= c.end else [[]])(c)
    add(c)

    # a sprite that has been there all along starts to alternate (a character is hurt): recognised one turn later
    c = Case("a3 solid for a while, then on/off", "flicker", STILL, None, None, settle=4)
    c.show = (lambda c: lambda t, ctx: [Spr(D, 120, 100)] if t < c.start or on(t, c) else [])(c)
    c.ideal = (lambda c: lambda t, ctx: [[Spr(D, 120, 100)], []] if live(t, c) else [[Spr(D, 120, 100)]] if t < c.start else [[]])(c)
    add(c)
    # (i) during a brightness fade, across the edge of a window, and with the cartridge's real blending
    fade_lines = lambda t: ["m 4000050 ff", "m 4000054 %x" % max(0, min(16, abs(((t % 40) - 20)) - 2))]
    flicker("i1 on/off during a brightness fade", lambda t, ctx: (120, 100), extra=fade_lines)
    window_lines = lambda t: ["m 4000000 3f40", "m 4000040 %x" % (0 << 8 | 126), "m 4000044 %x" % (0 << 8 | 160), "m 4000048 3f3f", "m 400004a 2f"]
    flicker("i2 on/off across the edge of a window", lambda t, ctx: (bounce(t, 1, 100, 140, 110), 100), extra=window_lines)
    blend_lines = lambda t: ["m 4000050 3f00", "m 4000052 0a08"]
    c = flicker("i3 on/off, a blended (semi-transparent) sprite", lambda t, ctx: (120, 100), extra=blend_lines)
    c.show = (lambda c: lambda t, ctx: [Spr(D, 120, 100, mode=1)] if on(t, c) else [])(c)
    c.ideal = (lambda c: lambda t, ctx: [[Spr(D, 120, 100, mode=1)], []] if live(t, c) else [[]])(c)
    c.tol = 0.08

    # (g) controls: none of these alternates, none may be touched
    def control(name, show, note="", **kw):
        c = Case(name, "control", STILL, None, None, note=note, **kw)
        c.show = lambda t, ctx: show(t)
        c.ideal = lambda t, ctx: [show(t)]
        add(c)
    control("g1 appears and stays", lambda t: [Spr(D, 120, 100)] if t >= 20 else [])
    control("g2 disappears", lambda t: [Spr(D, 120, 100)] if t < 20 else [])
    control("g3 animation, new picture every 4 frames", lambda t: [Spr(frame_of(t // 4), 120, 100, tile=600 + 4 * (t // 4 % 5))])
    control("g4 animation every 3 frames, moving 1 px", lambda t: [Spr(frame_of(t // 3), bounce(t, 1), 100, tile=600 + 4 * (t % 5))])
    control("g5 slow blink, 15 on / 15 off", lambda t: [Spr(D, 120, 100)] if t % 30 < 15 else [])
    control("g6 solid, moving 6 px/frame", lambda t: [Spr(D, bounce(t, 6), 100)])
    control("g7 solid, moving 3 px/frame, thin", lambda t: [Spr(LB, bounce(t, 3), 100)])
    control("g8 status display sprite, still", lambda t: [Spr(D, 200, 20, prio=0)])
    control("g9 blink 3 on / 1 off", lambda t: [Spr(D, 120, 100)] if t % 4 < 3 else [])
    control("g10 row of alike sprites moving 12 px/frame", lambda t: [Spr(D, 20 + (t * 12 + i * 24) % 192, 100, slot=64 + i) for i in range(4)],
            limit="each is exactly where the one ahead of it was two frames ago, with nothing there in between: "
                  "the sprite table is the same as that of sprites alternating in place")
    control("g11 dithered sprite moving 1 px/frame", lambda t: [Spr(checker(), bounce(t, 1), 90, tile=640)])

    # the per-pixel smoothing (frame.c): blinking drawn with a background must be steadied, movement not blurred
    def bg_block(bg, sx, sy, cam, tile, bank, w=4, h=4):
        """Screen entries that put `tile` in a w x h block of background `bg` at screen (sx, sy)."""
        base = 0x06000000 + ((0x1c, 0x1d, 0x1e, 0x1f)[bg]) * 0x800       # the field's four maps (see the game's BGnCNT)
        sy += cam if bg else 0
        return ["m %x %x" % (base + ((((sy >> 3) + y) & 31) * 32 + (((sx >> 3) + x) & 31)) * 2, tile | bank << 12) for y in range(h) for x in range(w)]
    def bg_tile(bg, tile, g):
        return tile_lines(g, tile, 0x06000000 + (0x8000 if bg == 0 else 0))
    bg_colours = ["m %x %x" % (0x05000000 + (15 * 16 + i) * 2, c) for i, c in ((1, 0x7C1F), (6, 0x03E0))]
    def blink_bg(name, extra=None):
        c = Case(name, "flicker", STILL, None, None, extra)
        there = lambda t: [Raw(bg_colours + bg_tile(0, 700, block(8, 8, 1, 6)) + bg_block(0, 112, 96, 0, 700, 15))]
        c.show = lambda t, ctx: there(t) if on(t, c) else []
        c.ideal = lambda t, ctx: [there(t), []] if live(t, c) else [[]]
        add(c)
    blink_bg("m1 background tiles blink (a menu option), still")
    blink_bg("m2 the same while the camera moves under it", camera_lines)
    cases[-1].limit = ("the per-pixel smoothing sees only finished colours, and what is under the blinking tiles changes every frame "
                       "(the decompilation shows no background blinking at this rate in the game)")
    c = Case("m3 dithered background scrolling 1 px/frame", "control", STILL, None, None, extra=lambda t: ["v 0 %d" % max(0, t)])
    c.show = lambda t, ctx: [Raw(bg_colours + bg_tile(2, 1023, checker(8)) + bg_block(2, 104, 80, ctx.cam(0)[1], 1023, 15))]
    c.ideal = lambda t, ctx: [c.show(t, ctx)]
    add(c)
    # a sprite whose colours are swapped every frame (a flash drawn with the palette)
    def flash(name, where, **kw):
        c = Case(name, "flicker", STILL, None, None, note="colours alternate, the sprite stays", **kw)
        def colours(ctx, which):
            return Raw(["m %x %x" % (0x05000200 + (ctx.banks[0] * 16 + i) * 2, PALETTES[which][i]) for i in (1, 2, 3)])
        c.show = lambda t, ctx: [Spr(D, *where(t)), colours(ctx, (t - c.start) % 2 if live(t, c) else 0)]
        c.ideal = lambda t, ctx: [[Spr(D, *where(t)), colours(ctx, 0)], [Spr(D, *where(t)), colours(ctx, 1)]] if live(t, c) else [[Spr(D, *where(t)), colours(ctx, 0)]]
        add(c)
    flash("n1 colours of a sprite alternate, still", lambda t: (120, 100))
    flash("n2 colours of a sprite alternate, moving 2 px", lambda t: (bounce(t, 2), 100),
          limit="not done by hiding a sprite, so only the per-pixel smoothing can steady it, and that leaves moving things alone "
                "(the decompilation shows no such effect in the game)")

    return cases

# ---------------------------------------------------------------- the game's own alternating sprite

# The local player's record has a flag that makes the game draw the player's body on odd frames only (bit 0x40000
# of the flags at 0x03003D10; RunPlayerVanishAnimationAndTemporaryVisibilityEffect sets it for a second). With it
# set, the game itself alternates real sprites: several entries, tiles handed out anew every frame, the walk
# animation running. The game plays differently with the flag than without, so there is no run to take a solid
# body or a clean background from. Instead the same run is drawn twice, with the renderer's logic on (T) and off
# (C, the console's pictures: body in one frame, plain background in the next), in scenes where the camera
# stands still, and the body is taken from C:
#   in a frame with the body: opacity of T between C of the frame before (background) and C of this frame (0.5 wanted)
#   in a frame without it:    how much of the body T still shows, against half of the body of the frame before
#                             (1.0 wanted, 0 is a blink), and pixels changed more than 12 px away from where it was
REAL = [("r1 the game's own: player body on odd frames", 9100, 72), ("r2 the same, later (walking)", 11300, 72), ("r3 the same, later", 12100, 72)]

def run_real(args, index, name, first, count):
    script = ["g 3003d12 4"]
    T = run_game(args, "real%d_t" % index, script, first, count)
    C = run_game(args, "real%d_c" % index, script, first, count, {"SS_FLICKER": "0", "SS_SMOOTH": "0"})
    width = T[0][6]
    out = []
    for which, column in (("ppu", 4), ("shown", 5)):
        shown, hidden, far_pixels, body, box = [], [], 0, None, None
        for t in range(1, count - 1):
            c0, c1, c2 = (memoryview(C[u][4]).cast("I") for u in (t - 1, t, t + 1))
            P = memoryview(T[t][column]).cast("I")
            # the body: the sprite table entries of this frame that the frame before did not have
            def entries(u):
                out = set()
                for i in range(128):
                    a0, a1, a2 = struct.unpack_from("<3H", C[u][3], i * 8)
                    if a0 & 255 < 160:
                        out.add((a1 & 511, a0 & 255, a0 >> 14, a1 >> 14, a2 >> 12))
                return out
            new = entries(t) - entries(t - 1)
            boxes = [(x, y, x + SIZES[shape][size][0], y + SIZES[shape][size][1]) for x, y, shape, size, bank in new]
            within = lambda i: any(x0 <= i % width < x1 and y0 <= i // width < y1 for x0, y0, x1, y1 in boxes)
            place = [i for i in differing(C[t][4], C[t - 1][4], width) if c0[i] == c2[i] and within(i)]
            if len(entries(t)) > len(entries(t - 1)) and place:     # the body is in this frame
                num = den = 0
                for i in place:
                    b, f, p = rgb(c0[i]), rgb(c1[i]), rgb(P[i])
                    num += sum((p[j] - b[j]) * (f[j] - b[j]) for j in range(3))
                    den += sum((f[j] - b[j]) ** 2 for j in range(3))
                shown.append(num / den)
                body = sum(sum(abs(a - b) for a, b in zip(rgb(c1[i]), rgb(c0[i]))) for i in place)
                xs = [i % width for i in place]; ys = [i // width for i in place]
                box = (min(xs) - 12, max(xs) + 12, min(ys) - 12, max(ys) + 12)
            elif body:                                          # it is hidden: C is the plain background
                changed = differing(T[t][column], C[t][4], width)
                near = [i for i in changed if box[0] <= i % width <= box[1] and box[2] <= i // width <= box[3]]
                hidden.append(sum(sum(abs(a - b) for a, b in zip(rgb(P[i]), rgb(c1[i]))) for i in near) / (body / 2))
                far_pixels = max(far_pixels, len(changed) - len(near))
        out.append((which, shown, hidden, far_pixels))
    return name, out

# ---------------------------------------------------------------- one case

def run_case(args, case, index):
    tag = "case%02d" % index
    off = {"SS_FLICKER": "0", "SS_SMOOTH": "0"}
    ctx = Ctx()
    ctx.banks = args.banks
    ctx.cam = lambda t: (0, 0)
    back = run_game(args, tag + "_b", script_for(ctx, case, case.back), case.first, case.count, off)
    cams = [f[2] for f in back]
    ctx.cam = lambda t: cams[max(0, min(len(cams) - 1, t))]
    width = back[0][6]
    same = run_game(args, tag + "_s", script_for(ctx, case, case.back), case.first, case.count, case.env)
    test = run_game(args, tag + "_t", script_for(ctx, case, lambda t: case.show(t, ctx)), case.first, case.count, case.env)
    sets = max(len(case.ideal(t, ctx)) for t in range(case.count))
    fulls = []
    for k in range(sets):
        pick = lambda t, k=k: (case.ideal(t, ctx) + [[]] * sets)[k]
        if all(not pick(t) for t in range(-40, case.count)):
            fulls.append(None)
        else:
            fulls.append(run_game(args, tag + "_f%d" % k, script_for(ctx, case, pick), case.first, case.count, off))
    plain = None
    if case.kind == "control":
        plain = run_game(args, tag + "_o", script_for(ctx, case, lambda t: case.show(t, ctx)), case.first, case.count, dict(case.env, SS_FLICKER="0"))
    result = {"case": case, "width": width}
    for which, column in (("ppu", 4), ("shown", 5)):
        rows = []
        for t in range(case.count):
            ideal = case.ideal(t, ctx)
            things = [fulls[k][t][4] for k in range(len(ideal)) if ideal[k]]
            m = measure(test[t][column], back[t][4], things, 1.0 / len(ideal), width, same[t][column])
            m["target"] = 1.0 / len(ideal)
            m["things"] = len(things)
            rows.append(m)
        result[which] = rows
    if plain:
        result["vs_off"] = sum(len(differing(test[t][4], plain[t][4], width)) for t in range(case.count))
        result["vs_off_shown"] = sum(len(differing(test[t][5], plain[t][5], width)) for t in range(case.count))
    if args.png:
        sheet(args, case, index, test, back, fulls, ctx, width, result)
    return result

def bad_frame(case, m):
    if m["stray"] or m["garbage"]:
        return True
    for o in m["opacity"]:
        if o is None:
            if m["area"]:               # (no area: fully faded or masked, there is nothing to judge)
                return True
        elif abs(o - m["target"]) > case.tol:
            return True
    return False

def summarise(case, rows):
    """The figures of one table line from the per-frame measures."""
    start, end = case.start, case.end
    if case.kind == "control":
        start, end, settle = 0, case.count, 0
        steady = rows
    else:
        settle = None
        for t in range(start, end):                 # frames until every later frame of the pattern is right
            if all(not bad_frame(case, rows[u]) for u in range(t, end)):
                settle = t - start
                break
        steady = rows[start + case.settle:end]
    ops = [o for m in steady for o in m["opacity"] if o is not None]
    change = 0.0
    for a, b in zip(steady, steady[1:]):
        for x, y in zip(a["opacity"], b["opacity"]):
            if x is not None and y is not None:
                change = max(change, abs(x - y))
    left = 0
    if case.kind != "control":
        tail = rows[end:]
        after = [u for u, m in enumerate(tail) if bad_frame(case, m)]
        left = (after[-1] + 1) if after else 0
    bad = sum(bad_frame(case, m) for m in steady)
    worst = max(steady, key=lambda m: m["stray"]) if steady else None
    ok = bad <= case.allow and left <= case.left and (case.kind == "control" or case.allow or (settle is not None and settle <= case.settle))
    return {"mean": sum(ops) / len(ops) if ops else None, "low": min(ops) if ops else None, "high": max(ops) if ops else None,
            "change": change, "error": sum(m["error"] for m in steady) / max(1, len(steady)), "stray": worst["stray"] if worst else 0,
            "far": max((m["far"] for m in steady), default=0), "garbage": max((m["garbage"] for m in steady), default=0),
            "bad": bad, "frames": len(steady), "settle": settle, "left": left, "ok": ok}

def line_of(name, which, s, extra=""):
    op = "   -" if s["mean"] is None else "%.2f [%.2f %.2f]" % (s["mean"], s["low"], s["high"])
    return "%-46s %-5s %-16s %5.2f %6.1f %5d@%-3d %5d %3d/%-3d %4s %4d  %-4s %s" % (
        name, which, op, s["change"], s["error"], s["stray"], s["far"], s["garbage"], s["bad"], s["frames"],
        "-" if s["settle"] is None else s["settle"], s["left"], s.get("verdict") or ("ok" if s["ok"] else "FAIL"), extra)

HEADER = "%-46s %-5s %-16s %5s %6s %9s %5s %7s %4s %4s  %s" % ("case", "pic", "opacity [range]", "step", "error", "stray@px", "garb", "bad/of", "setl", "left", "verdict")

# ---------------------------------------------------------------- pictures to look at

def png(path, width, height, rows):
    chunk = lambda tag, body: struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)
    data = b"".join(b"\0" + bytes(row) for row in rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(data)) + chunk(b"IEND", b""))

def sheet(args, case, index, test, back, fulls, ctx, width, result):
    """Eight frames in a row around the test sprites, enlarged: the renderer's picture, the shown one, the ideal."""
    t0 = min(case.count - 8, case.start + 12)
    union = set()
    for t in range(t0, t0 + 8):
        union |= set(result["ppu"][t]["union"])
    if not union:
        return
    xs = [i % width for i in union]; ys = [i // width for i in union]
    x0, y0 = max(0, min(xs) - 10), max(0, min(ys) - 10)
    w, h = min(64, max(xs) + 11 - x0, width - x0), min(48, max(ys) + 11 - y0, H - y0)
    scale = 3
    rows = []
    for column in ("ppu", "shown", "ideal"):
        for y in range(h):
            row = []
            for t in range(t0, t0 + 8):
                ideal = case.ideal(t, ctx)
                for x in range(w):
                    i = (y0 + y) * width + x0 + x
                    if column == "ideal":
                        px = [0, 0, 0]
                        for k in range(len(ideal)):
                            src = fulls[k][t][4] if ideal[k] else back[t][4]
                            v = struct.unpack_from("<I", src, i * 4)[0]
                            for j in range(3):
                                px[j] += rgb(v)[j] / len(ideal)
                        px = [int(c) for c in px]
                    else:
                        px = list(rgb(struct.unpack_from("<I", test[t][4 if column == "ppu" else 5], i * 4)[0]))
                    row += px * scale
                row += [255, 255, 255] * 2
            rows += [row] * scale
        rows += [[255] * len(rows[0])] * 4
    png(os.path.join(args.png, "case%02d.png" % index), len(rows[0]) // 3, len(rows), rows)

# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom")
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "dev32", "ss2", "ss2_dev32.exe"))
    ap.add_argument("--input", default=os.path.join(ROOT, "build", "stats", "input_play.txt"))
    ap.add_argument("--work", default=os.path.join(ROOT, "build", "bench"))
    ap.add_argument("--only", default="", help="run the cases whose name contains this text")
    ap.add_argument("--view", type=int, default=0, help="picture width (427: the wide view)")
    ap.add_argument("--png", default="", help="write a strip of enlarged frames per case into this folder")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--frames", action="store_true", help="print the measures of every frame")
    ap.add_argument("--field", default="", help="A-B: compare the game as it is with the sprite logic on and off over frames A..B-1")
    ap.add_argument("--time", action="store_true", help="also time the renderer over 4000 frames of the field, logic on and off")
    args = ap.parse_args()
    args.exe, args.rom, args.input, args.work = (os.path.abspath(p) for p in (args.exe, args.rom, args.input, args.work))
    os.makedirs(args.work, exist_ok=True)
    if args.png:
        args.png = os.path.abspath(args.png)
        os.makedirs(args.png, exist_ok=True)

    # sprite palettes and tiles the scene does not use, from the sprite tables of the frames the cases cover
    used = set()
    tiles = set()
    for first, count in (STILL, SCROLL):
        for f in run_game(args, "probe", [], first, count, {"SS_FLICKER": "0", "SS_SMOOTH": "0"}):
            for i in range(128):
                a0, a1, a2 = struct.unpack_from("<3H", f[3], i * 8)
                if (a0 & 0x300) == 0x200 or (a0 & 255) >= 160:
                    continue
                used.add(a2 >> 12)
                tiles.add(a2 & 0x3FF)
    args.banks = [b for b in (13, 12, 11, 10, 9, 8, 15, 14) if b not in used][:2]
    if len(args.banks) < 2 or any(t >= 560 for t in tiles):
        raise SystemExit("the scene uses the sprite palettes or tiles the bench wants (palettes %s, highest tile %d)" % (sorted(used), max(tiles)))

    cases = [c for c in make_cases() if args.only.lower() in c.name.lower()]
    if args.only in ("real", "none"):
        cases = []
    with ThreadPoolExecutor(args.jobs) as pool:
        results = list(pool.map(lambda ic: run_case(args, ic[1], ic[0]), enumerate(cases)))
    print("picture width %d, exe %s" % (results[0]["width"] if results else args.view or 240, os.path.relpath(args.exe, ROOT)))
    print(HEADER)
    failed = 0
    limits = {}
    for r in results:
        case = r["case"]
        for which in ("ppu", "shown"):
            s = summarise(case, r[which])
            extra = ""
            if case.kind == "control":
                differs = r["vs_off" if which == "ppu" else "vs_off_shown"]
                extra = "vs off: %s" % ("identical" if not differs else "%d pixels differ" % differs)
                if which == "ppu" and differs:
                    s["ok"] = False
            if not s["ok"] and case.limit:
                s["verdict"] = "limit"
                limits[case.name] = case.limit
            elif which == "shown" or case.kind == "control":
                failed += not s["ok"]
            print(line_of(case.name if which == "ppu" else "", which, s, extra))
            if args.frames:                 # every frame: opacity, strays and their distance, garbage
                for t, m in enumerate(r[which]):
                    print("      %3d %s %s stray %d@%d garbage %d of %d%s" % (t, "*" if case.start <= t < case.end else " ",
                          " ".join("-" if o is None else "%.2f" % o for o in m["opacity"]), m["stray"], m["far"], m["garbage"], m["area"],
                          "  <-- bad" if bad_frame(case, m) else ""))
    print("%d case(s), %d line(s) FAIL (for alternating things the shown picture is what counts; for controls both)" % (len(results), failed))
    for name, why in limits.items():
        print("limit: %s: %s" % (name.split()[0], why))

    if args.view and args.view != 240:
        print("(the game's own alternating sprite is measured at the console's width only: its place is read from the sprite table)")
    elif not args.only or args.only == "real":
        print()
        print("%-46s %-5s %-28s %-28s %s" % ("the game's own alternating sprite", "pic", "with body: opacity [range]", "hidden: body shown [range]", "changed far away"))
        with ThreadPoolExecutor(args.jobs) as pool:
            for name, lines in pool.map(lambda r: run_real(args, r[0], *r[1]), enumerate(REAL)):
                for which, shown, hidden, far in lines:
                    f = lambda v: "-" if not v else "%.2f [%.2f %.2f] (%d)" % (sum(v) / len(v), min(v), max(v), len(v))
                    print("%-46s %-5s %-28s %-28s %d" % (name if which == "ppu" else "", which, f(shown), f(hidden), far))
                    if args.frames:
                        print("      with body: " + " ".join("%.2f" % v for v in shown))
                        print("      hidden:    " + " ".join("%.2f" % v for v in hidden))
    if args.field:
        # The game as it is, over a long stretch of the reference run: wherever the sprite logic changes a pixel,
        # the game itself must be alternating something there. Every such frame is listed to be looked at.
        first, last = (int(v) for v in args.field.split("-"))
        chunks = [(f, min(400, last - f)) for f in range(first, last, 400)]
        def chunk(c):
            on = run_game(args, "field%d_on" % c[0], [], c[0], c[1], {"SS_SMOOTH": "0"})
            off = run_game(args, "field%d_off" % c[0], [], c[0], c[1], {"SS_FLICKER": "0", "SS_SMOOTH": "0"})
            out = []
            for a, b in zip(on, off):
                d = differing(a[4], b[4], a[6])
                if d:
                    out.append((a[0], len(d), min(i % a[6] for i in d), min(i // a[6] for i in d), max(i % a[6] for i in d), max(i // a[6] for i in d)))
            return out
        with ThreadPoolExecutor(max(1, args.jobs // 2)) as pool:
            changed = [row for rows in pool.map(chunk, chunks) for row in rows]
        print()
        print("the game as it is, frames %d..%d: the sprite logic changes the picture in %d frame(s)" % (first, last - 1, len(changed)))
        for frame, pixels, x0, y0, x1, y1 in changed[:60]:
            print("    frame %d: %d pixels in (%d,%d)..(%d,%d)" % (frame, pixels, x0, y0, x1, y1))
    if args.time:
        for name, env in (("sprite logic on", {}), ("sprite logic off", {"SS_FLICKER": "0"})):
            e = dict(os.environ)
            e.update({"SS_SKIP": "0", "SS_FRAMES": "13000", "SS_INPUT_FILE": args.input, "SS_BENCH": "9040:0"})
            if args.view:
                e["SS_VIEW"] = str(args.view)
            e.update(env)
            best = None
            for _ in range(3):
                text = subprocess.run([args.exe, args.rom], cwd=args.work, env=e, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE).stderr.decode(errors="replace")
                for line in text.splitlines():
                    if line.startswith("bench:"):
                        us = float(line.split("drawn,")[1].split()[0])
                        best = us if best is None else min(best, us)
            print("renderer, %s: %s microseconds a frame (best of 3, frames 9000..13000)" % (name, best))
    return 1 if failed else 0

if __name__ == "__main__":
    sys.exit(main())
