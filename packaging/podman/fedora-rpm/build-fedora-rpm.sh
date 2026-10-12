#!/usr/bin/env bash
set -euo pipefail

VARIANT="kf6"

if (( $# > 1 )); then
    echo "Usage: $0 [--kf6|--qt-only]" >&2
    exit 1
fi

if (( $# == 1 )); then
    case "$1" in
        --kf6) VARIANT="kf6" ;;
        --qt-only) VARIANT="qt-only" ;;
        *)
            echo "Usage: $0 [--kf6|--qt-only]" >&2
            exit 1
            ;;
    esac
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
OUTPUT_DIR="$PROJECT_ROOT/releases"
IMAGE_NAME="kerything-fedora-rpm-build"

mkdir -p "$OUTPUT_DIR"

if ! podman image exists "$IMAGE_NAME"; then
    podman build -f "$SCRIPT_DIR/Containerfile" -t "$IMAGE_NAME" "$SCRIPT_DIR"
fi

LOCAL_SCRIPT="/src/packaging/local/fedora-rpm/build-fedora-rpm.sh"

ARGS=()
if [[ "$VARIANT" == "qt-only" ]]; then
    ARGS=(--qt-only)
fi

podman run --rm \
    --userns=keep-id \
    -v "$PROJECT_ROOT:/src:ro,Z" \
    -v "$OUTPUT_DIR:/src/releases:Z" \
    -w /src \
    "$IMAGE_NAME" \
    "$LOCAL_SCRIPT" "${ARGS[@]}"
