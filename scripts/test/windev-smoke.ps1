# Smoke test for a cross-compiled EfiDSEFix.exe on the Windows host.
#
# Verifies that the MinGW link produced a binary that actually loads and runs,
# and that command dispatch and NTSTATUS propagation work. It does NOT exercise
# the driver: with no EfiGuard loaded, every backdoor command must fail with
# STATUS_INVALID_PARAMETER (0xC000000D).
#
# ONLY VALID FOR THE CONSOLE BUILD (EfiDSEFix.exe). The native-subsystem build
# (EfiDSEFix-native.exe, SUBSYSTEM_NATIVE / 0x1) is rejected by the Win32
# loader with "The %1 application cannot be run in Win32 mode" -- it can only be
# started by the kernel during early boot. This script therefore refuses to run
# against a native-subsystem image rather than reporting failures that say
# nothing about the binary.
#
# Usage, from the Windows host:
#   powershell -NoProfile -ExecutionPolicy Bypass -File windev-smoke.ps1 [-Path <exe>]
#
# Copy the script and console executable to the test host before invoking it.
param(
    [string]$Path = '.\EfiDSEFix.exe'
)

if (-not (Test-Path $Path)) {
    Write-Error "not found: $Path"
    exit 1
}

# Read the PE subsystem so a native-subsystem image is rejected up front.
# 0xE_lfanew at 0x3C; optional header starts at e_lfanew + 24; Subsystem is at
# offset 0x44 into the optional header (PE32+).
$bytes = [System.IO.File]::ReadAllBytes((Resolve-Path $Path))
if ($bytes.Length -lt 0x100) {
    Write-Error "not a PE file: $Path"
    exit 1
}
$e_lfanew = [BitConverter]::ToInt32($bytes, 0x3C)
$subsystem = [BitConverter]::ToUInt16($bytes, $e_lfanew + 24 + 0x44)

Write-Host ("subsystem: 0x{0:X2}" -f $subsystem)
if ($subsystem -eq 0x01) {
    Write-Host 'This is a NATIVE-subsystem build (EfiDSEFix-native.exe).'
    Write-Host 'It cannot run as a Win32 process; skipping. Nothing is wrong with the binary.'
    exit 0
}
if ($subsystem -ne 0x03) {
    Write-Host "unexpected subsystem 0x$('{0:X2}' -f $subsystem); expected 0x03 (console)"
    exit 1
}

# No-argument and help invocations succeed. Argument parsing and the
# backdoor-dependent commands fail with STATUS_INVALID_PARAMETER.
$expected = [ordered]@{
    ''   = 0x00000000
    '-h' = 0x00000000
    '-i' = 0x00000000
    '-z' = 0xC000000D
    '-c' = 0xC000000D
    '-r' = 0xC000000D
    '-d' = 0xC000000D
    # Reads firmware variables and the registry only; 0 when every source agrees. A
    # machine with Secure Boot genuinely off reports 1, so both are accepted.
    '-s' = 0x00000000
}

$failed = 0
foreach ($a in $expected.Keys) {
    $got = $null
    try {
        # -ArgumentList rejects an empty array, so omit it for the no-argument case.
        $spArgs = @{
            FilePath     = $Path
            Wait         = $true
            PassThru     = $true
            NoNewWindow  = $true
            ErrorAction  = 'Stop'
        }
        if ($a -ne '') { $spArgs['ArgumentList'] = $a }
        $p = Start-Process @spArgs
        $got = $p.ExitCode
    } catch {
        # Report the loader's own message; guessing at an exit code here would
        # hide a link or image-layout problem behind a plausible-looking number.
        '{0,-5} LAUNCH FAILED: {1}' -f "[$a]", $_.Exception.Message
        $failed++
        continue
    }

    $want = $expected[$a]
    $got32 = $got -band 0xFFFFFFFF
    # -s is a report, not an action: 1 means "not consistently enabled", which is a
    # legitimate result on firmware without Secure Boot rather than a failure.
    $ok = ($got32 -eq $want) -or ($a -eq '-s' -and $got32 -in 0, 1)
    if (-not $ok) { $failed++ }
    '{0,-5} want 0x{1:X8} got 0x{2:X8}  {3}' -f "[$a]", $want, $got32, $(if ($ok) {'ok'} else {'MISMATCH'})
}

Write-Host ''
if ($failed -gt 0) {
    Write-Host "$failed check(s) failed"
    exit 1
}
Write-Host 'all checks passed'
