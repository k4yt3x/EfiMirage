#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/scripts/common.sh"
[ "$#" -le 1 ] || fail "usage: just efi [release|debug|noopt]"
set_profile "${1:-release}"
require_tools git make gcc g++ nasm
check_dependencies

mkdir -p "$WS_DIR/Conf" "$PKGS_DIR" "$EFI_OUT"
if [ ! -e "$PKGS_DIR/EfiMiragePkg" ]; then
    ln -s ../.. "$PKGS_DIR/EfiMiragePkg"
fi
[ "$(readlink -f "$PKGS_DIR/EfiMiragePkg")" = "$ROOT" ] || fail "out/packages/EfiMiragePkg does not point to this repository; run just clean"
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
rm -f "$EFI_OUT/EfiMirageDxe.efi" "$EFI_OUT/Loader.efi" \
    "$EFI_OUT/EfiMirageDxe.debug" "$EFI_OUT/Loader.debug" \
    "$EFI_OUT/EfiMirageDxe.map" "$EFI_OUT/Loader.map" \
    "$EFI_OUT/test/SecureBootTest.efi" "$EFI_OUT/test/SecureBootRuntimeTest.efi"
"$PYTHON_COMMAND" "$EDK_TOOLS_PATH/Source/Python/build/build.py" \
    -a X64 -t GCC -b "$TARGET" -p EfiMiragePkg/EfiMiragePkg.dsc

BUILD_DIR="$WS_DIR/Build/EfiMirage/${TARGET}_GCC/X64"
for module in EfiMirageDxe Loader; do
    [ -f "$BUILD_DIR/$module.efi" ] || fail "$module.efi is missing after the build"
    cp "$BUILD_DIR/$module.efi" "$EFI_OUT/$module.efi"
    case "$module" in
        EfiMirageDxe) debug_dir="$BUILD_DIR/EfiMiragePkg/EfiMirageDxe/EfiMirageDxe/DEBUG" ;;
        Loader) debug_dir="$BUILD_DIR/EfiMiragePkg/Application/Loader/Loader/DEBUG" ;;
    esac
    for extra in debug map; do
        [ -f "$debug_dir/$module.$extra" ] || fail "$module.$extra is missing after the build"
        cp "$debug_dir/$module.$extra" "$EFI_OUT/$module.$extra"
    done
done

# Self-test application for the OVMF harness; kept out of the component output directory
mkdir -p "$EFI_OUT/test"
for test_app in SecureBootTest SecureBootRuntimeTest; do
    [ -f "$BUILD_DIR/$test_app.efi" ] || fail "$test_app.efi is missing after the build"
    cp "$BUILD_DIR/$test_app.efi" "$EFI_OUT/test/$test_app.efi"
done

printf 'EFI images: %s\n' "$EFI_OUT"
