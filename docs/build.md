# Build and development guide

## Quick start

Run commands from the repository root. `just` also finds the root justfile when
invoked from a source subdirectory.

```sh
just                 # list tasks
just bootstrap       # one-time dependency setup; requires network access
just doctor          # offline prerequisite and pinned-revision checks
just build           # checks, RELEASE EFI + Windows console builds, publication
just verify          # recheck dist/release/ headers and checksums
just test-emulation  # OVMF self-tests for the Secure Boot emulation
```

On Arch Linux, host prerequisites are `just git make python gcc nasm
mingw-w64-gcc util-linux`. Standard POSIX utilities and GNU coreutils are also
used. Bootstrap installs Python dependencies only inside `deps/venv`; it does
not install host packages. Wine and MSVC are not needed for cross-compilation.

`just check` needs only `just`, Python, a POSIX shell, standard utilities, and
`flock`; it does not need EDK2, MinGW, network access, or the QEMU tooling.
`just test-unit` also needs GCC and the pinned EDK2/Zydis headers from bootstrap. It compiles
the loader and driver with native firmware mocks to check configuration boundaries, marker
paths and boot-attempt lifetimes; it does not boot firmware or execute Windows images.
`just test-emulation` boots the images in OVMF and needs the private QEMU + OVMF
under `.tooling/`; prepare it once with `python3 scripts/test/fetch-ovmf.py`
(see `docs/secureboot.md`). `just doctor` checks
build tools and dependency checkouts, not Windows/firmware runtime compatibility.

## GitHub Actions

The workflows use the same `just` commands as local builds, in an Arch Linux container on
GitHub-hosted Linux runners. Host packages follow Arch's rolling repositories; EDK2 and the
source submodules remain pinned by the repository. Each run bootstraps a fresh workspace.

- **CI** (`.github/workflows/ci.yaml`) runs on pushes to any branch, pull requests, and manual
  dispatch. It checks scripts and pipeline tests, runs the native loader/driver regression
  tests, builds the release EFI and Windows console images, and verifies the bundle. A newer
  run on the same ref cancels the previous CI run.
- **CD** (`.github/workflows/release.yaml`) runs on every pushed tag and builds the release
  profile from that tag. Manual dispatch on a branch also offers `release`, `debug`, or
  `noopt`; dispatch on a tag always builds `release`. Checks and native regression tests must
  pass before the bundle is uploaded.

Download `efiguard-x64-<profile>` from the **CD run's Artifacts** section. Its ZIP contains
`EfiGuardDxe.efi`, `Loader.efi`, `EfiDSEFix.exe`, and `SHA256SUMS` at the archive root. Retention
uses the repository's artifact-retention setting. Component build logs are uploaded by both
workflows, including after build failures, and retained for 14 days when available.

CD produces Actions artifacts only: it does not create a GitHub Release, sign binaries, or
install them on a machine. Both workflows use read-only repository permissions and do not
persist checkout credentials. No extra repository secrets are required. OVMF emulation and
live Windows tests remain separate from these workflows; use `just test-emulation` locally
for the firmware suites.

## Workspace layout

```text
Application/             upstream Loader and EfiDSEFix sources
  SecureBootTest/        OVMF boot-services self-test for the emulation
  SecureBootRuntimeTest/ OVMF post-SetVirtualAddressMap self-test
EfiGuardDxe/             upstream runtime driver and Zydis submodule
Include/                 shared driver/loader/client interfaces
EfiGuardPkg.{dsc,dec}     EDK2 package definitions
EfiGuard.sln, *.props     native Visual Studio build definitions
justfile                 supported task entry points
scripts/                 build orchestration and artifact verification
  test/                  pipeline regression tests, OVMF harness, Windows smoke test
  compat/mingw/          Windows headers needed for Linux cross-compilation
docs/                    this guide and the Secure Boot emulation guide

deps/                    ignored: pinned EDK2 checkout, Python venv, task lock
out/                     ignored: intermediate and component build products
  edk2-ws/               EDK2 Conf/ state and Build/ tree
  packages/EfiGuardPkg    generated symlink to the repository root
  efi/<profile>/         EFI images, .debug symbols, .map files
  windows/<profile>/     console/ and native/ outputs, each with its own obj/
  logs/                  complete component build output
  bootstrap/             generated Python requirements
dist/<profile>/          ignored: complete, verified deployment bundle
.scratch/                ignored: temporary regression-test workspaces
.tooling/                ignored: private QEMU + OVMF for the emulation tests
```

Source, scripts, tests, and documentation belong to the same Git repository.
Do not put local credentials, machine access keys, or deployed-machine state in
it. Nothing in the build pipeline connects to or changes a Windows host.

## Build pipeline

`just build [profile]` performs these steps in order:

1. Check build prerequisites and pinned dependency revisions.
2. Run `just check`'s checks: justfile formatting, shell/Python syntax, and
   synthetic artifact/build-failure regression tests, then `just test-unit`'s native loader
   and driver regression tests. A failing test stops the build before publication.
3. Build the EFI driver and loader. A nonzero EDK2 exit status stops the pipeline.
4. Build the Windows console utility. A compiler or linker error stops the pipeline.
5. Validate all three images: x64 PE32+ headers, expected subsystems, section
   bounds, and 4 KiB EFI file/section alignment.
6. Prepare and verify a complete bundle with `SHA256SUMS`, then publish it to
   `dist/<profile>/`. Failed builds leave the previously published bundle alone.

