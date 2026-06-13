#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

VERSION="${1:-dev}"

DIST_DIR="$ROOT_DIR/dist"
HW_DIR="$ROOT_DIR/hardware"
PRODUCTION_DIR="$HW_DIR/production"
GERBER_DIR="$PRODUCTION_DIR/gerbers"
DRILL_DIR="$PRODUCTION_DIR/drill"
STAGING_DIR="$(mktemp -d "${TMPDIR:-/tmp}/pcwcc-hardware.XXXXXX")"

cleanup() {
    rm -rf "$STAGING_DIR"
}

trap cleanup EXIT

mkdir -p "$DIST_DIR"

if [ ! -w "$DIST_DIR" ]; then
    echo "Distribution directory is not writable: $DIST_DIR" >&2
    exit 1
fi

echo "==> Packaging hardware production files"
echo "Version: $VERSION"
echo "Hardware dir: $HW_DIR"

if ! command -v zip >/dev/null 2>&1; then
    echo "zip is required" >&2
    exit 1
fi

if [ ! -d "$GERBER_DIR" ]; then
    echo "Gerber directory not found: $GERBER_DIR" >&2
    exit 1
fi

if [ ! -d "$DRILL_DIR" ]; then
    echo "Drill directory not found: $DRILL_DIR" >&2
    exit 1
fi

if ! find "$GERBER_DIR" -maxdepth 1 -type f | grep -q .; then
    echo "No Gerber files found in $GERBER_DIR" >&2
    exit 1
fi

if ! find "$DRILL_DIR" -maxdepth 1 -type f | grep -q .; then
    echo "No drill files found in $DRILL_DIR" >&2
    exit 1
fi

echo "==> Preparing archive layout"
mkdir -p "$STAGING_DIR/gerbers" "$STAGING_DIR/drill"

cp -a "$GERBER_DIR"/. "$STAGING_DIR/gerbers/"
cp -a "$DRILL_DIR"/. "$STAGING_DIR/drill/"

OUT_FILE="$DIST_DIR/pc-water-cooling-controller-${VERSION}-hardware-gerbers-drill.zip"
TMP_OUT_FILE="$STAGING_DIR/hardware-gerbers-drill.zip"

echo "==> Creating archive"
(
    cd "$STAGING_DIR"
    zip -r "$TMP_OUT_FILE" gerbers drill
)
mv "$TMP_OUT_FILE" "$OUT_FILE"

echo "==> Hardware artifact:"
ls -lh "$OUT_FILE"
