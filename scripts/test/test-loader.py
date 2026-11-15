#!/usr/bin/env python3
"""Compile the actual loader with native UEFI mocks; no firmware or user files are opened."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class LoaderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        scratch = ROOT / ".scratch"
        scratch.mkdir(exist_ok=True)
        temporary = tempfile.TemporaryDirectory(prefix="loader-test-", dir=scratch)
        cls.addClassCleanup(temporary.cleanup)
        cls.work = Path(temporary.name)
        cls.binary = cls.work / "loader-regression"
        source = Path(os.environ.get("LOADER_TEST_SOURCE", ROOT / "Application/Loader/Loader.c"))
        shutil.copyfile(source, cls.work / "Loader.c")
        shutil.copyfile(ROOT / "scripts/test/loader-regression.c", cls.work / "loader-regression.c")
        includes = [ROOT / "Include", ROOT / "deps/edk2/MdePkg/Include",
                    ROOT / "deps/edk2/MdePkg/Include/X64", ROOT / "deps/edk2/MdeModulePkg/Include"]
        command = ["gcc", "-std=c11", "-fshort-wchar", "-ffunction-sections", "-fdata-sections",
                   "-DMDEPKG_NDEBUG", "-Wall", "-Wextra", "-Werror",
                   *[f"-I{path}" for path in includes], "loader-regression.c",
                   "-Wl,--gc-sections", "-o", str(cls.binary)]
        result = subprocess.run(command, cwd=cls.work, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise RuntimeError(f"Native loader build failed:\n{result.stdout}{result.stderr}")

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], cwd=self.work,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_configuration_encodings_and_exact_limit(self):
        for name in ("ascii", "utf8-bom", "utf16", "utf16-bom", "exact-limit", "utf16-exact-limit"):
            with self.subTest(name=name):
                self.run_case(name)

    def test_oversize_configuration_is_rejected_unchanged(self):
        for name in ("oversize-prefix", "oversize-crossing-value"):
            with self.subTest(name=name):
                self.run_case(name)

    def test_configuration_missing_and_read_error(self):
        for name in ("missing", "read-error"):
            with self.subTest(name=name):
                self.run_case(name)


if __name__ == "__main__":
    unittest.main()
