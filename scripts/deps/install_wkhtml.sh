#!/bin/bash
# =============================================================
# scripts/deps/install_wkhtml.sh — wkhtmltopdf with the PATCHED Qt build.
#
#   ./scripts/deps/install_wkhtml.sh              auto-detect the distro
#   ./scripts/deps/install_wkhtml.sh --flavour bookworm
#   ./scripts/deps/install_wkhtml.sh --keep       don't delete the .deb
#
# WHY NOT THE DISTRO PACKAGE. Debian's own wkhtmltopdf is built against stock
# Qt, and `--footer-html` silently does nothing there — ReportModule.cpp and
# PortalModule.cpp both depend on it for page numbering. Only the upstream
# "with patched qt" build works. Debian dropped the package entirely in 13
# (trixie), and upstream archived the project in 2023, so this .deb is the
# only supported route and there will not be a newer one.
#
# THE VERSION MATTERS. 0.12.6.1-2 has NO bookworm build — its only amd64
# artifacts are bullseye and jammy, so asking it for bookworm 404s. The
# bookworm build first appears in 0.12.6.1-3, which is why that is pinned
# here. Same upstream 0.12.6.1 binary, newer packaging.
#
#   published amd64 builds, 0.12.6.1-3:  bookworm, bullseye, jammy
#
# TRIXIE (Debian 13) uses the BOOKWORM build. Verified 2026-09-01 in a
# debian:13-slim container: apt resolves every dependency (trixie's t64
# packages satisfy the bookworm names), no unresolved shared objects,
# `--version` reports "with patched qt", and a 400-row invoice renders to a
# 10-page PDF in 2.1s at a peak RSS of 40 MiB. The jammy build cannot be used
# on Debian at all — it needs libjpeg.so.8, and Debian ships libjpeg.so.62.
# =============================================================
set -euo pipefail

WK_VERSION="0.12.6.1-3"
FLAVOUR=""
KEEP=0

usage() {
    sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
    case "$1" in
        --flavour)  FLAVOUR="${2:?--flavour needs bookworm|bullseye|jammy}"; shift 2 ;;
        --bookworm) FLAVOUR="bookworm"; shift ;;   # kept: the old flag name
        --keep)     KEEP=1; shift ;;
        -h|--help)  usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
    esac
done

# -------------------------------------------------------------
# Pick the build. Only bookworm, bullseye and jammy exist for amd64.
# Debian 13 (trixie) and anything newer take the bookworm build; that is the
# tested path, and it is the closest Debian-native ABI available.
# -------------------------------------------------------------
if [ -z "$FLAVOUR" ]; then
    CODENAME=""
    OSID=""
    if [ -r /etc/os-release ]; then
        CODENAME=$(. /etc/os-release && echo "${VERSION_CODENAME:-}")
        OSID=$(. /etc/os-release && echo "${ID:-}")
    fi
    case "$CODENAME" in
        bookworm|bullseye|jammy)
            FLAVOUR="$CODENAME"
            echo "Detected ${CODENAME} — using the matching build."
            ;;
        trixie|forky|sid)
            FLAVOUR="bookworm"
            echo "Detected Debian ${CODENAME}: no build exists for it; using bookworm (tested)."
            ;;
        noble|oracular|plucky)
            FLAVOUR="jammy"
            echo "Detected Ubuntu ${CODENAME}: no build exists for it; using jammy."
            ;;
        *)
            if [ "$OSID" = "debian" ]; then
                FLAVOUR="bookworm"
                echo "Unrecognised Debian codename '${CODENAME:-unknown}' — using bookworm."
            else
                FLAVOUR="jammy"
                echo "Unrecognised codename '${CODENAME:-unknown}' — using jammy."
            fi
            ;;
    esac
fi

PACKAGE="wkhtmltox_${WK_VERSION}.${FLAVOUR}_amd64.deb"
URL="https://github.com/wkhtmltopdf/packaging/releases/download/${WK_VERSION}/${PACKAGE}"

echo "--- wkhtmltopdf ${WK_VERSION} (patched Qt), ${FLAVOUR} build ---"

if [ ! -f "$PACKAGE" ]; then
    echo "Downloading ${PACKAGE}..."
    # wget exits non-zero on a 404 but still leaves the partial file behind, and
    # dpkg's complaint about a 9-byte "Not Found" is far more confusing than the
    # real cause. Remove it and say what actually went wrong.
    if ! wget -q --show-progress -O "$PACKAGE" "$URL"; then
        rm -f "$PACKAGE"
        echo "ERROR: could not download $URL" >&2
        echo "       amd64 builds for ${WK_VERSION}: bookworm, bullseye, jammy." >&2
        echo "       (0.12.6.1-2 has NO bookworm build — that is why -3 is pinned.)" >&2
        exit 1
    fi
else
    echo "Already downloaded — installing that."
fi

echo "Installing (apt pulls the X11/font dependencies)..."
sudo apt-get update -qq
sudo apt-get install -y "./$PACKAGE"

# -------------------------------------------------------------
# Verify for real. `--version` alone is not enough: an unpatched build reports
# a version quite happily and then ignores --footer-html at render time, which
# shows up much later as invoices with no page numbers.
# -------------------------------------------------------------
echo "--- Verification ---"
wkhtmltopdf --version

if wkhtmltopdf --version 2>&1 | grep -qi 'with patched qt'; then
    echo "  OK: patched Qt — --footer-html will work."
else
    echo "  WARNING: this build is NOT patched Qt." >&2
    echo "           --footer-html will be ignored and reports will lose their" >&2
    echo "           page numbers (ReportModule.cpp, PortalModule.cpp)." >&2
    exit 1
fi

if missing=$(ldd "$(command -v wkhtmltopdf)" 2>&1 | grep -i 'not found'); then
    echo "  ERROR: unresolved libraries:" >&2
    echo "$missing" | sed 's/^/    /' >&2
    exit 1
fi
echo "  OK: every shared library resolves."

# A render is the only proof that matters — the binary can load and still fail
# on a missing font package.
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
printf '<html><body><h1>ok</h1></body></html>' > "$tmp/t.html"
if QT_QPA_PLATFORM=offscreen wkhtmltopdf --quiet "$tmp/t.html" "$tmp/t.pdf" 2>"$tmp/err" \
   && [ -s "$tmp/t.pdf" ]; then
    echo "  OK: test render produced $(wc -c < "$tmp/t.pdf") bytes of PDF."
else
    echo "  ERROR: the test render failed:" >&2
    sed 's/^/    /' "$tmp/err" >&2
    exit 1
fi

if [ "$KEEP" -eq 0 ]; then
    rm -f "$PACKAGE"
    echo "Removed $PACKAGE (--keep to retain it)."
fi
echo "Done."
