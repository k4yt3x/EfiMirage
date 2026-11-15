# Shared by the build scripts; ROOT is the repository root supplied by the caller.
EDK2_TAG=edk2-stable202608
EDK2_REVISION=2970e5699ba6267f3384ffab20f96647578aebc8
EDK2_URL=https://github.com/tianocore/edk2.git
EDK2_DIR="$ROOT/deps/edk2"
VENV_DIR="$ROOT/deps/venv"
WS_DIR="$ROOT/out/edk2-ws"
PKGS_DIR="$ROOT/out/packages"
EDK2_SUBMODULES='
BaseTools/Source/C/BrotliCompress/brotli
MdePkg/Library/MipiSysTLib/mipisyst
MdeModulePkg/Library/BrotliCustomDecompressLib/brotli
'

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

require_tools() {
    for tool in "$@"; do
        command -v "$tool" >/dev/null 2>&1 || fail "missing host tool: $tool (see docs/build.md)"
    done
}

set_profile() {
    PROFILE=$1
    case "$PROFILE" in
        release) TARGET=RELEASE ;;
        debug) TARGET=DEBUG ;;
        noopt) TARGET=NOOPT ;;
        *) fail "unknown profile '$PROFILE'; expected release, debug, or noopt" ;;
    esac
    EFI_OUT="$ROOT/out/efi/$PROFILE"
    WINDOWS_OUT="$ROOT/out/windows/$PROFILE"
}

check_edk2_revision() {
    [ -d "$EDK2_DIR/.git" ] || fail "EDK2 is missing; run just bootstrap"
    revision=$(git -C "$EDK2_DIR" rev-parse HEAD)
    [ "$revision" = "$EDK2_REVISION" ] || fail "EDK2 must be at $EDK2_TAG ($EDK2_REVISION); existing checkout left untouched"
}

check_submodules() {
    repository=$1
    module=$2
    shift 2
    statuses=$(git -C "$repository" submodule status "$@" -- "$module")
    [ -n "$statuses" ] || fail "missing submodule metadata for $module; run just bootstrap"
    if printf '%s\n' "$statuses" | grep -q '^[+U-]'; then
        fail "submodule $module is missing or differs from its pinned revision; existing checkout left untouched"
    fi
}

check_dependencies() {
    check_edk2_revision
    check_submodules "$ROOT" EfiMirageDxe/Zydis --recursive
    for submodule in $EDK2_SUBMODULES; do
        check_submodules "$EDK2_DIR" "$submodule"
    done
    [ -x "$VENV_DIR/bin/python3" ] || fail "Python environment is missing; run just bootstrap"
    for tool in GenFw GenSec GenFfs; do
        [ -x "$EDK2_DIR/BaseTools/Source/C/bin/$tool" ] || fail "EDK2 $tool is missing; run just bootstrap"
    done
}
