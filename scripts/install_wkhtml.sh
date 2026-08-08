#!/bin/bash
# Exit immediately if a command exits with a non-zero status
set -e

WK_VERSION="0.12.6.1-2"
DISTRO=""

usage() {
    echo "Usage: $0 [--bookworm] [-h|--help]"
    echo ""
    echo "Installs wkhtmltopdf ${WK_VERSION} (patched Qt) from the official .deb."
    echo "The distro build is auto-detected from /etc/os-release (VERSION_CODENAME):"
    echo "  jammy or bookworm -> matching package"
    echo "  anything else / undetectable -> jammy (default)"
    echo ""
    echo "Options:"
    echo "  --bookworm   Force the Debian 12 (bookworm) package"
    echo "  -h, --help   Show this help"
}

# 1. Parse options
for arg in "$@"; do
    case "$arg" in
        --bookworm) DISTRO="bookworm" ;;
        -h|--help)  usage; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; usage; exit 1 ;;
    esac
done

# 2. Auto-detect the distro codename if not forced
if [ -z "$DISTRO" ]; then
    CODENAME=""
    if [ -r /etc/os-release ]; then
        # Read in a subshell so os-release vars don't clobber ours
        CODENAME=$(. /etc/os-release && echo "${VERSION_CODENAME:-}")
    fi
    case "$CODENAME" in
        jammy|bookworm)
            DISTRO="$CODENAME"
            echo "Detected distro codename: ${CODENAME}"
            ;;
        "")
            DISTRO="jammy"
            echo "Could not detect distro codename — defaulting to jammy."
            ;;
        *)
            DISTRO="jammy"
            echo "No wkhtmltox ${WK_VERSION} build for '${CODENAME}' — defaulting to jammy."
            ;;
    esac
fi

# 3. Define the package details
PACKAGE="wkhtmltox_${WK_VERSION}.${DISTRO}_amd64.deb"
URL="https://github.com/wkhtmltopdf/packaging/releases/download/${WK_VERSION}/${PACKAGE}"

echo "--- Starting wkhtmltopdf (Patched Qt) Installation [${DISTRO}] ---"

# 4. Download the package
if [ ! -f "$PACKAGE" ]; then
    echo "Downloading ${PACKAGE}..."
    wget "$URL"
else
    echo "Package already downloaded, skipping to install."
fi

# 5. Install the package
# We use 'apt install' on the local file because it automatically
# fetches any missing dependencies (like libxrender1, etc.)
echo "Installing package..."
sudo apt update
sudo apt install -y ./"$PACKAGE"

# 6. Verify installation
echo "--- Verification ---"
wkhtmltopdf --version

echo "Installation complete. You can now delete the .deb file."
# rm "$PACKAGE" # Uncomment this to auto-delete the installer
