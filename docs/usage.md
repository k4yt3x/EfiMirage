# Installation and usage

[Project overview](../README.md) · [Build guide](build.md) · [Secure Boot
configuration](secureboot.md)

Review the [compatibility and limitations](#compatibility-and-limitations) before installing.
For an existing installation, follow [Migrating from EfiGuard](#migrating-from-efiguard).

## Choosing a boot method

There are two ways to use EfiMirage: booting the **loader application**, which will load the driver
and start Windows for you, or installing the driver as a **UEFI driver entry** so it will be loaded
automatically by the firmware.

Installing the driver can be preferable in some advanced configurations such as when multi-booting,
but the loader is easiest to use and should work well in all configurations. See the table below for
the most important differences between the two methods. If unsure, choose the **loader
application**.

|                   | Location       | Installation   | Skippable?         | Which OS is booted? |
|-------------------|----------------|----------------|--------------------|---------------------|
| UEFI Driver Entry | Must be on ESP | Via UEFI Shell | :x:                | Same as before      |
| Loader            | Anywhere       | Not needed     | :heavy_check_mark: | Windows             |

Loader vs UEFI driver entry comparison

## Booting the loader

1. Download and extract the EfiMirage bundle. It contains `Loader.efi`, `EfiMirageDxe.efi`,
   `EfiDSEFix.exe`, and `SHA256SUMS` at the archive root.
2. Create `EFI/Boot` on a boot drive, such as a FAT32 USB stick or a VM disk. Copy the two
   `.efi` files there and rename the copied `Loader.efi` to `bootx64.efi`.
   Assuming drive `X:`, the paths should be `X:/EFI/Boot/{bootx64|EfiMirageDxe}.efi`.
3. Boot the machine from the drive you used in step 2.
   Most firmwares provide a boot menu to do this via F8/F10/F11/F12. If not, you will need to
   configure the BIOS to boot from the new drive.
4. Windows should now boot, and you should see EfiMirage messages during boot.
   To configure without the `HOME` prompt, place `\EFI\EfiMirage\EfiMirage.cfg` on the same
   volume (see [Secure Boot emulation](secureboot.md)). The loader reads it; a driver that is
   installed as a firmware driver entry and never goes through the loader uses the compiled-in
   defaults instead.
5. If you booted with the `SetVariable` hook (the default), run `EfiDSEFix.exe -d` from an
   Administrator command prompt after boot to disable DSE, or run `EfiDSEFix.exe` to see the full
   list of options.

Note that you **don't need to use a separate drive** for the loader. If preferable, you can install
EfiMirage on the ESP that Windows is already installed on. However, this is somewhat more
complicated as you will need to add a UEFI boot entry for the loader.

To do this, mount the ESP at `X:` using `mountvol X: /S` and follow the steps above, but do **not**
rename the loader and simply copy both files to `X:/EFI/Boot`. After that, you will need to manually
add a UEFI boot entry from the [UEFI
Shell](https://github.com/tianocore/edk2/blob/edk2-stable201903/ShellBinPkg/UefiShell/X64/Shell.efi?raw=true)
using `bcfg boot addp 0 Loader.efi "EfiMirage"`, or alternatively using `efibootmgr` (Linux),
EasyUEFI (Windows), or similar.

## Installing the driver

1. Mount the ESP at `X:` using `mountvol X: /S`.
2. Copy `EfiMirageDxe.efi` to `X:/EFI/Boot/EfiMirageDxe.efi`.
3. Boot to the [UEFI
   Shell](https://github.com/tianocore/edk2/blob/edk2-stable201903/ShellBinPkg/UefiShell/X64/Shell.efi?raw=true)
   and add a UEFI driver entry: `bcfg driver add 0 EfiMirageDxe.efi "EfiMirageDxe"`.
4. Windows should now boot, and you should see EfiMirage messages during boot.
   To configure without the `HOME` prompt, place `\EFI\EfiMirage\EfiMirage.cfg` on the same
   volume (see [Secure Boot emulation](secureboot.md)). The loader reads it; a driver that is
   installed as a firmware driver entry and never goes through the loader uses the compiled-in
   defaults instead.
5. If you booted with the `SetVariable` hook (the default), run `EfiDSEFix.exe -d` from an
   Administrator command prompt after boot to disable DSE, or run `EfiDSEFix.exe` to see the full
   list of options.

**Note**: depending on your firmware, you may need to use "add**p**" in step 3 instead of "add".
VirtualBox is known to require this, and possibly some motherboard firmwares do too.

**Note**: some very old or noncompliant firmwares may not support this method of installation at
all. On these systems you will have no option but to use the loader instead.

## Compatibility and limitations

Report EfiMirage bugs in this repository's issue tracker, not the upstream EfiGuard tracker.

- EfiMirage can not disable Hypervisor-enforced Code Integrity (HVCI or HyperGuard) due to HVCI
  running at a greater privilege level. EfiMirage **can** coexist with HVCI and even successfully
  disables PatchGuard in the normal kernel, but this is not useful in practice because HVCI will
  catch what PatchGuard did previously. Both types of DSE bypass are rendered useless by HVCI: the
  boot time patch has no effect because the kernel defers to the secure kernel for integrity checks,
  and the `SetVariable` hook will cause a `SECURE_KERNEL_ERROR` bugcheck if it is used to write to
  `g_CiOptions`.
- Checked kernels are not supported due to the differences in PatchGuard and DSE initialization code
  caused by disabled optimizations and added asserts, as well as additional changes to PatchGuard in
  checked kernels. This should not be an issue as checked kernels are not generally useful without a
  kernel debugger attached, which disables PatchGuard.

## Migrating from EfiGuard

The rename to EfiMirage changes public names and paths, not behavior or features.
For an existing installation:

1. Replace `EfiGuardDxe.efi` with the new `EfiMirageDxe.efi` build beside the loader,
   or at the location used by your firmware driver entry. Update `Loader.efi` from
   the same release so it uses the new driver and config paths. Update deployment
   scripts that refer to the old driver name.
2. Move `\EFI\EfiGuard\EfiGuard.cfg` to `\EFI\EfiMirage\EfiMirage.cfg` on the loader's
   volume. Configuration keys and values are unchanged. The path is absolute from
   the volume root; the loader does **not** fall back to the old path.
3. Update UEFI boot and driver entries to the new deployed paths and labels as needed.
   `Loader.efi` and `EfiDSEFix.exe` keep their names; the firmware fallback name
   `bootx64.efi` is unchanged too.
4. Remove or disable old driver entries before loading EfiMirage. **Do not load
   EfiGuard and EfiMirage drivers together**: they use the same protocol GUID.

For source integrations, use `EfiMirageDxe/`, `EfiMiragePkg.dec`, `EfiMiragePkg.dsc`,
`EfiMirage.sln`, and `EfiMirage.props`. The protocol header is now
`Include/Protocol/EfiMirage.h`; C names use `EFIMIRAGE_*` and
`gEfiMirageDriverProtocolGuid`. Build output uses `Build/EfiMirage`.
Use `efimirage` as the repository slug. Renaming the hosted repository and updating
clone remotes are separate from the source and build rename.

The rename preserves GUID values, the configuration schema version, and structure layouts
from this fork's pre-rename builds. The legacy backdoor variable string (`roodkcaBdrauGifE`,
reversed `EfiGuardBackdoor`) and cookie (`0xDEADC0DE`) are also intentionally unchanged.
These compatibility identifiers are not old configuration-path aliases.
