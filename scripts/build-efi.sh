#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/scripts/common.sh"
[ "$#" -le 1 ] || fail "usage: just efi [release|debug|noopt]"
set_profile "${1:-release}"
require_tools git make gcc g++ nasm
check_dependencies

mkdir -p "$WS_DIR/Conf" "$PKGS_DIR" "$EFI_OUT"
if [ ! -e "$PKGS_DIR/EfiGuardPkg" ]; then
    ln -s ../.. "$PKGS_DIR/EfiGuardPkg"
fi
[ "$(readlink -f "$PKGS_DIR/EfiGuardPkg")" = "$ROOT" ] || fail "out/packages/EfiGuardPkg does not point to this repository; run just clean"
for name in build_rule tools_def target; do
    if [ ! -f "$WS_DIR/Conf/$name.txt" ]; then
        cp "$EDK2_DIR/BaseTools/Conf/$name.template" "$WS_DIR/Conf/$name.txt"
    fi
done

export WORKSPACE="$WS_DIR"
export EDK_TOOLS_PATH="$EDK2_DIR/BaseTools"
export PACKAGES_PATH="$EDK2_DIR:$PKGS_DIR"
export PYTHON_COMMAND="$VENV_DIR/bin/python3"
export PYTHONPATH="$EDK_TOOLS_PATH/Source/Python"
export PATH="$EDK_TOOLS_PATH/BinWrappers/PosixLike:$EDK_TOOLS_PATH/Bin/Linux-x86_64:$VENV_DIR/bin:$PATH"

# A previous component output must not masquerade as the result of this build.
rm -f "$EFI_OUT/EfiGuardDxe.efi" "$EFI_OUT/Loader.efi" \
    "$EFI_OUT/EfiGuardDxe.debug" "$EFI_OUT/Loader.debug" \
    "$EFI_OUT/EfiGuardDxe.map" "$EFI_OUT/Loader.map"
"$PYTHON_COMMAND" "$EDK_TOOLS_PATH/Source/Python/build/build.py" \
    -a X64 -t GCC -b "$TARGET" -p EfiGuardPkg/EfiGuardPkg.dsc

BUILD_DIR="$WS_DIR/Build/EfiGuard/${TARGET}_GCC/X64"
for module in EfiGuardDxe Loader; do
    [ -f "$BUILD_DIR/$module.efi" ] || fail "$module.efi is missing after the build"
    cp "$BUILD_DIR/$module.efi" "$EFI_OUT/$module.efi"
    case "$module" in
        EfiGuardDxe) debug_dir="$BUILD_DIR/EfiGuardPkg/EfiGuardDxe/EfiGuardDxe/DEBUG" ;;
        Loader) debug_dir="$BUILD_DIR/EfiGuardPkg/Application/Loader/Loader/DEBUG" ;;
    esac
    for extra in debug map; do
        [ -f "$debug_dir/$module.$extra" ] || fail "$module.$extra is missing after the build"
        cp "$debug_dir/$module.$extra" "$EFI_OUT/$module.$extra"
    done
done
printf 'EFI images: %s\n' "$EFI_OUT"
