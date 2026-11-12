# Secure Boot emulation

EfiGuard can report Secure Boot as enabled to everything that asks, regardless of what
the firmware is actually configured to do. The firmware's own behaviour is unchanged: it
still validates whatever it validates, and unsigned EfiGuard images still have to be
loadable by it (Secure Boot disabled, or the images signed and enrolled).

The goal is the software side of Secure Boot, which is what games, anti-cheat and Windows
diagnostics check, not a real chain of trust.

## What is intercepted

`gRT->GetVariable` is hooked and answers three variables under the EFI global variable
GUID:

| Variable | Reported value | Reported attributes |
|---|---|---|
| `SecureBoot` | `1` | `BS+RT` |
| `SetupMode` | `0` | `BS+RT` |

Everything else is forwarded unchanged. `SecureBootEnable` is deliberately not answered: the
firmware keeps it under its own vendor GUID (`gEfiSecureBootEnableDisableGuid`), Windows does
not read it, and inventing it under the global GUID would make `GetVariable` answer for a
variable `GetNextVariableName` never lists.

The hook is installed and active as soon as the driver loads, because `winload.efi` reads
`SecureBoot` before `ExitBootServices()` to choose its image validation path; faking it only
at runtime would be too late.

`SetupMode` is faked in the same direction as `SecureBoot` on purpose. Reporting
`SecureBoot=1` while leaving the real `SetupMode=1` describes a state no firmware
produces, and makes `winload` take the enrollment path against a firmware with no keys.

Once either this feature or the `SetVariable` backdoor is on, the driver stays linked as a
runtime driver and its image is kept mapped past `ExitBootServices()`; the original
`GetVariable` pointer is converted in the `SetVirtualAddressMap()` notification so runtime
calls still reach the firmware.

## Turning it on and off

Emulation is **on by default** whenever the driver loads, including when it is installed as a
firmware driver entry with no loader involved. To turn it off for one boot, press `HOME`
within 1.5 seconds in the loader and answer No to "Report Secure Boot as enabled to Windows
and software running on it?".

For unattended boots, put `\EFI\EfiGuard\EfiGuard.cfg` on the same volume as the loader.
If it exists it is applied and the prompts are skipped; if it is absent or unreadable, the
`HOME` prompt applies.

```ini
# EfiGuard.cfg; UTF-16LE or ASCII, case-insensitive keys, '#' starts a comment
DseBypass = hook        # hook | boot | none
WaitForKeyPress = 0
FakeSecureBoot = 0      # omit to keep the default (on)
```

Unknown keys and unknown values are reported on screen and ignored, so an old config keeps
booting. The file must sit at that exact path because the loader looks for it relative to
each volume.

Programmatically, it is `EFIGUARD_CONFIGURATION_DATA.FakeSecureBoot`, sent through the
driver protocol `Configure()` together with `Version = EFIGUARD_CONFIGURATION_DATA_VERSION`.
A mismatched version or an out-of-range DSE bypass method is refused before anything is
committed. `Configure()` may be called repeatedly until Windows boot manager is loaded, so
the feature can be turned back off.

## What it does not do

- **TPM and BitLocker.** Secure Boot state is measured into PCR 7 by the firmware before
  EfiGuard exists, and the TPM event log keeps the real values. BitLocker with a PCR 7
  profile will ask for the recovery key. Faking is not possible from this layer.
- **Firmware validation.** Shapes such as `db`/`dbx`/`PK` are still the real ones. A
  missing or foreign `db` does not stop the boot because `ImgpValidateImageHash` is already
  patched out, but a tool that parses the signature databases sees the truth.
- **HVCI.** Unrelated; HVCI still wins on `g_CiOptions` writes as before.
- **`GetNextVariableName`.** Not hooked. Every UEFI 2.3.1+ firmware defines the two variables
  answered here, so enumeration lists them anyway.

## Verifying

`EfiDSEFix.exe -s` reports every source an application can use and a verdict:

```
  EFI variable SecureBoot:          1 (attributes 0x6)
  EFI variable SetupMode:           0 (attributes 0x6)
  SystemSecureBootInformation:      enabled 1, capable 1
  Registry UEFISecureBootEnabled:   1
  Verdict: Secure Boot appears ENABLED from all sources.
```

The same values can be checked with `Confirm-SecureBootUEFI` and `msinfo32`. Without the
emulation, the variables report the firmware's real state.

## Local firmware tests

`scripts/test/test-secureboot.sh` boots a real OVMF build with no platform key under QEMU
and runs one of two test applications against the driver. Both are part of the normal
`just efi` build and end up in `out/efi/<profile>/test/`.

| Variant | Checks |
|---|---|
| `boot` | every `GetVariable` call shape (size query, short buffer, oversized buffer, NULL pointers), attribute values, vendor GUID mismatch, pass-through, version and range checks, and reverting to the real state |
| `runtime` | the same hook after `ExitBootServices()` and `SetVirtualAddressMap()`, where a missed pointer conversion faults |

The host has no KVM, so the packages are fetched and unpacked into `.tooling/` instead of
being installed:

```sh
python3 scripts/test/fetch-ovmf.py   # once, needs network; Arch packages
just efi release                     # the images under test
just test-emulation                  # both variants
```

`set -e`-style pass/fail comes from the `[PASS]`/`[FAIL]`/`RESULT` lines the applications
write to the serial port. The runtime variant writes straight to COM1 because `ConOut`
does not survive `ExitBootServices()`.

## Booting a firmware VM without a console

OVMF does not honour `BootNext` and does not boot a `Boot####` entry that was created by
copying `{bootmgr}`; both are skipped silently and the next entry is used, which looks
exactly like a crash. Point the entry the firmware actually boots at the loader instead:

```bat
bcdedit /copy {bootmgr} /d "EfiGuard Test"     :: only needed once, and not used by OVMF
bcdedit /set {bootmgr} path \EFI\EfiGuard\Loader.efi
```

To confirm the firmware really took the entry, the loader writes
`loader-ran.txt` next to `Loader.efi` on every start; delete it before the reboot and check
whether it comes back. Revert with
`bcdedit /set {bootmgr} path \EFI\Microsoft\Boot\bootmgfw.efi`.

## Manual test on real hardware

1. Install the driver and boot through the loader with the feature enabled.
2. In Windows, `EfiDSEFix.exe -s` must report `Secure Boot` as enabled from every source
   and `SetupMode` as 0.
3. `msinfo32` must show "Secure Boot State: On"; `Confirm-SecureBootUEFI` must return True.
4. Check the fan-in: `Get-ComputerInfo | Select-Object DeviceGuard*`, and any launcher that
   gates on Secure Boot.
5. If BitLocker is on, expect one recovery-key prompt on the first boot after the firmware
   measured state diverges from what the protector was sealed against.

A machine with Secure Boot genuinely off is the interesting case; on a machine where it is
already on, everything looks the same and only the absence of regressions is proven.
