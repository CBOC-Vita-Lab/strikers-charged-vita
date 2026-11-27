"""Safety contracts for generated hardware-validation source and VPKs."""
import hashlib
import ast
import importlib.util
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
import warnings
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/stage-hardware-test.py"
SETTINGS = "native/platform/vita/src/np_vita_settings.cpp"


def sfo(title="CDXMP0001", content="EP9000-CDXMP0001_00-HARDWARETEST0000"):
    values = {"TITLE_ID": title, "CONTENT_ID": content}
    keys = bytearray()
    data = bytearray()
    entries = bytearray()
    for key, value in values.items():
        raw = value.encode() + b"\0"
        entries += struct.pack("<HHIII", len(keys), 0x204, len(raw), len(raw), len(data))
        keys += key.encode() + b"\0"
        data += raw
    return struct.pack("<4sIIII", b"\0PSF", 0x101, 20 + len(entries), 20 + len(entries) + len(keys), len(values)) + entries + keys + data


def compiled_fixture(extra=b"", compressed=False):
    strings = b"\0" * 92 + (b"CDXMP0001\0ux0:data/mscharged-perf-test/np-data\0"
               b"ux0:/data/mscharged-perf-test\0/data/mscharged-perf-test\0"
               b"ux0:data/mscharged-perf-test/ffp-cache/v1/v\0ux0:data/mscharged/game\0") + extra
    elf_header = struct.pack("<16sHHIIIIIHHHHHH", b"\x7fELF\x01\x01\x01" + b"\0" * 9, 0xFE00, 40, 1, 0x81000000, 52, 0, 0x5000000, 52, 32, 1, 0, 0, 0)
    phdr = struct.pack("<8I", 1, 84, 0x81000000, 0, len(strings), len(strings), 5, 16)
    velf = elf_header + phdr + strings
    transformed = bytearray(velf)
    nid = int.from_bytes(hashlib.sha256(hashlib.sha256(velf).digest()).digest()[:4], "big")
    struct.pack_into("<I", transformed, 84 + 52, nid)
    strings = bytes(transformed[84:])
    payload = zlib.compress(strings) if compressed else strings
    payload += b"\0" * ((-len(payload)) % 4)
    header = struct.pack("<4sIHHI14Q", b"SCE\0", 3, 0xC0, 1, 0x600,
                         4096, len(velf), 4096 + len(payload), 0, 4, 128, 160, 224, 0, 256, 288, 304, 80, 0)
    container = bytearray(header.ljust(4096, b"\0") + payload)
    container[160:212] = elf_header
    container[224:256] = phdr
    container[256:288] = struct.pack("<4Q", 4096, len(payload), 2 if compressed else 1, 2)
    container[304:320] = struct.pack("<4I", 4, 80, 0, 0)
    container[340:372] = hashlib.sha256(transformed).digest()
    return velf, bytes(container)


class HardwareStageTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(TOOL.exists(), "hardware staging tool is missing")
        spec = importlib.util.spec_from_file_location("hardware_stage", TOOL)
        self.tool = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.tool)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name).resolve()

    def fixture(self):
        source = self.base / "source"
        source.mkdir()
        shutil.copyfile(ROOT / "THIRD_PARTY_NOTICES.md", source / "THIRD_PARTY_NOTICES.md")
        for name in self.tool.REQUIRED:
            path = source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, path)
        for name, text in {"native/platform/compat/np_touchdiag.cpp": 'const char* p = "ux0:data/mscharged";\nconst char* status = "title=MSCN00003\\n";\n', "renderer/example.cpp": 'const char* p = "ux0:/data/mscharged/shots/a.png";\n'}.items():
            path = source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        path = source / "native/platform/vita/src/np_vita_internal.h"
        path.write_text('inline constexpr const char* kDataDirectory = "/data/mscharged";\nconst char* unrelated = "ux0:data/mscharged-other";\n')
        path = source / "dependencies/vitaGL/source/gxm.c"
        path.write_text('const char* crash = "ux0:data/vitagl_gpucrash.sgx";\n')
        return source

    def test_stage_is_deterministic_private_and_preserves_input(self):
        source = self.fixture()
        before = (source / self.tool.MAIN).read_bytes()
        first, second = self.base / "one", self.base / "two"
        self.tool.stage(source, first)
        self.tool.stage(source, second)
        self.assertEqual(before, (source / self.tool.MAIN).read_bytes())
        self.assertEqual((first / "hardware-isolation.json").read_bytes(), (second / "hardware-isolation.json").read_bytes())
        main = (first / self.tool.MAIN).read_text()
        self.assertIn("NpHardwareEnvScope hardware_scope;", main)
        self.assertIn("NpHardwareAccept(line, equals + 1)", main)
        self.assertNotIn("NpSaveImportRecompiled(", main)
        self.assertIn('"ux0:data/mscharged/game"', main)
        sdk = (first / self.tool.OS).read_text()
        self.assertIn('c->data_dir = "ux0:data/mscharged-perf-test/np-data";', sdk)
        nand = (first / self.tool.NAND).read_text()
        self.assertLess(nand.index("NpHardwareDataAllowed("), nand.index("np_mkdir_p(g_root)"))
        for name in self.tool.CACHES:
            text = (first / name).read_text()
            self.assertNotIn('"ux0:data/shader_cache', text)
            self.assertIn("ux0:data/mscharged-perf-test/ffp-cache", text)
        mk = (first / self.tool.MAKE).read_text()
        for value in ("override NP_TITLE_ID := CDXMP0001", "override NP_PLAYER := 1", "override NP_LOGGING := 0", "override LAB := 0"):
            self.assertIn(value, mk)
        self.assertIn("mscharged-perf-test", (first / "native/platform/compat/np_touchdiag.cpp").read_text())
        self.assertIn("title=CDXMP0001", (first / "native/platform/compat/np_touchdiag.cpp").read_text())
        self.assertIn("-s CONTENT_ID=EP9000-CDXMP0001_00-HARDWARETEST0000", (first / self.tool.VPK_MAKE).read_text())
        internal = (first / "native/platform/vita/src/np_vita_internal.h").read_text()
        self.assertIn('kDataDirectory = "/data/mscharged-perf-test"', internal)
        self.assertIn('"ux0:data/mscharged-other"', internal)
        self.assertIn('"ux0:data/mscharged-perf-test/gpucrash.sgx"', (first / "dependencies/vitaGL/source/gxm.c").read_text())
        self.assertNotIn('"ux0:data/vgl_cache', (first / "dependencies/vitaGL/source/vgl.c").read_text())
        self.assertTrue((first / "THIRD_PARTY_NOTICES.md").is_file())
        self.assertEqual((source / "THIRD_PARTY_NOTICES.md").read_bytes(), (first / "THIRD_PARTY_NOTICES.md").read_bytes())

    def test_logging_option_changes_only_the_profile(self):
        source = self.fixture()
        quiet, loud = self.base / "quiet", self.base / "loud"
        self.tool.stage(source, quiet)
        self.tool.stage(source, loud, logging=True)
        self.assertIn("override NP_LOGGING := 0", (quiet / self.tool.MAKE).read_text())
        self.assertIn("override NP_LOGGING := 1", (loud / self.tool.MAKE).read_text())
        self.assertIn("override NP_TITLE_ID := CDXMP0001", (loud / self.tool.MAKE).read_text())
        self.assertEqual(json.loads((loud / "hardware-isolation.json").read_text())["profile"], {"NP_PLAYER": 1, "NP_LOGGING": 1, "LAB": 0})
        for name in (self.tool.MAIN, self.tool.OS, self.tool.HEADER):
            self.assertEqual((quiet / name).read_bytes(), (loud / name).read_bytes())

    def test_guard_compiles_and_blocks_overrides_on_reread(self):
        source = self.fixture()
        dest = self.base / "stage"
        self.tool.stage(source, dest)
        cpp = self.base / "guard.cpp"
        cpp.write_text('#include "np_hardware_isolation.h"\n#include <cassert>\nint main(){\n'
                       'const char* bad[]={"ux0:data/mscharged/NAND","/tmp/x","../x","ux0:data/mscharged-perf-test/../mscharged/x","ux0:data/mscharged-perf-test//x","ux0:data/mscharged-perf-test/./x"};\n'
                       'for(auto p:bad) assert(!NpHardwareAccept("MSC_VGX_GLSTATS",p));\n'
                       'assert(NpHardwareAccept("MSC_VGX_GLSTATS","ux0:data/mscharged-perf-test/gl.csv"));\n'
                       'assert(NpHardwareAccept("NP_DET_TRACE","ux0:data/mscharged-perf-test/det.txt"));\n'
                       'assert(!NpHardwareAccept("NP_DET_TRACE","ux0:data/mscharged/det.txt"));\n'
                       'assert(NpHardwareAccept("MSC_VGX_SHADERTEXT_LOG","ux0:data/mscharged-perf-test/texts.txt"));\n'
                       'assert(!NpHardwareAccept("MSC_VGX_SHADERTEXT_LOG","ux0:data/mscharged/texts.txt"));\n'
                       'assert(!NpHardwareAccept("NP_DATA_DIR","ux0:data/mscharged"));\n'
                       'assert(!NpHardwareAccept("NEW_UNKNOWN_OUTPUT","/tmp/x"));\n'
                       'assert(NpHardwareAccept("MSC_VITA_RENDER","800x450"));\n'
                       'assert(NpHardwareAccept("MSC_VGX_CAPTURE_LIST","1-60,80-90"));\n'
                       'assert(NpHardwareAccept("MSC_DUMP_LIST","1-60"));\n'
                       'assert(!NpHardwareAccept("MSC_VGX_CAPTURE_LIST","ux0:data/mscharged-perf-test/ranges"));\n'
                       'assert(!NpHardwareAccept("MSC_DUMP_LIST","ux0:data/mscharged-perf-test/ranges"));\n'
                       'for(int i=0;i<2;i++){NpHardwareEnvScope scope;setenv("NP_DATA_DIR","/tmp",1);setenv("NP_IMPORT_SAVE","1",1);}\n'
                       'assert(NpHardwareDataAllowed(getenv("NP_DATA_DIR")));\n'
                       'assert(!NpHardwareDataAllowed("ux0:data/mscharged-perf-test/np-data/../escape"));\n'
                       'assert(!strcmp(getenv("NP_IMPORT_SAVE"),"0"));\n'
                       'assert(!strcmp(getenv("MSC_GAME_ROOT"),"ux0:data/mscharged/game"));}\n')
        subprocess.run(["c++", "-std=c++11", "-Wall", "-Werror", "-I", str(dest / Path(self.tool.MAIN).parent), str(cpp), "-o", str(self.base / "guard")], check=True, capture_output=True)
        subprocess.run([str(self.base / "guard")], check=True, capture_output=True)

    def test_staged_settings_shot_accepts_only_bounded_ascii_basenames(self):
        source = self.fixture()
        dest = self.base / "stage"
        self.tool.stage(source, dest)
        settings = (dest / SETTINGS).read_text()
        start = settings.index("void Shot(const char* name, const uint32_t* pixels) {")
        shot = settings[start:settings.index("\n}\n", start) + 2]
        cpp = self.base / "settings-shot.cpp"
        cpp.write_text(r'''#include "np_hardware_isolation.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
static std::vector<std::string> writes;
static int mkdirs = 0, failures = 0, checks = 0;
int sceIoMkdir(const char*, int) { ++mkdirs; return 0; }
bool WritePlain(const char* path, const std::string&) { writes.push_back(path); return true; }
''' + shot + r'''
static void Check(const char* label, const char* name, bool allowed) {
    static std::vector<uint32_t> pixels(960 * 544, 0x123456);
    writes.clear(); mkdirs = 0;
    Shot(name, pixels.data());
    bool ok = writes.size() == unsigned(allowed) && mkdirs == int(allowed);
    if (allowed && !writes.empty()) {
        std::string path = writes[0];
        if (path.compare(0, 5, "ux0:/") == 0) path.erase(4, 1);
        ok = ok && NpHardwarePrivatePath(path.c_str());
        ok = ok && path == std::string("ux0:data/mscharged-perf-test/shots/settings-") + name + ".ppm";
    }
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s: writes=%zu mkdirs=%d\n", label, writes.size(), mkdirs); }
}
int main() {
    Check("normal", "menu-01_OK", true);
    const size_t maximum = 159 - std::strlen("ux0:data/mscharged-perf-test/shots/settings-") - std::strlen(".ppm");
    Check("exact path limit", std::string(maximum, 'a').c_str(), true);
    Check("path truncation", std::string(maximum + 1, 'a').c_str(), false);
    Check("very long", std::string(2000, 'a').c_str(), false);
    Check("null", nullptr, false);
    Check("empty", "", false);
    Check("dot", ".", false);
    Check("parent", "..", false);
    Check("traversal", "x/../../../mscharged/isolation-canary", false);
    Check("forward separator", "a/b", false);
    Check("back separator", "a\\b", false);
    Check("device", "ux0:escape", false);
    Check("space", "two words", false);
    Check("newline", "line\nbreak", false);
    Check("tab", "tab\tname", false);
    Check("control", "a\x01", false);
    Check("delete", "a\x7f", false);
    Check("non-ascii", "\xc3\xa9", false);
    std::printf("settings shot: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
''')
        subprocess.run(["c++", "-std=c++11", "-Wall", "-Werror", "-I", str(dest / Path(self.tool.MAIN).parent), str(cpp), "-o", str(self.base / "settings-shot")], check=True, capture_output=True)
        result = subprocess.run([str(self.base / "settings-shot")], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("settings shot: 18 checks, 0 failed\n", result.stdout)

    def test_staging_refuses_missing_or_unsupported_settings_shot_hook(self):
        source = self.fixture()
        path = source / SETTINGS
        original = path.read_text()
        for label, old, new in (
            ("signature", "void Shot(", "void ChangedShot("),
            ("buffer", "char path[160];", "char path[80];"),
            ("format", '"ux0:/data/mscharged/shots/settings-%s.ppm"', '"ux0:/data/mscharged/shots/other-%s.ppm"'),
        ):
            with self.subTest(label=label):
                path.write_text(original.replace(old, new, 1))
                dest = self.base / ("bad-shot-" + label)
                with self.assertRaises(ValueError):
                    self.tool.stage(source, dest)
                self.assertFalse(dest.exists())
        path.unlink()
        with self.assertRaises(ValueError):
            self.tool.stage(source, self.base / "missing-settings")
        self.assertFalse((self.base / "missing-settings").exists())

    def test_rejects_missing_source_existing_destination_and_symlinks(self):
        source = self.fixture()
        for dest in (source, source / "nested", self.base):
            with self.assertRaises(ValueError):
                self.tool.stage(source, dest)
        link = self.base / "linked"
        link.symlink_to(self.base, target_is_directory=True)
        with self.assertRaises(ValueError):
            self.tool.stage(source, link / "stage")
        (source / "native/escape").symlink_to(ROOT)
        with self.assertRaises(ValueError):
            self.tool.stage(source, self.base / "out")
        (source / "native/escape").unlink()
        (source / self.tool.NAND).unlink()
        with self.assertRaises(ValueError):
            self.tool.stage(source, self.base / "out2")

    def test_rejects_top_level_symlink_and_protected_destination(self):
        source = self.fixture()
        outside = self.base / "outside-native"
        (source / "native").rename(outside)
        (source / "native").symlink_to(outside, target_is_directory=True)
        with self.assertRaises(ValueError):
            self.tool.stage(source, self.base / "out")
        (source / "native").unlink()
        outside.rename(source / "native")
        for name in ("MSCN00003", "mscharged"):
            with self.assertRaises(ValueError):
                self.tool.stage(source, self.base / name)

    def test_private_disc_mode_is_exact_and_manifested(self):
        source = self.fixture()
        dest = self.base / "fault-stage"
        manifest = self.tool.stage(source, dest, private_disc=True)
        self.assertEqual("ux0:data/mscharged-perf-test/game", manifest["read_only_disc"])
        sdk = (dest / self.tool.OS).read_text()
        self.assertIn('c->disc_dir = "ux0:data/mscharged-perf-test/game";', sdk)
        self.assertNotIn('c->disc_dir = "ux0:data/mscharged/game";', sdk)

    def archive(self, name, members):
        path = self.base / name
        with zipfile.ZipFile(path, "w") as out:
            for member, payload in members:
                out.writestr(member, payload)
        return path

    def package_inputs(self):
        exe = b"SCE\0isolated-test-eboot"
        built = self.archive("built.vpk", [("eboot.bin", exe), ("sce_sys/param.sfo", sfo()), ("licenses/test.txt", b"license")])
        donor = self.archive("donor.vpk", [("eboot.bin", b"old"), ("sce_sys/param.sfo", sfo("MSCN00003", "EP9000-MSCN00003_00-PRODUCTION000000")), ("sce_sys/icon0.png", b"art"), ("shadercache/cache.bin", b"shader"), ("game/files/private", b"must not copy")])
        return built, donor, hashlib.sha256(exe).hexdigest()

    def test_package_retains_built_sfo_and_only_approved_donor_members(self):
        built, donor, digest = self.package_inputs()
        out = self.base / "test.vpk"
        result = self.tool.package(built, donor, out, digest)
        self.assertEqual("CDXMP0001", result["title_id"])
        self.assertEqual(digest, result["eboot_sha256"])
        with zipfile.ZipFile(out) as z:
            self.assertEqual(sfo(), z.read("sce_sys/param.sfo"))
            self.assertNotIn("game/files/private", z.namelist())
            self.assertEqual(b"shader", z.read("shadercache/cache.bin"))
        self.assertEqual(result, self.tool.verify(out, digest))
        other = self.base / "test2.vpk"
        self.tool.package(built, donor, other, digest)
        self.assertEqual(out.read_bytes(), other.read_bytes())

    def test_livearea_layout_is_the_builds_not_the_donors(self):
        # front20: the Settings tile is gone; the donor's 1.2 template (with the tile) must not come back through the package
        exe = b"SCE\0isolated-test-eboot"
        tpl = "sce_sys/livearea/contents/template.xml"
        built = self.archive("built-t.vpk", [("eboot.bin", exe), ("sce_sys/param.sfo", sfo()), (tpl, b"<new/>")])
        donor = self.archive("donor-t.vpk", [("eboot.bin", b"old"), ("sce_sys/param.sfo", sfo("MSCN00003", "EP9000-MSCN00003_00-PRODUCTION000000")), (tpl, b"<old-with-tile/>"), ("sce_sys/icon0.png", b"art"), ("shadercache/cache.bin", b"shader")])
        out = self.base / "tpl.vpk"
        self.tool.package(built, donor, out, hashlib.sha256(exe).hexdigest())
        with zipfile.ZipFile(out) as z:
            self.assertEqual(b"<new/>", z.read(tpl))
            self.assertEqual(b"art", z.read("sce_sys/icon0.png"))
        # a build without a template keeps the donor's
        built2 = self.archive("built-t2.vpk", [("eboot.bin", exe), ("sce_sys/param.sfo", sfo())])
        out2 = self.base / "tpl2.vpk"
        self.tool.package(built2, donor, out2, hashlib.sha256(exe).hexdigest())
        with zipfile.ZipFile(out2) as z:
            self.assertEqual(b"<old-with-tile/>", z.read(tpl))

    def test_verifier_rejects_stale_malformed_sfo_and_wrong_eboot(self):
        for index, blob in enumerate((sfo("MSCN00003"), sfo(content="EP9000-MSCN00003_00-PRODUCTION000000"), b"bad", sfo()[:22])):
            path = self.archive(f"bad{index}.vpk", [("eboot.bin", b"SCE\0x"), ("sce_sys/param.sfo", blob)])
            with self.assertRaises(ValueError):
                self.tool.verify(path, hashlib.sha256(b"SCE\0x").hexdigest())
        built, _, _ = self.package_inputs()
        with self.assertRaises(ValueError):
            self.tool.verify(built, "0" * 64)

    def test_public_art_members_are_preserved_including_settings(self):
        self.assertTrue(self.tool.donor_allowed("sce_sys/livearea/contents/settings.png"))

    def test_rejects_reisolating_sources_and_missing_transform_anchors(self):
        source = self.fixture()
        path = source / self.tool.MAIN
        path.write_text(path.read_text().replace("void LoadEnvFile() {", "void ChangedShape() {"))
        with self.assertRaises(ValueError):
            self.tool.stage(source, self.base / "bad-shape")
        self.assertFalse((self.base / "bad-shape").exists())

    def test_compiled_gate_validates_velf_self_and_staged_sources(self):
        self.assertTrue(hasattr(self.tool, "verify_build"), "compiled source/SELF gate is missing")
        source = self.fixture()
        staged = self.base / "stage"
        self.tool.stage(source, staged)
        for compressed in (False, True):
            velf, eboot = compiled_fixture(compressed=compressed)
            elf_path = self.base / "build.velf"
            elf_path.write_bytes(velf)
            path = self.archive(f"compiled-{compressed}.vpk", [("eboot.bin", eboot), ("sce_sys/param.sfo", sfo())])
            result = self.tool.verify_build(path, hashlib.sha256(eboot).hexdigest(), elf_path, staged / "hardware-isolation.json")
            self.assertEqual("compiled-and-source-verified", result["isolation_gate"])
            self.assertEqual(hashlib.sha256(velf).hexdigest(), result["velf_sha256"])
        (staged / self.tool.NAND).write_text("tampered")
        with self.assertRaises(ValueError):
            self.tool.verify_build(path, hashlib.sha256(eboot).hexdigest(), elf_path, staged / "hardware-isolation.json")

    def test_compiled_gate_allows_real_disc_repair_message_not_writable_roots(self):
        source = self.fixture()
        staged = self.base / "stage"
        self.tool.stage(source, staged)
        runtime = (ROOT / "native/platform/compat/np_disc_error.cpp").read_text()
        format_block = runtime.split("std::snprintf(details, sizeof details,", 1)[1].split(", error);", 1)[0]
        message = "".join(ast.literal_eval(token) for token in re.findall(r'"(?:\\.|[^"\\])*"', format_block))
        self.assertIn("ux0:data/mscharged/game using the installer", message)
        elf_path = self.base / "repair.velf"
        for index, extra in enumerate((message.encode() + b"\0",
                                       b"Copy assets to ux0:data/mscharged/game/sys/ (boot.bin, bi2.bin).\0")):
            velf, eboot = compiled_fixture(extra)
            elf_path.write_bytes(velf)
            path = self.archive(f"repair-{index}.vpk", [("eboot.bin", eboot), ("sce_sys/param.sfo", sfo())])
            result = self.tool.verify_build(path, hashlib.sha256(eboot).hexdigest(), elf_path, staged / "hardware-isolation.json")
            self.assertEqual("compiled-and-source-verified", result["isolation_gate"])
        for index, extra in enumerate((b"ux0:data/mscharged/game/../NAND repair\0",
                                       b"ux0:data/mscharged/game /../NAND repair\0",
                                       b"ux0:data/mscharged/game//sys repair\0",
                                       b"ux0:data/mscharged/NAND using installer\0",
                                       b"ux0:data/mscharged using installer\0")):
            velf, eboot = compiled_fixture(extra)
            elf_path.write_bytes(velf)
            path = self.archive(f"repair-bad-{index}.vpk", [("eboot.bin", eboot), ("sce_sys/param.sfo", sfo())])
            with self.assertRaises(ValueError):
                self.tool.verify_build(path, hashlib.sha256(eboot).hexdigest(), elf_path, staged / "hardware-isolation.json")

    def test_compiled_gate_rejects_protected_paths_titles_and_mismatched_self(self):
        self.assertTrue(hasattr(self.tool, "verify_build"), "compiled source/SELF gate is missing")
        source = self.fixture()
        staged = self.base / "stage"
        self.tool.stage(source, staged)
        for index, extra in enumerate((b"ux0:data/mscharged/NAND\0", b"/data/mscharged\0", b"ux0:/data/mscharged/log.txt\0", b"ux0:data/shader_cache/v1\0", b"MSCN00003\0")):
            velf, eboot = compiled_fixture(extra)
            elf_path = self.base / "bad.velf"
            elf_path.write_bytes(velf)
            path = self.archive(f"compiled-bad{index}.vpk", [("eboot.bin", eboot), ("sce_sys/param.sfo", sfo())])
            with self.assertRaises(ValueError):
                self.tool.verify_build(path, hashlib.sha256(eboot).hexdigest(), elf_path, staged / "hardware-isolation.json")
        velf, eboot = compiled_fixture()
        elf_path.write_bytes(velf)
        damaged = eboot[:-1] + bytes([eboot[-1] ^ 1])
        path = self.archive("mismatch.vpk", [("eboot.bin", damaged), ("sce_sys/param.sfo", sfo())])
        with self.assertRaises(ValueError):
            self.tool.verify_build(path, hashlib.sha256(damaged).hexdigest(), elf_path, staged / "hardware-isolation.json")

    def test_zip_rejects_duplicates_traversal_symlink_and_unexpected_payloads(self):
        _, _, digest = self.package_inputs()
        for index, name in enumerate(("../escape", "/absolute", "game/files/a", "NAND/save", "sce_sys/../eboot.bin", "C:\\escape")):
            path = self.archive(f"escape{index}.vpk", [("eboot.bin", b"SCE\0isolated-test-eboot"), ("sce_sys/param.sfo", sfo()), (name, b"x")])
            with self.assertRaises(ValueError):
                self.tool.verify(path, digest)
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            duplicate = self.archive("duplicate.vpk", [("eboot.bin", b"x"), ("eboot.bin", b"y")])
        with self.assertRaises(ValueError):
            self.tool.verify(duplicate, digest)
        bad = self.base / "notzip.vpk"
        bad.write_bytes(b"not a zip")
        with self.assertRaises(ValueError):
            self.tool.verify(bad, digest)
        symlink = self.base / "symlink.vpk"
        with zipfile.ZipFile(symlink, "w") as z:
            info = zipfile.ZipInfo("shadercache/link")
            info.create_system = 3
            info.external_attr = 0o120777 << 16
            z.writestr(info, "../../game")
        with self.assertRaises(ValueError):
            self.tool.verify(symlink, digest)

    def test_zip_rejects_nul_truncation_and_pipe_without_reading(self):
        path = self.archive("nul.vpk", [("evilXfile", b"x")])
        path.write_bytes(path.read_bytes().replace(b"evilXfile", b"evil\0file"))
        with self.assertRaises(ValueError):
            self.tool.read_zip(path)
        pipe = self.base / "pipe.vpk"
        import os
        os.mkfifo(pipe)
        with self.assertRaises(ValueError):
            self.tool.read_zip(pipe)


if __name__ == "__main__":
    unittest.main()
