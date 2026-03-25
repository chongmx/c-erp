#!/bin/bash
# Exit immediately if a command exits with a non-zero status
set -e

# 1. Define the package details
URL="https://github.com/wkhtmltopdf/packaging/releases/download/0.12.6.1-2/wkhtmltox_0.12.6.1-2.jammy_amd64.deb"
PACKAGE="wkhtmltox_0.12.6.1-2.jammy_amd64.deb"

echo "--- Starting wkhtmltopdf (Patched Qt) Installation ---"

# 2. Download the package
if [ ! -f "$PACKAGE" ]; then
    echo "Downloading package..."
    wget $URL
else
    echo "Package already downloaded, skipping to install."
fi

# 3. Install the package
# We use 'apt install' on the local file because it automatically 
# fetches any missing dependencies (like libxrender1, etc.)
echo "Installing package..."
sudo apt update
sudo apt install -y ./"$PACKAGE"

# 4. Verify installation
echo "--- Verification ---"
wkhtmltopdf --version

echo "Installation complete. You can now delete the .deb file."
# rm "$PACKAGE" # Uncomment this to auto-delete the installer