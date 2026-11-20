# Installs (or removes) a boot-time scheduled task that runs "EfiDSEFix -a".
#
# -a has to run once per boot: Windows clears the DeviceGuard configuration keys when VBS fails to
# start, CI\State\HVCIEnabled is volatile, and the kernel reinitialises g_CiOptions at every boot.
# Running as SYSTEM at start also covers checks that happen before anyone logs in.
#
#   powershell -ExecutionPolicy Bypass -File install-vbs-task.ps1
#   powershell -ExecutionPolicy Bypass -File install-vbs-task.ps1 -Remove

param(
    [string]$Exe = (Join-Path $PSScriptRoot 'EfiDSEFix.exe'),
    [switch]$Remove
)

$TaskName = 'EfiMirage VBS state'

if ($Remove) {
    schtasks /delete /tn $TaskName /f
    exit $LASTEXITCODE
}

if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
    Write-Error "not found: $Exe"
    exit 1
}

# SYSTEM starts the task in a different working directory; store the resolved path.
$ExePath = (Resolve-Path -LiteralPath $Exe).ProviderPath
schtasks /create /tn $TaskName /tr "`"$ExePath`" -a" /sc onstart /ru SYSTEM /rl HIGHEST /f
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

schtasks /query /tn $TaskName /v /fo list | Select-String -Pattern 'TaskName|Task To Run|Run As User|Schedule Type'
