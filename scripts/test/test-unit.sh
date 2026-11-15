#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$ROOT/scripts/common.sh"
require_tools git gcc python3
check_edk2_revision
check_submodules "$ROOT" EfiMirageDxe/Zydis --recursive
export PYTHONDONTWRITEBYTECODE=1
python3 "$ROOT/scripts/test/test-loader.py"
python3 "$ROOT/scripts/test/test-driver-lifecycle.py"
