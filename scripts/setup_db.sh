#!/usr/bin/env bash
set -euo pipefail

# ============================================================
# Configuration — override with environment variables
# ============================================================
DB_HOST="${DB_HOST:-localhost}"
DB_PORT="${DB_PORT:-5432}"
DB_NAME="${DB_NAME:-odoo}"
DB_USER="${DB_USER:-odoo}"
DB_PASSWORD="${DB_PASSWORD:-odoo}"

# ============================================================
# Helpers
# ============================================================
info()    { echo "[INFO]  $*"; }
success() { echo "[OK]    $*"; }
warn()    { echo "[WARN]  $*"; }
fatal()   { echo "[ERROR] $*" >&2; exit 1; }

pg_running() {
    pg_isready -h "${DB_HOST}" -p "${DB_PORT}" -q 2>/dev/null
}

psql_super() {
    sudo -u postgres psql -v ON_ERROR_STOP=1 "$@"
}

# ============================================================
# 1. Install PostgreSQL and postgresql-common if missing
# ============================================================
info "Checking PostgreSQL installation..."

NEED_INSTALL=0
command -v psql            &>/dev/null || NEED_INSTALL=1
command -v pg_isready      &>/dev/null || NEED_INSTALL=1
command -v pg_createcluster &>/dev/null || NEED_INSTALL=1

if [ "${NEED_INSTALL}" = "1" ]; then
    info "Installing PostgreSQL..."
    sudo apt-get update -q
    sudo apt-get install -y \
        postgresql \
        postgresql-contrib \
        postgresql-common
    success "PostgreSQL installed."
fi

PG_VER=$(psql --version | grep -oP '\d+' | head -1)
success "PostgreSQL ${PG_VER} ready."

# ============================================================
# 2. Ensure a cluster exists (may be missing on fresh installs)
# ============================================================
CLUSTER_COUNT=$(pg_lsclusters --no-header 2>/dev/null | wc -l || echo "0")

if [ "${CLUSTER_COUNT}" -eq 0 ]; then
    info "No PostgreSQL cluster found — creating cluster ${PG_VER}/main..."
    sudo pg_createcluster "${PG_VER}" main
    success "Cluster ${PG_VER}/main created."
fi

# ============================================================
# 3. Start PostgreSQL
# ============================================================
if pg_running; then
    success "PostgreSQL is already running on port ${DB_PORT}."
else
    info "Starting PostgreSQL cluster ${PG_VER}/main..."
    sudo pg_ctlcluster "${PG_VER}" main start

    # Wait up to 15 seconds for PostgreSQL to accept connections
    for i in $(seq 1 15); do
        pg_running && break
        sleep 1
    done
    pg_running \
        || fatal "PostgreSQL started but is not accepting connections. Check: sudo pg_ctlcluster ${PG_VER} main status"
    success "PostgreSQL started."
fi

# ============================================================
# 4. Create role if it doesn't exist
# ============================================================
ROLE_EXISTS=$(psql_super -tAc \
    "SELECT 1 FROM pg_roles WHERE rolname='${DB_USER}';" 2>/dev/null || echo "")

if [ "${ROLE_EXISTS}" = "1" ]; then
    info "Role '${DB_USER}' already exists — updating password..."
    psql_super -c "ALTER ROLE ${DB_USER} WITH LOGIN PASSWORD '${DB_PASSWORD}';"
    success "Password updated for role '${DB_USER}'."
else
    info "Creating role '${DB_USER}'..."
    psql_super -c "CREATE ROLE ${DB_USER} WITH LOGIN PASSWORD '${DB_PASSWORD}';"
    success "Role '${DB_USER}' created."
fi

# ============================================================
# 5. Create database if it doesn't exist
# ============================================================
DB_EXISTS=$(psql_super -tAc \
    "SELECT 1 FROM pg_database WHERE datname='${DB_NAME}';" 2>/dev/null || echo "")

if [ "${DB_EXISTS}" = "1" ]; then
    success "Database '${DB_NAME}' already exists."
else
    info "Creating database '${DB_NAME}'..."
    psql_super -c "CREATE DATABASE ${DB_NAME} OWNER ${DB_USER} ENCODING 'UTF8';"
    success "Database '${DB_NAME}' created."
fi

# ============================================================
# 6. Grant privileges
# ============================================================
info "Granting privileges on '${DB_NAME}' to '${DB_USER}'..."
psql_super -d "${DB_NAME}" \
    -c "GRANT ALL PRIVILEGES ON DATABASE ${DB_NAME} TO ${DB_USER};"
psql_super -d "${DB_NAME}" \
    -c "GRANT ALL ON SCHEMA public TO ${DB_USER};"
success "Privileges granted."

# ============================================================
# 7. Auto-start PostgreSQL on every new WSL session
# ============================================================
# Write to the real user's .bashrc even when running under sudo
REAL_HOME=$(getent passwd "${SUDO_USER:-$USER}" | cut -d: -f6)
BASHRC="${REAL_HOME}/.bashrc"
AUTOSTART_MARKER="# odoo-cpp: auto-start postgresql"

if ! grep -q "${AUTOSTART_MARKER}" "${BASHRC}" 2>/dev/null; then
    info "Adding PostgreSQL auto-start to ${BASHRC}..."
    cat >> "${BASHRC}" << BASHRC_EOF

${AUTOSTART_MARKER}
if ! pg_isready -q 2>/dev/null; then
    sudo pg_ctlcluster ${PG_VER} main start > /dev/null 2>&1
fi
BASHRC_EOF
    success "Auto-start added to ${BASHRC}."
else
    # Update the auto-start in case it has an old/broken command
    sed -i "/${AUTOSTART_MARKER}/,+3d" "${BASHRC}"
    cat >> "${BASHRC}" << BASHRC_EOF

${AUTOSTART_MARKER}
if ! pg_isready -q 2>/dev/null; then
    sudo pg_ctlcluster ${PG_VER} main start > /dev/null 2>&1
fi
BASHRC_EOF
    success "Auto-start updated in ${BASHRC}."
fi

# ============================================================
# 8. Verify connection as the app user
# ============================================================
info "Verifying connection as '${DB_USER}'..."
PGPASSWORD="${DB_PASSWORD}" psql \
    -h "${DB_HOST}" \
    -p "${DB_PORT}" \
    -U "${DB_USER}" \
    -d "${DB_NAME}" \
    -c "SELECT version();" -tA | head -1 \
    && success "Connection verified." \
    || fatal "Could not connect as '${DB_USER}'. Check credentials or run: sudo pg_ctlcluster ${PG_VER} main status"

# ============================================================
# Done
# ============================================================
echo ""
echo "================================================================"
echo " Database ready. Run the server with:"
echo ""
echo "   ./build/c-erp"
echo ""
echo " Custom credentials (if you changed the defaults above):"
echo ""
echo "   DB_HOST=${DB_HOST} DB_PORT=${DB_PORT} DB_NAME=${DB_NAME} \\"
echo "   DB_USER=${DB_USER} DB_PASSWORD=${DB_PASSWORD} ./build/c-erp"
echo "================================================================"