#!/usr/bin/env python3
"""Informe de una carpeta del diagnostico GA36 (shiningsourcega36/diagnostico-NNN).

  ga36_diag_report.py DIR [--reference REF] [--json OUT]

DIR       la carpeta tal como salio de la tarjeta (status.txt, summary.txt, system.txt, census/, NN-caso/).
REF       hashes de referencia (lineas "frame N  hash XXXXXXXX", p. ej. tools/tests/ga36_hash_reference.txt):
          cada caso hash-* se compara con ella y se da el primer fotograma distinto.
OUT       todo lo calculado, en JSON.

Que hace: comprueba en status.txt que la ejecucion este completa (START, END census, BEGIN/END por caso, SKIP,
TIMEOUT, INTERRUPTED, END 0 final); por caso lee env.txt, exit.txt, time.txt, stderr.txt, monitor.txt y
frames.csv (SS_FRAMELOG: frame,interval_us,logic_us,sound_us,net_us,blank_us,extras_us,ppu_us,finish_us,
present_us,presented,late,view) y da fotogramas, FPS (1e6 / intervalo medio), mediana, P95, P99, media por etapa,
imagenes omitidas y fotogramas tarde; la capacidad de los casos cap-* (los cap-show van con SS_TEST_SLOW=17: se
restan 17 ms); el desglose de costes que sale de comparar casos; los hechos del sistema (pantalla, CPU, sonido,
entradas, codigos de teclas y un mapa de botones propuesto); y una lista ordenada de "que mejorar" con su
evidencia. Una ejecucion incompleta nunca se llama completa, pero se analiza lo que haya. Si todos los casos
fallaron enseguida con el mismo error se dice en la primera linea.

Solo biblioteca estandar. Texto plano, etiquetas en castellano.
"""
import argparse, json, os, re, statistics, sys
from pathlib import Path

FRAMELOG_COLUMNS = ["frame", "interval_us", "logic_us", "sound_us", "net_us", "blank_us", "extras_us", "ppu_us", "finish_us",
                    "present_us", "presented", "late", "view"]
STAGES = ["logic_us", "sound_us", "net_us", "blank_us", "extras_us", "ppu_us", "finish_us", "present_us"]
STAGE_NAMES = {"logic_us": "logica del juego", "sound_us": "sonido", "net_us": "enlace", "blank_us": "vblank/lineas",
               "extras_us": "trucos/logros", "ppu_us": "dibujo (ppu)", "finish_us": "acabado", "present_us": "presentar+espera"}
CONSOLE_FPS = 59.7275
FRAME_MS = 1000.0 / CONSOLE_FPS
SLOW_DEFAULT_MS = 17
QUICK_FAIL_S = 3.0

EXIT_MEANING = {
    0: "correcto", 1: "error general", 2: "uso incorrecto", 120: "no se pudo leer ga36-env.sh",
    126: "el programa no es ejecutable", 127: "programa o cargador no encontrado (exec fallo)",
    132: "instruccion ilegal (SIGILL)", 134: "abortado (SIGABRT)", 135: "error de bus (SIGBUS)",
    136: "excepcion de coma flotante (SIGFPE)", 137: "matado (SIGKILL; vigilante o memoria)",
    139: "violacion de segmento (SIGSEGV)", 143: "terminado (SIGTERM; vigilante)",
}

# Linux input codes (input-event-codes.h) that matter on handhelds
KEY_NAMES = {
    103: "KEY_UP", 108: "KEY_DOWN", 105: "KEY_LEFT", 106: "KEY_RIGHT", 28: "KEY_ENTER", 1: "KEY_ESC", 57: "KEY_SPACE",
    114: "KEY_VOLUMEDOWN", 115: "KEY_VOLUMEUP", 116: "KEY_POWER", 139: "KEY_MENU", 158: "KEY_BACK", 172: "KEY_HOMEPAGE",
    304: "BTN_SOUTH", 305: "BTN_EAST", 306: "BTN_C", 307: "BTN_NORTH", 308: "BTN_WEST", 309: "BTN_Z", 310: "BTN_TL",
    311: "BTN_TR", 312: "BTN_TL2", 313: "BTN_TR2", 314: "BTN_SELECT", 315: "BTN_START", 316: "BTN_MODE",
    317: "BTN_THUMBL", 318: "BTN_THUMBR", 544: "BTN_DPAD_UP", 545: "BTN_DPAD_DOWN", 546: "BTN_DPAD_LEFT",
    547: "BTN_DPAD_RIGHT",
}
ABS_NAMES = {0: "ABS_X", 1: "ABS_Y", 2: "ABS_Z", 3: "ABS_RX", 4: "ABS_RY", 5: "ABS_RZ", 16: "ABS_HAT0X", 17: "ABS_HAT0Y"}
# the runtime's default raw evdev table (runtime/plat/fbdev_linux.c, key_map): code -> GBA button
RUNTIME_EVDEV_MAP = {
    103: "up", 108: "down", 105: "left", 106: "right", 544: "up", 545: "down", 546: "left", 547: "right",
    305: "a", 304: "b", 310: "l", 311: "r", 312: "l", 313: "r", 314: "select", 315: "start", 316: "menu",
    45: "a", 44: "b", 46: "l", 47: "r", 28: "start", 54: "select", 1: "menu", 29: "a", 56: "b", 57: "select", 104: "l", 109: "r",
}
# what this port would propose for a pad that has these codes (A right, B bottom, as on the GBA)
PROPOSED = [(305, "a"), (304, "b"), (308, "b"), (307, "a"), (310, "l"), (311, "r"), (312, "l"), (313, "r"), (314, "select"),
            (315, "start"), (316, "menu"), (544, "up"), (545, "down"), (546, "left"), (547, "right"), (103, "up"),
            (108, "down"), (105, "left"), (106, "right")]


# ---------------------------------------------------------------- reading

def read_text(path):
    try:
        return Path(path).read_bytes().decode("utf-8", "replace").replace("\r\n", "\n")
    except OSError:
        return None


