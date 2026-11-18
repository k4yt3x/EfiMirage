# VBS and memory integrity emulation

EfiMirage can make Windows report Virtualization-Based Security as enabled and running, with
Hypervisor-enforced Code Integrity (memory integrity) as one of its running services, while VBS
itself stays off. VBS has to stay off because the `SetVariable` backdoor cannot write
`g_CiOptions` on a machine where the secure kernel really runs: the write ends in
`SECURE_KERNEL_ERROR`.

The kernel keeps behaving exactly as it does with VBS off. Only what
`NtQuerySystemInformation` reports changes, which is what Windows, `msinfo32`, the Windows
Security app and the software on top of them read.

## What is changed

| Surface | Reported | Notes |
|---|---|---|
| `SystemCodeIntegrityInformation` (0x67) | `0xF405` | `ENABLED \| UMCI \| WHQL \| HVCI_KMCI \| KVCI_STRICT \| HVCI_IUM`; audit mode reports `0xEC05` |
| `SystemIsolatedUserModeInformation` (0xA5) | `37 10 01` | Secure Kernel running, HVCI enabled and strict, firmware page protection, VBS key available/persistent/TPM-bound |
| `SystemCodeIntegrityPolicyInformation` (0xA4) | unchanged | on a real machine its `HVCIOptions` stays 0 while HVCI runs, so it is left alone |
| `SystemVsmProtectionInformation` (0xA9) | unchanged | all zero on a real machine too |
| `VbsPolicyDisabled` EFI variable | hidden at runtime | `winload` still reads it before `ExitBootServices`, so VBS stays off |

The values are the measured output of Windows 11 26300 with memory integrity on; see
`docs/vbs.md` history in the repository for the off/on diff.

## What is not changed

- **The secure kernel is not running.** Anything that asks for it through a path other than the
  two sysinfo classes, or that inspects VTL1 directly, sees the truth. The `Secure System`
  process is absent, `hvix64.exe` never started, and no VTL1 memory exists.
- **`ci.dll!g_CiOptions` still has no HVCI bits.** A kernel driver that locates the variable and
  reads it directly (as `kvc` does) sees `0x6`, not the `0x1C000` family.
- **CPUID.** The hypervisor's `0x40000003:EBX` AccessVsm bit describes whether VBS was launched;
  with `VbsPolicyDisabled` set, it is not.
- **The registry.** `CI\State\HVCIEnabled` is a volatile key the kernel creates only when HVCI
  really runs. Write it yourself (volatile, never persistent) if a check needs it. The
  `DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity` keys describe configuration, not
  state; writing `Enabled=1` there makes Windows try to start HVCI on the next boot, which
  `VbsPolicyDisabled` then prevents.
- **TPM measurements.** The boot was measured without VBS; PCRs and the event log say so.

## Enabling it

```ini
# \EFI\EfiMirage\EfiMirage.cfg
FakeVbs = 1        # 0 off (default), 1 HVCI enforced, 2 HVCI audit
```

or press `HOME` in the loader and answer the VBS question. `Configure()` also accepts it through
`EFIMIRAGE_CONFIGURATION_DATA.FakeVbs`.

Keep `hypervisorlaunchtype auto` and a working Hyper-V installation: with the hypervisor really
running, CPUID vendor, `SystemHypervisorInformation`, and the `AvailableSecurityProperties`
"hypervisor support" entry all agree with a VBS machine. Without it they do not.

## How the driver tells the client

`Configure()` writes a boot-service + runtime (never non-volatile) variable `MirageState` under a
private GUID with the flags of what this boot emulates. `EfiDSEFix` reads it before its own VBS
check, so `-c`, `-d` and `-r` keep working even though the reported state says VBS is running.
The variable disappears on the next power cycle and is never written to NVRAM.

## Measured results

Windows 11 26300, VBS genuinely off, `FakeVbs = 1`:

| Surface | Real HVCI machine | EfiMirage |
|---|---|---|
| `0x67` options | `0xF405` | `0xF405` |
| `0xA5` | `37 10 01` | `37 10 01` |
| `0x5B` | `01 00 01 04` | `01 00 01 04` |
| WMI status | 2 | 2 |
| WMI configured | {2} | {2} (after `EfiDSEFix -a`) |
| WMI running | {2} | {2} |
| `CI\State\HVCIEnabled` | 1 (volatile) | 1 (volatile, after `EfiDSEFix -a`) |
| `g_CiOptions` | `0x1C006` | `0x1C006` (after `EfiDSEFix -a`) |
| AvailableSecurityProperties | {1,2,4,5} | {2,4,5,8} |
| `Secure System` process | present | absent |
| `0xA9` | `00 00 00 00` | `00 00 00 01` (APIC virtualization; CPU property, not VBS) |

Windows clears `EnableVirtualizationBasedSecurity` and the HVCI scenario `Enabled` whenever VBS
fails to start, which is every boot here, so those two keys have to be written again after each
boot (writing `Locked = 0` does not prevent it). `EfiDSEFix -a` does that, creates
`CI\State\HVCIEnabled` as a **volatile** key the way a real HVCI boot does (it is gone after a
reboot and leaves nothing behind), and ORs `0x1C000` into `g_CiOptions` so kernel drivers that
pattern-scan the variable see the same value as on a VBS machine. `-d` and `-e` preserve those
bits. It is needed once per boot, not once ever: `scripts/install-vbs-task.ps1` installs a
scheduled task that runs it as SYSTEM at start (remove it with `-Remove`). A boot-start driver that
reads `g_CiOptions` before the task runs still sees `0x6`.

## Why the remaining differences exist

- **AvailableSecurityProperties 1.** `Win32_DeviceGuard.dll` sets this property from
  `CPUID.0x40000003:EBX[16]` (the hypervisor's AccessVsm bit), falling back to
  `IsProcessorFeaturePresent(20/21)`. Both describe a hypervisor that actually launched VBS, so
  only patching the hypervisor (`hvix64.exe`) or the provider can restore it.
- **`Secure System`.** It is a *minimal process* the kernel creates for VSM (no PEB, no TEB,
  `EPROCESS.Minimal = 1`), not an ordinary process. A kernel driver could create one with
  `NtCreateProcessEx` flag `0x800`; nothing running in EFI can.
- **CPUID AccessVsm.** The hypervisor decides it. With `VbsPolicyDisabled` set, VBS is not
  launched and the bit stays clear.
- **`0xA9`.** The structure is `DMA available/in use`, `MBEC`, `APIC virtualization`; the byte that
  differs here is the last one and reflects the host CPU, not VBS.

## Verifying

`scripts/test/vbs-probe.ps1` prints every surface above. With the emulation on, the output should
match a real memory-integrity machine for the sysinfo classes and WMI, and differ for the process
list, CPUID and `g_CiOptions`. `EfiDSEFix -v` prints the same kernel-side values compactly.
