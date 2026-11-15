#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/scripts/common.sh"
[ "$#" -le 2 ] || fail "usage: scripts/build-windows.sh [release|debug|noopt] [console|native]"
set_profile "${1:-release}"
variant=${2:-console}
case "$variant" in
    console) subsystem=console:6.0; name=EfiDSEFix.exe ;;
    native) subsystem=native:6.0; name=EfiDSEFix-native.exe ;;
    *) fail "unknown Windows variant '$variant'; expected console or native" ;;
esac
require_tools git x86_64-w64-mingw32-gcc x86_64-w64-mingw32-g++ x86_64-w64-mingw32-windres
check_edk2_revision

SRC="$ROOT/Application/EfiDSEFix/src"
LIB="$ROOT/Application/EfiDSEFix/lib/x64/ntdllp.lib"
COMPAT="$ROOT/scripts/compat/mingw"
OUT="$WINDOWS_OUT/$variant"
OBJ="$OUT/obj"
[ -f "$LIB" ] || fail "the repository's x64 ntdllp.lib is missing"
mkdir -p "$OBJ"
rm -f "$OUT/$name"

case "$PROFILE" in
    release) set -- -O2 -DNDEBUG ;;
    debug|noopt) set -- -O0 -g -D_DEBUG ;;
esac
if [ "$variant" = native ]; then set -- "$@" -D_NATIVE; fi
# Keep the Vista ABI and use the same private ntdll imports as the MSVC build.
set -- "$@" -fno-stack-protector -fno-asynchronous-unwind-tables \
    -I "$COMPAT" -I "$SRC" -I "$ROOT/Include" \
    -I "$EDK2_DIR/MdePkg/Include" -I "$EDK2_DIR/MdePkg/Include/X64" \
    -D_M_AMD64 -DWINVER=0x0600 -D_WIN32_WINNT=0x0600 -DNTDDI_VERSION=0x06000200 \
    -include "$COMPAT/efimirage-mingw.h"
for source in main EfiDSEFix pe sysinfo; do
    x86_64-w64-mingw32-g++ "$@" -std=c++20 -fno-exceptions -fno-rtti -fno-threadsafe-statics \
        -c "$SRC/$source.cpp" -o "$OBJ/$source.o"
done
x86_64-w64-mingw32-gcc "$@" -std=gnu11 -c "$SRC/hde/hde64.c" -o "$OBJ/hde64.o"
x86_64-w64-mingw32-windres -O coff -I "$SRC" -i "$SRC/resource.rc" -o "$OBJ/resource.o"
set --
if [ "$variant" = console ]; then set -- -lkernel32; fi
x86_64-w64-mingw32-g++ -nostdlib -o "$OUT/$name" \
    "$OBJ/main.o" "$OBJ/EfiDSEFix.o" "$OBJ/pe.o" "$OBJ/sysinfo.o" "$OBJ/hde64.o" "$OBJ/resource.o" \
    "$LIB" "-Wl,--subsystem,$subsystem" -Wl,-e,NtProcessStartupW \
    -Wl,--disable-auto-import -Wl,--no-insert-timestamp "$@"
printf 'Windows image: %s\n' "$OUT/$name"
