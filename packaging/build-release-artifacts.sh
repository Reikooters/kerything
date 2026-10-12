#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat >&2 <<EOF
Usage: $0 [options]

Build release artifacts for supported distributions.

By default, this builds both variants for all configured targets:

  - Arch Linux KF6 tarball
  - Arch Linux Qt-only tarball
  - Fedora KF6 RPM via Podman
  - Fedora Qt-only RPM via Podman
  - KDE Neon KF6 tarball via Podman
  - KDE Neon Qt-only tarball via Podman

Options:
  --kf6-only        Build only KDE Frameworks 6 variants.
  --qt-only         Build only Qt-only variants.
  --skip-archlinux  Skip Arch Linux tarballs.
  --skip-fedora     Skip Fedora RPMs.
  --skip-kdeneon    Skip KDE Neon tarballs.
  --rebuild-images  Rebuild Podman images before building Podman targets.
  -h, --help        Show this help text.

Environment:
  OUTPUT_DIR  Optional output directory for finished artifacts.
              Defaults to <project-root>/releases.
EOF
}

BUILD_KF6=true
BUILD_QT_ONLY=true
BUILD_ARCHLINUX=true
BUILD_FEDORA=true
BUILD_KDENEON=true
REBUILD_IMAGES=false

while (( $# > 0 )); do
    case "$1" in
        --kf6-only)
            BUILD_KF6=true
            BUILD_QT_ONLY=false
            ;;
        --qt-only)
            BUILD_KF6=false
            BUILD_QT_ONLY=true
            ;;
        --skip-archlinux)
            BUILD_ARCHLINUX=false
            ;;
        --skip-fedora)
            BUILD_FEDORA=false
            ;;
        --skip-kdeneon)
            BUILD_KDENEON=false
            ;;
        --rebuild-images)
            REBUILD_IMAGES=true
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

if [[ "$BUILD_KF6" != true && "$BUILD_QT_ONLY" != true ]]; then
    echo "Error: Nothing to build; both KF6 and Qt-only variants are disabled." >&2
    exit 1
fi

if [[ "$BUILD_ARCHLINUX" != true && "$BUILD_FEDORA" != true && "$BUILD_KDENEON" != true ]]; then
    echo "Error: Nothing to build; all targets are disabled." >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
VERSION_FILE="$PROJECT_ROOT/shared/Version.h"
OUTPUT_DIR="${OUTPUT_DIR:-$PROJECT_ROOT/releases}"

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

mkdir -p "$OUTPUT_DIR"

run_step() {
    local title="$1"
    shift

    echo
    echo "========================================================="
    echo "$title"
    echo "========================================================="
    "$@"
}

PODMAN_IMAGE_ARGS=()
if [[ "$REBUILD_IMAGES" == true ]]; then
    PODMAN_IMAGE_ARGS=(--rebuild-image)
fi

echo "====> Starting Kerything release artifact build..."
echo "      Version:       $VERSION"
echo "      Output:        $OUTPUT_DIR"
echo "      Build KF6:     $BUILD_KF6"
echo "      Build Qt-only: $BUILD_QT_ONLY"
echo "      Arch Linux:    $BUILD_ARCHLINUX"
echo "      Fedora RPM:    $BUILD_FEDORA"
echo "      KDE Neon:      $BUILD_KDENEON"

if [[ "$BUILD_ARCHLINUX" == true ]]; then
    if [[ "$BUILD_KF6" == true ]]; then
        run_step \
            "Building Arch Linux KF6 tarball" \
            env OUTPUT_DIR="$OUTPUT_DIR" \
            "$SCRIPT_DIR/local/archlinux/build-archlinux.sh" --kf6
    fi

    if [[ "$BUILD_QT_ONLY" == true ]]; then
        run_step \
            "Building Arch Linux Qt-only tarball" \
            env OUTPUT_DIR="$OUTPUT_DIR" \
            "$SCRIPT_DIR/local/archlinux/build-archlinux.sh" --qt-only
    fi
fi

if [[ "$BUILD_FEDORA" == true ]]; then
    if [[ "$BUILD_KF6" == true ]]; then
        run_step \
            "Building Fedora KF6 RPM via Podman" \
            env OUTPUT_DIR="$OUTPUT_DIR" \
            "$SCRIPT_DIR/podman/fedora-rpm/build-fedora-rpm.sh" \
            --kf6 "${PODMAN_IMAGE_ARGS[@]}"
    fi

    if [[ "$BUILD_QT_ONLY" == true ]]; then
        run_step \
            "Building Fedora Qt-only RPM via Podman" \
            env OUTPUT_DIR="$OUTPUT_DIR" \
            "$SCRIPT_DIR/podman/fedora-rpm/build-fedora-rpm.sh" \
            --qt-only "${PODMAN_IMAGE_ARGS[@]}"
    fi
fi

if [[ "$BUILD_KDENEON" == true ]]; then
    if [[ "$BUILD_KF6" == true ]]; then
        run_step \
            "Building KDE Neon KF6 tarball via Podman" \
            env OUTPUT_DIR="$OUTPUT_DIR" \
            "$SCRIPT_DIR/podman/kdeneon/build-kdeneon.sh" \
            --kf6 "${PODMAN_IMAGE_ARGS[@]}"
    fi

    if [[ "$BUILD_QT_ONLY" == true ]]; then
        run_step \
            "Building KDE Neon Qt-only tarball via Podman" \
            env OUTPUT_DIR="$OUTPUT_DIR" \
            "$SCRIPT_DIR/podman/kdeneon/build-kdeneon.sh" \
            --qt-only "${PODMAN_IMAGE_ARGS[@]}"
    fi
fi

echo
echo "========================================================="
echo "Success! Release artifact build completed."
echo "Artifacts are in:"
echo " -> $OUTPUT_DIR"
echo "========================================================="

if command -v find >/dev/null; then
    echo
    echo "Built artifacts:"
    find "$OUTPUT_DIR" -maxdepth 1 -type f \
        \( -name "kerything-v${VERSION}-*.tar.gz" \
        -o -name "kerything-qt-v${VERSION}-*.tar.gz" \
        -o -name "kerything-${VERSION}-*.rpm" \
        -o -name "kerything-qt-${VERSION}-*.rpm" \) \
        -printf " -> %p\n" |
        sort
fi