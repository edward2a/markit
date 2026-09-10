#!/bin/bash
# Wrapper so the pty harness (which execs a single binary path) can drive
# markit under valgrind tools. Usage: vg_markit.sh <tool> <args...>
# Run from the repo root, e.g.:
#   python3 helper_scripts/check_search.py  # with BINARY overridden, or
#   helper_scripts/vg_markit.sh helgrind --config <cfg> <doc>
tool="$1"
shift
exec valgrind --tool="$tool" --error-exitcode=42 ./build/markit "$@"
