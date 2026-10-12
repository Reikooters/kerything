#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat >&2 <<EOF
Usage: $0 [--kf6|--qt-only]

Build Arch Linux release tarballs.

Options:
  --kf6       Build the KDE Frameworks 6 variant. This is the default.
  --qt-only   Build the Qt-only variant.
  -h, --help  Show this help text.
EOF
}

VARIANT="kf6"
PACKAGE_NAME="kerything"
KERYTHING_WITH_KF6="ON"

while (( $# > 0 )); do
    case "$1" in
        --kf6)
            VARIANT="kf6"
            PACKAGE_NAME="kerything"
            KERYTHING_WITH_KF6="ON"
            ;;
        --qt-only)
            VARIANT="qt-only"
            PACKAGE_NAME="kerything-qt"
            KERYTHING_WITH_KF6="OFF"
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Error: Unknown argument: $1" >&2
            usage
            exit 1
            ;;
    esac

    shift
done

# Determine paths relative to this script.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
VERSION_FILE="$PROJECT_ROOT/shared/Version.h"

if [[ ! -f "$VERSION_FILE" ]]; then
    echo "Error: Version file not found: $VERSION_FILE" >&2
    exit 1
fi

VERSION="$(
    sed -nE 's/^[[:space:]]*inline[[:space:]]+constexpr[[:space:]]+auto[[:space:]]+Version[[:space:]]*=[[:space:]]*"([^"]+)";[[:space:]]*$/\1/p' "$VERSION_FILE" |
        head -n 1
)"

if [[ -z "$VERSION" ]]; then
    echo "Error: Could not read Kerything version from $VERSION_FILE" >&2
    exit 1
fi

OUTPUT_DIR="$PROJECT_ROOT/releases"
BUILD_DIR="$PROJECT_ROOT/build-archlinux-$VARIANT"
PACKAGE_DIR="$PROJECT_ROOT/package-archlinux-$VARIANT"
TARBALL_NAME="${PACKAGE_NAME}-v${VERSION}-archlinux-x86_64.tar.gz"

echo "====> Starting Arch Linux release build pipeline..."
echo "      Version: $VERSION"
echo "      Variant: $VARIANT"
echo "      Package: $PACKAGE_NAME"
echo "      KF6:     $KERYTHING_WITH_KF6"

mkdir -p "$OUTPUT_DIR"

echo "====> Cleaning previous build and package staging directories..."
rm -rf "$BUILD_DIR" "$PACKAGE_DIR"

echo "====> Configuring CMake..."
cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DKERYTHING_SYSTEMD_SYSTEM_UNIT_DIR=/usr/lib/systemd/system \
  -DKERYTHING_WITH_KF6="$KERYTHING_WITH_KF6"

echo "====> Building binaries..."
cmake --build "$BUILD_DIR" --parallel

echo "====> Checking for missing dependencies..."
ldd "$BUILD_DIR/kerything" | grep 'not found' || true
ldd "$BUILD_DIR/kerythingd" | grep 'not found' || true

echo "====> Performing staged installation..."
DESTDIR="$PACKAGE_DIR" cmake --install "$BUILD_DIR"

echo "====> Creating distribution tarball..."
tar -C "$PACKAGE_DIR" -czf "$OUTPUT_DIR/$TARBALL_NAME" .

echo "====> Cleaning up build and package staging directories..."
rm -rf "$BUILD_DIR" "$PACKAGE_DIR"

echo "========================================================="
echo "Success! Your Arch Linux package tarball is ready:"
echo " -> $OUTPUT_DIR/$TARBALL_NAME"
echo "========================================================="