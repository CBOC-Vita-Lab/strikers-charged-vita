import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools/build-mbedtls.py"


def digest(data):
    return hashlib.sha256(data).hexdigest()


class MbedTlsBuildTests(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location("build_mbedtls", SCRIPT)
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.project = self.root / "project"
        self.source = self.root / "source"
        self.destination = self.root / "work/overlays/tls13"
        self.prefix = self.root / "work/mbedtls/host32"
        self.original = b"int tls13_setup(void) {\n    return 1;\n}\n"
        self.patched = self.original.replace(b"return 1", b"return 0")
        self.input = self.source / "library/ssl_tls13_keys.c"
        self.input.parent.mkdir(parents=True)
        self.input.write_bytes(self.original)
        self.patch = self.project / "dependencies/mbedtls/patches/cleanup.patch"
        self.patch.parent.mkdir(parents=True)
        self.patch.write_text("--- a/library/ssl_tls13_keys.c\n+++ b/library/ssl_tls13_keys.c\n"
                              "@@ -1,3 +1,3 @@\n int tls13_setup(void) {\n-    return 1;\n+    return 0;\n }\n")
        self.row = {"name": "Mbed-TLS_mbedtls", "commit": "a" * 40, "patches": [{
            "path": "dependencies/mbedtls/patches/cleanup.patch",
            "sha256": digest(self.patch.read_bytes()), "upstream_commit": "b" * 40,
            "source": "library/ssl_tls13_keys.c", "before_sha256": digest(self.original),
            "after_sha256": digest(self.patched)}]}
        self.root_patch = mock.patch.object(self.module, "ROOT", self.project)
        self.root_patch.start()
        self.addCleanup(self.root_patch.stop)

    def prepare(self):
        self.assertTrue(callable(getattr(self.module, "prepare_overlay", None)),
                        "the builder must prepare a verified immutable one-file overlay")
        return self.module.prepare_overlay(self.source, self.destination, self.row)

    def test_applies_exact_patch_without_modifying_original(self):
        overlay, proofs = self.prepare()
        self.assertEqual(self.input.read_bytes(), self.original)
        self.assertEqual((overlay / "library/ssl_tls13_keys.c").read_bytes(), self.patched)
        self.assertEqual([p.relative_to(overlay).as_posix() for p in overlay.rglob("*") if p.is_file()],
                         ["library/ssl_tls13_keys.c"])
        self.assertEqual(proofs, self.row["patches"])

    def test_verified_overlay_is_reused_without_rewriting_it(self):
        overlay, _ = self.prepare()
        output = overlay / "library/ssl_tls13_keys.c"
        timestamp = output.stat().st_mtime_ns
        self.assertEqual(self.prepare()[0], overlay)
        self.assertEqual(output.stat().st_mtime_ns, timestamp)

    def test_patch_hash_mismatch_does_not_promote_output(self):
        self.patch.write_bytes(self.patch.read_bytes() + b"\n")
        with self.assertRaisesRegex(ValueError, "patch.*hash"):
            self.prepare()
        self.assertFalse(self.destination.exists())
        self.assertEqual(self.input.read_bytes(), self.original)

    def test_modified_pinned_input_does_not_promote_output(self):
        self.input.write_bytes(b"local change\n")
        with self.assertRaisesRegex(ValueError, "source.*hash"):
            self.prepare()
        self.assertFalse(self.destination.exists())
        self.assertEqual(self.input.read_bytes(), b"local change\n")

    def test_wrong_result_hash_does_not_promote_output(self):
        self.row["patches"][0]["after_sha256"] = "c" * 64
        with self.assertRaisesRegex(ValueError, "overlay.*hash"):
            self.prepare()
        self.assertFalse(self.destination.exists())
        self.assertEqual(self.input.read_bytes(), self.original)

    def test_partial_or_inapplicable_patch_leaves_no_output(self):
        self.patch.write_bytes(self.patch.read_bytes().replace(b"return 1", b"return 8"))
        self.row["patches"][0]["sha256"] = digest(self.patch.read_bytes())
        with self.assertRaises((ValueError, subprocess.CalledProcessError)):
            self.prepare()
        self.assertFalse(self.destination.exists())
        self.assertEqual(self.input.read_bytes(), self.original)
        self.assertEqual(list(self.destination.parent.glob(".*-overlay-*")), [])

    def test_symlinked_source_patch_and_destination_are_rejected(self):
        self.assertTrue(callable(getattr(self.module, "prepare_overlay", None)))
        for selected in (self.input, self.patch, self.destination):
            with self.subTest(path=selected.name):
                saved = selected.with_name(selected.name + ".saved")
                selected.parent.mkdir(parents=True, exist_ok=True)
                if selected.exists():
                    selected.rename(saved)
                else:
                    saved.mkdir()
                selected.symlink_to(saved, target_is_directory=saved.is_dir())
                try:
                    with self.assertRaisesRegex(ValueError, "symlink"):
                        self.prepare()
                finally:
                    selected.unlink()
                    saved.rename(selected)
                    if selected.is_dir():
                        selected.rmdir()

    def test_patch_traversal_and_additional_source_are_rejected(self):
        self.assertTrue(callable(getattr(self.module, "prepare_overlay", None)))
        for field, value in (("path", "../outside.patch"), ("source", "../outside.c"),
                             ("source", "library/other.c")):
            with self.subTest(field=field, value=value):
                row = json.loads(json.dumps(self.row))
                row["patches"][0][field] = value
                with self.assertRaises(ValueError):
                    self.module.prepare_overlay(self.source, self.destination, row)
        self.assertFalse(self.destination.exists())

    def test_unexpected_existing_overlay_file_is_preserved_and_rejected(self):
        overlay, _ = self.prepare()
        extra = overlay / "keep.txt"
        extra.write_text("do not remove\n")
        with self.assertRaisesRegex(ValueError, "overlay.*contents"):
            self.prepare()
        self.assertEqual(extra.read_text(), "do not remove\n")

    def test_altered_existing_overlay_is_preserved_and_rejected(self):
        overlay, _ = self.prepare()
        output = overlay / "library/ssl_tls13_keys.c"
        output.write_text("my change\n")
        with self.assertRaisesRegex(ValueError, "overlay.*hash"):
            self.prepare()
        self.assertEqual(output.read_text(), "my change\n")

    def test_patch_cannot_add_an_unlocked_second_file(self):
        self.patch.write_bytes(self.patch.read_bytes() +
                               b"--- /dev/null\n+++ b/library/extra.c\n@@ -0,0 +1 @@\n+int extra;\n")
        self.row["patches"][0]["sha256"] = digest(self.patch.read_bytes())
        with self.assertRaisesRegex(ValueError, "(only|expected|contents|file)"):
            self.prepare()
        self.assertFalse(self.destination.exists())

    def test_existing_unpatched_prefix_receipt_is_preserved(self):
        self.assertTrue(callable(getattr(self.module, "prepare_prefix", None)),
                        "old prefix receipts must never be silently overwritten")
        self.prefix.mkdir(parents=True)
        receipt = self.prefix / "build-receipt.json"
        receipt.write_text('{"dependency":{"tag":"old"}}\n')
        before = receipt.read_bytes()
        with self.assertRaisesRegex(ValueError, "(new|provenance|prefix)"):
            self.module.prepare_prefix(self.prefix, {"schema_version": 2})
        self.assertEqual(receipt.read_bytes(), before)

    def test_changed_patch_identity_requires_new_prefix(self):
        self.assertTrue(callable(getattr(self.module, "prepare_prefix", None)))
        identity = {"schema_version": 2, "patches": self.row["patches"]}
        self.module.prepare_prefix(self.prefix, identity)
        marker = (self.prefix / "build-inputs.json").read_bytes()
        self.module.prepare_prefix(self.prefix, identity)
        with self.assertRaisesRegex(ValueError, "(new|provenance|prefix)"):
            self.module.prepare_prefix(self.prefix, {**identity, "patches": []})
        self.assertEqual((self.prefix / "build-inputs.json").read_bytes(), marker)

    def compile_fixture(self):
        self.assertTrue(callable(getattr(self.module, "collect_compile_inputs", None)),
                        "receipts must bind actual dependency and object inputs")
        overlay, _ = self.prepare()
        obj = self.prefix / "obj"
        obj.mkdir(parents=True)
        header = self.prefix / "include/profile.h"
        header.parent.mkdir()
        header.write_text("profile\n")
        source = overlay / "library/ssl_tls13_keys.c"
        (obj / "ssl_tls13_keys.d").write_text(str(obj / "ssl_tls13_keys.o") + ": " + str(source) +
                                             " \\\n " + str(header) + "\n")
        (obj / "ssl_tls13_keys.o").write_bytes(b"object\n")
        return overlay, header

    def test_compile_input_receipt_hashes_source_overlay_headers_and_depfiles(self):
        overlay, header = self.compile_fixture()
        result = self.module.collect_compile_inputs(self.source, self.prefix, overlay)
        self.assertEqual(result["compile_inputs"], {
            "overlay/library/ssl_tls13_keys.c": digest(self.patched),
            "prefix/include/profile.h": digest(header.read_bytes())})
        self.assertEqual(set(result["depfiles"]), {"ssl_tls13_keys.d"})
        self.assertEqual(result["objects"], {"ssl_tls13_keys.o": digest(b"object\n")})

    def test_depfile_outside_verified_roots_is_rejected(self):
        overlay, _ = self.compile_fixture()
        outside = self.root / "outside.h"
        outside.write_text("unverified\n")
        depfile = self.prefix / "obj/ssl_tls13_keys.d"
        depfile.write_text(depfile.read_text().rstrip() + " " + str(outside) + "\n")
        with self.assertRaisesRegex(ValueError, "(outside|unverified)"):
            self.module.collect_compile_inputs(self.source, self.prefix, overlay)

    def test_missing_depfile_or_object_stops_receipt(self):
        overlay, _ = self.compile_fixture()
        (self.prefix / "obj/ssl_tls13_keys.o").unlink()
        with self.assertRaisesRegex(ValueError, "(object|missing)"):
            self.module.collect_compile_inputs(self.source, self.prefix, overlay)

    def test_recipe_compiles_overlay_instead_of_original_translation_unit(self):
        overlay, _ = self.prepare()
        for include in ("mbedtls", "psa"):
            (self.source / "include" / include).mkdir(parents=True)
        (self.source / "include/mbedtls/mbedtls_config.h").write_text("/* fixture */\n")
        recipe = self.project / "dependencies/Makefile.mbedtls"
        recipe.write_bytes((ROOT / "dependencies/Makefile.mbedtls").read_bytes())
        (self.project / "dependencies/mbedtls/np_user_config.h").write_bytes(
            (ROOT / "dependencies/mbedtls/np_user_config.h").read_bytes())
        obj = self.prefix / "obj/ssl_tls13_keys.o"
        command = ["make", "-s", "-f", str(recipe),
                   "SRC=" + str(self.source), "OUT=" + str(self.prefix), "TARGET=host32",
                   "TLS13_KEYS_SRC=" + str(overlay / "library/ssl_tls13_keys.c"),
                   "ARCH=", "CC=cc", str(obj)]
        subprocess.run(command, check=True, capture_output=True)
        driver = self.root / "driver.c"
        driver.write_text("int tls13_setup(void); int main(void) { return tls13_setup() != 0; }\n")
        binary = self.root / "driver"
        subprocess.run(["cc", str(driver), str(obj), "-o", str(binary)], check=True, capture_output=True)
        self.assertEqual(subprocess.run([str(binary)]).returncode, 0,
                         "the actual make recipe must use the verified overlay, not original source")

    def test_build_identity_covers_effective_flags_tools_and_builder(self):
        self.assertTrue(callable(getattr(self.module, "build_identity", None)),
                        "the receipt must cover compiler, flags, recipe, profile and builder")
        overlay, proofs = self.prepare()
        recipe = self.project / "dependencies/Makefile.mbedtls"
        profile = self.project / "dependencies/mbedtls/np_user_config.h"
        recipe.write_text("fixture recipe\n")
        profile.write_text("fixture profile\n")
        self.row.update(recipe="dependencies/Makefile.mbedtls", profile="dependencies/mbedtls/np_user_config.h")
        real_output = subprocess.check_output
        def output(command, **kwargs):
            if command[0] == "make":
                flags = "-O2 -I" + str(self.prefix) + "/include -I" + str(self.source) + "/library"
                return (sys.executable + "\0" + sys.executable + "\0" + flags + "\0").encode()
            return real_output(command, **kwargs)
        with mock.patch.object(self.module.subprocess, "check_output", side_effect=output):
            identity, command, env = self.module.build_identity(
                self.source, self.prefix, overlay, self.row, "host32", self.root / "sdk", proofs)
        self.assertEqual(identity["schema_version"], 2)
        self.assertEqual(identity["flags"], ["-O2", "-I@PREFIX@/include", "-I@SOURCE@/library"])
        self.assertEqual(identity["compiler"]["executable_sha256"], digest(Path(sys.executable).read_bytes()))
        self.assertEqual(identity["builder_sha256"], digest(SCRIPT.read_bytes()))
        self.assertEqual(identity["recipe_sha256"], digest(recipe.read_bytes()))
        self.assertEqual(identity["profile_sha256"], digest(profile.read_bytes()))
        self.assertEqual(identity["patches"], proofs)
        self.assertIn("TLS13_KEYS_SRC=" + str(overlay / "library/ssl_tls13_keys.c"), command)

    def test_failed_build_does_not_promote_or_replace_receipt(self):
        self.assertTrue(callable(getattr(self.module, "build_target", None)),
                        "a failed make must not promote a new TLS build receipt")
        identity = {"schema_version": 2, "dependency": self.row}
        self.module.prepare_prefix(self.prefix, identity)
        overlay, header = self.compile_fixture()
        proofs = self.row["patches"]
        self.cache_receipt(identity, overlay)
        receipt = self.prefix / "build-receipt.json"
        before = receipt.read_bytes()
        with mock.patch.object(self.module, "build_identity", return_value=(identity, ["make"], {})), \
                mock.patch.object(self.module.subprocess, "run",
                                  side_effect=subprocess.CalledProcessError(2, "make")):
            with self.assertRaises(subprocess.CalledProcessError):
                self.module.build_target(self.source, self.prefix, overlay, self.row, "host32",
                                         self.root / "sdk", proofs, False)
        self.assertEqual(receipt.read_bytes(), before)

    def cache_receipt(self, identity, overlay):
        libraries = {}
        for name in ("libmbedtls.a", "libmbedx509.a", "libmbedcrypto.a"):
            library = self.prefix / "lib" / name
            library.parent.mkdir(exist_ok=True)
            library.write_bytes(name.encode())
            libraries[name] = digest(library.read_bytes())
        receipt = {**identity, **self.module.collect_compile_inputs(self.source, self.prefix, overlay),
                   "libraries": libraries}
        (self.prefix / "build-receipt.json").write_text(json.dumps(receipt) + "\n")

    def test_changed_cached_library_object_depfile_or_header_stops_before_make(self):
        identity = {"schema_version": 2, "dependency": self.row}
        self.module.prepare_prefix(self.prefix, identity)
        overlay, header = self.compile_fixture()
        self.cache_receipt(identity, overlay)
        receipt = self.prefix / "build-receipt.json"
        before = receipt.read_bytes()
        for artifact in (self.prefix / "lib/libmbedtls.a", self.prefix / "obj/ssl_tls13_keys.o",
                         self.prefix / "obj/ssl_tls13_keys.d", header):
            with self.subTest(artifact=artifact.name):
                original = artifact.read_bytes()
                artifact.write_bytes(original + b"changed\n")
                try:
                    with mock.patch.object(self.module, "build_identity", return_value=(identity, ["make"], {})), \
                            mock.patch.object(self.module.subprocess, "run",
                                              side_effect=AssertionError("modified cache reached make")) as make:
                        with self.assertRaisesRegex(ValueError, "(changed|hash|provenance)"):
                            self.module.build_target(self.source, self.prefix, overlay, self.row,
                                                     "host32", self.root / "sdk", self.row["patches"], False)
                        make.assert_not_called()
                    self.assertEqual(receipt.read_bytes(), before)
                finally:
                    artifact.write_bytes(original)

    def test_interrupted_unreceipted_prefix_forces_full_rebuild(self):
        self.assertTrue(callable(getattr(self.module, "build_target", None)))
        overlay, proofs = self.prepare()
        identity = {"schema_version": 2, "dependency": self.row}
        self.module.prepare_prefix(self.prefix, identity)
        with mock.patch.object(self.module, "build_identity", return_value=(identity, ["make"], {})), \
                mock.patch.object(self.module.subprocess, "run",
                                  side_effect=subprocess.CalledProcessError(2, "make")) as make:
            with self.assertRaises(subprocess.CalledProcessError):
                self.module.build_target(self.source, self.prefix, overlay, self.row, "host32",
                                         self.root / "sdk", proofs, False)
        self.assertIn("-B", make.call_args.args[0], "unverified partial objects must never be reused")
        self.assertFalse((self.prefix / "build-receipt.json").exists())


if __name__ == "__main__":
    unittest.main()
