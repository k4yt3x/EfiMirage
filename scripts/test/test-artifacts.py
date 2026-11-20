#!/usr/bin/env python3
"""Artifact regression tests using synthetic PE images in temporary workspaces."""

import contextlib
import hashlib
import importlib.util
import io
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "artifacts.py"
SPEC = importlib.util.spec_from_file_location("artifacts", SCRIPT)
artifacts = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(artifacts)

INSTALLER = Path(__file__).resolve().parents[1] / "install-vbs-task.ps1"
NAMES = ("EfiMirageDxe.efi", "Loader.efi", "EfiDSEFix.exe", INSTALLER.name)
PE = 128
OPTIONAL = PE + 24
SECTIONS = OPTIONAL + 240


def minimal_pe(subsystem, marker=0):
    alignment = 4096 if subsystem in (10, 12) else 512
    image = bytearray(alignment * 2)
    image[:2] = b"MZ"
    struct.pack_into("<I", image, 60, PE)
    image[PE:PE + 4] = b"PE\0\0"
    struct.pack_into("<HH", image, PE + 4, 0x8664, 1)
    struct.pack_into("<HH", image, PE + 20, 240, 0x22)
    struct.pack_into("<H", image, OPTIONAL, 0x20B)
    struct.pack_into("<II", image, OPTIONAL + 32, 4096, alignment)
    struct.pack_into("<II", image, OPTIONAL + 56, 8192, alignment)
    struct.pack_into("<H", image, OPTIONAL + 68, subsystem)
    struct.pack_into("<I", image, OPTIONAL + 108, 16)
    image[SECTIONS:SECTIONS + 8] = b".text\0\0\0"
    struct.pack_into("<IIII", image, SECTIONS + 8, 1, 4096, alignment, alignment)
    image[alignment] = marker
    return bytes(image)


def changed(image, offset, format_, value):
    image = bytearray(image)
    struct.pack_into(format_, image, offset, value)
    return bytes(image)


class ArtifactTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="artifacts-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.log = io.StringIO()
        redirect = contextlib.redirect_stderr(self.log)
        redirect.__enter__()
        self.addCleanup(redirect.__exit__, None, None, None)

    def inputs(self, profile="release", marker=1):
        paths = {
            "EfiMirageDxe.efi": (f"efi/{profile}/EfiMirageDxe.efi", 12),
            "Loader.efi": (f"efi/{profile}/Loader.efi", 10),
            "EfiDSEFix.exe": (f"windows/{profile}/console/EfiDSEFix.exe", 3),
        }
        result = {}
        for name, (relative, subsystem) in paths.items():
            path = self.root / "out" / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(minimal_pe(subsystem, marker))
            result[name] = path
        installer = self.root / "scripts" / INSTALLER.name
        installer.parent.mkdir(parents=True, exist_ok=True)
        installer.write_bytes(INSTALLER.read_bytes())
        result[INSTALLER.name] = installer
        return result

    def snapshot(self, profile="release"):
        bundle = self.root / "dist" / profile
        return {path.name: path.read_bytes() for path in bundle.iterdir()}

    def assert_only_bundle(self, profile="release"):
        self.assertEqual({path.name for path in (self.root / "dist").iterdir()}, {profile})

    def test_valid_stage_and_checksums(self):
        sources = self.inputs()
        native = self.root / "out/windows/release/native/EfiDSEFix.exe"
        native.parent.mkdir(parents=True)
        native.write_bytes(b"not a deployment artifact")
        bundle = artifacts.stage(self.root, "release")
        self.assertEqual(bundle, self.root / "dist/release")
        self.assertEqual(set(self.snapshot()), set(NAMES) | {"SHA256SUMS"})
        expected_manifest = ""
        for name in NAMES:
            content = sources[name].read_bytes()
            self.assertEqual((bundle / name).read_bytes(), content)
            expected_manifest += f"{hashlib.sha256(content).hexdigest()}  {name}\n"
        self.assertEqual((bundle / "SHA256SUMS").read_text(), expected_manifest)
        self.assertEqual(artifacts.verify(self.root, "release"), bundle)
        self.assert_only_bundle()

    def test_successful_replacement(self):
        self.inputs(marker=1)
        artifacts.stage(self.root, "release")
        old = self.snapshot()
        self.inputs(marker=2)
        artifacts.stage(self.root, "release")
        self.assertNotEqual(self.snapshot(), old)
        artifacts.verify(self.root, "release")
        self.assert_only_bundle()

    def test_invalid_and_truncated_pe(self):
        valid = minimal_pe(10)
        cases = {
            "empty": (b"", "truncated"),
            "short DOS header": (valid[:63], "truncated"),
            "DOS signature": (changed(valid, 0, "<H", 0), "DOS signature"),
            "PE signature": (changed(valid, PE, "<I", 0), "PE signature"),
            "offset within DOS": (changed(valid, 60, "<I", 32), "header offset"),
            "offset beyond file": (changed(valid, 60, "<I", 0xFFFFFFFF), "truncated"),
            "short COFF": (valid[:PE + 23], "truncated"),
            "x86 machine": (changed(valid, PE + 4, "<H", 0x14C), "x64"),
            "PE32 magic": (changed(valid, OPTIONAL, "<H", 0x10B), "PE32\\+"),
            "subsystem": (changed(valid, OPTIONAL + 68, "<H", 12), "subsystem"),
            "short optional size": (changed(valid, PE + 20, "<H", 100), "optional header"),
            "short optional data": (valid[:OPTIONAL + 239], "truncated"),
            "directory table": (changed(valid, OPTIONAL + 108, "<I", 17), "directory table"),
            "zero sections": (changed(valid, PE + 6, "<H", 0), "no sections"),
            "section table": (changed(valid, PE + 6, "<H", 200), "headers size"),
            "header bounds": (changed(valid, OPTIONAL + 60, "<I", 12288), "headers size"),
            "section data": (valid[:-1], "raw data"),
            "section raw offset": (changed(valid, SECTIONS + 20, "<I", 0), "raw data"),
            "section virtual extent": (changed(valid, SECTIONS + 12, "<I", 8192), "outside"),
            "file alignment": (changed(valid, OPTIONAL + 36, "<I", 512), "alignment"),
            "section alignment": (changed(valid, OPTIONAL + 32, "<I", 8192), "alignment"),
            "image alignment": (changed(valid, OPTIONAL + 56, "<I", 8193), "aligned"),
            "section raw alignment": (changed(valid, SECTIONS + 16, "<I", 4095), "aligned"),
        }
        path = self.root / "invalid.efi"
        for description, (content, diagnostic) in cases.items():
            with self.subTest(description=description):
                path.write_bytes(content)
                with self.assertRaisesRegex(artifacts.ArtifactError, diagnostic):
                    artifacts.validate_pe(path, 10)

    def test_missing_inputs(self):
        for missing in NAMES:
            with self.subTest(missing=missing):
                sources = self.inputs()
                sources[missing].unlink()
                error = artifacts.ArtifactError if missing == INSTALLER.name else OSError
                with self.assertRaises(error):
                    artifacts.stage(self.root, "release")
                self.assertFalse((self.root / "dist/release").exists())

    def test_failed_validation_retains_bundle(self):
        self.inputs()
        artifacts.stage(self.root, "release")
        old = self.snapshot()
        sources = self.inputs(marker=2)
        sources["EfiDSEFix.exe"].write_bytes(minimal_pe(1))
        with self.assertRaisesRegex(artifacts.ArtifactError, "subsystem 3"):
            artifacts.stage(self.root, "release")
        self.assertEqual(self.snapshot(), old)
        self.assert_only_bundle()

    def test_failed_copy_retains_bundle_and_cleans_staging(self):
        self.inputs()
        artifacts.stage(self.root, "release")
        old = self.snapshot()
        self.inputs(marker=2)
        copyfile = artifacts.shutil.copyfile

        def fail_second_copy(source, target):
            if source.name == "Loader.efi":
                target.write_bytes(b"partial copy")
                raise OSError("injected copy failure")
            return copyfile(source, target)

        with mock.patch.object(artifacts.shutil, "copyfile", side_effect=fail_second_copy):
            with self.assertRaisesRegex(OSError, "copy failure"):
                artifacts.stage(self.root, "release")
        self.assertEqual(self.snapshot(), old)
        self.assert_only_bundle()

    def test_copied_files_are_validated_before_publication(self):
        self.inputs()
        artifacts.stage(self.root, "release")
        old = self.snapshot()
        with mock.patch.object(artifacts.shutil, "copyfile",
                               side_effect=lambda source, target: target.write_bytes(b"truncated")):
            with self.assertRaises(artifacts.ArtifactError):
                artifacts.stage(self.root, "release")
        self.assertEqual(self.snapshot(), old)
        self.assert_only_bundle()

    def test_rename_failure_rolls_back_and_cleans_staging(self):
        self.inputs()
        artifacts.stage(self.root, "release")
        old = self.snapshot()
        self.inputs(marker=2)
        rename = Path.rename
        for failed_move in (1, 2):
            calls = []

            def failing_rename(source, target):
                calls.append((source, target))
                if len(calls) == failed_move:
                    raise OSError("injected rename failure")
                return rename(source, target)

            with self.subTest(failed_move=failed_move):
                with mock.patch.object(Path, "rename", failing_rename):
                    with self.assertRaisesRegex(OSError, "rename failure"):
                        artifacts.stage(self.root, "release")
                self.assertEqual(len(calls), 1 if failed_move == 1 else 3)
                self.assertEqual(self.snapshot(), old)
                artifacts.verify(self.root, "release")
                self.assert_only_bundle()

    def test_failed_rollback_preserves_backup(self):
        self.inputs()
        bundle = artifacts.stage(self.root, "release")
        old = self.snapshot()
        self.inputs(marker=2)
        rename = Path.rename

        def fail_publish_and_rollback(source, target):
            if source == bundle:
                return rename(source, target)
            raise OSError("injected publication and rollback failure")

        with mock.patch.object(Path, "rename", fail_publish_and_rollback):
            with self.assertRaisesRegex(artifacts.ArtifactError, "previous bundle retained at"):
                artifacts.stage(self.root, "release")
        remaining = list((self.root / "dist").iterdir())
        self.assertEqual(len(remaining), 1)
        self.assertTrue(remaining[0].name.startswith(".release-previous-"))
        self.assertEqual({path.name: path.read_bytes() for path in remaining[0].iterdir()}, old)

    def test_verify_rejects_absent_corrupt_and_unexpected_bundle(self):
        with self.assertRaisesRegex(artifacts.ArtifactError, "missing"):
            artifacts.verify(self.root, "release")
        self.inputs()
        bundle = artifacts.stage(self.root, "release")
        (bundle / "extra.txt").write_text("unexpected")
        with self.assertRaisesRegex(artifacts.ArtifactError, "unexpected: extra.txt"):
            artifacts.verify(self.root, "release")
        (bundle / "extra.txt").unlink()
        (bundle / "Loader.efi").unlink()
        with self.assertRaisesRegex(artifacts.ArtifactError, "missing: Loader.efi"):
            artifacts.verify(self.root, "release")
        (bundle / "Loader.efi").write_bytes(b"invalid")
        with self.assertRaisesRegex(artifacts.ArtifactError, "truncated"):
            artifacts.verify(self.root, "release")

    def test_missing_or_tampered_installer_rejected(self):
        self.inputs()
        bundle = artifacts.stage(self.root, "release")
        installer = bundle / INSTALLER.name
        content = installer.read_bytes()
        installer.unlink()
        with self.assertRaisesRegex(artifacts.ArtifactError, "missing: install-vbs-task.ps1"):
            artifacts.verify(self.root, "release")
        installer.write_bytes(content + b"\n# changed\n")
        with self.assertRaisesRegex(artifacts.ArtifactError, "install-vbs-task.ps1.*checksum mismatch"):
            artifacts.verify(self.root, "release")

    def test_installer_checksum_entry_required(self):
        self.inputs()
        bundle = artifacts.stage(self.root, "release")
        manifest = bundle / "SHA256SUMS"
        manifest.write_text("".join(line for line in manifest.read_text().splitlines(keepends=True)
                                    if INSTALLER.name not in line))
        with self.assertRaisesRegex(artifacts.ArtifactError, "missing checksum entries"):
            artifacts.verify(self.root, "release")

    def test_corrupt_digest_and_content(self):
        self.inputs()
        bundle = artifacts.stage(self.root, "release")
        manifest = bundle / "SHA256SUMS"
        original = manifest.read_text()
        manifest.write_text("0" * 64 + original[64:])
        with self.assertRaisesRegex(artifacts.ArtifactError, "checksum mismatch"):
            artifacts.verify(self.root, "release")
        manifest.write_text(original)
        (bundle / "Loader.efi").write_bytes(minimal_pe(10, marker=99))
        with self.assertRaisesRegex(artifacts.ArtifactError, "checksum mismatch"):
            artifacts.verify(self.root, "release")

    def test_manifest_requires_one_digest_per_artifact(self):
        self.inputs()
        bundle = artifacts.stage(self.root, "release")
        manifest = bundle / "SHA256SUMS"
        lines = manifest.read_text().splitlines(keepends=True)
        for content in ("", "".join(lines[:2]), lines[0] * 2 + lines[2], "not a checksum\n"):
            with self.subTest(content=content):
                manifest.write_text(content)
                with self.assertRaises(artifacts.ArtifactError):
                    artifacts.verify(self.root, "release")

    def test_profile_isolation(self):
        for marker, profile in enumerate(("release", "debug", "noopt")):
            self.inputs(profile, marker)
            artifacts.stage(self.root, profile)
        release = self.snapshot("release")
        noopt = self.snapshot("noopt")
        self.inputs("debug", 99)
        artifacts.stage(self.root, "debug")
        self.assertEqual(self.snapshot("release"), release)
        self.assertEqual(self.snapshot("noopt"), noopt)
        self.assertNotEqual(self.snapshot("debug"), release)
        for profile in ("release", "debug", "noopt"):
            artifacts.verify(self.root, profile)

    def test_cli_defaults_to_script_root_not_working_directory(self):
        self.inputs()
        script = self.root / "scripts/artifacts.py"
        script.write_bytes(SCRIPT.read_bytes())
        unrelated = self.root / "unrelated"
        unrelated.mkdir()
        for action in ("stage", "verify"):
            result = subprocess.run([sys.executable, str(script), action, "release"],
                                    cwd=unrelated, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            expected = f"{self.root / 'dist/release'}\n" if action == "stage" else ""
            self.assertEqual(result.stdout, expected)
            self.assertIn("Verifying", result.stderr)
        self.assertFalse((unrelated / "dist").exists())

    def test_cli_output_and_error_status(self):
        self.inputs()
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(artifacts.main(["stage", "release"], root=self.root), 0)
        self.assertEqual(output.getvalue(), f"{self.root / 'dist/release'}\n")
        self.assertIn("Verifying", self.log.getvalue())
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(artifacts.main(["verify", "release"], root=self.root), 0)
            for args in (["stage", "../escape"], ["verify", "debug"], ["unknown", "release"], []):
                self.assertEqual(artifacts.main(args, root=self.root), 1)
        self.assertEqual(output.getvalue(), "")
        self.assertIn("error:", self.log.getvalue())


if __name__ == "__main__":
    unittest.main()
