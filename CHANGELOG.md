# Changelog

## [Unreleased]

### Added

- VBS and memory integrity emulation, off by default and configurable per boot.
- `EfiDSEFix -v` reports VBS/HVCI surfaces; `-V` restores the registry state after a boot.

### Changed

- Documentation-only pushes and pull requests no longer trigger CI.

## [2.0.0] - 2026-11-15

### Added

- Secure Boot state emulation, enabled by default and configurable per boot.
- Unattended configuration through `EfiMirage.cfg`, with `HOME` overrides.
- `EfiDSEFix -s` compares Secure Boot state across firmware, Windows, and the registry.
- A Linux `just` workflow for cross-compilation, bundle verification, and firmware tests.
- GitHub Actions checks for pushes and pull requests.
- Tag-triggered draft GitHub Releases with changelog notes and verified x64 bundles.

### Changed

- **Breaking:** EfiGuard becomes EfiMirage; [upgrade guide](docs/usage.md#migrating-from-efiguard).

### Fixed

- `EfiDSEFix` output redirected to a file or pipe is no longer lost.
- Failed Windows boots no longer leave the memory-access hook active for a fallback OS.
