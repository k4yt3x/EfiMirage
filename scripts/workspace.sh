#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/scripts/common.sh"
cd "$ROOT"

action=${1:-}
[ "$#" -ge 1 ] || fail "use just --list to see the available tasks"
shift
case "$action" in
    build|efi|windows|native|verify)
        [ "$#" -le 1 ] || fail "expected at most one profile"
        set_profile "${1:-release}"
        ;;
    bootstrap|doctor|check|clean)
        [ "$#" -eq 0 ] || fail "$action takes no arguments"
        ;;
    *) fail "unknown task '$action'; use just --list" ;;
esac

# The lock lives outside out/ so clean cannot unlink a lock another process holds.
require_tools flock
mkdir -p "$ROOT/deps"
exec 9>"$ROOT/deps/.build.lock"
flock -n 9 || fail "another workspace task is running; wait for it to finish"

run_logged() {
    label=$1
    shift
    mkdir -p "$ROOT/out/logs"
    log="$ROOT/out/logs/$label.log"
    printf '==> %s (log: out/logs/%s.log)\n' "$label" "$label" >&2
    if "$@" >"$log" 2>&1; then
        printf '==> %s passed\n' "$label" >&2
    else
        status=$?
        tail -n 80 "$log" >&2
        printf 'error: %s failed (exit %s); full log: %s\n' "$label" "$status" "$log" >&2
        exit "$status"
    fi
}

case "$action" in
    bootstrap) "$ROOT/scripts/bootstrap.sh" ;;
    doctor)
        require_tools just git make python3 gcc g++ nasm x86_64-w64-mingw32-gcc x86_64-w64-mingw32-g++ x86_64-w64-mingw32-windres
        check_dependencies
        printf 'Build prerequisites are ready (EDK2 %s).\n' "$EDK2_TAG"
        ;;
    check) "$ROOT/scripts/check.sh" ;;
    build)
        require_tools just git make python3 gcc g++ nasm x86_64-w64-mingw32-gcc x86_64-w64-mingw32-g++ x86_64-w64-mingw32-windres
        check_dependencies
        "$ROOT/scripts/check.sh"
        run_logged "efi-$PROFILE" "$ROOT/scripts/build-efi.sh" "$PROFILE"
        run_logged "windows-$PROFILE" "$ROOT/scripts/build-windows.sh" "$PROFILE" console
        python3 "$ROOT/scripts/artifacts.py" stage "$PROFILE"
        ;;
    efi) run_logged "efi-$PROFILE" "$ROOT/scripts/build-efi.sh" "$PROFILE" ;;
    windows) run_logged "windows-$PROFILE" "$ROOT/scripts/build-windows.sh" "$PROFILE" console ;;
    native) run_logged "native-$PROFILE" "$ROOT/scripts/build-windows.sh" "$PROFILE" native ;;
    verify) python3 "$ROOT/scripts/artifacts.py" verify "$PROFILE" ;;
    clean)
        rm -rf -- "$ROOT/out" "$ROOT/dist"
        printf 'Removed out/ and dist/; dependencies and sources retained.\n'
        ;;
esac