Publication uses a prepared directory and two renames with rollback on failure;
it is not an atomic directory exchange. Tasks invoked through `just` share a
nonblocking workspace lock, so a second task refuses to race a build or clean.
A process or machine crash during publication can leave a `.PROFILE-previous-*`
backup in `dist/`; preserve it when recovering an interrupted publication.

Component recipes (`just efi`, `just windows`, `just native`) write **only to
out/**. They do not update published bundles. Use `just build` when you want a
complete bundle built from the current sources. Call the `just` recipes rather
than individual implementation scripts to get task locking and captured logs.

### Profiles and optional builds

| Command | EFI target | Windows configuration | Published bundle |
|---|---|---|---|
| `just build` / `just build release` | RELEASE | `-O2 -DNDEBUG` | `dist/release/` |
| `just build debug` | DEBUG | `-O0 -g -D_DEBUG` | `dist/debug/` |
| `just build noopt` | NOOPT | `-O0 -g -D_DEBUG` | `dist/noopt/` |
| `just native [profile]` | not built | native subsystem | none |

Profiles are lowercase. Each has isolated component outputs; debug/noopt cannot
replace release bundles. Windows has no separate NOOPT configuration, so that
profile uses the debug compiler flags in a separate output directory.

Every published bundle contains exactly:

```text
EfiGuardDxe.efi
Loader.efi
EfiDSEFix.exe
SHA256SUMS
```

`just verify debug` selects a different bundle. Verification checks the complete
file set and recomputes checksums as well as inspecting PE headers. It does not
prove that a bundle matches later source edits, nor that it will boot.
The pipeline does not sign images or create boot entries.

### Logs and cleanup

Full build output is saved to `out/logs/efi-<profile>.log`,
`windows-<profile>.log`, or `native-<profile>.log`. Successful builds print a
short summary; failures print the last 80 log lines and the full log path.
The next build of the same component/profile replaces its log.

```sh
just clean           # asks for confirmation; removes out/ and dist/ only
just build           # cold rebuild, reusing deps/
```

`just --yes clean` skips the confirmation for unattended use. No task runs
`git clean`, resets edits, deletes dependency checkouts, or changes Git history.

## Dependencies and toolchain details

`scripts/common.sh` pins EDK2 to `edk2-stable202608` and its exact commit. Bootstrap
initializes only the required EDK2 submodules, plus Zydis and its Zycore dependency
at the commits recorded by this repository. Existing checkouts at different
revisions are refused rather than overwritten. Bootstrap can be rerun: the venv
is reused, Python requirements are installed when their input hash changes,
and BaseTools uses incremental make. Host tools and EDK2's ranged Python
requirements are **not** a fully locked or bit-for-bit reproducible toolchain.

EDK2 resolves `EfiGuardPkg/...` through `out/packages/EfiGuardPkg`; its private
workspace keeps generated Conf/ and Build/ files out of the dependency checkout.
The added `SafeIntLib` mapping is needed by the loader's current library chain.
EDK2's optional PDB-copy make rules ignore missing PDB files themselves; there
is no wrapper exception that converts a failed build into success.

The MinGW build uses the repository's x64 `ntdllp.lib`, an explicit
`NtProcessStartupW` entry point, and no CRT. The console variant additionally
links `kernel32` for `WriteConsoleW`. The compatibility headers and the two
existing source adjustments preserve the upstream Vista-targeted ABI.

The optional native-subsystem executable uses `NtDisplayString` and cannot be
launched as a normal Win32 process. It stays in
`out/windows/<profile>/native/`, never in a published bundle.

## Validation status

Local validation on 2026-11-10 passed: release, debug, and noopt bundle builds,
optional native-subsystem builds, bundle verification, and the pipeline
regression suite. Debug/noopt/native builds left the release bundle unchanged.
Existing pinned dependencies were reused.

GCC reports loader LTO type-mismatch warnings for `BmRepairAllControllers`,
`BmSetMemoryTypeInformationVariable`, and `BmIsAutoCreateBootOption` against the
pinned EDK2 declarations. These remain in the logs; build success is not runtime
validation, and this build baseline does not alter those upstream functions.

The loader writes `loader-ran.txt` next to itself on every start, so a headless boot can be
told apart from "the firmware never loaded us". See
`docs/secureboot.md` for the OVMF boot-entry caveat.

### Runtime testing

The Windows smoke test (2026-11-06, Windows 11 Pro build 26300.9711 with Secure
Boot enabled) records successful help/information commands and expected failure
statuses without a loaded driver. With the driver loaded, the Secure Boot
emulation was verified on 2026-11-12 on Windows 11 with Secure Boot disabled in
the firmware: `EfiDSEFix -s` reported every source as enabled,
`Confirm-SecureBootUEFI` returned True and the `UEFISecureBootEnabled` registry
value flipped from 0 to 1, over repeated boots. Cryptographic authorization
remains a planned feature.

For a manual console smoke test on Windows, copy `scripts/test/windev-smoke.ps1` and the
console executable to that host, then run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\windev-smoke.ps1 -Path .\EfiDSEFix.exe
```

The script probes `-c` first and expects the backdoor-dependent commands to succeed when a
working driver is loaded and to fail with `STATUS_INVALID_PARAMETER` when it is not, so the
same run is meaningful in both states. It includes state-changing commands (`-d` disables
DSE) and is not a general loaded-driver diagnostic. It skips
native-subsystem images. No remote access credentials or host-specific paths are
required by the script.
