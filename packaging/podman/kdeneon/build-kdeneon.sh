#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat >&2 <<EOF
Usage: $0 [--kf6|--qt-only] [--rebuild-image]

Build KDE Neon release tarballs inside a Podman container.

Options:
  --kf6            Build the KDE Frameworks 6 variant. This is the default.
  --qt-only        Build the Qt-only variant.
  --rebuild-image  Rebuild the Podman image before building.
  -h, --help       Show this help text.

Environment:
  OUTPUT_DIR  Optional output directory for finished tarballs.
              Defaults to <project-root>/releases.
EOF
}

VARIANT="kf6"
PACKAGE_NAME="kerything"
KERYTHING_WITH_KF6="ON"
REBUILD_IMAGE=false

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
        --rebuild-image)
            REBUILD_IMAGE=true
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

if ! command -v podman >/dev/null; then
    cat >&2 <<EOF
Error: podman is missing.

Install Podman first. For example:

  sudo pacman -S podman

or on Fedora:

  sudo dnf install podman

EOF
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
VERSION_FILE="$PROJECT_ROOT/shared/Version.h"
CONTAINERFILE="$SCRIPT_DIR/Containerfile"

if [[ ! -f "$CONTAINERFILE" ]]; then
    echo "Error: Containerfile not found: $CONTAINERFILE" >&2
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

IMAGE_NAME="kerything-kdeneon-build"
VOLUME_NAME="kerything-kdeneon-build-${VARIANT}-$$"
OUTPUT_DIR="${OUTPUT_DIR:-$PROJECT_ROOT/releases}"
TARBALL_NAME="${PACKAGE_NAME}-v${VERSION}-kdeneon-x86_64.tar.gz"

cleanup() {
    echo "====> Cleaning up scratch volume..."
    podman volume rm -f "$VOLUME_NAME" >/dev/null 2>&1 || true
}
trap cleanup EXIT

mkdir -p "$OUTPUT_DIR"

echo "====> Starting KDE Neon Podman build pipeline..."
echo "      Version: $VERSION"
echo "      Variant: $VARIANT"
echo "      Package: $PACKAGE_NAME"
echo "      KF6:     $KERYTHING_WITH_KF6"
echo "      Image:   $IMAGE_NAME"
echo "      Output:  $OUTPUT_DIR"

if [[ "$REBUILD_IMAGE" == true ]]; then
    echo "====> Rebuilding Podman image..."
    podman build -f "$CONTAINERFILE" -t "$IMAGE_NAME" "$SCRIPT_DIR"
elif ! podman image exists "$IMAGE_NAME"; then
    echo "====> Building missing Podman image..."
    podman build -f "$CONTAINERFILE" -t "$IMAGE_NAME" "$SCRIPT_DIR"
else
    echo "====> Reusing existing Podman image: $IMAGE_NAME"
fi

echo "====> Creating temporary scratch volume..."
podman volume create "$VOLUME_NAME" >/dev/null

echo "====> Building inside the KDE Neon container..."
podman run --rm \
    -i \
    --userns=keep-id \
    -e KERYTHING_WITH_KF6="$KERYTHING_WITH_KF6" \
    -e TARBALL_NAME="$TARBALL_NAME" \
    -v "$PROJECT_ROOT:/src:ro,Z" \
    -v "$VOLUME_NAME:/workspace:Z" \
    -w /workspace \
    "$IMAGE_NAME" \
    bash -s <<'EOF'
set -euo pipefail

echo '--> Configuring CMake...'
cmake -S /src -B build \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DKERYTHING_SYSTEMD_SYSTEM_UNIT_DIR=/lib/systemd/system \
  -DKERYTHING_WITH_KF6="$KERYTHING_WITH_KF6"

echo '--> Building binaries...'
cmake --build build --parallel

echo '--> Checking for missing dependencies...'
ldd build/kerything | grep 'not found' || true
ldd build/kerythingd | grep 'not found' || true

echo '--> Performing staged installation...'
DESTDIR=/workspace/package cmake --install build

echo '--> Creating distribution tarball...'
tar -C /workspace/package -czf "/workspace/$TARBALL_NAME" .
EOF

echo "====> Copying artifact back to host..."
podman run --rm \
    -v "$VOLUME_NAME:/workspace:Z" \
    -v "$OUTPUT_DIR:/host:Z" \
    "$IMAGE_NAME" \
    cp "/workspace/$TARBALL_NAME" /host/

echo "========================================================="
echo "Success! Your KDE Neon package tarball is ready:"
echo " -> $OUTPUT_DIR/$TARBALL_NAME"
echo "========================================================="