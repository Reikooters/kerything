#!/usr/bin/env bash
set -euo pipefail

if (( $# > 1 )) || { (( $# == 1 )) && [[ "$1" != "--qt-only" ]]; }; then
    echo "Usage: $0 [--qt-only]" >&2
    exit 1
fi

PACKAGE_NAME=kerything
BUILD_OPTIONS=()
if (( $# == 1 )); then
    PACKAGE_NAME=kerything-qt
    BUILD_OPTIONS=(--without kf6)
fi

if ! command -v rpmbuild >/dev/null; then
    echo "rpmbuild is missing; install rpm-build and the spec's build dependencies first." >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SPEC_FILE="$SCRIPT_DIR/kerything.spec"
VERSION="$(awk '$1 == "Version:" { print $2; exit }' "$SPEC_FILE")"

if [[ -z "$VERSION" ]]; then
    echo "Could not read the version from $SPEC_FILE" >&2
    exit 1
fi

BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/kerything-rpm.XXXXXX")"
trap 'rm -rf "$BUILD_DIR"' EXIT
mkdir -p "$BUILD_DIR/SOURCES" "$BUILD_DIR/tmp"

echo "====> Creating source archive for Kerything $VERSION..."
cd "$PROJECT_ROOT"
git ls-files --cached --others --exclude-standard -z | \
    tar --null -T - --transform="s,^,kerything-$VERSION/," \
        -czf "$BUILD_DIR/SOURCES/kerything-$VERSION.tar.gz"

echo "====> Building $PACKAGE_NAME Fedora RPM..."
rpmbuild -ba "${BUILD_OPTIONS[@]}" \
    --define "_topdir $BUILD_DIR" \
    --define "_tmppath $BUILD_DIR/tmp" \
    "$SPEC_FILE"

shopt -s nullglob
packages=("$BUILD_DIR"/RPMS/*/"$PACKAGE_NAME-$VERSION-"*.rpm)
if (( ${#packages[@]} != 1 )); then
    echo "Expected one Kerything RPM, found ${#packages[@]}" >&2
    exit 1
fi

OUTPUT_DIR="$PROJECT_ROOT/releases"
mkdir -p "$OUTPUT_DIR"
cp "${packages[0]}" "$OUTPUT_DIR/"
echo "====> RPM ready: $OUTPUT_DIR/${packages[0]##*/}"
