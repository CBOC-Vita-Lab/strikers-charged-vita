"""The Vita link's --require-defined list (native/platform/vita/vita_require.mk) against the sources.

A weak reference never pulls an object out of an archive; the symbols in that list are the ones the game reaches only through weak declarations and that live in
an archive (libnpsdk, libnpgx, libvgx-native, libvitaGL). The list must stay true:
  - every symbol is still declared weak somewhere in native/ or renderer/ (a name left over after a rename would fail the real link);
  - every symbol has a definition in the sources of the archives (native/platform/sdk/src, sdk/vita, gx, renderer, dependencies/vitaGL/source), so the link can succeed;
  - nothing in the list is defined in a directory whose objects are linked directly (always present: the entry would be noise).
And a new weak declaration whose definition is in an archive directory must be listed or named in ALLOWED_OPTIONAL with a reason.
"""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
MK = ROOT / "native/platform/vita/vita_require.mk"
EXT = {".c", ".cc", ".cpp", ".h", ".hpp"}
SKIP = {"tests", "tools", "test"}
ARCHIVE_DIRS = ["native/platform/sdk/src", "native/platform/sdk/vita", "native/platform/gx/src", "renderer/src", "renderer/vita", "renderer/native", "dependencies/vitaGL/source"]
SOURCE_DIRS = ["native", "renderer"]

# weak references to archive members that are NOT required, with the reason
ALLOWED_OPTIONAL = {
    "NpThpLogStats": "diagnostic line of the harness build (np_harness.cpp)",
    "np_gx_request_dump": "picture dump of the `shot` script command (np_harness.cpp, host pictures only; absent on the console)",
}


def sources(dirs):
    for d in dirs:
        for p in sorted((ROOT / d).rglob("*")):
            if p.suffix in EXT and p.is_file() and not (set(p.relative_to(ROOT).parts) & SKIP):
                yield p


def required():
    text = MK.read_text()
    m = re.search(r"NP_REQUIRE_SYMS\s*:=\s*((?:[^\n]*\\\n)*[^\n]*)", text)
    return m.group(1).replace("\\", " ").split()


WEAK_DECL = re.compile(r'__attribute__\(\(weak\)\)|#pragma weak|__asm__\("(\w+)"\)')


def weak_names():
    """Names (C symbol) the sources declare weak: the declared identifier, and the asm label when there is one."""
    out = {}
    for p in sources(SOURCE_DIRS):
        for line in p.read_text(errors="replace").splitlines():
            if "weak" not in line:
                continue
            m = re.search(r'#pragma weak\s+(\w+)', line)
            if m:
                out.setdefault(m.group(1), str(p.relative_to(ROOT)))
                continue
            if "__attribute__((weak))" not in line:
                continue
            asm = re.search(r'(?:__asm__|asm)\s*\(\s*"(\w+)"\s*\)', line)
            if asm:
                out.setdefault(asm.group(1), str(p.relative_to(ROOT)))
            m = re.search(r'(\w+)\s*\([^;{]*\)\s*(?:__asm__|asm|__attribute__)', line)
            if m:
                out.setdefault(m.group(1), str(p.relative_to(ROOT)))
    return out


def defined_in(dirs, name):
    pat = re.compile(r'^[A-Za-z_][^;\n]*?[\s\*&]' + re.escape(name) + r'\s*\(')
    for p in sources(dirs):
        for line in p.read_text(errors="replace").splitlines():
            if pat.search(line) and "weak" not in line and not line.rstrip().endswith(";") and not line.lstrip().startswith("//"):
                return str(p.relative_to(ROOT))
    return None


