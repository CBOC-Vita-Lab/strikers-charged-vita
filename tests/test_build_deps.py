import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[1] / "tools/build-deps.py"


def git(root, *args):
    return subprocess.check_output(["git", "-c", "core.hooksPath=/dev/null", "-c",
                                   "commit.gpgsign=false", "-c", "user.name=Fixture",
                                   "-c", "user.email=fixture@example.invalid", *args],
                                  cwd=root, text=True, stderr=subprocess.DEVNULL).strip()


class DependencyTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(SCRIPT.exists(), "Pinned dependency setup is missing")
        spec = importlib.util.spec_from_file_location("build_deps", SCRIPT)
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.origin = self.root / "origin"
        self.origin.mkdir()
        git(self.origin, "init", "-b", "main")
        (self.origin / "code.c").write_text("first\n")
        git(self.origin, "add", "code.c")
        git(self.origin, "commit", "-m", "fixture")
        self.commit = git(self.origin, "rev-parse", "HEAD")
        (self.origin / "code.c").write_text("second\n")
        git(self.origin, "commit", "-am", "newer fixture")
        self.destination = self.root / "checkout"

    def fetch(self):
        self.module.ensure_checkout(self.destination, str(self.origin), self.commit)

    def test_fetch_checks_out_pinned_commit_not_latest_and_is_repeatable(self):
        self.fetch()
        self.fetch()
        self.assertEqual((self.destination / "code.c").read_text(), "first\n")
        self.assertEqual(git(self.destination, "rev-parse", "HEAD"), self.commit)

    def test_local_edits_are_preserved_and_rejected(self):
        self.fetch()
        (self.destination / "code.c").write_text("my edit\n")
        with self.assertRaisesRegex(ValueError, "changes"):
            self.fetch()
        self.assertEqual((self.destination / "code.c").read_text(), "my edit\n")

    def test_wrong_origin_is_not_rewritten(self):
        self.fetch()
        git(self.destination, "remote", "set-url", "origin", "https://example.invalid/other.git")
        with self.assertRaisesRegex(ValueError, "origin"):
            self.fetch()
        self.assertEqual(git(self.destination, "remote", "get-url", "origin"), "https://example.invalid/other.git")

    def test_existing_non_repository_directory_is_preserved(self):
        self.destination.mkdir()
        (self.destination / "keep").write_text("keep")
        with self.assertRaises(ValueError):
            self.fetch()
        self.assertEqual((self.destination / "keep").read_text(), "keep")

    def test_symlink_checkout_is_not_followed(self):
        self.destination.symlink_to(self.origin, target_is_directory=True)
        with self.assertRaises(ValueError):
            self.fetch()
        self.assertEqual((self.origin / "code.c").read_text(), "second\n")

    def test_changed_commit_is_not_silently_reset(self):
        self.fetch()
        (self.destination / "code.c").write_text("my commit\n")
        git(self.destination, "commit", "-am", "local work")
        with self.assertRaisesRegex(ValueError, "revision"):
            self.fetch()
        self.assertEqual((self.destination / "code.c").read_text(), "my commit\n")

    def test_lock_rejects_traversal_duplicate_names_and_unpinned_refs(self):
        valid = {"name": "sample", "url": "https://github.com/example/sample.git", "commit": "a" * 40}
        for rows in [[{**valid, "name": "../outside"}], [valid, valid],
                     [{**valid, "commit": "main"}], [{**valid, "url": "http://example.invalid/repo"}]]:
            lock = self.root / "lock.json"
            lock.write_text(json.dumps({"dependencies": rows}))
            with self.subTest(rows=rows), self.assertRaises(ValueError):
                self.module.load_lock(lock)

    def test_failed_fetch_does_not_break_retry(self):
        real_git = self.module.git
        def fail_fetch(directory, *args):
            if args[0] == "fetch":
                raise subprocess.CalledProcessError(128, "git fetch")
            return real_git(directory, *args)
        with mock.patch.object(self.module, "git", side_effect=fail_fetch):
            with self.assertRaises(subprocess.CalledProcessError):
                self.fetch()
        self.assertFalse(self.destination.exists())
        self.fetch()
        self.assertEqual((self.destination / "code.c").read_text(), "first\n")
        self.assertEqual(set(self.root.iterdir()), {self.origin, self.destination})

    def build_fixture(self):
        project = self.root / "project"
        (project / "dependencies").mkdir(parents=True)
        sdk = self.root / "sdk"
        (sdk / "bin").mkdir(parents=True)
        (sdk / "bin/arm-vita-eabi-gcc").touch()
        (project / "shared.h").write_text("original header\n")
        libraries = "libvitaGL.a libvitashark.a libmathneon.a libSceShaccCgExt.a libtaihen_stub.a"
        recipe = project / "dependencies/Makefile.vitadeps"
        recipe.write_text("RING = 8\nall: " + " ".join("$(DEPS_OUT)/lib/" + name for name in libraries.split())
                          + "\n$(DEPS_OUT)/lib/%.a:\n\t@mkdir -p $(@D)\n"
                          + "\t@cat shared.h > $@\n\t@printf '%s\\n' '$(RING)' >> $@\n")
        (project / "dependencies/lock.json").write_text(json.dumps({"dependencies": [
            {"name": "fixture", "url": "https://github.com/example/fixture.git", "commit": "a" * 40}]}))
        return project, sdk, recipe

    def run_build_fixture(self, project, sdk):
        with mock.patch.object(self.module, "ROOT", project), \
                mock.patch.object(self.module.platform, "system", return_value="Linux"), \
                mock.patch.object(self.module, "ensure_checkout"), \
                mock.patch.dict(self.module.os.environ, {"VITASDK": str(sdk)}), \
                mock.patch.object(self.module.sys, "argv", ["build-deps.py"]):
            self.assertEqual(self.module.main(), 0)
        return (project / "build/dependencies/prefix/lib/libvitaGL.a").read_text()

    def test_rebuild_includes_changed_internal_header(self):
        project, sdk, recipe = self.build_fixture()
        self.assertEqual(self.run_build_fixture(project, sdk), "original header\n8\n")
        (project / "shared.h").write_text("changed header\n")
        self.assertEqual(self.run_build_fixture(project, sdk), "changed header\n8\n")

    def test_rebuild_includes_changed_recipe_options(self):
        project, sdk, recipe = self.build_fixture()
        self.assertEqual(self.run_build_fixture(project, sdk), "original header\n8\n")
        recipe.write_text(recipe.read_text().replace("RING = 8", "RING = 16"))
        self.assertEqual(self.run_build_fixture(project, sdk), "original header\n16\n")


if __name__ == "__main__":
    unittest.main()
