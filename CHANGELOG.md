# Changelog

## [Unreleased]

## [2.0.0] - 2026-11-15

### Added

- GitHub Actions CI checks branch pushes and pull requests.
- Tag pushes create draft GitHub Releases with matching changelog notes and verified x64 bundles.
- Manual branch builds allow selection of release, debug, or noopt profiles.
- Secure Boot state emulation is on by default without changing firmware validation or TPM state.
- `\EFI\EfiMirage\EfiMirage.cfg` supports unattended configuration; `HOME` overrides its settings.
- `EfiDSEFix -s/--secureboot` compares Secure Boot state from firmware, Windows, and the registry.
- The loader writes `loader-ran.txt` beside itself on each start for headless boot verification.
- Redirected `EfiDSEFix` output falls back to UTF-8 instead of producing no output.
- Windows executable properties report the release version.
- A Linux `just` workflow handles dependency setup, checks, builds, and verified deployment bundles.
- MinGW-w64 builds support both console and optional native-subsystem versions of `EfiDSEFix.exe`.
- Deployment bundles include SHA-256 manifests and undergo PE32+ header and alignment validation.
- Build-pipeline regression tests, a Windows smoke test, and component build logs aid verification.
- `just test-emulation` checks Secure Boot emulation before and after entering runtime services.

### Changed

- **Breaking:** New EfiMirage names and paths; [migrate](docs/usage.md#migrating-from-efiguard).

### Fixed

- Container builds retain Git trust for the checkout after the checkout action finishes.
- Runtime hooks are removed independently when their DSE bypass or emulation mode is not needed.
- Failed Windows boot attempts no longer leave the memory-access hook enabled for a fallback OS.
- Configuration parsing handles inline comments, boolean words, BOMs, and whole-token matching.
- Non-ASCII configuration text no longer truncates the file.
- Invalid configuration lines are reported; missing files no longer crash the loader.
- Configuration files over 4096 bytes are rejected in full rather than partially applied.
- `GetVariable` rejects a NULL buffer with sufficient capacity without changing the reported size.
- `SecureBootEnable` is no longer reported under the global variable GUID.
- `EfiDSEFix -s` no longer reports missing or mistyped registry values as Secure Boot enabled.
- Boot markers stay beside the loader when firmware supplies split directory and filename nodes.
- Configuration stays at `\EFI\EfiMirage\EfiMirage.cfg`, with the loader's volume searched first.
- Runtime tests use non-identity mappings so missing pointer conversions fail instead of passing.
- The loader builds against `edk2-stable202608` with the required safe-integer library mapping.
- MinGW-w64 builds of `EfiDSEFix` use compatible declarations and entry-point linkage.
