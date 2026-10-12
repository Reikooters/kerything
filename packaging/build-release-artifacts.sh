#!/usr/bin/env bash
set -euo pipefail

if (( $# != 1 )); then
    echo "Usage: $0 <version>" >&2
    exit 1
fi

VERSION="$1"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

"$SCRIPT_DIR/local/archlinux/build-archlinux.sh" "$VERSION"
"$SCRIPT_DIR/local/archlinux/build-archlinux.sh" --qt-only "$VERSION"

"$SCRIPT_DIR/podman/fedora-rpm/build-fedora-rpm.sh"
"$SCRIPT_DIR/podman/fedora-rpm/build-fedora-rpm.sh" --qt-only

"$SCRIPT_DIR/podman/kdeneon/build-kdeneon.sh" "$VERSION"
"$SCRIPT_DIR/podman/kdeneon/build-kdeneon.sh" --qt-only "$VERSION"
