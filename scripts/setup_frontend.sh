#!/usr/bin/env bash
set -euo pipefail

OWL_VERSION="2.7.0"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WEB_DIR="${SCRIPT_DIR}/../web/static"
LIB_DIR="${WEB_DIR}/lib"

info()    { echo "[INFO]  $*"; }
success() { echo "[OK]    $*"; }
fatal()   { echo "[ERROR] $*" >&2; exit 1; }

mkdir -p "${LIB_DIR}"

# ============================================================
# 1. Download OWL
# ============================================================
info "Downloading OWL ${OWL_VERSION}..."

OWL_URL="https://cdn.jsdelivr.net/npm/@odoo/owl@${OWL_VERSION}/dist/owl.iife.js"
OWL_OUT="${LIB_DIR}/owl.iife.js"

if [ -f "${OWL_OUT}" ]; then
    success "OWL already present at ${OWL_OUT}."
else
    curl -fsSL "${OWL_URL}" -o "${OWL_OUT}" \
        || fatal "Failed to download OWL from: ${OWL_URL}"
    success "OWL ${OWL_VERSION} downloaded."
fi

# Sanity check
OWL_SIZE=$(wc -c < "${OWL_OUT}")
[ "${OWL_SIZE}" -gt 10000 ] \
    && success "OWL file valid (${OWL_SIZE} bytes)." \
    || fatal "OWL file too small — download likely failed."

echo ""
echo "================================================================"
echo " Frontend ready. Directory layout:"
echo ""
echo "   web/static/"
echo "   ├── lib/"
echo "   │   └── owl.iife.js       ← OWL framework"
echo "   ├── src/"
echo "   │   ├── components/       ← your OWL components"
echo "   │   └── services/         ← RPC / state helpers"
echo "   └── index.html            ← entry point"
echo ""
echo " Build the C++ server then open:"
echo "   http://localhost:8069"
echo "================================================================"