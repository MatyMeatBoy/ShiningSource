"""Tests for tools/ga36_diag_report.py.

  py -3.12 -m unittest tools/tests/test_ga36_diag_report.py

The fixture fixtures/ga36-diagnostico-001 is the real first run on the console (every case exited 127 in 1.2 s
because $game was empty in the re-invoked case), without its 614400-byte fb0-before.raw.
"""
import io, json, sys, tempfile, unittest
from contextlib import redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import ga36_diag_report as rep

FIXTURE = HERE / "fixtures" / "ga36-diagnostico-001"
REFERENCE = HERE / "ga36_hash_reference.txt"


def framelog(intervals, presented=1, late=0, view=320, ppu=3000, clock="cntvct32"):
    lines = ["# SS_FRAMELOG clock %s" % clock, ",".join(rep.FRAMELOG_COLUMNS)]
    for i, interval in enumerate(intervals, 2):
        p = presented(i) if callable(presented) else presented
        l = late(i) if callable(late) else late
        lines.append("%d,%d,5000,1000,0,100,0,%d,200,%d,%d,%d,%d" % (i, interval, ppu, max(0, interval - 9300), p, l, view))
    lines.append("# end frame %d, %d ms since the first frame" % (len(intervals) + 1, sum(intervals) // 1000))
    return "\n".join(lines) + "\n"


def make_case(root, index, name, env, exit_code=0, seconds=30.0, frames=None, stderr="", kind="sdl"):
    d = root / ("%02d-%s" % (index, name))
    d.mkdir()
    (d / "env.txt").write_text("SS_SKIP=0\nSS_NET=\nSS_FRAMELOG=/dev/shm/x/%s/frames.csv\n%s\nkind=%s headless=yes\n" % (
        name, "\n".join(env), kind), newline="\n")
    (d / "exit.txt").write_text("%d\n" % exit_code)
    (d / "time.txt").write_text("start 1 end 2 seconds %.1f timeout 0\n" % seconds)
    (d / "stderr.txt").write_text(stderr)
    (d / "monitor.txt").write_text("120.0 30000 50000 1200000 1200000 1200000 1200000 0.5 0.3 0.1\n")
    if frames is not None:
        (d / "frames.csv").write_text(frames, newline="\n")
    return d


def synthetic_run(root, final=True, cut=False):
    status = ["START ShiningSource GA36 diagnostico v2 uptime 10", "GAME /x", "HEADLESS yes", "WORK /dev/shm/s", "END census 0 12",
              "CLOCK counter"]
    hashes = "".join("frame %5d  hash %08X  stubs reached 0\n" % (f, f * 7) for f in range(1, 11))
    make_case(root, 1, "cap-headless", ["SS_FRAMES=4000"], frames=framelog([5000] * 100, presented=0, view=0), kind="headless")
    make_case(root, 2, "cap-render-320", ["SS_FRAMES=4000", "SS_TEST_RENDER=320"], frames=framelog([8000] * 100, view=320),
              kind="headless", stderr="picture: 4000 frames 320 wide, hash 0123ABCD\n")
    make_case(root, 3, "cap-show-320", ["SS_TEST_SLOW=17", "SS_FRAMESKIP=0"], frames=framelog([27000] * 100))
    # 100 frames: 90 at the console's pace, 10 slow ones (not shown, late)
    make_case(root, 4, "play-paced-320", ["SS_FB_VIEW=320"],
              frames=framelog([16743] * 90 + [40000] * 10, presented=lambda i: 0 if i > 91 else 1, late=lambda i: 1 if i > 91 else 0))
    make_case(root, 5, "hash-headless", ["SS_FRAMES=10", "SS_HASH_ALL=1"], stderr=hashes, kind="headless")
    names = ["cap-headless", "cap-render-320", "cap-show-320", "play-paced-320", "hash-headless"]
    for n in names:
        status += ["BEGIN " + n, "END %s 0 30.0" % n]
    if cut:
        status += ["BEGIN stability-320"]
    if final:
        status.append("END 0")
    (root / "status.txt").write_text("\n".join(status) + "\n", newline="\n")
    (root / "census").mkdir()
    return hashes


class RealFixture(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.r = rep.analyse(FIXTURE)
        cls.text = rep.render(cls.r)

    def test_fixture_has_no_framebuffer_dump(self):
        self.assertFalse((FIXTURE / "fb0-before.raw").exists())

    def test_every_case_failed_alike(self):
        af = self.r["all_failed"]
        self.assertIsNotNone(af)
        self.assertEqual(af["count"], 22)
        self.assertEqual(af["code"], 127)
        self.assertAlmostEqual(af["seconds"], 1.2)
        self.assertIn("ld-linux-armhf.so.3: not found", af["message"])

    def test_flag_is_on_top(self):
        first = self.text.split("\n")[3]
        self.assertIn("TODOS LOS CASOS FALLARON", first)
        self.assertIn("$game estaba vacia", self.text)
        self.assertIn("valida como medida: NO", self.text)
        self.assertIn("arranque", self.r["improve"][0]["text"])

    def test_status_parsed(self):
        st = self.r["status"]
        self.assertTrue(st["final_end"])
        self.assertEqual(len(st["begun"]), 22)
        self.assertIn("gov-performance-320", st["skipped"])
        self.assertEqual(st["clock"], "ms")
        self.assertEqual(self.r["useful_cases"], 0)

    def test_system_facts(self):
        f = self.r["facts"]
        self.assertEqual((f["fb"]["width"], f["fb"]["height"], f["fb"]["bpp"], f["fb"]["line"], f["fb"]["pages"]), (640, 480, 32, 2560, 1))
        self.assertEqual(f["cpu_count"], 4)
        self.assertEqual(f["cpu_part"], "Cortex-A7")
        self.assertEqual(f["cpufreq"][0]["max_khz"], 1200000)
        self.assertEqual(f["audio_cards"][0][0], "GA36mbAudio")
        self.assertIn("7.2.9", f["kernel"])
        self.assertEqual(f["app_exits"][0]["rc"], 139)
        self.assertIsNone(f["tools"]["taskset"])

    def test_pad(self):
        pad = self.r["facts"]["pad"]
        self.assertEqual(pad["device"], "GA36-MB Gamepad")
        self.assertEqual(pad["event"], "event2")
        self.assertEqual(len(pad["codes"]), 17)
        if "js_matches_caps" in pad:     # the joystick capture (census/input/*.raw) is not in the repository (*.raw ignored)
            self.assertTrue(pad["js_matches_caps"])
        self.assertEqual(pad["js_buttons"][0], "BTN_SOUTH")
        self.assertEqual(pad["js_buttons"][16], "BTN_DPAD_RIGHT")
        self.assertEqual(pad["axes"], ["ABS_X", "ABS_Y", "ABS_RX", "ABS_RY"])
        self.assertIn("b8", pad["sdl_shared"])
        proposal = {name: button for _, name, button in pad["proposal"]}
        self.assertEqual(proposal["BTN_EAST"], "a")
        self.assertEqual(proposal["BTN_START"], "start")
        self.assertFalse(pad["pressed_during_run"])

    def test_crash_before_diagnostic_ranked(self):
        texts = [i["text"] for i in self.r["improve"]]
        self.assertTrue(any("139" in t for t in texts))

    def test_reference_with_no_hashes(self):
        r = rep.analyse(FIXTURE, rep.load_reference(REFERENCE))
        self.assertEqual(r["reference_frames"], 3000)
        self.assertIn("sin hashes", rep.render(r))


class Synthetic(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_complete_run(self):
        hashes = synthetic_run(self.root)
        reference = rep.parse_hashes(hashes.replace("frame     5  hash %08X" % 35, "frame     5  hash DEADBEEF"))
        r = rep.analyse(self.root, reference)
        self.assertTrue(r["status"]["complete"])
        self.assertIsNone(r["all_failed"])
        cases = {c["name"]: c for c in r["cases"]}
        head = cases["cap-headless"]["frames"]
        self.assertAlmostEqual(head["fps"], 200.0)
        self.assertEqual(head["frames"], 100)
        self.assertAlmostEqual(head["stages_ms"]["logic_us"], 5.0)
        show = cases["cap-show-320"]["frames"]
        self.assertAlmostEqual(show["capacity_fps"], 100.0)    # 27 ms - 17 ms
        play = cases["play-paced-320"]["frames"]
        self.assertEqual(play["not_presented"], 10)
        self.assertEqual(play["late"], 10)
        self.assertAlmostEqual(play["median_ms"], 16.743)
        self.assertAlmostEqual(play["p95_ms"], 40.0)
        self.assertAlmostEqual(play["p99_ms"], 40.0)
        self.assertAlmostEqual(play["fps"], 1e6 / (0.9 * 16743 + 0.1 * 40000))
        parts = {row["evidence"]: row["ms"] for row in r["breakdown"]}
        self.assertAlmostEqual(parts["cap-render-320, cap-headless"], 3.0)
        hc = cases["hash-headless"]["hash_compare"]
        self.assertFalse(hc["equal"])
        self.assertEqual(hc["first_diff"], 5)
        self.assertEqual(hc["compared"], 10)
        text = rep.render(r)
        self.assertIn("completa segun status.txt: si", text)
        self.assertIn("valida como medida: si", text)
        self.assertIn("DISTINTOS desde el fotograma 5", text)
        self.assertIn("100.0 FPS (-17 ms)", text)
        self.assertEqual(r["improve"][0]["score"], 97)        # the determinism break leads
        self.assertIn("0123ABCD", text)

    def test_equal_hashes(self):
        hashes = synthetic_run(self.root)
        r = rep.analyse(self.root, rep.parse_hashes(hashes))
        hc = [c for c in r["cases"] if c["name"] == "hash-headless"][0]["hash_compare"]
        self.assertTrue(hc["equal"])

    def test_incomplete_run_is_not_called_complete(self):
        synthetic_run(self.root, final=False, cut=True)
        r = rep.analyse(self.root)
        self.assertFalse(r["status"]["complete"])
        problems = " ".join(r["status"]["problems"])
        self.assertIn("stability-320", problems)
        self.assertIn("END 0", problems)
        text = rep.render(r)
        self.assertIn("completa segun status.txt: NO", text)
        self.assertIn("valida como medida: NO", text)
        self.assertIn("200.0", text)                          # what exists is still analysed

    def test_missing_status(self):
        synthetic_run(self.root)
        (self.root / "status.txt").unlink()
        r = rep.analyse(self.root)
        self.assertFalse(r["status"]["complete"])
        self.assertIn("no hay status.txt", r["status"]["problems"])

    def test_crlf_and_json(self):
        synthetic_run(self.root)
        f = self.root / "01-cap-headless" / "frames.csv"
        f.write_bytes(f.read_bytes().replace(b"\n", b"\r\n"))
        out = self.root / "r.json"
        with redirect_stdout(io.StringIO()) as text:
            code = rep.main([str(self.root), "--json", str(out)])
        self.assertEqual(code, 0)
        data = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual(data["cases"][0]["frames"]["frames"], 100)
        self.assertNotIn("_hashes", data["cases"][0])
        self.assertIn("INFORME", text.getvalue())

    def test_one_case_crashes(self):
        synthetic_run(self.root)
        make_case(self.root, 6, "scene-crash-320", ["SS_QUIT=9000"], exit_code=139, seconds=95.0,
                  stderr="crash: SIGSEGV at 0x0800ABCD\n")
        (self.root / "06-scene-crash-320" / "crash.txt").write_text("backtrace\n")
        r = rep.analyse(self.root)
        self.assertIsNone(r["all_failed"])
        top = r["improve"][0]
        self.assertIn("scene-crash-320", top["text"])
        self.assertIn("139", top["text"])
        self.assertIn("crash.txt", rep.render(r))


class Pieces(unittest.TestCase):
    def test_bitmask_32(self):
        bits = rep.decode_bitmask("f 0 0 0 0 0 0 0 7fdb0000 0 0 0 0 0 0 0 0 0", 32)
        self.assertIn(304, bits)
        self.assertIn(547, bits)
        self.assertNotIn(306, bits)
        self.assertEqual(len(bits), 17)

    def test_bitmask_abs(self):
        self.assertEqual(rep.decode_bitmask("1b", 32), {0, 1, 3, 4})

    def test_evdev_decode(self):
        rec = (0).to_bytes(4, "little") * 2 + (1).to_bytes(2, "little") + (305).to_bytes(2, "little") + (1).to_bytes(4, "little")
        self.assertEqual(rep.decode_evdev(rec * 2)["keys"], {305: 2})

    def test_explain_other_exec(self):
        hints = rep.explain_failure("x: exec: line 3: /opt/game/bin: not found", 127)
        self.assertIn("/opt/game/bin", hints[0])


if __name__ == "__main__":
    unittest.main()
