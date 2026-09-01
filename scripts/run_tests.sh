#!/usr/bin/env bash
# =============================================================
# run_tests.sh — kept as a forwarder. The suite now lives in tests/.
#
#   ./tests/run.sh [options]
#
# Everything this script used to do — the unit build, the pre-test snapshot,
# the baseline load, the ordered groups, the restore — moved to tests/run.sh,
# where order and database state are declared per test in a `meta` file rather
# than implied by filename (docs/109).
#
# The forwarder exists because the old path is in CI, in docs, and in muscle
# memory, and a command that silently does nothing is worse than one that
# tells you where it went. Every flag is passed straight through.
# =============================================================
cd "$(dirname "$0")/.." || exit 1
echo "  (run_tests.sh now forwards to tests/run.sh — see docs/109)"
exec bash tests/run.sh "$@"
