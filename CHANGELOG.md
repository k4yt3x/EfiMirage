# Changelog

## [Unreleased]

## [2.1.0] - 2026-11-20

### Added

- VBS and memory integrity emulation, on by default and configurable per boot.
- `EfiDSEFix -v` reports VBS/HVCI surfaces; `-a` restores registry and CI state.
- Release bundles include `install-vbs-task.ps1` to run `-a` at boot as SYSTEM.

### Changed

- Documentation-only pushes and pull requests no longer trigger CI.

### Fixed

- Failed VBS disablement no longer lets `EfiDSEFix` access kernel memory.
- `EfiDSEFix -a` refuses inactive emulation and reports incomplete state restoration.
- `EfiDSEFix -a` replaces persistent HVCI state keys with volatile ones.

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
