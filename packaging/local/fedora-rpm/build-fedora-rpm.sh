#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat >&2 <<EOF
Usage: $0 [--kf6|--qt-only]

Build Fedora RPM packages.

Options:
  --kf6       Build the KDE Frameworks 6 variant. This is the default.
  --qt-only   Build the Qt-only variant.
  -h, --help  Show this help text.

Environment:
  OUTPUT_DIR  Optional output directory for finished RPMs.
              Defaults to <project-root>/releases.
EOF
}

VARIANT="kf6"
PACKAGE_NAME="kerything"
BUILD_OPTIONS=()

while (( $# > 0 )); do
    case "$1" in
        --kf6)
            VARIANT="kf6"
            PACKAGE_NAME="kerything"
            BUILD_OPTIONS=()
            ;;
        --qt-only)
            VARIANT="qt-only"
            PACKAGE_NAME="kerything-qt"
            BUILD_OPTIONS=(--without kf6)
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

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
SPEC_FILE="$SCRIPT_DIR/kerything.spec"
VERSION_FILE="$PROJECT_ROOT/shared/Version.h"

if [[ ! -f "$SPEC_FILE" ]]; then
    echo "Error: Spec file not found: $SPEC_FILE" >&2
    exit 1
fi

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

if ! command -v git >/dev/null; then
    cat >&2 <<EOF
Error: git is missing.

This script uses git to create the source archive from tracked and unignored files.

Install git first:

  sudo dnf install git

EOF
    exit 1
fi

if ! command -v rpmbuild >/dev/null; then
    cat >&2 <<EOF
Error: rpmbuild is missing.

Install RPM build tooling first:

  sudo dnf install rpm-build dnf-plugins-core

Then install this package's build dependencies:

  sudo dnf builddep --define "kerything_version $VERSION" "$SPEC_FILE"

For the Qt-only variant, use:

  sudo dnf builddep --without kf6 --define "kerything_version $VERSION" "$SPEC_FILE"

EOF
    exit 1
fi

OUTPUT_DIR="${OUTPUT_DIR:-$PROJECT_ROOT/releases}"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/kerything-rpm.XXXXXX")"

cleanup() {
    rm -rf "$BUILD_DIR"
}
trap cleanup EXIT

mkdir -p "$BUILD_DIR/SOURCES" "$BUILD_DIR/tmp" "$OUTPUT_DIR"

echo "====> Starting Fedora RPM build pipeline..."
echo "      Version: $VERSION"
echo "      Variant: $VARIANT"
echo "      Package: $PACKAGE_NAME"
echo "      Spec:    $SPEC_FILE"
echo "      Output:  $OUTPUT_DIR"

echo "====> Creating source archive for Kerything $VERSION..."
cd "$PROJECT_ROOT"

git ls-files --cached --others --exclude-standard -z | \
    tar --null -T - --transform="s,^,kerything-$VERSION/," \
        -czf "$BUILD_DIR/SOURCES/kerything-$VERSION.tar.gz"

echo "====> Building $PACKAGE_NAME Fedora RPM..."
rpmbuild -ba "${BUILD_OPTIONS[@]}" \
    --define "kerything_version $VERSION" \
    --define "_topdir $BUILD_DIR" \
    --define "_tmppath $BUILD_DIR/tmp" \
    "$SPEC_FILE"

shopt -s nullglob
packages=("$BUILD_DIR"/RPMS/*/"$PACKAGE_NAME-$VERSION-"*.rpm)

if (( ${#packages[@]} != 1 )); then
    echo "Error: Expected one $PACKAGE_NAME RPM, found ${#packages[@]}" >&2

    if (( ${#packages[@]} > 0 )); then
        printf 'Found packages:\n' >&2
        printf '  %s\n' "${packages[@]}" >&2
    fi

    exit 1
fi

cp "${packages[0]}" "$OUTPUT_DIR/"

echo "========================================================="
echo "Success! Your Fedora RPM is ready:"
echo " -> $OUTPUT_DIR/${packages[0]##*/}"
echo "========================================================="