def parse_status(text):
    """status.txt -> dict with the events and the completeness verdict."""
    result = {"start": None, "census_end": False, "begun": [], "ended": {}, "skipped": {}, "timeouts": [], "interrupted": False,
              "final_end": False, "missing": [], "notes": [], "clock": None, "headless": None, "work": None, "game": None,
              "lines": 0, "problems": []}
    if text is None:
        result["problems"].append("no hay status.txt")
        result["complete"] = False
        return result
    lines = [l for l in text.split("\n") if l.strip()]
    result["lines"] = len(lines)
    for index, line in enumerate(lines):
        words = line.split()
        tag = words[0]
        if tag == "START":
            result["start"] = line
        elif tag == "GAME" and len(words) > 1:
            result["game"] = line[5:]
        elif tag == "HEADLESS" and len(words) > 1:
            result["headless"] = words[1]
        elif tag == "WORK" and len(words) > 1:
            result["work"] = words[1]
        elif tag == "MISSING":
            result["missing"].append(" ".join(words[1:]))
        elif tag == "CLOCK" and len(words) > 1:
            result["clock"] = words[1]
        elif tag == "BEGIN" and len(words) > 1:
            result["begun"].append(words[1])
        elif tag == "SKIP" and len(words) > 1:
            result["skipped"][words[1]] = " ".join(words[2:])
        elif tag == "TIMEOUT" and len(words) > 1:
            result["timeouts"].append(words[1])
        elif tag == "INTERRUPTED":
            result["interrupted"] = True
        elif tag == "END":
            if len(words) >= 2 and words[1] == "census":
                result["census_end"] = True
            elif len(words) == 2 and words[1] == "0" and index == len(lines) - 1:
                result["final_end"] = True
            elif len(words) >= 3:
                result["ended"][words[1]] = {"code": _int(words[2]), "seconds": _float(words[3]) if len(words) > 3 else None}
            else:
                result["notes"].append(line)
        else:
            result["notes"].append(line)
    if not result["start"]:
        result["problems"].append("falta la linea START")
    if not result["census_end"]:
        result["problems"].append("el censo no termino (falta END census)")
    for name in result["begun"]:
        if name not in result["ended"]:
            result["problems"].append("el caso %s empezo y no termino (cortado: apagado, bateria o cuelgue)" % name)
    if result["interrupted"]:
        result["problems"].append("la ejecucion fue interrumpida (INTERRUPTED)")
    if not result["final_end"]:
        result["problems"].append("falta el END 0 final: la ejecucion no llego al final")
    result["complete"] = not result["problems"]
    return result


def _int(text):
    try:
        return int(text)
    except (TypeError, ValueError):
        return None


def _float(text):
    try:
        return float(text)
    except (TypeError, ValueError):
        return None


def parse_env(text):
    env, extra = {}, {}
    for line in (text or "").split("\n"):
        line = line.strip()
        if not line:
            continue
        if line.startswith("kind="):
            for part in line.split():
                if "=" in part:
                    k, v = part.split("=", 1)
                    extra[k] = v
            continue
        if "=" in line:
            k, v = line.split("=", 1)
            env[k] = v
    return env, extra


def parse_framelog(text):
    """frames.csv -> {clock, rows (list of dicts), end_note}."""
    out = {"clock": None, "rows": [], "end": None, "bad_lines": 0}
    if text is None:
        return None
    for line in text.split("\n"):
        line = line.strip()
        if not line:
            continue
        if line.startswith("#"):
            m = re.match(r"# SS_FRAMELOG clock (\S+)", line)
            if m:
                out["clock"] = m.group(1)
            m = re.match(r"# end frame (\d+), (\d+) ms", line)
            if m:
                out["end"] = {"frame": int(m.group(1)), "ms": int(m.group(2))}
            continue
        if line.startswith("frame,"):
            continue
        parts = line.split(",")
        if len(parts) != len(FRAMELOG_COLUMNS):
            out["bad_lines"] += 1
            continue
        try:
            out["rows"].append(dict(zip(FRAMELOG_COLUMNS, (int(p) for p in parts))))
        except ValueError:
            out["bad_lines"] += 1
    return out


def percentile(sorted_values, fraction):
    if not sorted_values:
        return None
    k = max(0, min(len(sorted_values) - 1, int(round(fraction * (len(sorted_values) - 1)))))
    return sorted_values[k]


def frame_stats(log, slow_ms=0):
    rows = log["rows"] if log else []
    if not rows:
        return None
    intervals = sorted(r["interval_us"] for r in rows)
    mean_us = sum(intervals) / len(intervals)
    stats = {
        "frames": len(rows), "first": rows[0]["frame"], "last": rows[-1]["frame"],
        "mean_ms": mean_us / 1000.0, "fps": 1e6 / mean_us if mean_us > 0 else None,
        "median_ms": statistics.median(intervals) / 1000.0, "p95_ms": percentile(intervals, 0.95) / 1000.0,
        "p99_ms": percentile(intervals, 0.99) / 1000.0, "max_ms": intervals[-1] / 1000.0,
        "stages_ms": {s: sum(r[s] for r in rows) / len(rows) / 1000.0 for s in STAGES},
        "not_presented": sum(1 for r in rows if not r["presented"]), "late": sum(1 for r in rows if r["late"]),
        "views": sorted({r["view"] for r in rows}), "clock": log.get("clock"),
        "over_frame": sum(1 for v in intervals if v > FRAME_MS * 1000 * 1.5),
    }
    work_ms = sum(stats["stages_ms"][s] for s in STAGES if s != "present_us")
    stats["work_ms"] = work_ms
    if slow_ms:
        real = stats["mean_ms"] - slow_ms
        stats["slow_ms"] = slow_ms
        stats["capacity_fps"] = 1000.0 / real if real > 0 else None
    else:
        stats["capacity_fps"] = stats["fps"]
    return stats


HASH_RE = re.compile(r"frame\s+(\d+)\s+hash\s+([0-9A-Fa-f]{8})")


def parse_hashes(text):
    hashes = {}
    for m in HASH_RE.finditer(text or ""):
        hashes[int(m.group(1))] = m.group(2).upper()
    return hashes


def compare_hashes(mine, reference):
    common = sorted(set(mine) & set(reference))
    first = next((f for f in common if mine[f] != reference[f]), None)
    return {"compared": len(common), "first_diff": first, "equal": first is None and bool(common),
            "mine_at": mine.get(first) if first is not None else None, "ref_at": reference.get(first) if first is not None else None,
            "last_compared": common[-1] if common else None,
            "missing_frames": len([f for f in reference if f not in mine and f <= max(mine or [0])])}


def parse_stderr(text):
    info = {"prof": [], "stats": [], "picture": None, "errors": [], "save": None, "warp": None, "shop": None, "fbdev": [],
            "evdev": [], "audio": None, "hash_count": 0}
    for line in (text or "").split("\n"):
        s = line.strip()
        if not s:
            continue
        if s.startswith("prof:"):
            info["prof"].append(s)
        elif s.startswith("stats:"):
            info["stats"].append(s)
        elif s.startswith("fbdev:"):
            info["fbdev"].append(s)
        elif s.startswith("evdev:"):
            info["evdev"].append(s)
        elif s.startswith("audio:"):
            info["audio"] = s
        elif s.startswith("picture:"):
            m = re.match(r"picture: (\d+) frames (\d+) wide, hash ([0-9A-F]{8})", s)
            info["picture"] = {"frames": int(m.group(1)), "width": int(m.group(2)), "hash": m.group(3)} if m else s
        elif s.startswith("save test:"):
            m = re.search(r"returned (\d+)", s)
            info["save"] = int(m.group(1)) if m else s
        elif s.startswith("warp test:"):
            info["warp"] = s
        elif s.startswith("shop test:"):
            info["shop"] = s
        elif HASH_RE.match(s):
            info["hash_count"] += 1
        elif re.search(r"not found|error|cannot|failed|fault|illegal|denied|no such|segmentation|abort", s, re.I):
            info["errors"].append(s)
    m = [l for l in info["stats"] if "game frames in" in l]
    if m:
        g = re.search(r"(\d+) game frames in ([\d.]+) s: ([\d.]+) a second.*?(\d+) of them without a picture", m[-1])
        if g:
            info["stats_frames"] = {"frames": int(g.group(1)), "seconds": float(g.group(2)), "fps": float(g.group(3)),
                                    "no_picture": int(g.group(4))}
    return info


