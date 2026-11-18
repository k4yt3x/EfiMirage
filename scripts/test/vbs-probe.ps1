# Dumps every NtQuerySystemInformation class that reports VBS/HVCI state, plus the
# registry, WMI and CPU properties that the same tools read. Read-only.
#
# Run it twice on one machine -- once with VBS/memory integrity off, once with it on --
# and keep both outputs: the diff is the ground truth an emulation has to reproduce.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File vbs-probe.ps1

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class NtInfo {
    [DllImport("ntdll.dll")]
    public static extern int NtQuerySystemInformation(int Class, IntPtr Buffer, int Length, out int ReturnLength);
}
"@

# Each class has its own idea of the right buffer size: 0x67 takes a caller-initialised
# Length in the first field and rejects a larger buffer, 0xA4/0xA5 reject anything but
# their exact size.
function Get-Info([int]$Class, [int]$Size, [bool]$SetLength) {
    $buffer = [Runtime.InteropServices.Marshal]::AllocHGlobal($Size)
    try {
        for ($i = 0; $i -lt $Size; $i++) { [Runtime.InteropServices.Marshal]::WriteByte($buffer, $i, 0) }
        if ($SetLength -and $Size -ge 4) { [Runtime.InteropServices.Marshal]::WriteInt32($buffer, 0, $Size) }
        $returned = 0
        $status = [NtInfo]::NtQuerySystemInformation($Class, $buffer, $Size, [ref]$returned)
        if ($status -ne 0) {
            return [pscustomobject]@{ Status = ('0x{0:X8}' -f $status); Bytes = $null }
        }
        $count = [Math]::Min($returned, $Size)
        $bytes = New-Object byte[] $count
        [Runtime.InteropServices.Marshal]::Copy($buffer, $bytes, 0, $count)
        return [pscustomobject]@{ Status = 'OK'; Bytes = $bytes }
    } finally {
        [Runtime.InteropServices.Marshal]::FreeHGlobal($buffer)
    }
}

function Get-U32($info, [int]$offset) {
    if ($null -eq $info.Bytes -or $info.Bytes.Length -lt ($offset + 4)) { return 0 }
    return [BitConverter]::ToUInt32($info.Bytes, $offset)
}

function Get-Hex($info) {
    if ($null -eq $info.Bytes) { return '' }
    return (($info.Bytes | ForEach-Object { '{0:X2}' -f $_ }) -join ' ')
}

function Format-CiOptions([uint32]$value) {
    # Ordered array rather than a hashtable: -band against hashtable keys coerced the
    # key text and reported the wrong bit names.
    $bits = @(
        @{ Mask = 0x1; Name = 'ENABLED' },
        @{ Mask = 0x2; Name = 'TESTSIGN' },
        @{ Mask = 0x4; Name = 'UMCI_ENABLED' },
        @{ Mask = 0x8; Name = 'UMCI_AUDITMODE' },
        @{ Mask = 0x10; Name = 'UMCI_EXCLUSIONPATHS' },
        @{ Mask = 0x20; Name = 'TEST_BUILD' },
        @{ Mask = 0x40; Name = 'PREPRODUCTION' },
        @{ Mask = 0x80; Name = 'DEBUGMODE' },
        @{ Mask = 0x100; Name = 'FLIGHT_BUILD' },
        @{ Mask = 0x200; Name = 'FLIGHTING' },
        @{ Mask = 0x400; Name = 'HVCI_KMCI_ENABLED' },
        @{ Mask = 0x800; Name = 'HVCI_KMCI_AUDITMODE' },
        @{ Mask = 0x1000; Name = 'HVCI_KMCI_STRICTMODE' },
        @{ Mask = 0x2000; Name = 'HVCI_IUM_ENABLED' },
        @{ Mask = 0x4000; Name = 'WHQL_ENFORCEMENT' },
        @{ Mask = 0x8000; Name = 'WHQL_AUDITMODE' }
    )
    $set = @()
    foreach ($bit in $bits) {
        if (($value -band $bit.Mask) -ne 0) { $set += $bit.Name }
    }
    if ($set.Count -eq 0) { return '(none)' }
    return ($set -join ' | ')
}

Write-Output ("=== {0}, build {1} ===" -f (Get-CimInstance Win32_OperatingSystem).Caption,
    (Get-CimInstance Win32_OperatingSystem).Version)

$ci = Get-Info 0x67 8 $true
Write-Output ("`nSystemCodeIntegrityInformation (0x67): status {0}, options 0x{1:X8} [{2}]" -f `
    $ci.Status, (Get-U32 $ci 4), (Format-CiOptions (Get-U32 $ci 4)))

$policy = Get-Info 0xA4 32 $false
Write-Output ("SystemCodeIntegrityPolicyInformation (0xA4): status {0}, options 0x{1:X8}, hvci 0x{2:X8}, raw {3}" -f `
    $policy.Status, (Get-U32 $policy 0), (Get-U32 $policy 4), (Get-Hex $policy))

$ium = Get-Info 0xA5 16 $false
Write-Output ("SystemIsolatedUserModeInformation (0xA5): status {0}, raw {1}" -f $ium.Status, (Get-Hex $ium))
if ($null -ne $ium.Bytes) {
    $low = $ium.Bytes[0]; $high = $ium.Bytes[1]
    $flags = [ordered]@{
        SecureKernelRunning    = [bool]($low -band 0x01)
        HvciEnabled            = [bool]($low -band 0x02)
        HvciStrictMode         = [bool]($low -band 0x04)
        DebugEnabled           = [bool]($low -band 0x08)
        FirmwarePageProtection = [bool]($low -band 0x10)
        EncryptionKeyAvailable = [bool]($low -band 0x20)
        TrustletRunning        = [bool]($high -band 0x01)
        HvciDisableAllowed     = [bool]($high -band 0x02)
    }
    $flags.GetEnumerator() | ForEach-Object { Write-Output ("    {0,-24} {1}" -f $_.Key, $_.Value) }
}

$vsm = Get-Info 0xA9 4 $false
Write-Output ("SystemVsmProtectionInformation (0xA9): status {0}, raw {1}" -f $vsm.Status, (Get-Hex $vsm))

$hv = Get-Info 0x5B 16 $false
Write-Output ("SystemHypervisorInformation (0x5B): status {0}, raw {1}" -f $hv.Status, (Get-Hex $hv))

Write-Output "`n--- Win32_DeviceGuard ---"
Get-CimInstance -Namespace root\Microsoft\Windows\DeviceGuard -ClassName Win32_DeviceGuard -ErrorAction SilentlyContinue |
    Format-List VirtualizationBasedSecurityStatus, SecurityServicesConfigured, SecurityServicesRunning,
        AvailableSecurityProperties, CodeIntegrityPolicyEnforcementStatus, UsermodeCodeIntegrityPolicyEnforcementStatus

Write-Output "--- CPU / hypervisor ---"
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
Write-Output ("HypervisorPresent: {0}, VirtualizationFirmwareEnabled: {1}, SLAT: {2}" -f `
    (Get-CimInstance Win32_ComputerSystem).HypervisorPresent, $cpu.VirtualizationFirmwareEnabled,
    $cpu.SecondLevelAddressTranslationExtensions)

Write-Output "`n--- DeviceGuard registry ---"
reg query "HKLM\SYSTEM\CurrentControlSet\Control\DeviceGuard" /s 2>&1 | Select-String -Pattern 'HKEY|REG_'
Write-Output "--- CI policy registry ---"
reg query "HKLM\SYSTEM\CurrentControlSet\Control\CI\Policy" 2>&1 | Select-String -Pattern 'HKEY|REG_'
