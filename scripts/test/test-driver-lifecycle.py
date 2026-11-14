#!/usr/bin/env python3
"""Run the actual driver lifecycle with native mocks, not firmware or Windows images."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class DriverLifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        scratch = ROOT / ".scratch"
        scratch.mkdir(exist_ok=True)
        temporary = tempfile.TemporaryDirectory(prefix="driver-lifecycle-", dir=scratch)
        cls.addClassCleanup(temporary.cleanup)
        cls.work = Path(temporary.name)
        cls.binary = cls.work / "driver-lifecycle-regression"
        source = Path(os.environ.get("DRIVER_LIFECYCLE_TEST_SOURCE",
                                     ROOT / "EfiGuardDxe/EfiGuardDxe.c"))
        shutil.copyfile(source, cls.work / "EfiGuardDxe.c")
        shutil.copyfile(ROOT / "scripts/test/driver-lifecycle-regression.c",
                        cls.work / "driver-lifecycle-regression.c")
        includes = [ROOT / "Include", ROOT / "EfiGuardDxe",
                    ROOT / "deps/edk2/MdePkg/Include",
                    ROOT / "deps/edk2/MdePkg/Include/X64",
                    ROOT / "EfiGuardDxe/Zydis/include",
                    ROOT / "EfiGuardDxe/Zydis/dependencies/zycore/include"]
        command = ["gcc", "-std=c11", "-fshort-wchar", "-ffunction-sections", "-fdata-sections",
                   "-DMDEPKG_NDEBUG", "-Wall", "-Wextra", "-Werror",
                   *[f"-I{path}" for path in includes], "driver-lifecycle-regression.c",
                   "-Wl,--gc-sections", "-o", str(cls.binary)]
        result = subprocess.run(command, cwd=cls.work, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise RuntimeError(f"Native driver build failed:\n{result.stdout}{result.stderr}")

    def run_case(self, name, mode=2, fake_secure_boot=False):
        result = subprocess.run([str(self.binary), name, str(mode), str(int(fake_secure_boot))],
                                cwd=self.work, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_load_alone_and_returned_windows_do_not_keep_hook(self):
        for name in ("load-only", "return-failure", "return-success"):
            for fake in (False, True):
                with self.subTest(name=name, fake_secure_boot=fake):
                    self.run_case(name, fake_secure_boot=fake)

    def test_active_windows_requires_selected_mode(self):
        for mode in (0, 1, 2):
            for fake in (False, True):
                with self.subTest(mode=mode, fake_secure_boot=fake):
                    self.run_case("active-windows", mode, fake)

    def test_unknown_failed_and_reused_handles_are_not_windows(self):
        for name in ("unknown", "bootmgr", "protocol-failure", "protocol-null", "reused-handle"):
            for fake in (False, True):
                with self.subTest(name=name, fake_secure_boot=fake):
                    self.run_case(name, fake_secure_boot=fake)

    def test_active_windows_still_requires_historical_load_gate(self):
        self.run_case("windows-without-load")

    def test_nested_windows_loader_keeps_hook(self):
        for fake in (False, True):
            with self.subTest(fake_secure_boot=fake):
                self.run_case("nested-winload-ebs", fake_secure_boot=fake)

    def test_nested_nonwindows_suppresses_then_restores_windows(self):
        for name in ("nested-return", "nested-ebs", "nested-failed-protocol", "nested-winload-ebs"):
            for fake in (False, True):
                with self.subTest(name=name, fake_secure_boot=fake):
                    self.run_case(name, fake_secure_boot=fake)

    def test_unload_denied_inflight_but_configuration_allowed(self):
        self.run_case("unload-inflight")

    def test_normal_unload_restores_startimage(self):
        self.run_case("normal-unload")

    def test_initialization_failure_restores_installed_hooks(self):
        self.run_case("initialize-failure")


if __name__ == "__main__":
    unittest.main()