def parse_monitor(text):
    samples = []
    for line in (text or "").split("\n"):
        parts = line.split()
        if len(parts) < 3:
            continue
        try:
            samples.append({"t": float(parts[0]), "rss_kb": int(parts[1]), "temp": int(parts[2]) if parts[2] != "-" else None,
                            "freqs": [int(p) for p in parts[3:7] if p.isdigit()]})
        except ValueError:
            continue
    if not samples:
        return None
    temps = [s["temp"] for s in samples if s["temp"] is not None]
    freqs = [f for s in samples for f in s["freqs"]]
    return {"samples": len(samples), "rss_max_kb": max(s["rss_kb"] for s in samples),
            "temp_max_c": max(temps) / 1000.0 if temps else None, "freq_min_khz": min(freqs) if freqs else None,
            "freq_max_khz": max(freqs) if freqs else None}


CASE_DIR_RE = re.compile(r"^(\d\d)-(.+)$")


def read_case(path, reference=None):
    env, extra = parse_env(read_text(path / "env.txt"))
    case = {"dir": path.name, "index": int(CASE_DIR_RE.match(path.name).group(1)), "name": CASE_DIR_RE.match(path.name).group(2),
            "env": env, "kind": extra.get("kind"), "exit": _int((read_text(path / "exit.txt") or "").strip()), "seconds": None,
            "timeout": False}
    t = read_text(path / "time.txt") or ""
    m = re.search(r"seconds ([\d.]+) timeout (\d)", t)
    if m:
        case["seconds"] = float(m.group(1))
        case["timeout"] = m.group(2) == "1"
    stderr = read_text(path / "stderr.txt")
    case["stderr_tail"] = [l for l in (stderr or "").split("\n") if l.strip()][-6:]
    case["stderr"] = parse_stderr(stderr)
    slow = _int(env.get("SS_TEST_SLOW")) or (SLOW_DEFAULT_MS if case["name"].startswith("cap-show") else 0)
    case["frames"] = frame_stats(parse_framelog(read_text(path / "frames.csv")), slow if case["name"].startswith("cap-show") else 0)
    case["monitor"] = parse_monitor(read_text(path / "monitor.txt"))
    hashes = parse_hashes(stderr)
    case["hash_frames"] = len(hashes)
    if hashes and reference:
        case["hash_compare"] = compare_hashes(hashes, reference)
    case["_hashes"] = hashes
    case["files"] = sorted(p.name for p in path.iterdir() if p.is_file())
    case["crash_log"] = [n for n in case["files"] if "crash" in n.lower()]
    case["ok"] = case["exit"] == 0 and not case["timeout"]
    return case


# ---------------------------------------------------------------- census

def section(text, name):
    """The block after '== name' in system.txt."""
    if not text:
        return ""
    m = re.search(r"^== %s\n(.*?)(?=^== |\Z)" % re.escape(name), text, re.S | re.M)
    return m.group(1) if m else ""


def decode_bitmask(text, word_bits):
    """sysfs capabilities ('f 0 7fdb0000 0'): words from the highest down -> set of bit numbers."""
    words = (text or "").split()
    bits = set()
    for i, word in enumerate(reversed(words)):
        try:
            value = int(word, 16)
        except ValueError:
            continue
        for b in range(word_bits):
            if value >> b & 1:
                bits.add(i * word_bits + b)
    return bits


def decode_js(data):
    """js0.raw: struct js_event {u32 time; s16 value; u8 type; u8 number}."""
    buttons, axes, presses = set(), {}, []
    for off in range(0, len(data) - 7, 8):
        time = int.from_bytes(data[off:off + 4], "little")
        value = int.from_bytes(data[off + 4:off + 6], "little", signed=True)
        kind, number = data[off + 6], data[off + 7]
        if kind & 0x01:
            buttons.add(number)
            if not kind & 0x80 and value:
                presses.append(("boton", number, time))
        if kind & 0x02:
            axes[number] = value
            if not kind & 0x80:
                presses.append(("eje", number, value))
    return {"buttons": sorted(buttons), "axes": axes, "events": presses}


def decode_evdev(data, size=16):
    """eventN.raw: struct input_event on 32-bit ARM: {s32 sec, s32 usec, u16 type, u16 code, s32 value}."""
    keys, axes = {}, {}
    for off in range(0, len(data) - size + 1, size):
        rec = data[off:off + size]
        etype = int.from_bytes(rec[size - 8:size - 6], "little")
        code = int.from_bytes(rec[size - 6:size - 4], "little")
        value = int.from_bytes(rec[size - 4:size], "little", signed=True)
        if etype == 1 and value == 1:
            keys[code] = keys.get(code, 0) + 1
        elif etype == 3:
            axes.setdefault(code, set()).add(value)
    return {"keys": keys, "axes": {k: sorted(v) for k, v in axes.items()}}


def joydev_order(keys):
    """joydev numbers buttons from BTN_MISC (0x100) up to KEY_MAX, then those below BTN_MISC."""
    return sorted(k for k in keys if k >= 0x100) + sorted(k for k in keys if k < 0x100)


