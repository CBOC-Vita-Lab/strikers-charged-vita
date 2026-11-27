"""The shader seed tools (1.2): make_shader_pack.py --base keeps the shaders of an earlier package, repack-release.py --shadercache swaps the cache."""
import importlib.util
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACK_TOOL = ROOT / "native/platform/vita/tools/make_shader_pack.py"
REPACK = ROOT / "tools/repack-release.py"


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def write_gxp(seed, key, kind, h, data):
    """<seed>/<key>/v3/{v,f}/<HH>/<16 hex digits>.gxp"""
    d = seed / key / "v3" / ("v" if kind == 0 else "f") / ("%02X" % (h >> 56))
    d.mkdir(parents=True, exist_ok=True)
    (d / ("%016X.gxp" % h)).write_bytes(data)


class ShaderSeedTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.tool = load(PACK_TOOL, "make_shader_pack")

    def tearDown(self):
        self.tmp.cleanup()

    def pack(self, seed, out, base=None, older=None):
        args = [sys.executable, "-I", str(PACK_TOOL)] + (["--base", str(base)] if base else []) + (["--base-key", older] if older else []) + [str(seed), str(out)]
        return subprocess.run(args, check=True, capture_output=True, text=True).stdout

    def test_base_pack_is_kept_and_the_seed_wins_a_shared_hash(self):
        old_seed = self.dir / "old"
        write_gxp(old_seed, "bc447993", 0, 0x1100000000000001, b"old vertex 1")
        write_gxp(old_seed, "bc447993", 1, 0x2200000000000002, b"old fragment 2")
        write_gxp(old_seed, "bc447993", 1, 0x3300000000000003, b"shared")
        (old_seed / "vgx_shaderlist.bin").write_bytes(b"OLDLIST")
        self.pack(old_seed, self.dir / "old-pack")
        self.assertEqual(len(self.tool.read_pack(self.dir / "old-pack/bc447993.pack")), 3)

        delta = self.dir / "delta"
        write_gxp(delta, "bc447993", 1, 0x4400000000000004, b"new fragment 4")
        write_gxp(delta, "bc447993", 1, 0x3300000000000003, b"shared")
        (delta / "vgx_shaderlist.bin").write_bytes(b"NEWLIST")
        out = self.pack(delta, self.dir / "new-pack", base=self.dir / "old-pack")
        self.assertIn("4 shaders (2 kept from the base pack bc447993.pack)", out)
        merged = self.tool.read_pack(self.dir / "new-pack/bc447993.pack")
        self.assertEqual(merged[(0, 0x1100000000000001)], b"old vertex 1")
        self.assertEqual(merged[(1, 0x4400000000000004)], b"new fragment 4")
        self.assertEqual(len(merged), 4)
        self.assertEqual((self.dir / "new-pack/vgx_shaderlist.bin").read_bytes(), b"NEWLIST")
        # the file is sorted by (kind, hash) with 16-byte aligned data, as the console's reader expects
        data = (self.dir / "new-pack/bc447993.pack").read_bytes()
        count, start = struct.unpack("<II", data[8:16])
        keys = [(k, h) for h, k, _o, _s, _z in (struct.unpack("<QIIII", data[16 + 24 * i:40 + 24 * i]) for i in range(count))]
        self.assertEqual(keys, sorted(keys))
        for i in range(count):
            self.assertEqual(struct.unpack("<QIIII", data[16 + 24 * i:40 + 24 * i])[2] % 16, 0)

    def test_without_a_base_nothing_changes_and_an_unrelated_base_pack_is_copied(self):
        seed = self.dir / "seed"
        write_gxp(seed, "30223cc9", 0, 0x0100000000000009, b"v")
        self.pack(seed, self.dir / "plain")
        self.assertEqual(sorted(p.name for p in (self.dir / "plain").iterdir()), ["30223cc9.pack"])
        other = self.dir / "otherbase"
        other.mkdir()
        (other / "bc447993.pack").write_bytes((self.dir / "plain/30223cc9.pack").read_bytes())
        (other / "vgx_shaderlist.bin").write_bytes(b"BASELIST")
        self.pack(seed, self.dir / "with-other", base=other)
        self.assertEqual(sorted(p.name for p in (self.dir / "with-other").iterdir()), ["30223cc9.pack", "bc447993.pack", "vgx_shaderlist.bin"])
        self.assertEqual((self.dir / "with-other/vgx_shaderlist.bin").read_bytes(), b"BASELIST")   # the seed had no list: the base's is kept

    def test_a_new_cache_key_takes_the_older_packs_shaders(self):
        old_seed = self.dir / "old"
        write_gxp(old_seed, "bc447993", 0, 0x1100000000000001, b"old vertex")
        self.pack(old_seed, self.dir / "base")
        delta = self.dir / "delta"
        write_gxp(delta, "5a5a5a5a", 1, 0x4400000000000004, b"new fragment")
        # without --base-key the new key finds no pack of its own, and the old one is carried along unchanged beside it
        self.pack(delta, self.dir / "plain", base=self.dir / "base")
        self.assertEqual(sorted(p.name for p in (self.dir / "plain").iterdir()), ["5a5a5a5a.pack", "bc447993.pack"])
        self.assertEqual(len(self.tool.read_pack(self.dir / "plain/5a5a5a5a.pack")), 1)
        # with it the new pack has both, and the older file is not shipped as well
        out = self.pack(delta, self.dir / "keyed", base=self.dir / "base", older="bc447993")
        self.assertIn("1 kept from the base pack bc447993.pack", out)
        self.assertEqual(sorted(p.name for p in (self.dir / "keyed").iterdir()), ["5a5a5a5a.pack"])
        self.assertEqual(len(self.tool.read_pack(self.dir / "keyed/5a5a5a5a.pack")), 2)

    def test_repack_swaps_the_whole_shadercache(self):
        prev, built, out = self.dir / "prev.vpk", self.dir / "built.vpk", self.dir / "out.vpk"
        with zipfile.ZipFile(prev, "w") as z:
            z.writestr("eboot.bin", b"old eboot")
            z.writestr("sce_sys/param.sfo", b"sfo")
            z.writestr("shadercache/bc447993.pack", b"old pack")
            z.writestr("shadercache/deadbeef.pack", b"older key")
            z.writestr("shadercache/vgx_shaderlist.bin", b"old list")
        with zipfile.ZipFile(built, "w") as z:
            z.writestr("eboot.bin", b"new eboot")
            z.writestr("licenses/a.txt", b"notice")
        cache = self.dir / "cache"
        cache.mkdir()
        (cache / "bc447993.pack").write_bytes(b"new pack")
        (cache / "vgx_shaderlist.bin").write_bytes(b"new list")
        subprocess.run([sys.executable, str(REPACK), "--keep-donor-version", "--shadercache", str(cache), str(prev), str(built), str(out)], check=True, capture_output=True)
        with zipfile.ZipFile(out) as z:
            names = z.namelist()
            self.assertEqual(z.read("shadercache/bc447993.pack"), b"new pack")
            self.assertEqual(z.read("shadercache/vgx_shaderlist.bin"), b"new list")
            self.assertNotIn("shadercache/deadbeef.pack", names)
            self.assertEqual(z.read("eboot.bin"), b"new eboot")
            self.assertEqual(z.read("sce_sys/param.sfo"), b"sfo")
        # without the option the previous package's cache is kept byte for byte (as before)
        out2 = self.dir / "out2.vpk"
        subprocess.run([sys.executable, str(REPACK), "--keep-donor-version", str(prev), str(built), str(out2)], check=True, capture_output=True)
        with zipfile.ZipFile(out2) as z:
            self.assertEqual(z.read("shadercache/bc447993.pack"), b"old pack")
        # a folder with no pack is refused
        empty = self.dir / "empty"
        empty.mkdir()
        bad = subprocess.run([sys.executable, str(REPACK), "--keep-donor-version", "--shadercache", str(empty), str(prev), str(built), str(self.dir / "x.vpk")], capture_output=True)
        self.assertNotEqual(bad.returncode, 0)


if __name__ == "__main__":
    unittest.main()
