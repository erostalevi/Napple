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


def yaz0_literal(data):
    """Yaz0 with literal groups only (every code byte 0xff)."""
    out = bytearray(b"Yaz0" + struct.pack(">I", len(data)) + bytes(8))
    for i in range(0, len(data), 8):
        chunk = data[i:i + 8]
        out.append((0xff00 >> len(chunk)) & 0xff)
        out += chunk
    return bytes(out)


def sarc(files):
    """Little-endian SARC with the given file payloads (names left out: the scanner does not read SFNT)."""
    n = len(files)
    hdr_len = 0x14 + 0xc + 16 * n + 8
    data_off = (hdr_len + 0xff) & ~0xff
    body, nodes, o = bytearray(), bytearray(), 0
    for f in files:
        nodes += struct.pack("<IIII", 0, 0x01000000, o, o + len(f))
        body += f + bytes((-len(f)) % 4)
        o = len(body)
    head = b"SARC" + struct.pack("<HH", 0x14, 0xfeff) + struct.pack("<III", data_off + len(body), data_off, 0x100)
    head = head[:6] + b"\xff\xfe" + head[8:]
    sfat = b"SFAT" + struct.pack("<HHI", 0xc, n, 0x65) + bytes(nodes) + b"SFNT" + struct.pack("<HH", 8, 0)
    blob = head + sfat
    return blob + bytes(data_off - len(blob)) + bytes(body)


def romfs(files):
    """Minimal RomFS image (header 0x50, one root directory, a flat file table). files: list of (name, bytes)."""
    dmeta = struct.pack("<6I", 0, 0xffffffff, 0xffffffff, 0, 0xffffffff, 0)  # root: no name
    fmeta, data = bytearray(), bytearray()
    for i, (name, blob) in enumerate(files):
        nb = name.encode()
        off = len(data)
        data += blob + bytes((-len(blob)) % 16)
        ent = struct.pack("<IIQQII", 0, 0xffffffff, off, len(blob), 0xffffffff, len(nb)) + nb
        fmeta += ent + bytes((-len(ent)) % 4)
    dmeta_off = 0x50
    fmeta_off = dmeta_off + len(dmeta)
    data_off = (fmeta_off + len(fmeta) + 15) & ~15
    hdr = struct.pack("<10Q", 0x50, 0, 0, dmeta_off, len(dmeta), 0, 0, fmeta_off, len(fmeta), data_off)
    img = hdr + dmeta + bytes(fmeta)
    return img + bytes(data_off - len(img)) + bytes(data)


class T(unittest.TestCase):
    def test_romfs_sarc_yaz0(self):
        """A RomFS image whose files hide sections in a Yaz0-compressed SARC and in plain data; distinct counts by content."""
        inner = sarc([b"pad!" * 3 + section(2), section(0, xfb=True)])
        img = romfs([("Shader/a.szs", yaz0_literal(inner)), ("Model/b.szs", yaz0_literal(sarc([section(2)]))), ("c.bin", b"xxxx" + section(1))])
        self.assertEqual(S.yaz0(yaz0_literal(inner)), inner)
        hdr = lambda n: b"Yaz0" + struct.pack(">I", n) + bytes(8)
        self.assertEqual(S.yaz0(hdr(9) + bytes([0xe0]) + b"abc" + bytes([0x40, 0x02])), b"abcabcabc")  # overlapping back-reference
        self.assertEqual(S.yaz0(hdr(8) + bytes([0xf0]) + b"abcd" + bytes([0x20, 0x03])), b"abcdabcd")  # slice copy
        with tempfile.TemporaryDirectory() as d:
            p = d + "/t.romfs"
            open(p, "wb").write(img)
            self.assertTrue(S.is_romfs(p))
            self.assertEqual([n for n, _, _ in S.romfs_entries(img)], ["Shader/a.szs", "Model/b.szs", "c.bin"])
            progs, st = S.scan_paths([p])
            r = S.summarize(progs, "R", st)
            self.assertEqual((r["stages"]["gs"], r["stages"]["vs"], r["stages"]["fs"], r["stages"]["xfb"]), (2, 1, 1, 1))
            self.assertEqual(r["distinct"]["gs"], 1)  # the same geometry section in two archives
            self.assertEqual(st["inflated"], 2)
            progs, st = S.scan_paths([p], inflate_all=False)  # --quick: only the shader-looking path is opened
            self.assertEqual(S.summarize(progs, "R")["stages"]["gs"], 1)
            self.assertEqual(st["compressed_not_opened"], 1)
            self.assertTrue(any(x["file"].startswith("t.romfs:Shader/a.szs/#0") for x in r["stage_programs"]))


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
