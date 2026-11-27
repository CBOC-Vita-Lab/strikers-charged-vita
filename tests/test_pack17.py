"""tools/pack17: the shader pack top-up request (item 12 of wave 11). No shader compiler is used; the hash program is replaced by a stub that hashes key bytes."""
import hashlib
import os
import stat
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "native/tools/seed"))
import shl  # noqa: E402

REQ = os.path.join(ROOT, "tools/pack17/pack17_request.py")
CHECK = os.path.join(ROOT, "tools/pack17/pack17_check.py")
BUILD = os.path.join(ROOT, "tools/pack17/pack17_build.sh")

BACKEND_P, BACKEND_T, BACKEND_TEXT = 0x1D26B4DDD84890A8, 0x3900112D15101288, "precision mediump float;\nvoid main() { gl_FragColor = vec4(1.0); }"

STUB = """#!/usr/bin/env python3
import hashlib, sys
sys.path.insert(0, %r)
import shl
L = shl.read(sys.argv[1])
for t, keys in (('vs', L.vs), ('fs', L.fs)):
    for i, k in enumerate(keys):
        print(t, i, '%%016X' %% int.from_bytes(hashlib.sha256(t.encode() + k).digest()[:8], 'big'), '%%016X' %% int.from_bytes(hashlib.sha256(b'T' + t.encode() + k).digest()[:8], 'big'))
""" % os.path.join(ROOT, "native/tools/seed")


def fake_hash(kind, key):
    return int.from_bytes(hashlib.sha256(kind.encode() + key).digest()[:8], 'big')


def fake_t(kind, key):
    return int.from_bytes(hashlib.sha256(b'T' + kind.encode() + key).digest()[:8], 'big')


def make_list(path, vs, fs, progs):
    L = shl.SList()
    L.vsSize = L.fsSize = 8
    L.gen = 2
    for k in vs:
        L.add_vs(k)
    for k in fs:
        L.add_fs(k)
    for v, f in progs:
        L.add_prog(vs[v], fs[f])
    L.dups = 0
    shl.write(L, path)


def make_pack(path, entries):
    entries = sorted(entries)
    start = 16 + 24 * len(entries)
    data = b""
    head = b""
    for kind, h in entries:
        head += struct.pack("<QIIII", h, kind, start + len(data), 4, 0)
        data += b"GXP0"
    open(path, "wb").write(b"VGXPACK1" + struct.pack("<II", len(entries), start) + head + data)


