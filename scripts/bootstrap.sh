#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/scripts/common.sh"
[ "$#" -eq 0 ] || fail "usage: just bootstrap"
require_tools git make python3 gcc g++ nasm sha256sum

mkdir -p "$ROOT/deps" "$ROOT/out/bootstrap"
if [ ! -e "$EDK2_DIR" ]; then
    git clone --depth 1 --branch "$EDK2_TAG" "$EDK2_URL" "$EDK2_DIR"
fi
check_edk2_revision

prepare_submodule() {
    repository=$1
    module=$2
    shift 2
    statuses=$(git -C "$repository" submodule status "$@" -- "$module")
    if printf '%s\n' "$statuses" | grep -q '^[+U]'; then
        fail "submodule $module differs from its pinned revision; existing checkout left untouched"
    fi
    git -C "$repository" submodule update --init --depth 1 "$@" -- "$module"
}
prepare_submodule "$ROOT" EfiGuardDxe/Zydis --recursive
for submodule in $EDK2_SUBMODULES; do
    prepare_submodule "$EDK2_DIR" "$submodule"
done

if [ ! -d "$VENV_DIR" ]; then
    python3 -m venv "$VENV_DIR"
fi
[ -x "$VENV_DIR/bin/python3" ] || fail "deps/venv is incomplete; recreate it before running just bootstrap"
# Device-tree tooling is not used by this package; pylibfdt's sdist does not
# build with the host Python. Keep the generated requirements out of EDK2.
requirements="$ROOT/out/bootstrap/requirements.txt"
grep -viE '^[[:space:]]*(pylibfdt|libfdt)' "$EDK2_DIR/pip-requirements.txt" >"$requirements"
requirements_hash=$(sha256sum "$requirements" | cut -d ' ' -f 1)
stamp="$VENV_DIR/.requirements.sha256"
if [ ! -f "$stamp" ] || [ "$(cat "$stamp")" != "$requirements_hash" ]; then
    "$VENV_DIR/bin/python3" -m pip install -r "$requirements"
    printf '%s\n' "$requirements_hash" >"$stamp"
fi
"$VENV_DIR/bin/python3" -m pip check
make -C "$EDK2_DIR/BaseTools" -j"$(getconf _NPROCESSORS_ONLN)"
check_dependencies
printf '\nDependencies ready. Next: just build\n'
