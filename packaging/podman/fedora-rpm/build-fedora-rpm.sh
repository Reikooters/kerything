#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat >&2 <<EOF
Usage: $0 [--kf6|--qt-only] [--rebuild-image]

Build Fedora RPM packages inside a Podman container.

Options:
  --kf6            Build the KDE Frameworks 6 variant. This is the default.
  --qt-only        Build the Qt-only variant.
  --rebuild-image  Rebuild the Podman image before building.
  -h, --help       Show this help text.

Environment:
  OUTPUT_DIR  Optional output directory for finished RPMs.
              Defaults to <project-root>/releases.
EOF
}

VARIANT="kf6"
PACKAGE_NAME="kerything"
REBUILD_IMAGE=false

while (( $# > 0 )); do
    case "$1" in
        --kf6)
            VARIANT="kf6"
            PACKAGE_NAME="kerything"
            ;;
        --qt-only)
            VARIANT="qt-only"
            PACKAGE_NAME="kerything-qt"
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

  sudo dnf install podman

or on Arch Linux:

  sudo pacman -S podman

EOF
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
CONTAINERFILE="$SCRIPT_DIR/Containerfile"
LOCAL_SCRIPT="/src/packaging/local/fedora-rpm/build-fedora-rpm.sh"
OUTPUT_DIR="${OUTPUT_DIR:-$PROJECT_ROOT/releases}"
IMAGE_NAME="kerything-fedora-rpm-build"

if [[ ! -f "$CONTAINERFILE" ]]; then
    echo "Error: Containerfile not found: $CONTAINERFILE" >&2
    exit 1
fi

mkdir -p "$OUTPUT_DIR"

ARGS=()
if [[ "$VARIANT" == "qt-only" ]]; then
    ARGS=(--qt-only)
fi

echo "====> Starting Fedora RPM Podman build pipeline..."
echo "      Variant: $VARIANT"
echo "      Package: $PACKAGE_NAME"
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

echo "====> Building Fedora RPM inside container..."
podman run --rm \
    --userns=keep-id \
    -e OUTPUT_DIR=/out \
    -v "$PROJECT_ROOT:/src:ro,Z" \
    -v "$OUTPUT_DIR:/out:Z" \
    -w /src \
    "$IMAGE_NAME" \
    "$LOCAL_SCRIPT" "${ARGS[@]}" |
    sed "s# -> /out/# -> $OUTPUT_DIR/#"