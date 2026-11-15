#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/scripts/common.sh"
require_tools just python3
cd "$ROOT"
just --fmt --check
for script in "$ROOT"/scripts/*.sh "$ROOT"/scripts/test/*.sh; do sh -n "$script"; done

mkdir -p "$ROOT/.scratch"
scratch=$(mktemp -d "$ROOT/.scratch/check.XXXXXX")
trap 'rm -rf -- "$scratch"' 0
trap 'exit 130' INT
trap 'exit 143' TERM
export TMPDIR="$scratch"
export PYTHONDONTWRITEBYTECODE=1
cd "$scratch"
python3 - "$ROOT/scripts" <<'PY'
import ast
from pathlib import Path
import sys
for path in Path(sys.argv[1]).rglob('*.py'):
    ast.parse(path.read_text(), filename=str(path))
PY
python3 "$ROOT/scripts/test/test-artifacts.py"
python3 "$ROOT/scripts/test/test-build.py"
python3 "$ROOT/scripts/test/test-release-notes.py"
printf 'Workspace checks passed.\n' >&2
