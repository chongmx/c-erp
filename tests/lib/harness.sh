#!/usr/bin/env bash
# =============================================================
# harness.sh — the one file a test sources.
#
# Every test opens with the same four lines, which find the repository root by
# walking up for CMakeLists.txt rather than counting `../`:
#
#     R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
#     while [ "$R" != "/" ] && [ ! -f "$R/CMakeLists.txt" ]; do R="$(dirname "$R")"; done
#     cd "$R" || exit 1
#     source tests/lib/harness.sh
#
# Depth-independent on purpose: a test can be nested one folder deeper for
# organisation without a preamble edit, and it behaves identically whether the
# runner invoked it or you ran it directly from any directory.
#
# The relative paths inside a test therefore always resolve from the repository
# root — `./build/c-erp`, `web/static/...`, `db/snapshots/...` — exactly as
# they did when these scripts lived in scripts/.
# =============================================================
[ -n "${ERP_HARNESS_LOADED:-}" ] && return 0
ERP_HARNESS_LOADED=1

ERP_ROOT="${ERP_ROOT:-$PWD}"
export ERP_ROOT

source "$ERP_ROOT/tests/lib/assert.sh"
source "$ERP_ROOT/tests/lib/db.sh"
source "$ERP_ROOT/tests/lib/api.sh"

# Fixtures are NOT sourced automatically. A test that declares `needs=fixtures`
# in its meta gets them from the runner; one that does not should not be able
# to reach fx_create by accident and quietly seed rows nobody expected.
fixtures(){ source "$ERP_ROOT/tests/lib/fixtures.sh"; }
