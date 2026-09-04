#!/bin/bash
# =============================================================
# tools/migrate_all_tenants.sh
#
# Deploy step: run pending schema migrations (and ensureSchema) for EVERY
# tenant database registered in config/tenants.json, plus the primary db.
# This is the cross-tenant migration runner (docs/072 §3.5): `--provision`
# boots the container, loops all tenants applying ensureSchema + migrations,
# then exits without serving.
#
# Run this before restarting the server on any deploy that adds migrations.
# =============================================================
set -euo pipefail
cd "$(dirname "$0")/.."
echo "[migrate] applying schema + migrations to all tenants ..."
exec ./build/c-erp --provision