def read_census(root):
    system = read_text(root / "system.txt")
    census = root / "census"
    facts = {"problems": []}
    if system is None:
        facts["problems"].append("no hay system.txt")
    uname = section(system, "uname").strip()
    facts["kernel"] = uname
    cpuinfo = read_text(census / "cpuinfo.txt") or section(system, "/proc/cpuinfo")
    facts["cpu_count"] = len(re.findall(r"^processor\s*:", cpuinfo or "", re.M))
    m = re.search(r"model name\s*:\s*(.+)", cpuinfo or "")
    facts["cpu_model"] = m.group(1).strip() if m else None
    m = re.search(r"CPU part\s*:\s*(0x[0-9a-f]+)", cpuinfo or "")
    facts["cpu_part"] = {"0xc07": "Cortex-A7", "0xc09": "Cortex-A9", "0xd03": "Cortex-A53", "0xd04": "Cortex-A35"}.get(
        m.group(1) if m else "", m.group(1) if m else None)
    m = re.search(r"Hardware\s*:\s*(.+)", cpuinfo or "")
    facts["hardware"] = m.group(1).strip() if m else None
    m = re.search(r"Features\s*:\s*(.+)", cpuinfo or "")
    facts["cpu_features"] = m.group(1).split() if m else []
    cpu = section(system, "cpu")
    freqs = []
    for line in cpu.split("\n"):
        g = re.match(r"(cpu\d+) online=(\S*) gov=(\S*) min=(\d*) max=(\d*) cur=(\d*)", line)
        if g:
            freqs.append({"cpu": g.group(1), "governor": g.group(3), "min_khz": _int(g.group(4)), "max_khz": _int(g.group(5)),
                          "cur_khz": _int(g.group(6))})
    facts["cpufreq"] = freqs
    m = re.search(r"govs=(.*)", cpu)
    facts["governors"] = m.group(1).split() if m else []
    mem = read_text(census / "meminfo.txt") or section(system, "/proc/meminfo")
    m = re.search(r"MemTotal:\s+(\d+)", mem or "")
    facts["mem_total_mb"] = int(m.group(1)) / 1024.0 if m else None
    m = re.search(r"MemAvailable:\s+(\d+)", mem or "")
    facts["mem_available_mb"] = int(m.group(1)) / 1024.0 if m else None
    fb = section(system, "fb0")
    fbset = section(system, "fbset")
    fbinfo = {}
    m = re.search(r"geometry (\d+) (\d+) (\d+) (\d+) (\d+)", fbset)
    if m:
        fbinfo.update(width=int(m.group(1)), height=int(m.group(2)), virtual_w=int(m.group(3)), virtual_h=int(m.group(4)),
                      bpp=int(m.group(5)))
    m = re.search(r"virtual_size: (\d+),(\d+)", fb)
    if m:
        fbinfo.setdefault("virtual_w", int(m.group(1)))
        fbinfo["virtual_h"] = int(m.group(2))
    m = re.search(r"modes: \S*?(\d+)x(\d+)", fb)
    if m:
        fbinfo.setdefault("width", int(m.group(1)))
        fbinfo.setdefault("height", int(m.group(2)))
    m = re.search(r"bits_per_pixel: (\d+)", fb)
    if m:
        fbinfo.setdefault("bpp", int(m.group(1)))
    m = re.search(r"stride: (\d+)", fb)
    if m:
        fbinfo["line"] = int(m.group(1))
    m = re.search(r"rgba (\S+)", fbset)
    if m:
        fbinfo["rgba"] = m.group(1)
    m = re.search(r"^0 (\S+)", section(system, "/proc/fb"), re.M)
    if m:
        fbinfo["driver"] = m.group(1)
    if fbinfo.get("height") and fbinfo.get("virtual_h"):
        fbinfo["pages"] = fbinfo["virtual_h"] // fbinfo["height"]
    facts["fb"] = fbinfo
    cards = read_text(census / "asound-cards.txt") or section(system, "/proc/asound/cards")
    facts["audio_cards"] = re.findall(r"^\s*\d+ \[(\S+)\s*\]:\s*(.+)$", cards or "", re.M)
    facts["audio_pcm"] = section(system, "/proc/asound/pcm").strip()
    devices = read_text(census / "input-devices.txt") or section(system, "/proc/bus/input/devices")
    inputs = []
    for block in (devices or "").split("\n\n"):
        name = re.search(r'N: Name="(.*)"', block)
        if not name:
            continue
        handlers = re.search(r"H: Handlers=(.*)", block)
        key = re.search(r"B: KEY=(.*)", block)
        absb = re.search(r"B: ABS=(.*)", block)
        inputs.append({"name": name.group(1), "handlers": handlers.group(1).split() if handlers else [],
                       "key_mask": key.group(1).strip() if key else "", "abs_mask": absb.group(1).strip() if absb else ""})
    # 32-bit kernel: capability words are 32 bits; a 64-bit kernel prints 64-bit words
    word_bits = 64 if re.search(r"aarch64|x86_64", uname or "") else 32
    for dev in inputs:
        dev["keys"] = sorted(decode_bitmask(dev["key_mask"], word_bits))
        dev["key_names"] = [KEY_NAMES.get(k, str(k)) for k in dev["keys"]]
        dev["axes"] = [ABS_NAMES.get(a, str(a)) for a in sorted(decode_bitmask(dev["abs_mask"], word_bits))]
    facts["inputs"] = inputs
    raw = {}
    for f in sorted((census / "input").glob("*.raw")) if (census / "input").is_dir() else []:
        data = f.read_bytes()
        raw[f.stem] = {"bytes": len(data)}
        if f.stem.startswith("js"):
            raw[f.stem].update(decode_js(data))
        else:
            raw[f.stem].update(decode_evdev(data))
    facts["raw_input"] = raw
    env = section(system, "env")
    m = re.search(r"^SDL_GAMECONTROLLERCONFIG=(.*)$", env, re.M)
    facts["sdl_pad_config"] = m.group(1) if m else None
    tools = {}
    for line in system.split("\n") if system else []:
        g = re.match(r"^(taskset|timeout|setsid|nice|renice|hexdump|od|dd|awk|ip|ifconfig|iw|fbset|amixer|aplay|evtest)=(.*)$", line)
        if g:
            tools[g.group(1)] = g.group(2) or None
    facts["tools"] = tools
    dd = read_text(census / "dd.txt") or ""
    facts["write_speed"] = [{"target": g.group(1), "seconds": float(g.group(2)), "mb_s": 4.0 / float(g.group(2)) if float(g.group(2)) else None}
                            for g in re.finditer(r"dd 4MiB to (\S+): ([\d.]+) s", dd)]
    m = re.search(r"(thermal_zone0) (\S+) (\d+)", section(system, "thermal"))
    facts["temp_c"] = int(m.group(3)) / 1000.0 if m else None
    m = re.search(r"PRETTY_NAME=\"?([^\"\n]+)", section(system, "/etc/os-release"))
    facts["os"] = m.group(1) if m else None
    facts["mounts_tmpfs"] = re.findall(r"^tmpfs (\S+) tmpfs", section(system, "/proc/mounts"), re.M)
    dmesg = read_text(root / "dmesg-end.txt") or ""
    facts["app_exits"] = [{"t": float(g.group(1)), "rc": int(g.group(2))} for g in re.finditer(r"\[\s*([\d.]+)\] appd: app exited \(rc=(\d+)\)", dmesg)]
    facts["app_launches"] = [{"t": float(g.group(1)), "cmd": g.group(2)} for g in re.finditer(r"\[\s*([\d.]+)\] appd: launching app: (.*)", dmesg)]
    facts["kernel_faults"] = [l.strip() for l in dmesg.split("\n") if re.search(r"segfault|unhandled|Out of memory|Killed process|oom-kill", l)]
    facts["pad"] = propose_mapping(inputs, raw, facts["sdl_pad_config"])
    return facts


