#!/usr/bin/env bash
set -euo pipefail

# 1. Ensure the user passed a version argument
if [ $# -lt 1 ] || [ -z "$1" ]; then
    echo "Error: Missing version argument."
    echo "Usage: $0 <version>"
    echo "Example: $0 1.2.3"
    exit 1
fi

# Configuration
VERSION="$1"
TARBALL_NAME="kerything-v${VERSION}-archlinux-x86_64.tar.gz"

echo "====> Starting Arch Linux release build pipeline for version ${VERSION}..."

# 2. Determine paths relative to this script
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUTPUT_DIR="$PROJECT_ROOT/releases"
BUILD_DIR="$PROJECT_ROOT/build-archlinux"
PACKAGE_DIR="$PROJECT_ROOT/package-archlinux"

# Create the dedicated releases directory on the host if it doesn't exist
mkdir -p "$OUTPUT_DIR"

# Clean previous build/package staging folders
echo "====> Cleaning previous build and package staging directories..."
rm -rf "$BUILD_DIR" "$PACKAGE_DIR"

# 3. Configure the release build
echo "====> Configuring CMake..."
cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DKERYTHING_WITH_KF6=ON

# 4. Build binaries
echo "====> Building binaries..."
cmake --build "$BUILD_DIR" --parallel

# 5. Check for missing runtime dependencies
echo "====> Checking for missing dependencies..."
ldd "$BUILD_DIR/kerything" | grep 'not found' || true
ldd "$BUILD_DIR/kerythingd" | grep 'not found' || true

# 6. Perform staged installation
echo "====> Performing staged installation..."
cmake --install "$BUILD_DIR" --prefix "$PACKAGE_DIR"

# 7. Create distribution tarball
echo "====> Creating distribution tarball..."
tar -C "$PACKAGE_DIR" -czf "$OUTPUT_DIR/$TARBALL_NAME" .

# 8. Clean up the build and staged package folders
echo "====> Cleaning up build and package staging directories..."
rm -rf "$BUILD_DIR" "$PACKAGE_DIR"

echo "========================================================="
echo "Success! Your Arch Linux package tarball is ready:"
echo " -> $OUTPUT_DIR/$TARBALL_NAME"
echo "========================================================="