class Pack17(unittest.TestCase):
    def setUp(self):
        self.t = tempfile.mkdtemp()
        self.stub = os.path.join(self.t, "hashlist")
        open(self.stub, "w").write(STUB)
        os.chmod(self.stub, os.stat(self.stub).st_mode | stat.S_IXUSR)
        self.vs = [bytes([1, i, 0, 0, 0, 0, 0, 0]) for i in range(4)]
        self.fs = [bytes([2, i, 0, 0, 0, 0, 0, 0]) for i in range(5)]
        self.list = os.path.join(self.t, "run.shl")
        make_list(self.list, self.vs, self.fs, [(0, 0), (1, 1), (2, 3), (3, 4)])
        # the pack holds every shader but vs 2 and fs 3, fs 4
        ent = [(0, fake_hash("vs", k)) for i, k in enumerate(self.vs) if i != 2] + [(1, fake_hash("fs", k)) for i, k in enumerate(self.fs) if i not in (3, 4)]
        self.pack = os.path.join(self.t, "x.pack")
        make_pack(self.pack, ent)
        self.log = os.path.join(self.t, "c.log")
        h3 = fake_hash("fs", self.fs[3])
        open(self.log, "w").write(
            "[vgx-vita] shader pack MISS #1: fragment shader %016X asked for at present 3398 is not in the pack (compiled now, or read from the folder cache)\n"
            "[shaderplay] picture 3398: a fragment shader (text hash %016X) took 433.8 ms to compile in play (the pack did not answer it; see the 'shader pack MISS' lines)\n"
            "[shaderplay] picture 3400: a fragment shader (text hash %016X) took 120.0 ms to compile in play (the pack did not answer it; see the 'shader pack MISS' lines)\n"   # a back end program (text log)
            "[vgx-vita] shader pack MISS #2: fragment shader %016X asked for at present 9 is not in the pack\n"   # in the pack: an older pack's miss
            "[vgx-vita] shader pack MISS #3: vertex shader DEADBEEF00000001 asked for at present 12 is not in the pack\n"   # in no list
            "[shaderplay] picture 5: a vertex shader (text hash DEADBEEF00000002) took 9.0 ms to compile in play (the pack did not answer it)\n"   # in no list or text log
            % (h3, fake_t("fs", self.fs[3]), BACKEND_T, fake_hash("fs", self.fs[0])))
        self.texts = os.path.join(self.t, "dump.texts")
        open(self.texts, "w").write("@@ fs %016X %016X %d\n%s\n@@ fs %016X %016X %d\n%s\n" % (BACKEND_P, BACKEND_T, len(BACKEND_TEXT), BACKEND_TEXT, fake_hash("fs", self.fs[0]), fake_t("fs", self.fs[0]), 3, "abc"))

    def run_req(self, out):
        r = subprocess.run([sys.executable, "-I", REQ, "--pack", self.pack, "--hashlist", self.stub, "--console", self.log, "--lists", self.list, "--texts", self.texts, "--out", out],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r

    def test_request_lists_what_the_pack_lacks(self):
        out = os.path.join(self.t, "o1")
        self.run_req(out)
        L = shl.read(os.path.join(out, "pack17-request.shl"))
        self.assertEqual(sorted(L.vs), sorted([self.vs[2], self.vs[3]]))   # the list names a vertex shader the pack lacks; vs 3 comes with the program that uses fs 4 (the warm-up compiles only what the pack lacks)
        self.assertEqual(sorted(L.fs), sorted([self.fs[3], self.fs[4]]))   # one console miss (resolved through the list), one list-only
        self.assertEqual(len(L.prog), 2)                                   # their programs come along
        txt = open(os.path.join(out, "pack17-request.txt")).read()
        self.assertIn("DEADBEEF00000001", txt)                              # unresolved: no list holds it
        self.assertIn("DEADBEEF00000002", txt)                              # unresolved by its text hash too
        self.assertIn("program", txt)                                       # the back end program, found by its T hash in the text log
        self.assertEqual(open(os.path.join(out, "pack17-routes.txt")).read().strip(), "tools/console-1.2/fastboot/routes/fb-dump.txt")
        self.assertIn(BACKEND_TEXT, open(os.path.join(out, "pack17-request-texts.txt")).read())
        self.assertIn("UNRESOLVED", txt)
        self.assertIn("433.8", txt)
        self.assertIn("already answered by this pack (the logs are of an older pack): 1", txt)

    def test_stable_over_two_runs(self):
        a, b = os.path.join(self.t, "a"), os.path.join(self.t, "b")
        self.run_req(a)
        self.run_req(b)
        for f in ("pack17-request.shl", "pack17-request.txt"):
            self.assertEqual(open(os.path.join(a, f), "rb").read(), open(os.path.join(b, f), "rb").read(), f)

    def test_empty_request_when_the_pack_answers_all(self):
        full = os.path.join(self.t, "full.pack")
        make_pack(full, [(0, fake_hash("vs", k)) for k in self.vs] + [(1, fake_hash("fs", k)) for k in self.fs])
        out = os.path.join(self.t, "o2")
        r = subprocess.run([sys.executable, "-I", REQ, "--pack", full, "--hashlist", self.stub, "--lists", self.list, "--out", out], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse(os.path.exists(os.path.join(out, "pack17-request.shl")))

    def test_check_finds_a_shader_the_new_pack_lacks(self):
        out = os.path.join(self.t, "o3")
        self.run_req(out)
        req = os.path.join(out, "pack17-request.shl")
        r = subprocess.run([sys.executable, "-I", CHECK, self.pack, req, self.stub], capture_output=True, text=True)
        self.assertEqual(r.returncode, 1)
        self.assertIn("NOT IN PACK", r.stdout)
        new = os.path.join(self.t, "new.pack")
        make_pack(new, [(0, fake_hash("vs", k)) for k in self.vs] + [(1, fake_hash("fs", k)) for k in self.fs])
        r = subprocess.run([sys.executable, "-I", CHECK, new, req, self.stub, "--base-pack", self.pack], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout)
        # a new pack that dropped an old shader is refused
        shrunk = os.path.join(self.t, "shrunk.pack")
        make_pack(shrunk, [(0, fake_hash("vs", self.vs[2])), (1, fake_hash("fs", self.fs[3])), (1, fake_hash("fs", self.fs[4]))])
        r = subprocess.run([sys.executable, "-I", CHECK, shrunk, req, self.stub, "--base-pack", self.pack], capture_output=True, text=True)
        self.assertEqual(r.returncode, 1)

    def test_build_script_dry_run_and_no_compiler_in_repo(self):
        base = os.path.join(self.t, "base")
        os.mkdir(base)
        os.rename(self.pack, os.path.join(base, "874a13e0.pack"))
        open(os.path.join(base, "vgx_shaderlist.bin"), "wb").write(open(self.list, "rb").read())
        lab = os.path.join(self.t, "lab.vpk")
        open(lab, "wb").write(b"PK")
        r = subprocess.run(["bash", BUILD, "-b", base, "-r", self.list, "-l", lab, "-o", os.path.join(self.t, "out"), "-n"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("dry run", r.stdout)
        # nothing named libshacccg is part of the tools
        for dp, _dn, fn in os.walk(os.path.join(ROOT, "tools/pack17")):
            for f in fn:
                self.assertNotIn("libshacccg", f.lower())


if __name__ == "__main__":
    unittest.main()
