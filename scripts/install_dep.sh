#!/usr/bin/env bash
set -euo pipefail

LIBPQXX_TAG="7.9.2"
LIBPQXX_URL="https://github.com/jtv/libpqxx.git"
LIBPQXX_DIR="3rdparty/libpqxx"

# ============================================================
# Helpers
# ============================================================
info()    { echo "[INFO]  $*"; }
success() { echo "[OK]    $*"; }
warn()    { echo "[WARN]  $*"; }

# ============================================================
# 1. Remove conflicting system libpqxx
#
#    libpqxx-dev from apt is version 7.8, but the bundled
#    submodule is 7.9.  If both are present, the compiler
#    picks up 7.9 headers (std::source_location in exceptions)
#    but the linker finds the 7.8 .so — causing unresolved
#    symbols at link time.  Remove it unconditionally.
# ============================================================
info "Removing system libpqxx (conflicts with bundled version)..."
sudo apt-get remove -y libpqxx-dev libpqxx-7.8t64 2>/dev/null || true
sudo apt-get autoremove -y 2>/dev/null || true
success "System libpqxx removed."

# ============================================================
# 2. System packages
# ============================================================
info "Installing system packages..."

sudo apt-get update -q

# Build tools
sudo apt-get install -y \
    build-essential \
    cmake \
    git \
    pkg-config

# PostgreSQL C client — libpqxx links against this at build time.
# libpq-dev is safe to keep; only the C++ libpqxx-dev must be removed.
sudo apt-get install -y \
    libpq-dev \
    postgresql-client

# Drogon required dependencies
sudo apt-get install -y \
    libjsoncpp-dev \
    uuid-dev \
    zlib1g-dev \
    libssl-dev \
    libbrotli-dev \
    brotli \
    libc-ares-dev

# Drogon optional backends (MySQL, SQLite, Redis)
#libmysqlclient-dev \ # Replaced by libmariadb-dev-compat and libmariadb-dev, which provide the same API but are more up to date.
sudo apt-get install -y \
    libmariadb-dev-compat \
    libmariadb-dev \
    libsqlite3-dev \
    libhiredis-dev

# Docs (optional)
sudo apt-get install -y \
    doxygen \
    graphviz

success "System packages installed."

# ============================================================
# 3. Git submodules (drogon, json, etc.)
# ============================================================
info "Initialising git submodules..."
git submodule update --init --recursive
success "Submodules ready."

# ============================================================
# 4. libpqxx submodule
# ============================================================
if [ ! -f "${LIBPQXX_DIR}/CMakeLists.txt" ]; then
    info "libpqxx not found at ${LIBPQXX_DIR} — adding as git submodule..."
    git submodule add --branch "${LIBPQXX_TAG}" \
        "${LIBPQXX_URL}" "${LIBPQXX_DIR}" 2>/dev/null || true
    git submodule update --init --recursive -- "${LIBPQXX_DIR}"
    success "libpqxx ${LIBPQXX_TAG} added at ${LIBPQXX_DIR}."
else
    info "libpqxx already present at ${LIBPQXX_DIR} — updating..."
    git submodule update --init --recursive -- "${LIBPQXX_DIR}"
    success "libpqxx submodule up to date."
fi

# ============================================================
# 5. Wipe stale CMake cache
#
#    Any previous configure run may have cached the now-removed
#    system libpqxx path.  The cache must be deleted so CMake
#    re-discovers everything cleanly via add_subdirectory.
# ============================================================
if [ -d "build" ]; then
    info "Removing stale build directory to clear CMake cache..."
    rm -rf build
    success "Build directory removed."
fi

# ============================================================
# 6. Sanity checks
# ============================================================
echo ""
info "Verifying libpq (system)..."
pkg-config --modversion libpq \
    && success "libpq $(pkg-config --modversion libpq) found." \
    || warn "libpq not found by pkg-config — check libpq-dev install."

info "Verifying libpqxx source..."
[ -f "${LIBPQXX_DIR}/CMakeLists.txt" ] \
    && success "libpqxx source present at ${LIBPQXX_DIR}." \
    || warn "libpqxx CMakeLists.txt missing — submodule may not have cloned correctly."

info "Confirming system libpqxx is gone..."
dpkg -l libpqxx-dev 2>/dev/null | grep -q "^ii" \
    && warn "libpqxx-dev is still installed — remove it manually: sudo apt-get remove libpqxx-dev" \
    || success "libpqxx-dev not installed (correct)."

# ============================================================
# 7. Done
# ============================================================
echo ""
echo "================================================================"
echo " All dependencies ready. Build with:"
echo ""
echo "   cmake -B build"
echo "   cmake --build build -j\$(nproc)"
echo "================================================================"