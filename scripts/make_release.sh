#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

VERSION="${1:-}"

if [ -z "$VERSION" ]; then
    if git describe --tags --exact-match >/dev/null 2>&1; then
        VERSION="$(git describe --tags --exact-match)"
    else
        VERSION="dev-$(git rev-parse --short HEAD)"
    fi
fi

DIST_DIR="$ROOT_DIR/dist"

echo "==> Making release artifacts"
echo "Version: $VERSION"

rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"

./scripts/build_firmware.sh "$VERSION"
./scripts/build_appimage.sh "$VERSION"

echo "==> Creating checksums"
(
    cd "$DIST_DIR"
    sha256sum * > "pc-water-cooling-controller-${VERSION}-checksums.sha256"
)

echo "==> Release artifacts are ready:"
ls -lh "$DIST_DIR"