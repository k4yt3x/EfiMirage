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
| `SystemDmaProtectionInformation` (0xA9) | byte 0 = 1 with `FakeDma=1` | unchanged otherwise; does not enable an IOMMU |
| `SystemDmaGuardPolicyInformation` (0xCA) | byte 0 = 1 with `FakeDma=1` | unchanged otherwise; policy report only |
| `VbsPolicyDisabled` EFI variable | hidden at runtime | `winload` still reads it before `ExitBootServices`, so VBS stays off |

The values are the measured output of Windows 11 26100.9974 with memory integrity on; see
`docs/vbs.md` history in the repository for the off/on diff.

## What is not changed

- **The secure kernel is not running.** Anything that asks for it through a path other than the
  two sysinfo classes, or that inspects VTL1 directly, sees the truth. The `Secure System`
  process is absent, `hvix64.exe` never started, and no VTL1 memory exists.
- **`ci.dll!g_CiOptions` starts without HVCI bits.** Before `EfiDSEFix -a` runs, a kernel driver
  that reads it directly sees `0x6`, not the `0x1C000` family.
- **CPUID.** The hypervisor's `0x40000003:EBX` AccessVsm bit describes whether VBS was launched;
  with `VbsPolicyDisabled` set, it is not.
- **The registry.** `CI\State\HVCIEnabled` is a volatile key the kernel creates only when HVCI
  really runs. Write it yourself (volatile, never persistent) if a check needs it. The
  `DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity` keys describe configuration, not
  state; writing `Enabled=1` there makes Windows try to start HVCI on the next boot, which
  `VbsPolicyDisabled` then prevents.
- **TPM measurements.** The boot was measured without VBS; PCRs and the event log say so.

## Configuring it

```ini
# \EFI\EfiMirage\EfiMirage.cfg
FakeVbs = 1        # 0 off, 1 HVCI enforced (default), 2 HVCI audit
FakeDma = 1        # 0 off, 1 report Kernel DMA Protection (default)
```

or press `HOME` in the loader and answer the VBS and DMA questions. `Configure()` accepts both
through `EFIMIRAGE_CONFIGURATION_DATA.FakeVbs` and `.FakeDma`.

Keep `hypervisorlaunchtype auto` and a working Hyper-V installation: with the hypervisor really
running, CPUID vendor, `SystemHypervisorInformation`, and the `AvailableSecurityProperties`
"hypervisor support" entry all agree with a VBS machine. Without it they do not.

## Kernel DMA Protection

`FakeDma` is on by default and makes the two queries that carry DMA-protection state report it as present:

- `SystemDmaProtectionInformation` (0xA9) byte 0 feeds
  `Win32_DeviceGuard.AvailableSecurityProperties` value 3.
- `SystemDmaGuardPolicyInformation` (0xCA) byte 0 feeds msinfo32's
  "Kernel DMA Protection" row and the Windows Security memory-access-protection entry.

Nothing else changes: no DMAR/IVRS table is installed, no IOMMU exists, and devices are not
actually remapped. The reported state is a claim about the platform, not protection. Because the
0xCA byte is the DMA-guard policy state, other components that consult it see the same claim.

Measured on Windows 11 26100.9974 with VBS genuinely off, `FakeVbs = 1, FakeDma = 1`:
`0xA9 = 01 00 00 00`, `0xCA = 01`, WMI properties `{2, 3, 4, 5}`, and msinfo32 reports
"Kernel DMA Protection: On".

## How the driver tells the client

The driver writes a boot-service + runtime (never non-volatile) variable `MirageState` under a
private GUID. It records the Secure Boot setting at driver load or `Configure()`, but records the
fake-VBS flag only after winload's `VbsPolicyDisabled` write succeeded for this boot. `EfiDSEFix`
trusts that flag when its ordinary VBS queries are emulated, so `-c`, `-d` and `-r` keep working
without overriding a failed VBS-disable attempt. A failure to patch the kernel can still leave
VBS reporting incomplete. The flag is cleared if the Windows boot returns; if the firmware
refuses to clear it, EfiMirage cold-reboots rather than continuing with a stale safety flag.
Unload removes the variable. `-a` refuses to modify Windows state unless the flag is present and
both kernel VBS queries report HVCI. The variable is volatile and never written to NVRAM.

## Measured results

Windows 11 26100.9974, VBS genuinely off, `FakeVbs = 1, FakeDma = 0` (explicitly disabled):

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
| `0xA9` | `00 00 00 00` | first three bytes zero; last byte is host APIC virtualization |

Windows clears `EnableVirtualizationBasedSecurity` and the HVCI scenario `Enabled` whenever VBS
fails to start, which is every boot here, so those two keys have to be written again after each
boot (writing `Locked = 0` does not prevent it). `EfiDSEFix -a` does that, creates
`CI\State\HVCIEnabled` as a **volatile** key the way a real HVCI boot does (it is gone after a
reboot and leaves nothing behind), and ORs `0x1C000` into `g_CiOptions` so kernel drivers that
pattern-scan the variable see the same value as on a VBS machine. `-d` and `-e` preserve those
bits. It is needed once per boot, not once ever. In a permanent directory containing the
extracted release bundle, run `powershell -ExecutionPolicy Bypass -File .\install-vbs-task.ps1`
as Administrator. This installs a task that runs the adjacent `EfiDSEFix.exe -a` as SYSTEM at
startup. The executable must remain at that path. `-a` makes no changes and exits with failure if
this boot has not established VBS emulation; it also reports failures to restore any CI or registry
state. Before turning `FakeVbs` off or removing EfiMirage, remove the task with
`powershell -ExecutionPolicy Bypass -File .\install-vbs-task.ps1 -Remove`. Removing the task does
not revert DeviceGuard configuration written by previous `-a` runs. A boot-start driver that reads
`g_CiOptions` before the task runs still sees `0x6`.

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
- **`0xA9` with `FakeDma=0`.** The last byte reflects the host CPU's APIC virtualization, not VBS.
  `FakeDma=1` changes the first byte instead; it does not enable DMA remapping.

## Verifying

`scripts/test/vbs-probe.ps1` prints the VBS queries, `0xA9`, WMI, registry, and CPU state.
With VBS emulation on, the output should match a real memory-integrity machine for its VBS
queries and WMI, and differ for the process list and CPUID. Check msinfo32 for the `0xCA` DMA
policy report. `g_CiOptions` differs until `-a` runs. `EfiDSEFix -v` reports VBS state compactly.
