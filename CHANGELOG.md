# Changelog

## [Unreleased]

### Added

- Secure Boot state emulation: `gRT->GetVariable` is hooked to report `SecureBoot` as enabled
  and `SetupMode` as user mode, so Windows and software on it see a secure boot chain even on
  firmware without one. Configured per boot through the loader menu or
  `EFIGUARD_CONFIGURATION_DATA.FakeSecureBoot`. On by default; see `docs/secureboot.md`.
- `EFIGUARD_CONFIGURATION_DATA` now carries a `Version`; mismatched or out-of-range
  configuration is refused instead of partially applied.
- `EfiDSEFix.exe -s/--secureboot` reports the Secure Boot state from the firmware variables,
  `SystemSecureBootInformation` and the registry, with a consistency verdict.
- OVMF self-tests for the emulation (`just test-emulation`): boot-services and post-
  `SetVirtualAddressMap` variants, driven by two test applications.
- The loader writes `loader-ran.txt` next to itself on every start, so a boot can be
  confirmed without a console.
- `EfiDSEFix` output now falls back to UTF-8 on a redirected handle instead of writing
  nothing when no console is attached.
- Linux cross-compilation workflow via `just` for dependency setup, offline prerequisite checks, component builds, and complete verified bundles (`dist/<profile>/`).
- MinGW-w64 cross-compilation support and compatibility headers for `EfiDSEFix.exe` (console and optional native-subsystem variants).
- SHA-256 manifests (`SHA256SUMS`) and PE32+ header/alignment validation for published bundles.
- Build-pipeline regression tests, Windows console smoke test, and per-component build logs.

### Fixed

- The `SetVariable` backdoor is unhooked at `ExitBootServices()` whenever it is not the selected bypass for a
  boot that reached `bootmgfw.efi`, independently of Secure Boot emulation, and it refuses requests in that
  state instead of relying on the hook having been removed.
- `Loader.efi` configuration parsing: inline `#` comments, `true`/`false`/`yes`/`no` values, invalid values
  (reported and skipped instead of silently turning the setting off), UTF-8 BOM and non-ASCII bytes (no longer
  truncate the file), whole-token key matching, and a `%%u` that printed literally.
- `GetVariable` now follows the UEFI contract for a NULL buffer that is large enough (`EFI_INVALID_PARAMETER`,
  size untouched), matching EDK2's variable service.
- `SecureBootEnable` is no longer answered under the global variable GUID, where standard firmware does not
  define it.
- `EfiDSEFix -s` reports a missing `UEFISecureBootEnabled` as an error instead of `4294967295`, and only
  counts an explicit 1 as enabled.
- The boot marker is written next to the loader instead of a fixed `\EFI\EfiGuard` path, so it also works in
  the documented `\EFI\Boot\bootx64.efi` layout.
- The runtime self-test now uses a non-identity virtual mapping and removes the identity alias for runtime
  code, so a missing pointer conversion faults instead of passing silently; a run that never reaches an
  assertion no longer counts as a pass.
- `SecureBoot.c` added to the Visual Studio project; `Version` moved to the start of
  `EFIGUARD_CONFIGURATION_DATA`, and the DSE bypass range check compares unsigned so negative enum values are
  rejected under MSVC.
- `test-secureboot.sh` accepts the `noopt` profile, kills QEMU on interruption, and lints under `just check`;
  `fetch-ovmf.py` always extracts its targets and verifies package checksums.

- The runtime table hooks were torn down at `ExitBootServices()` whenever `bootmgfw.efi` had
  not been seen, even when Secure Boot emulation still needed them.
- Add `SafeIntLib` mapping to `EfiGuardPkg.dsc` for building `Loader.efi` against `edk2-stable202608`.
- Guard `wcstoul` redeclaration under MinGW and give `NtProcessStartupW` C linkage in `EfiDSEFix`.
