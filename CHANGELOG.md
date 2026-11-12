# Changelog

## [Unreleased]

### Added

- Secure Boot state emulation: `gRT->GetVariable` is hooked to report `SecureBoot` as enabled
  and `SetupMode` as user mode, so Windows and software on it see a secure boot chain even on
  firmware without one. Configured per boot through the loader menu or
  `EFIGUARD_CONFIGURATION_DATA.FakeSecureBoot`. See `docs/secureboot.md`.
- `EFIGUARD_CONFIGURATION_DATA` now carries a `Version`; mismatched or out-of-range
  configuration is refused instead of partially applied.
- `EfiDSEFix.exe -s/--secureboot` reports the Secure Boot state from the firmware variables,
  `SystemSecureBootInformation` and the registry, with a consistency verdict.
- OVMF self-tests for the emulation (`just test-emulation`): boot-services and post-
  `SetVirtualAddressMap` variants, driven by two test applications.
- The loader writes `\EFI\EfiGuard\loader-ran.txt` on every start, so a boot can be
  confirmed without a console.
- `EfiDSEFix` output now falls back to UTF-8 on a redirected handle instead of writing
  nothing when no console is attached.
- Linux cross-compilation workflow via `just` for dependency setup, offline prerequisite checks, component builds, and complete verified bundles (`dist/<profile>/`).
- MinGW-w64 cross-compilation support and compatibility headers for `EfiDSEFix.exe` (console and optional native-subsystem variants).
- SHA-256 manifests (`SHA256SUMS`) and PE32+ header/alignment validation for published bundles.
- Build-pipeline regression tests, Windows console smoke test, and per-component build logs.

### Fixed

- The runtime table hooks were torn down at `ExitBootServices()` whenever `bootmgfw.efi` had
  not been seen, even when Secure Boot emulation still needed them.
- Add `SafeIntLib` mapping to `EfiGuardPkg.dsc` for building `Loader.efi` against `edk2-stable202608`.
- Guard `wcstoul` redeclaration under MinGW and give `NtProcessStartupW` C linkage in `EfiDSEFix`.
