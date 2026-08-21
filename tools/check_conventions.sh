#!/usr/bin/env bash
# Comment conventions from CLAUDE.md; --changed limits it to the diff.
set -uo pipefail
cd "$(dirname "$0")/.."
exec python3 tools/check_conventions.py "$@"
