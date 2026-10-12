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
IMAGE_NAME="kerything-kdeneon-build"
VOLUME_NAME="kdeneon-artifacts-scratch"
TARBALL_NAME="kerything-v${VERSION}-kdeneon-x86_64.tar.gz"

echo "====> Starting KDE Neon clean build pipeline for version ${VERSION}..."

# 2. Determine paths relative to this script
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUTPUT_DIR="$PROJECT_ROOT/releases"

# Create the dedicated releases directory on the host if it doesn't exist
mkdir -p "$OUTPUT_DIR"

# 3. Check if the Podman image exists
if ! podman image exists "$IMAGE_NAME"; then
    echo "====> Building missing Podman image..."
    podman build -f "$SCRIPT_DIR/Containerfile.kdeneon" -t "$IMAGE_NAME" "$SCRIPT_DIR"
fi

# 4. Create a clean temporary Podman volume for build artifacts
echo "====> Creating temporary scratch volume..."
podman volume rm -f "$VOLUME_NAME" >/dev/null 2>&1 || true
podman volume create "$VOLUME_NAME" >/dev/null

# 5. Run the compilation non-interactively inside the container
echo "====> Compiling inside the KDE Neon environment..."
podman run --rm \
  --userns=keep-id \
  -v "$PROJECT_ROOT:/src:ro,Z" \
  -v "$VOLUME_NAME:/workspace:Z" \
  -w /workspace \
  "$IMAGE_NAME" \
  bash -c "
    set -euo pipefail

    echo '--> Configuring CMake...'
    cmake -S /src -B build \
      -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr \
      -DKERYTHING_WITH_KF6=ON

    echo '--> Building binaries...'
    cmake --build build

    echo '--> Checking for missing dependencies...'
    ldd build/kerything | grep 'not found' || true
    ldd build/kerythingd | grep 'not found' || true

    echo '--> Performing staged installation...'
    cmake --install build --prefix ./package

    echo '--> Creating distribution tarball...'
    tar -C package -czf '$TARBALL_NAME' .
  "

# 6. Copy the finished tarball back to the dedicated releases directory
echo "====> Copying artifact back to host..."
podman run --rm \
  -v "$VOLUME_NAME:/workspace:Z" \
  -v "$OUTPUT_DIR:/host:Z" \
  "$IMAGE_NAME" \
  cp "/workspace/$TARBALL_NAME" /host/

# 7. Post-build cleanup
echo "====> Cleaning up scratch volumes..."
podman volume rm "$VOLUME_NAME" >/dev/null

echo "========================================================="
echo "Success! Your pristine package is ready:"
echo " -> $OUTPUT_DIR/$TARBALL_NAME"
echo "========================================================="
