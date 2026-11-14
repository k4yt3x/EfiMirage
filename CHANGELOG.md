# Changelog

## [Unreleased]

Everything below is part of the same unreleased change set relative to the last
published release.

### Added

- Secure Boot state emulation, on by default: the driver makes `SecureBoot` read as enabled
  and `SetupMode` as user mode, so Windows, its loader and the software on it see a secure
  boot chain even on firmware without one. The firmware's own validation is untouched and
  TPM measurements still show the truth. See `docs/secureboot.md`.
- `\EFI\EfiGuard\EfiGuard.cfg` for unattended configuration of the DSE bypass method, the
  patch-stage keypress prompt and the emulation. Holding `HOME` still opens the prompts and
  overrides the file.
- `EfiDSEFix.exe -s/--secureboot` reports the Secure Boot state from the firmware variables,
  `SystemSecureBootInformation` and the registry, with a consistency verdict.
- The loader writes `loader-ran.txt` next to itself on every start, so a boot can be confirmed
  without a console.
- `EfiDSEFix` output falls back to UTF-8 when its output is redirected instead of writing
  nothing.
- Linux cross-compilation workflow via `just` for dependency setup, offline prerequisite
  checks, component builds, and complete verified bundles (`dist/<profile>/`).
- MinGW-w64 cross-compilation support and compatibility headers for `EfiDSEFix.exe` (console
  and optional native-subsystem variants).
- SHA-256 manifests (`SHA256SUMS`) and PE32+ header/alignment validation for published
  bundles.
- Build-pipeline regression tests, a Windows console smoke test, per-component build logs, and
  OVMF self-tests for the emulation (`just test-emulation`).

### Fixed

- The runtime memory-access hook is retained only when its DSE bypass mode is selected and
  Windows is the active boot attempt. A failed or returned Windows boot does not leave it
  enabled for a fallback OS. Secure Boot emulation is independent and keeps only its own hook
  when the memory-access hook is not needed.
- `Loader.efi`'s configuration file accepts inline `#` comments, `true`/`false`/`yes`/`no`,
  UTF-8 with a BOM and non-ASCII text, matches keys and values as whole tokens, and reports a
  line it cannot parse instead of silently ignoring or misapplying it. A missing file no
  longer crashes the loader. Files exceeding 4096 bytes are reported and rejected in full
  rather than applying a truncated configuration.
- `GetVariable` follows the UEFI contract for a NULL buffer that is large enough
  (`EFI_INVALID_PARAMETER` with the size untouched), matching EDK2's variable service.
- `SecureBootEnable` is no longer answered under the global variable GUID, where standard
  firmware does not define it.
- `EfiDSEFix -s` distinguishes a missing or mistyped `UEFISecureBootEnabled` from a value of 1
  instead of reporting `4294967295` as enabled.
- The boot marker is written beside the loader, including when firmware supplies its
  directory and filename separately. Configuration remains at `\EFI\EfiGuard\EfiGuard.cfg`,
  with the loader's volume preferred over other volumes.
- The runtime self-test maps runtime regions at a non-zero offset and drops the identity alias
  for runtime code, so a missing pointer conversion faults instead of passing silently.
- `Add SafeIntLib mapping to EfiGuardPkg.dsc` for building `Loader.efi` against
  `edk2-stable202608`.
- Guard `wcstoul` redeclaration under MinGW and give `NtProcessStartupW` C linkage, so
  `EfiDSEFix.exe` links and starts with MinGW-w64.
