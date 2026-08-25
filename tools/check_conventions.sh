#!/usr/bin/env bash
# Comment conventions from CLAUDE.md; --changed limits it to the diff.
set -uo pipefail
cd "$(dirname "$0")/.."
python3 tools/check_conventions.py "$@"
rc=$?

# The library owns its own rules, and this tree's git ls-files cannot reach them.
# radio_devices_docs/specs/04-tooling-and-coordination.md
if [ -f radio_stack/tools/check_conventions.py ]; then
    echo
    ( cd radio_stack && python3 tools/check_conventions.py ) || rc=1
fi
exit $rc
