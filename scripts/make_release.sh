#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

VERSION="${1:-}"

if [ -z "$VERSION" ]; then
    git config --global --add safe.directory "$ROOT_DIR"
    if git describe --tags --exact-match >/dev/null 2>&1; then
        VERSION="$(git describe --tags --exact-match)"
    else
        VERSION="dev-$(git rev-parse --short HEAD)"
    fi
fi

DIST_DIR="$ROOT_DIR/dist"

remove_path_entry() {
    local target="${1%/}"
    local path_value="${2:-}"
    local part
    local result=""

    IFS=':' read -r -a parts <<< "$path_value"
    for part in "${parts[@]}"; do
        [ -n "$part" ] || continue
        if [ "${part%/}" = "$target" ]; then
            continue
        fi
        if [ -n "$result" ]; then
            result="${result}:$part"
        else
            result="$part"
        fi
    done

    printf '%s\n' "$result"
}

echo "==> Making release artifacts"
echo "Version: $VERSION"

rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"

./scripts/build_firmware.sh "$VERSION"
./scripts/build_appimage.sh "$VERSION"
./scripts/build_hardware.sh "$VERSION"

echo "==> Creating checksums"
(
    cd "$DIST_DIR"
    sha256sum * > "pc-water-cooling-controller-${VERSION}-checksums.sha256"
)

echo "==> Release artifacts are ready:"
ls -lh "$DIST_DIR"
