import json, os, struct, subprocess, sys, tempfile, unittest
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scan_stages as S

HERE = os.path.dirname(os.path.abspath(__file__))


def section(stage, xfb=False, tess=0, length=0x880):
    b = bytearray(length)
    struct.pack_into("<IIIII", b, 0, S.MAGIC, 1, 0xf, 0x120, 0xb)
    struct.pack_into("<I", b, 0x20, length)
    struct.pack_into("<III", b, 0x6fc, 0x40, 0x100, 0x400)
    struct.pack_into("<I", b, 0x714, stage)
    if xfb:
        struct.pack_into("<I", b, 0x30, 16); struct.pack_into("<I", b, 0x40, 4)
        b[0x50:0x54] = bytes([0x20, 0x21, 0x22, 0x23])
        b[0x54:0x50 + 128] = b"\xff" * 0 + bytes(128 - 4)
    if tess:
        struct.pack_into("<I", b, 0x718, tess)
    return bytes(b)


class T(unittest.TestCase):
    def test_offsets_from_header(self):
        self.assertIn("control.h", S.OFF_SOURCE)
        self.assertEqual(S.OFF["GLSLC_XFB_BLOCK_OFF"], 0x30)
        self.assertEqual(S.OFF["stage"], 0x714)

    def test_decode(self):
        self.assertEqual(S.decode(section(2))["stage"], "gs")
        self.assertEqual(S.decode(section(3))["stage"], "tcs")
        self.assertIn("xfb", S.decode(section(0, xfb=True))["features"])
        self.assertNotIn("xfb", S.decode(section(1, xfb=True))["features"])
        self.assertEqual(S.decode(section(4, tess=2))["features"], ["tess_quad"])
        self.assertIsNone(S.decode(section(4, tess=0x3) and b"x" * 0x800))
        bad = bytearray(section(0)); bad[0x714] = 9
        self.assertIsNone(S.decode(bytes(bad)))

    def test_scan_dump_and_archive(self):
        with tempfile.TemporaryDirectory() as d:
            os.mkdir(d + "/dump")
            open(d + "/dump/s2-1.ctl", "wb").write(section(2))
            open(d + "/dump/s0-2.ctl", "wb").write(section(0, xfb=True))
            open(d + "/dump/s2-1.code", "wb").write(b"\0" * 64)
            os.mkdir(d + "/romfs")
            open(d + "/romfs/a.bfsha", "wb").write(b"junk" * 9 + section(4, tess=1) + b"pad\0" + section(1))
            progs, nfiles = S.scan_dir(d + "/dump")
            r = S.summarize(progs, "T1")
            self.assertEqual((r["programs"], r["stages"]["gs"], r["stages"]["xfb"]), (2, 1, 1))
            self.assertEqual(r["features"], ["gs", "xfb"])
            progs, nfiles = S.scan_dir(d + "/romfs")
            r = S.summarize(progs, "T2")
            self.assertEqual((nfiles, r["stages"]["tes"], r["stages"]["tess_triangle"], r["stages"]["fs"]), (1, 1, 1, 1))

    def test_cover(self):
        mk = lambda t, f: {"title_id": t, "features": f}
        res = S.cover([mk("A", ["gs", "xfb"]), mk("B", ["gs"]), mk("C", ["tes", "tess_quad"]), mk("D", ["xfb", "tes"]), mk("E", ["tess_quad", "gs"])])
        self.assertEqual(len(res["exact"]), 2)
        self.assertLessEqual(len(res["exact"]), len(res["greedy"]))
        self.assertIn("tcs", res["features_no_title_covers"])
        self.assertIn("cs", res["features_no_title_covers"])
        self.assertEqual(S.cover([])["exact"], [])

    def test_cli(self):
        with tempfile.TemporaryDirectory() as d:
            open(d + "/s2-1.ctl", "wb").write(section(2))
            out = d + "/o.json"
            r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "scan_stages.py"), "scan", d, "--title-id", "0100", "-o", out], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(json.load(open(out))["title_id"], "0100")
            r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "scan_stages.py"), "cover", out], capture_output=True, text=True)
            self.assertEqual(json.loads(r.stdout)["exact"], ["0100"])


if __name__ == "__main__":
    unittest.main()