def propose_mapping(inputs, raw, sdl_config):
    pad = next((d for d in inputs if any(k >= 0x130 for k in d["keys"]) or any(h.startswith("js") for h in d["handlers"])), None)
    if not pad:
        return None
    out = {"device": pad["name"], "event": next((h for h in pad["handlers"] if h.startswith("event")), None),
           "js": next((h for h in pad["handlers"] if h.startswith("js")), None), "codes": pad["keys"], "axes": pad["axes"]}
    order = joydev_order(pad["keys"])
    out["js_buttons"] = {i: KEY_NAMES.get(code, str(code)) for i, code in enumerate(order)}
    js = raw.get(out["js"] or "", {})
    if js.get("buttons"):
        out["js_button_count"] = len(js["buttons"])
        out["js_axis_count"] = len(js.get("axes", {}))
        out["js_matches_caps"] = len(js["buttons"]) == len(order)
        out["js_axes_rest"] = js.get("axes", {})
    proposal, used = [], set()
    for code, button in PROPOSED:
        if code in pad["keys"] and code not in used:
            proposal.append((code, KEY_NAMES.get(code, str(code)), button))
            used.add(code)
    out["proposal"] = proposal
    out["runtime_default"] = [(c, KEY_NAMES.get(c, str(c)), RUNTIME_EVDEV_MAP.get(c, "-")) for c in pad["keys"]]
    out["unmapped_by_runtime"] = [KEY_NAMES.get(c, str(c)) for c in pad["keys"] if c not in RUNTIME_EVDEV_MAP]
    extra = [(c, b) for c, _, b in proposal if RUNTIME_EVDEV_MAP.get(c) != b]
    out["evdev_map"] = ",".join("%d=%s" % (c, b) for c, b in extra)
    pressed = set()
    for name, data in raw.items():
        if name == out["event"]:
            pressed |= set(data.get("keys", {}))
    out["pressed_codes"] = sorted(pressed)
    out["pressed_during_run"] = bool(pressed) or bool([e for e in js.get("events", [])])
    if sdl_config:
        fields = dict(f.split(":", 1) for f in sdl_config.split(",")[2:] if ":" in f)
        out["sdl_config"] = fields
        by_target = {}
        for role, target in fields.items():
            by_target.setdefault(target, []).append(role)
        out["sdl_shared"] = {t: r for t, r in by_target.items() if len(r) > 1}
        names = {"b%d" % i: n for i, n in out["js_buttons"].items()}
        out["sdl_named"] = {role: names.get(target, target) for role, target in fields.items() if target.startswith("b")}
    return out


# ---------------------------------------------------------------- analysis

def find(cases, name):
    return next((c for c in cases if c["name"] == name), None)


def cost_breakdown(cases):
    """What the case matrix says one frame costs, part by part (ms a frame)."""
    def ms(name):
        c = find(cases, name)
        if not c or not c["ok"] or not c["frames"]:
            return None
        f = c["frames"]
        if name.startswith("cap-show"):
            return f["mean_ms"] - f.get("slow_ms", SLOW_DEFAULT_MS)
        return f["mean_ms"]
    rows = []

    def add(label, value, evidence):
        if value is not None:
            rows.append({"part": label, "ms": value, "evidence": evidence})
    head = ms("cap-headless")
    add("juego sin ventana (logica + sonido)", head, "cap-headless")
    for width in ("240", "320", "480"):
        r = ms("cap-render-" + width)
        if r is not None and head is not None:
            add("dibujo %s columnas (cap-render-%s - cap-headless)" % (width, width), r - head, "cap-render-%s, cap-headless" % width)
    nopic = ms("cap-nopic-alsa")
    if nopic is not None and head is not None:
        add("ventana SDL + eventos + sonido ALSA (cap-nopic-alsa - cap-headless)", nopic - head, "cap-nopic-alsa, cap-headless")
    dummy = ms("cap-nopic-dummy")
    if nopic is not None and dummy is not None:
        add("ALSA frente a sonido nulo (cap-nopic-alsa - cap-nopic-dummy)", nopic - dummy, "cap-nopic-alsa, cap-nopic-dummy")
    for view in ("240", "320"):
        noshow = ms("cap-noshow-" + view)
        show = ms("cap-show-" + view)
        if noshow is not None and nopic is not None:
            add("dibujar y acabar en ventana %s (cap-noshow - cap-nopic)" % view, noshow - nopic, "cap-noshow-%s, cap-nopic-alsa" % view)
        if show is not None and noshow is not None:
            add("presentar en /dev/fb0 %s (cap-show - 17 ms - cap-noshow)" % view, show - noshow, "cap-show-%s, cap-noshow-%s" % (view, view))
        elif show is not None and nopic is not None:
            add("dibujar + presentar %s (cap-show - 17 ms - cap-nopic)" % view, show - nopic, "cap-show-%s, cap-nopic-alsa" % view)
    pinned, free = ms("cores-pinned-320"), ms("cap-noshow-320")
    if pinned is not None and free is not None:
        add("un nucleo frente a cuatro (cores-pinned-320 - cap-noshow-320)", pinned - free, "cores-pinned-320, cap-noshow-320")
    gov = ms("gov-performance-320")
    if gov is not None and free is not None:
        add("regulador performance (gov-performance-320 - cap-noshow-320)", gov - free, "gov-performance-320, cap-noshow-320")
    return rows


def all_failed_alike(cases):
    """Every case (that ran) failed within a few seconds with the same message."""
    ran = [c for c in cases if c["exit"] is not None]
    if len(ran) < 2:
        return None
    failed = [c for c in ran if c["exit"] != 0]
    if len(failed) != len(ran):
        return None
    quick = [c for c in failed if c["seconds"] is not None and c["seconds"] <= QUICK_FAIL_S]
    if len(quick) != len(failed):
        return None

    def normal(c):
        last = c["stderr_tail"][-1] if c["stderr_tail"] else ""
        return re.sub(r"\d+", "N", last)
    messages = {normal(c) for c in failed}
    codes = {c["exit"] for c in failed}
    if len(messages) != 1 or len(codes) != 1:
        return None
    seconds = [c["seconds"] for c in failed]
    return {"count": len(failed), "code": codes.pop(), "seconds": statistics.median(seconds),
            "message": failed[0]["stderr_tail"][-1] if failed[0]["stderr_tail"] else "(stderr vacio)"}


def explain_failure(message, code):
    hints = []
    if code == 127 and re.search(r"exec: .*: (/\S+): not found", message or ""):
        path = re.search(r"exec: .*: (/\S+): not found", message).group(1)
        if re.match(r"^/lib\w*/ld-linux", path) or path.startswith("/libarmhf"):
            hints.append("la ruta del cargador empieza en /: la variable $game estaba vacia en el caso hijo (el script se "
                         "reinvoca con sh --child y game no se exporta ni se recalcula), asi que ss_run buscaba "
                         "/libarmhf/ld-linux-armhf.so.3")
        else:
            hints.append("exec no encontro %s: comprobar que el paquete lo contiene y es ejecutable" % path)
    elif code == 127:
        hints.append("127: el shell no encontro el programa; ver la ruta en stderr")
    if code == 126:
        hints.append("126: permiso de ejecucion (tarjeta vfat montada sin exec?)")
    return hints


