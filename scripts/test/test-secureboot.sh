#!/bin/sh
# Secure Boot emulation self-test under OVMF.
#
# Boots a test application in a scratch ESP on firmware with no platform key, where
# SecureBoot reads 0 and SetupMode 1, and checks that the emulation takes effect with the
# correct UEFI GetVariable semantics. Two variants exist:
#
#   boot     every GetVariable call shape, attribute set and fall-through case, at boot services time
#   runtime  the same hook after ExitBootServices() and SetVirtualAddressMap(), where a missed
#            pointer conversion would fault
#
# The firmware comes from a locally extracted edk2-ovmf package (see docs/secureboot.md).
#
# Usage: scripts/test/test-secureboot.sh [boot|runtime] [release|debug]
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TOOLING="$ROOT/.tooling"

VARIANT=boot
PROFILE=release
for arg in "$@"; do
	case "$arg" in
	boot|runtime) VARIANT=$arg ;;
	release|debug|noopt) PROFILE=$arg ;;
	*) printf 'error: unknown argument %s\n' "$arg" >&2; exit 2 ;;
	esac
done

case "$VARIANT" in
boot) APP=SecureBootTest ;;
runtime) APP=SecureBootRuntimeTest ;;
esac

EFI_DIR="$ROOT/out/efi/$PROFILE"
QEMU="$TOOLING/root/usr/bin/qemu-system-x86_64"
CODE="$TOOLING/root/usr/share/edk2/x64/OVMF_CODE.secboot.4m.fd"
VARS_TEMPLATE="$TOOLING/root/usr/share/edk2/x64/OVMF_VARS.4m.fd"

[ -x "$QEMU" ] || { printf 'error: QEMU is missing; see docs/secureboot.md for the tooling setup\n' >&2; exit 2; }
[ -f "$CODE" ] && [ -f "$VARS_TEMPLATE" ] || { printf 'error: OVMF firmware is missing from %s\n' "$TOOLING" >&2; exit 2; }
[ -f "$EFI_DIR/EfiGuardDxe.efi" ] || { printf 'error: driver is missing; run just efi %s first\n' "$PROFILE" >&2; exit 2; }
[ -f "$EFI_DIR/test/$APP.efi" ] || { printf 'error: %s is missing; run just efi %s first\n' "$APP" "$PROFILE" >&2; exit 2; }

mkdir -p "$ROOT/.scratch"
SCRATCH=$(mktemp -d "$ROOT/.scratch/secureboot.XXXXXX")
QEMU_PID=
stop_qemu() {
	if [ -n "$QEMU_PID" ]; then
		kill "$QEMU_PID" 2>/dev/null || true
		wait "$QEMU_PID" 2>/dev/null || true
		QEMU_PID=
	fi
}
trap 'stop_qemu; rm -rf -- "$SCRATCH"' 0
trap 'exit 130' INT
trap 'exit 143' TERM

mkdir -p "$SCRATCH/esp/EFI/BOOT"
cp "$EFI_DIR/test/$APP.efi" "$SCRATCH/esp/EFI/BOOT/BOOTX64.EFI"
cp "$EFI_DIR/EfiGuardDxe.efi" "$SCRATCH/esp/EFI/BOOT/EfiGuardDxe.efi"
cp "$VARS_TEMPLATE" "$SCRATCH/OVMF_VARS.fd"
LOG="$SCRATCH/serial.log"

LD_LIBRARY_PATH="$TOOLING/root/usr/lib:$TOOLING/root/usr/lib64" \
	"$QEMU" \
	-machine q35 -m 512 -smp 1 \
	-display none -monitor none -serial "file:$LOG" \
	-drive if=pflash,format=raw,unit=0,readonly=on,file="$CODE" \
	-drive if=pflash,format=raw,unit=1,file="$SCRATCH/OVMF_VARS.fd" \
	-drive format=raw,file="fat:rw:$SCRATCH/esp" \
	-net none -no-reboot -boot order=c &
QEMU_PID=$!

for _ in $(seq 1 240); do
	grep -q 'RESULT:' "$LOG" 2>/dev/null && break
	kill -0 "$QEMU_PID" 2>/dev/null || break
	sleep 0.5
done
sleep 1
stop_qemu

if [ ! -s "$LOG" ]; then
	printf 'error: the firmware produced no serial output\n' >&2
	exit 1
fi

# BDS retries the boot option after a nonzero exit, so judge only the first run.
sed -n '1,/^RESULT:/p' "$LOG" > "$SCRATCH/first.log"
LOG="$SCRATCH/first.log"

grep -E '^\[(PASS|FAIL)\]|^RESULT:|^Firmware before' "$LOG" || true

if ! grep -q 'RESULT:' "$LOG"; then
	printf 'FAIL: the self-test did not complete\n' >&2
	printf '%s\n' '--- serial log ---' >&2
	cat "$LOG" >&2
	exit 1
fi

# A run that never got as far as its first assertion must not count as a pass.
if ! grep -qE '^RESULT: [1-9][0-9]* passed, 0 failed' "$LOG"; then
	printf '%s\n' '--- serial log ---' >&2
	cat "$LOG" >&2
	exit 1
fi

if grep -q '^\[FAIL\]' "$LOG"; then
	printf '%s\n' '--- serial log ---' >&2
	cat "$LOG" >&2
	exit 1
fi

printf 'Secure Boot emulation %s self-test passed (%s).\n' "$VARIANT" "$PROFILE"
