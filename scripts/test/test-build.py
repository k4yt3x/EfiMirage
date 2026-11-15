"""Exercise the real build wrappers with local fake tools, never the checkout."""
from pathlib import Path
import fcntl
import os
import shutil
import subprocess
import tempfile
import unittest

SCRIPTS = Path(__file__).resolve().parents[1]


class BuildFailureTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        scripts = self.root / "scripts"
        scripts.mkdir()
        for name in ("workspace.sh", "common.sh", "build-efi.sh"):
            shutil.copy2(SCRIPTS / name, scripts / name)
        self.executable(scripts / "check.sh", "exit 0\n")
        self.executable(scripts / "test/test-unit.sh", 'exit "${FAKE_UNIT_RC:-0}"\n')
        self.executable(scripts / "build-windows.sh", 'touch "${0%/*}/windows-ran"\nexit 27\n')
        self.bin = self.root / "bin"
        self.bin.mkdir()
        for name in ("just", "make", "gcc", "g++", "nasm", "x86_64-w64-mingw32-gcc",
                     "x86_64-w64-mingw32-g++", "x86_64-w64-mingw32-windres"):
            self.executable(self.bin / name, "exit 0\n")
        self.executable(self.bin / "git", '''
ROOT=$TEST_ROOT
. "$ROOT/scripts/common.sh"
case "$*" in
    *'rev-parse HEAD') printf '%s\\n' "$EDK2_REVISION" ;;
    *'submodule status'*) printf ' pinned submodule\\n' ;;
    *) exit 99 ;;
esac
''')
        edk2 = self.root / "deps/edk2"
        (edk2 / ".git").mkdir(parents=True)
        for tool in ("GenFw", "GenSec", "GenFfs"):
            self.executable(edk2 / "BaseTools/Source/C/bin" / tool, "exit 0\n")
        for name in ("build_rule", "tools_def", "target"):
            self.write(edk2 / "BaseTools/Conf" / f"{name}.template", "fixture\n")
        self.executable(self.root / "deps/venv/bin/python3", 'exit "${FAKE_EDK2_RC:-23}"\n')
        build = self.root / "out/edk2-ws/Build/EfiMirage/RELEASE_GCC/X64"
        for module, relative in (("EfiMirageDxe", "EfiMirageDxe/EfiMirageDxe"),
                                 ("Loader", "Application/Loader/Loader")):
            self.write(build / f"{module}.efi", "old cached image\n")
            for extension in ("debug", "map"):
                self.write(build / "EfiMiragePkg" / relative / "DEBUG" / f"{module}.{extension}", "old symbols\n")
            self.write(self.root / "out/efi/release" / f"{module}.efi", "old component image\n")
        for module in ("SecureBootTest", "SecureBootRuntimeTest"):
            self.write(build / f"{module}.efi", "old cached test image\n")
        self.bundle = self.root / "dist/release/EfiMirageDxe.efi"
        self.write(self.bundle, "last successful bundle\n")
        self.environment = dict(os.environ, PATH=f"{self.bin}{os.pathsep}{os.environ['PATH']}",
                                TEST_ROOT=str(self.root), FAKE_EDK2_RC="23")

    @staticmethod
    def write(path, text):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def executable(self, path, body):
        self.write(path, "#!/bin/sh\nset -eu\n" + body)
        path.chmod(0o755)

    def build(self):
        return subprocess.run(["sh", str(self.root / "scripts/workspace.sh"), "build", "release"],
                              cwd=self.root, env=self.environment, capture_output=True, text=True)

    def test_failed_unit_tests_stop_before_building_or_publishing(self):
        self.environment["FAKE_UNIT_RC"] = "19"
        result = self.build()
        self.assertEqual(result.returncode, 19, result.stdout + result.stderr)
        self.assertFalse((self.root / "out/logs").exists())
        self.assertFalse((self.root / "scripts/windows-ran").exists())
        self.assertEqual(self.bundle.read_text(), "last successful bundle\n")

    def test_failed_efi_build_rejects_cached_images(self):
        result = self.build()
        self.assertEqual(result.returncode, 23, result.stdout + result.stderr)
        self.assertFalse((self.root / "scripts/windows-ran").exists())
        self.assertFalse((self.root / "out/efi/release/EfiMirageDxe.efi").exists())
        self.assertEqual(self.bundle.read_text(), "last successful bundle\n")
        self.assertTrue((self.root / "out/logs/efi-release.log").exists())

    def test_failed_windows_build_does_not_publish_efi_only_bundle(self):
        self.environment["FAKE_EDK2_RC"] = "0"
        result = self.build()
        self.assertEqual(result.returncode, 27, result.stdout + result.stderr)
        self.assertTrue((self.root / "scripts/windows-ran").exists())
        self.assertEqual(self.bundle.read_text(), "last successful bundle\n")

    def test_clean_keeps_dependencies_and_sources(self):
        source = self.root / "Application/source.cpp"
        self.write(source, "uncommitted source\n")
        result = subprocess.run(["sh", str(self.root / "scripts/workspace.sh"), "clean"],
                                cwd=self.root, env=self.environment, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.root / "out").exists())
        self.assertFalse((self.root / "dist").exists())
        self.assertTrue((self.root / "deps/venv/bin/python3").exists())
        self.assertTrue((self.root / "deps/.build.lock").exists())
        self.assertEqual(source.read_text(), "uncommitted source\n")

    def test_clean_refuses_while_workspace_locked(self):
        lock = self.root / "deps/.build.lock"
        with lock.open("w") as held:
            fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = subprocess.run(["sh", str(self.root / "scripts/workspace.sh"), "clean"],
                                    cwd=self.root, env=self.environment, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.bundle.read_text(), "last successful bundle\n")
        self.assertTrue((self.root / "out").exists())

    def test_unknown_profile_is_rejected_before_creating_output(self):
        result = subprocess.run(["sh", str(self.root / "scripts/workspace.sh"), "build", "unknown"],
                                cwd=self.root, env=self.environment, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / "out/logs").exists())
        self.assertEqual(self.bundle.read_text(), "last successful bundle\n")


if __name__ == "__main__":
    unittest.main()
