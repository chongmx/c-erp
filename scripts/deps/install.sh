#!/usr/bin/env bash
# =============================================================
# scripts/deps/install.sh — everything a fresh machine needs, in order.
#
#   ./scripts/deps/install.sh            all four steps
#   ./scripts/deps/install.sh --no-db    skip PostgreSQL (already have one)
#
# The steps, and why they are separate scripts you can also run alone:
#
#   install_deps.sh    build toolchain, system libraries, bundled libpqxx.
#                      Also removes apt's libpqxx-dev — 7.8 headers against
#                      the 7.9 submodule is an unresolved-symbol link error.
#   install_wkhtml.sh  wkhtmltopdf with the patched Qt build. The distro
#                      package lacks --footer-html, which the reports use.
#   setup_frontend.sh  downloads OWL into web/static/lib. No npm, no build.
#   setup_db.sh        installs PostgreSQL if missing, creates the role and
#                      database. Tables are created by the app on first boot.
#
# Nothing here touches the source tree beyond web/static/lib and 3rdparty/.
# =============================================================
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

WITH_DB=1
for a in "$@"; do
    case "$a" in
        --no-db)   WITH_DB=0 ;;
        -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "[ERROR] unknown option: $a" >&2; exit 1 ;;
    esac
done

step() { echo; echo "=== $1 ==="; }

step "1/4 build toolchain and libraries";  bash "$HERE/install_deps.sh"
step "2/4 wkhtmltopdf";                    bash "$HERE/install_wkhtml.sh"
step "3/4 frontend (OWL)";                 bash "$HERE/setup_frontend.sh"
if [ "$WITH_DB" = "1" ]; then
    step "4/4 PostgreSQL";                 bash "$HERE/setup_db.sh"
else
    step "4/4 PostgreSQL";                 echo "skipped (--no-db)"
fi

echo
echo "Dependencies installed. Next:"
echo "   cmake -B ./build && cmake --build ./build"
