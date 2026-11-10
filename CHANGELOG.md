# Changelog

## [Unreleased]

### Added

- Linux cross-compilation workflow via `just` for dependency setup, offline prerequisite checks, component builds, and complete verified bundles (`dist/<profile>/`).
- MinGW-w64 cross-compilation support and compatibility headers for `EfiDSEFix.exe` (console and optional native-subsystem variants).
- SHA-256 manifests (`SHA256SUMS`) and PE32+ header/alignment validation for published bundles.
- Build-pipeline regression tests, Windows console smoke test, and per-component build logs.

### Fixed

- Add `SafeIntLib` mapping to `EfiGuardPkg.dsc` for building `Loader.efi` against `edk2-stable202608`.
- Guard `wcstoul` redeclaration under MinGW and give `NtProcessStartupW` C linkage in `EfiDSEFix`.