class LinkRequires(unittest.TestCase):
    def test_list_parses(self):
        syms = required()
        self.assertGreater(len(syms), 60)
        self.assertEqual(len(syms), len(set(syms)), "duplicates")

    def test_every_symbol_is_declared_weak_and_defined_in_an_archive_directory(self):
        weak = weak_names()
        bad = []
        for s in required():
            plain = s
            if s.startswith("_Z"):   # mangled in the list: the last identifier before the parameters
                plain = re.match(r"_ZN3vgx(\d+)(\w+)", s)
                n = int(plain.group(1))
                plain = plain.group(2)[:n]
            if plain not in weak:
                bad.append(f"{s}: no weak declaration in the sources (stale entry?)")
            if not defined_in(ARCHIVE_DIRS, plain):
                bad.append(f"{s}: no definition in {ARCHIVE_DIRS}")
        self.assertEqual(bad, [], "\n".join(bad))

    def test_new_weak_reference_to_an_archive_member_is_decided(self):
        listed = set()
        for s in required():
            if s.startswith("_ZN3vgx"):
                n = int(re.match(r"_ZN3vgx(\d+)", s).group(1))
                listed.add(re.match(r"_ZN3vgx\d+(\w+)", s).group(1)[:n])
            else:
                listed.add(s)
        undecided = [f"{n} ({f}) -> {d}" for n, f in sorted(weak_names().items())
                     if n not in listed and n not in ALLOWED_OPTIONAL and (d := defined_in(ARCHIVE_DIRS, n))]
        self.assertEqual(undecided, [], "weak reference(s) to a definition in an archive: add to vita_require.mk (must be linked) or to ALLOWED_OPTIONAL with the reason:\n" + "\n".join(undecided))

    def test_makefile_uses_the_list_for_the_game_image_only(self):
        mk = (ROOT / "native/platform/vita/Makefile.vpk").read_text()
        self.assertIn("vita_require.mk", mk)
        self.assertRegex(mk, r"mscharged-np\.elf: NP_REQUIRE := \$\(NP_REQUIRE_FLAGS\)")
        self.assertNotRegex(mk, r"np-smoke\.elf: NP_REQUIRE")


class ElfHooks(unittest.TestCase):
    """native/platform/vita/tests/check_elf_hooks.py (the post-link nm check of the multiplayer / Nearby / settings hooks)."""

    @classmethod
    def setUpClass(cls):
        import importlib.util
        spec = importlib.util.spec_from_file_location("check_elf_hooks", ROOT / "native/platform/vita/tests/check_elf_hooks.py")
        cls.mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.mod)

    def test_every_hook_is_still_declared_weak_and_defined_in_directly_linked_sources(self):
        weak = weak_names()
        bad = []
        for h in self.mod.HOOKS:
            if h not in weak:
                bad.append(f"{h}: no weak declaration any more (stale entry, or the hook became a hard reference: drop it)")
            if not defined_in(["native/platform/sdk/net", "native/platform/sdk/dwc", "native/platform/sdk/input", "native/platform/vita/src"], h):
                bad.append(f"{h}: no definition in the directly linked Vita sources")
        self.assertEqual(bad, [], "\n".join(bad))

    def test_parser_reads_nm_output(self):
        nm = "00001000 T NpVitaSettingsRequestOpen\n00001010 W np_vita_radio_rx_dropped\n         U np_vita_radio_ring_take_peak\n00001020 t np_vita_radio_system_init\n"
        self.assertEqual(self.mod.defined_symbols(nm), {"NpVitaSettingsRequestOpen", "np_vita_radio_rx_dropped", "np_vita_radio_system_init"})
        hooks = {"NpVitaSettingsRequestOpen": ("a", None), "np_vita_radio_ring_take_peak": ("b", None), "gone": ("c", "WITNESS")}
        self.assertEqual(self.mod.missing(nm, b"nothing here", hooks), ["np_vita_radio_ring_take_peak", "gone"])   # an undefined (U) reference does not count
        self.assertEqual(self.mod.missing(nm, b"..WITNESS..", hooks), ["np_vita_radio_ring_take_peak"])   # inlined by LTO: the witness string saves it

    def test_witnesses_are_in_the_sources(self):
        # the witness must be a literal of the object that defines the hook, or the check would pass for the wrong reason
        homes = {"np_vita_radio_rx_dropped": "native/platform/sdk/net/vlink_vita_radio.cpp", "np_vita_radio_ring_take_peak": "native/platform/sdk/net/vlink_vita_radio.cpp",
                 "np_vita_radio_dialog_active": "native/platform/sdk/net/vlink_vita_radio.cpp", "np_vita_radio_system_init": "native/platform/sdk/net/vlink_vita_radio.cpp",
                 "NpLanOnlineSessionTuning": "native/platform/sdk/net/np_lan.cpp"}
        for h, (_, witness) in self.mod.HOOKS.items():
            if witness:
                self.assertIn('"' + witness, (ROOT / homes[h]).read_text(), h)

    def test_mutant_dropped_hook_is_reported(self):
        # an ELF without the object: no symbol, no witness -> reported; with only the witness (LTO inlined it) -> fine
        full = "".join(f"0000 T {h}\n" for h in self.mod.HOOKS)
        self.assertEqual(self.mod.missing(full, b""), [])
        for h, (_, witness) in self.mod.HOOKS.items():
            nm = full.replace(f"0000 T {h}\n", "")
            self.assertEqual(self.mod.missing(nm, b""), [h], h)
            if witness:
                self.assertEqual(self.mod.missing(nm, witness.encode()), [], h)


if __name__ == "__main__":
    unittest.main()
