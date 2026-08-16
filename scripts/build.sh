#!/bin/bash
# =============================================================
# scripts/build.sh — build BOTH binaries.
#
#   ./build/c-erp       the ERP server            (default target)
#   ./build/erp-admin   the IT admin console      (docs/073)
#
# Binaries stay in ./build (paths / systemd unit unchanged).
#
#   ./scripts/build.sh              configure + build both
#   ./scripts/build.sh --clean      wipe ./build first (full rebuild)
#   ./scripts/build.sh --jobs 8     override parallelism
# =============================================================
set -euo pipefail
cd "$(dirname "$0")/.."

JOBS="$(nproc 2>/dev/null || echo 4)"
CLEAN=

while [ $# -gt 0 ]; do
    case "$1" in
        --clean)  CLEAN=1; shift ;;
        --jobs)   JOBS="${2:?--jobs needs a number}"; shift 2 ;;
        -j)       JOBS="${2:?-j needs a number}"; shift 2 ;;
        *)        echo "unknown option: $1"; exit 2 ;;
    esac
done

if [ -n "$CLEAN" ]; then
    echo "[build] --clean: removing ./build"
    rm -rf ./build
fi

echo "[build] configuring (cmake -B ./build) ..."
cmake -B ./build

echo "[build] building c-erp (-j $JOBS) ..."
cmake --build ./build --target c-erp -j "$JOBS"

echo "[build] building erp-admin (-j $JOBS) ..."
cmake --build ./build --target erp-admin -j "$JOBS"

echo
echo "[build] done:"
rc=0
for b in build/c-erp build/erp-admin; do
    if [ -x "$b" ]; then
        printf "  %-18s %s\n" "$b" "$(du -h "$b" 2>/dev/null | cut -f1)"
    else
        echo "  MISSING: $b"; rc=1
    fi
done
exit $rc
