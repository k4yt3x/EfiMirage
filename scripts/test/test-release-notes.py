"""Release-note selection tests; CLI fixtures live in temporary workspaces."""

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "release-notes.py"
SPEC = importlib.util.spec_from_file_location("release_notes", SCRIPT)
release_notes = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release_notes)


class ReleaseNotesTests(unittest.TestCase):
    def test_exact_section_excludes_unreleased_and_other_versions(self):
        changelog = """# Changelog

## [Unreleased]

- Future change.

## [2.0.01] - 2026-11-16

- Different version.

## [2.0.0] - 2026-11-15

### Added

- Feature.

### Fixed

- Fix.

## [1.0.0] - 2025-01-01

- Old change.
"""
        self.assertEqual(release_notes.extract_notes(changelog, "2.0.0"),
                         "### Added\n\n- Feature.\n\n### Fixed\n\n- Fix.\n")

    def test_last_section_preserves_content_and_trims_blank_lines(self):
        changelog = "## [2.0.0]\r\n \r\n### Added\r\n\r\n- Feature.\r\n  Detail.\r\n\t\r\n"
        self.assertEqual(release_notes.extract_notes(changelog, "2.0.0"),
                         "### Added\n\n- Feature.\n  Detail.\n")

    def test_tag_is_literal_not_a_regex_or_implicit_v_prefix(self):
        changelog = "## [2x0x0]\n- Wrong.\n\n## [v2.0.0]\n- Prefixed.\n"
        with self.assertRaisesRegex(ValueError, "no changelog section"):
            release_notes.extract_notes(changelog, "2.0.0")
        self.assertEqual(release_notes.extract_notes(changelog, "v2.0.0"), "- Prefixed.\n")

    def test_missing_version_does_not_fall_back_to_unreleased(self):
        with self.assertRaisesRegex(ValueError, "no changelog section"):
            release_notes.extract_notes("## [Unreleased]\n- Future.\n", "2.0.0")

    def test_empty_and_heading_only_sections_are_refused(self):
        for content in ("", " \n\t", "### Added\n\n### Fixed\n"):
            with self.subTest(content=content), self.assertRaisesRegex(ValueError, "is empty"):
                release_notes.extract_notes(f"## [2.0.0]\n{content}\n## [1.0.0]\n- Old.\n", "2.0.0")

    def test_duplicate_version_is_refused(self):
        with self.assertRaisesRegex(ValueError, "duplicate changelog sections"):
            release_notes.extract_notes("## [2.0.0]\n- First.\n## [2.0.0] - 2026-11-15\n- Second.\n", "2.0.0")

    def test_unreleased_is_not_a_release(self):
        for tag in ("", "Unreleased", "unreleased"):
            with self.subTest(tag=tag), self.assertRaisesRegex(ValueError, "must name a version"):
                release_notes.extract_notes(f"## [{tag}]\n- Future.\n", tag)

    def test_stops_at_the_next_level_two_heading(self):
        self.assertEqual(release_notes.extract_notes("## [2.0.0]\n- Feature.\n## Appendix\nPrivate.\n", "2.0.0"),
                         "- Feature.\n")

    def test_cli_outputs_only_notes_and_reports_failures(self):
        with tempfile.TemporaryDirectory(prefix="release-notes-test-") as directory:
            work = Path(directory)
            (work / "CHANGELOG.md").write_text("\ufeff## [2.0.0]\n\n- Feature.\n", encoding="utf-8")
            result = subprocess.run([sys.executable, str(SCRIPT), "--", "2.0.0"], cwd=work,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, "- Feature.\n")
            self.assertEqual(result.stderr, "")
            result = subprocess.run([sys.executable, str(SCRIPT), "--", "3.0.0"], cwd=work,
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, "")
            self.assertIn("no changelog section", result.stderr)
            (work / "CHANGELOG.md").unlink()
            result = subprocess.run([sys.executable, str(SCRIPT), "--", "2.0.0"], cwd=work,
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, "")
            self.assertIn("error:", result.stderr)


if __name__ == "__main__":
    unittest.main()