def analyse(root, reference=None):
    root = Path(root)
    status = parse_status(read_text(root / "status.txt"))
    case_dirs = sorted(p for p in root.iterdir() if p.is_dir() and CASE_DIR_RE.match(p.name))
    cases = [read_case(p, reference) for p in case_dirs]
    facts = read_census(root)
    for c in cases:
        if c["name"] in status["timeouts"]:
            c["timeout"] = True
            c["ok"] = False
    hashes = {c["name"]: c["_hashes"] for c in cases if c["_hashes"]}
    internal = None
    if "hash-headless" in hashes and "hash-sdl" in hashes:
        internal = compare_hashes(hashes["hash-sdl"], hashes["hash-headless"])
    report = {
        "dir": str(root), "status": status, "cases": cases, "facts": facts, "breakdown": cost_breakdown(cases),
        "all_failed": all_failed_alike(cases), "hash_internal": internal, "reference_frames": len(reference or {}),
        "useful_cases": sum(1 for c in cases if c["ok"] and (c["frames"] or c["hash_frames"])),
    }
    report["improve"] = rank_improvements(report)
    return report


def rank_improvements(r):
    items = []

    def add(score, text, evidence):
        items.append({"score": score, "text": text, "evidence": evidence})
    af = r["all_failed"]
    if af:
        add(100, "Arreglar el arranque de los casos del diagnostico: ninguno llego a ejecutar el juego",
            "%d casos con salida %d en ~%.1f s: %s" % (af["count"], af["code"], af["seconds"], af["message"]))
        add(99, "Agregar un caso previo que pruebe que el binario arranca y aborte todo con un mensaje en pantalla si no",
            "un solo error de script gasto la ejecucion entera")
    for c in r["cases"]:
        if af:
            break
        if c["exit"] not in (0, None) and not c["timeout"]:
            add(90 if c["exit"] in (132, 134, 135, 139) else 70, "El caso %s termino con %s (%s)" % (c["name"], c["exit"], EXIT_MEANING.get(c["exit"], "?")),
                "; ".join(c["stderr_tail"][-2:]) or "stderr vacio")
        if c["timeout"]:
            add(75, "El caso %s se colgo o fue lento: lo paro el vigilante" % c["name"], "TIMEOUT en status.txt")
    for e in r["facts"].get("app_exits", []):
        if e["rc"] != 0:
            launch = [l for l in r["facts"].get("app_launches", []) if l["t"] < e["t"]]
            started = launch[-1]["t"] if launch else None
            add(95 if e["rc"] in (139, 134, 132) else 60,
                "El juego normal termino con %d (%s) antes del diagnostico" % (e["rc"], EXIT_MEANING.get(e["rc"], "?")),
                "dmesg: appd app exited rc=%d a %.1f s%s" % (e["rc"], e["t"], (", %.0f s despues de lanzarse (%s)" % (e["t"] - started, launch[-1]["cmd"])) if launch else ""))
    for name, data in (("hash interno sdl/headless", r["hash_internal"]),):
        if data and not data["equal"]:
            add(96, "Determinismo: hash-sdl y hash-headless difieren", "primer fotograma distinto %s" % data["first_diff"])
    for c in r["cases"]:
        hc = c.get("hash_compare")
        if hc and not hc["equal"]:
            add(97, "Determinismo ARMv7 frente a la referencia de Windows: %s difiere" % c["name"],
                "primer fotograma distinto %s (%s frente a %s)" % (hc["first_diff"], hc["mine_at"], hc["ref_at"]))
    for c in r["cases"]:
        f = c["frames"]
        if not c["ok"] or not f:
            continue
        if c["name"].startswith("play-paced") or c["name"].startswith("town") or c["name"].startswith("stability"):
            if f["fps"] and f["fps"] < CONSOLE_FPS - 0.5:
                worst = max(STAGES[:-1], key=lambda s: f["stages_ms"][s])
                add(80 + min(15, (CONSOLE_FPS - f["fps"])), "%s va a %.1f FPS (objetivo 59.7): reducir %s" % (c["name"], f["fps"], STAGE_NAMES[worst]),
                    "media %.2f ms, P99 %.2f ms; %s %.2f ms por fotograma" % (f["mean_ms"], f["p99_ms"], STAGE_NAMES[worst], f["stages_ms"][worst]))
            if f["frames"] and f["late"] / f["frames"] > 0.01:
                add(55, "%s: %.1f%% de fotogramas tarde" % (c["name"], 100.0 * f["late"] / f["frames"]), "columna late de frames.csv")
            if f["frames"] and f["not_presented"] / f["frames"] > 0.02 and c["env"].get("SS_PICTURE", "1") == "1":
                add(60, "%s: %.1f%% de imagenes omitidas (frameskip)" % (c["name"], 100.0 * f["not_presented"] / f["frames"]),
                    "columna presented = 0 en %d de %d" % (f["not_presented"], f["frames"]))
            if f["p99_ms"] > 2 * FRAME_MS:
                add(50, "%s: tirones (P99 %.1f ms)" % (c["name"], f["p99_ms"]), "max %.1f ms" % f["max_ms"])
    for row in sorted(r["breakdown"], key=lambda x: -x["ms"])[:3]:
        if row["ms"] > 2.0:
            add(40 + min(20, row["ms"]), "Coste grande: %s" % row["part"], "%.2f ms por fotograma (%s)" % (row["ms"], row["evidence"]))
    pad = r["facts"].get("pad")
    if pad and pad.get("sdl_shared"):
        for target, roles in pad["sdl_shared"].items():
            add(45, "El mapa SDL del mando asigna %s a la vez a %s" % (target, ", ".join(roles)),
                "SDL_GAMECONTROLLERCONFIG del sistema (%s = %s)" % (target, pad.get("sdl_named", {}).get(roles[0], target)))
    if pad and pad.get("unmapped_by_runtime"):
        add(20, "Botones del mando sin uso en la tabla evdev del juego: %s" % ", ".join(pad["unmapped_by_runtime"]),
            "capabilities/key de %s" % pad["device"])
    tools = r["facts"].get("tools", {})
    if "taskset" in tools and not tools["taskset"]:
        add(25, "No hay taskset: el caso cores-pinned no fija nucleo (resultado igual que sin fijar)", "system.txt: taskset=")
    if "gov-performance-320" in r["status"]["skipped"]:
        add(15, "No se puede cambiar el regulador de CPU como usuario: el caso gov-performance no corre",
            "SKIP gov-performance-320 %s" % r["status"]["skipped"]["gov-performance-320"])
    if r["status"].get("clock") == "ms":
        add(30 if not r["all_failed"] else 10, "El reloj de precision (cntvct) no se uso: tiempos por etapa a 1 ms",
            "CLOCK ms en status.txt" + (" (el caso clock-probe ni arranco)" if r["all_failed"] else ""))
    for problem in r["status"]["problems"]:
        add(85, "Ejecucion incompleta: " + problem, "status.txt")
    for c in r["cases"]:
        m = c["monitor"]
        if m and m["temp_max_c"] and m["temp_max_c"] > 75:
            add(50, "Temperatura alta en %s: %.0f C" % (c["name"], m["temp_max_c"]), "monitor.txt")
        if m and m["freq_min_khz"] and m["freq_max_khz"] and m["freq_min_khz"] < 0.8 * m["freq_max_khz"] and c["ok"]:
            add(35, "La CPU bajo de frecuencia durante %s (%d a %d MHz)" % (c["name"], m["freq_min_khz"] // 1000, m["freq_max_khz"] // 1000),
                "monitor.txt")
    items.sort(key=lambda x: -x["score"])
    return items


# ---------------------------------------------------------------- output

def fmt(value, pattern="%.2f", none="-"):
    return none if value is None else pattern % value


def render(r):
    out = []
    p = out.append
    st = r["status"]
    p("INFORME DEL DIAGNOSTICO GA36")
    p("carpeta: %s" % r["dir"])
    p("")
    af = r["all_failed"]
    if af:
        p("!!! TODOS LOS CASOS FALLARON: %d de %d terminaron con salida %d en ~%.1f s, con el mismo error:" % (
            af["count"], len(r["cases"]), af["code"], af["seconds"]))
        p("!!!   %s" % af["message"])
        for h in explain_failure(af["message"], af["code"]):
            p("!!!   causa probable: %s" % h)
        p("!!! No hay ninguna medida del juego en esta ejecucion; solo vale el censo del sistema.")
        p("")
    p("== Estado de la ejecucion")
    if st["complete"]:
        p("completa segun status.txt: si (START, censo, %d casos con BEGIN/END, END 0 final)" % len(st["ended"]))
    else:
        p("completa segun status.txt: NO")
        for problem in st["problems"]:
            p("  - %s" % problem)
    p("casos empezados %d, terminados %d, saltados %d, vigilante %d; con datos utiles: %d" % (
        len(st["begun"]), len(st["ended"]), len(st["skipped"]), len(st["timeouts"]), r["useful_cases"]))
    valid = st["complete"] and r["useful_cases"] > 0 and not af
    p("valida como medida: %s" % ("si" if valid else "NO"))
    if st["complete"] and not valid:
        p("  (el guion llego al final, pero sin datos utiles del juego)")
    for name, why in st["skipped"].items():
        p("  saltado: %s (%s)" % (name, why))
    for name in st["missing"]:
        p("  falta: %s" % name)
    p("reloj de etapas: %s; sin ventana disponible: %s; trabajo en: %s" % (st.get("clock") or "-", st.get("headless") or "-", st.get("work") or "-"))
    p("")
    p("== Casos")
    p("%-26s %6s %7s %7s %7s %7s %7s %7s %6s %6s %s" % ("caso", "salida", "seg", "fotog", "FPS", "med ms", "P95 ms", "P99 ms",
                                                     "omit", "tarde", "capacidad"))
    for c in r["cases"]:
        f = c["frames"]
        cap = ""
        if f and c["name"].startswith("cap"):
            cap = fmt(f.get("capacity_fps"), "%.1f FPS") + (" (-%d ms)" % f["slow_ms"] if f.get("slow_ms") else "")
        p("%-26s %6s %7s %7s %7s %7s %7s %7s %6s %6s %s" % (
            c["dir"][:26], fmt(c["exit"], "%d"), fmt(c["seconds"], "%.1f"), fmt(f and f["frames"], "%d"), fmt(f and f["fps"], "%.1f"),
            fmt(f and f["median_ms"]), fmt(f and f["p95_ms"]), fmt(f and f["p99_ms"]), fmt(f and f["not_presented"], "%d"),
            fmt(f and f["late"], "%d"), cap + (" VIGILANTE" if c["timeout"] else "")))
    failed = [c for c in r["cases"] if c["exit"] not in (0, None)]
    if failed and not af:
        p("")
        p("casos con error:")
        for c in failed:
            p("  %s: salida %s (%s) a los %s s; %s" % (c["name"], c["exit"], EXIT_MEANING.get(c["exit"], "?"), fmt(c["seconds"], "%.1f"),
                                                     c["stderr_tail"][-1] if c["stderr_tail"] else "stderr vacio"))
            if c["crash_log"]:
                p("    registro del fallo: %s" % ", ".join(c["crash_log"]))
    with_frames = [c for c in r["cases"] if c["frames"]]
    if with_frames:
        p("")
        p("== Media por etapa (ms por fotograma)")
        p("%-26s " % "caso" + " ".join("%8s" % s.replace("_us", "") for s in STAGES))
        for c in with_frames:
            p("%-26s " % c["name"][:26] + " ".join("%8.2f" % c["frames"]["stages_ms"][s] for s in STAGES))
        clocks = {c["frames"]["clock"] for c in with_frames}
        if "ms" in clocks:
            p("(reloj ms: cada tiempo es un milisegundo entero; las medias valen, los percentiles por etapa no)")
    if r["breakdown"]:
        p("")
        p("== Desglose de costes (comparando casos)")
        for row in r["breakdown"]:
            p("  %-70s %7.2f ms" % (row["part"], row["ms"]))
    if any(c["hash_frames"] for c in r["cases"]) or r["reference_frames"]:
        p("")
        p("== Hashes")
        p("referencia: %d fotogramas" % r["reference_frames"] if r["reference_frames"] else "referencia: no dada (--reference)")
        for c in r["cases"]:
            if not c["name"].startswith("hash") and not c["hash_frames"]:
                continue
            hc = c.get("hash_compare")
            if hc:
                verdict = ("iguales en los %d fotogramas comparados" % hc["compared"]) if hc["equal"] else (
                    "DISTINTOS desde el fotograma %d (%s, referencia %s); %d comparados" % (hc["first_diff"], hc["mine_at"], hc["ref_at"], hc["compared"]))
            else:
                verdict = "sin comparar" if c["hash_frames"] else "sin hashes (el caso no produjo ninguno)"
            p("  %s: %d hashes; %s" % (c["name"], c["hash_frames"], verdict))
        if r["hash_internal"]:
            hi = r["hash_internal"]
            p("  hash-sdl frente a hash-headless: %s" % ("iguales" if hi["equal"] else "distintos desde %s" % hi["first_diff"]))
        pics = [(c["name"], c["stderr"]["picture"]) for c in r["cases"] if isinstance(c["stderr"]["picture"], dict)]
        for name, pic in pics:
            p("  %s: imagen %d columnas, %d fotogramas, hash %s" % (name, pic["width"], pic["frames"], pic["hash"]))
    render_facts(r["facts"], p)
    p("")
    p("== Que mejorar (ordenado)")
    if not r["improve"]:
        p("  nada destacable")
    for i, item in enumerate(r["improve"], 1):
        p("%2d. %s" % (i, item["text"]))
        p("    evidencia: %s" % item["evidence"])
    return "\n".join(out) + "\n"


def render_facts(f, p):
    p("")
    p("== Sistema")
    if f.get("problems"):
        for problem in f["problems"]:
            p("  %s" % problem)
    p("  sistema: %s" % (f.get("os") or "-"))
    p("  nucleo: %s" % (f.get("kernel") or "-"))
    p("  CPU: %d x %s (%s), %s" % (f.get("cpu_count") or 0, f.get("cpu_part") or "-", f.get("cpu_model") or "-", f.get("hardware") or "-"))
    if f.get("cpufreq"):
        c0 = f["cpufreq"][0]
        p("  frecuencia: %s-%s MHz, ahora %s MHz, regulador %s (disponibles: %s)" % (
            fmt(c0["min_khz"] and c0["min_khz"] / 1000, "%.0f"), fmt(c0["max_khz"] and c0["max_khz"] / 1000, "%.0f"),
            fmt(c0["cur_khz"] and c0["cur_khz"] / 1000, "%.0f"), c0["governor"] or "-", " ".join(f.get("governors") or []) or "-"))
    if "neon" in f.get("cpu_features", []):
        p("  extensiones: %s" % " ".join(x for x in f["cpu_features"] if x in ("neon", "vfpv4", "idiva", "lpae", "evtstrm")))
    p("  memoria: %s MiB (libre %s MiB)" % (fmt(f.get("mem_total_mb"), "%.0f"), fmt(f.get("mem_available_mb"), "%.0f")))
    fb = f.get("fb") or {}
    if fb:
        p("  pantalla: %sx%s, %s bpp, linea %s bytes, virtual %sx%s (%s pagina%s), %s, rgba %s" % (
            fb.get("width", "?"), fb.get("height", "?"), fb.get("bpp", "?"), fb.get("line", "?"), fb.get("virtual_w", "?"),
            fb.get("virtual_h", "?"), fb.get("pages", "?"), "" if fb.get("pages") == 1 else "s", fb.get("driver", "?"), fb.get("rgba", "?")))
        if fb.get("pages") == 1:
            p("    (una sola pagina: sin doble bufer por desplazamiento; el juego escribe en la pagina visible)")
    for card in f.get("audio_cards", []):
        p("  sonido: %s (%s)" % (card[0], card[1].strip()))
    if f.get("temp_c") is not None:
        p("  temperatura al empezar: %.1f C" % f["temp_c"])
    for w in f.get("write_speed", []):
        p("  escritura 4 MiB en %s: %.2f s (%s MB/s)" % (w["target"], w["seconds"], fmt(w["mb_s"], "%.0f")))
    tools = f.get("tools") or {}
    missing = [k for k, v in tools.items() if not v]
    if missing:
        p("  herramientas que faltan: %s" % ", ".join(missing))
    for e in f.get("app_exits", []):
        p("  dmesg: una app termino con %d (%s) a %.1f s" % (e["rc"], EXIT_MEANING.get(e["rc"], "?"), e["t"]))
    for line in f.get("kernel_faults", [])[:5]:
        p("  dmesg: %s" % line)
    p("")
    p("== Entradas")
    for dev in f.get("inputs", []):
        p("  %s [%s]: teclas %s%s" % (dev["name"], " ".join(dev["handlers"]), ", ".join(dev["key_names"]) or "-",
                                     ("; ejes " + ", ".join(dev["axes"])) if dev["axes"] else ""))
    for name, data in sorted((f.get("raw_input") or {}).items()):
        if name.startswith("js"):
            p("  %s.raw: %d bytes, %d botones, %d ejes (reposo %s), %d eventos pulsados" % (
                name, data["bytes"], len(data.get("buttons", [])), len(data.get("axes", {})),
                " ".join("%d:%d" % kv for kv in sorted(data.get("axes", {}).items())), len(data.get("events", []))))
        else:
            keys = data.get("keys", {})
            p("  %s.raw: %d bytes%s" % (name, data["bytes"], (", teclas pulsadas " + ", ".join(
                "%s x%d" % (KEY_NAMES.get(k, str(k)), n) for k, n in sorted(keys.items()))) if keys else ", nada pulsado"))
    pad = f.get("pad")
    if pad:
        p("")
        p("== Mapa de botones propuesto (%s, %s)" % (pad["device"], pad.get("event") or "-"))
        if pad.get("js_buttons"):
            p("  numeracion de %s: %s" % (pad.get("js") or "js", ", ".join("b%d=%s" % kv for kv in pad["js_buttons"].items())))
        if "js_matches_caps" in pad:
            p("  %s confirma %d botones y %d ejes: %s" % (pad.get("js"), pad["js_button_count"], pad["js_axis_count"],
                                                     "coincide con capabilities" if pad["js_matches_caps"] else "NO coincide con capabilities"))
        for code, name, button in pad["proposal"]:
            default = RUNTIME_EVDEV_MAP.get(code)
            note = "" if default == button else ("  (el juego hoy: %s)" % (default or "sin asignar"))
            p("  %4d %-15s -> %s%s" % (code, name, button, note))
        p("  SS_EVDEV_MAP para lo que cambia: %s" % (pad["evdev_map"] or "(nada: la tabla por defecto ya sirve)"))
        if pad.get("unmapped_by_runtime"):
            p("  sin uso en la tabla del juego: %s" % ", ".join(pad["unmapped_by_runtime"]))
        if pad.get("sdl_named"):
            p("  SDL_GAMECONTROLLERCONFIG del sistema: %s" % ", ".join("%s=%s" % kv for kv in sorted(pad["sdl_named"].items())))
        for target, roles in (pad.get("sdl_shared") or {}).items():
            p("  aviso: %s esta asignado a la vez a %s" % (target, ", ".join(roles)))
        if not pad.get("pressed_during_run"):
            p("  (nadie pulso botones durante la ejecucion: el mapa sale de capabilities, no de pulsaciones reales)")


def load_reference(path):
    text = read_text(path)
    if text is None:
        sys.exit("no se puede leer la referencia %s" % path)
    return parse_hashes(text)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dir")
    parser.add_argument("--reference")
    parser.add_argument("--json")
    args = parser.parse_args(argv)
    root = Path(args.dir)
    if not root.is_dir():
        sys.exit("no es una carpeta: %s" % root)
    reference = load_reference(args.reference) if args.reference else None
    report = analyse(root, reference)
    try:
        sys.stdout.reconfigure(errors="replace")
    except AttributeError:
        pass
    sys.stdout.write(render(report))
    if args.json:
        def clean(o):
            if isinstance(o, dict):
                return {str(k): clean(v) for k, v in o.items() if not str(k).startswith("_")}
            if isinstance(o, (list, tuple)):
                return [clean(v) for v in o]
            if isinstance(o, set):
                return sorted(o)
            return o
        Path(args.json).write_text(json.dumps(clean(report), indent=1, ensure_ascii=False), encoding="utf-8")
    return 0 if report["status"]["complete"] and not report["all_failed"] else 1


if __name__ == "__main__":
    sys.exit(main())
