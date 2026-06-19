#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

VERSION="${1:-dev}"

DIST_DIR="$ROOT_DIR/dist"
FW_SRC_DIR="$ROOT_DIR/firmware"
STAGING_DIR="$(mktemp -d "${TMPDIR:-/tmp}/pcwcc-firmware.XXXXXX")"
FW_DIR="$STAGING_DIR/firmware"

IDF_PATH="${IDF_PATH:-/opt/esp/idf}"
IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-/opt/esp/tools}"
IDF_TARGET="${IDF_TARGET:-esp32c3}"
IDF_CCACHE_ENABLE="${IDF_CCACHE_ENABLE:-1}"


export IDF_PATH
export IDF_TOOLS_PATH
export IDF_TARGET
export IDF_CCACHE_ENABLE

cleanup() {
    rm -rf "$STAGING_DIR"
}

trap cleanup EXIT

mkdir -p "$DIST_DIR"

if ! command -v zip >/dev/null 2>&1; then
    echo "zip is required" >&2
    exit 1
fi

echo "==> Building firmware"
echo "Version:        $VERSION"
echo "Firmware src:   $FW_SRC_DIR"
echo "Firmware dir:   $FW_DIR"
echo "Staging dir:    $STAGING_DIR"
echo "IDF_PATH:       $IDF_PATH"
echo "IDF_TOOLS_PATH: $IDF_TOOLS_PATH"
echo "IDF_TARGET:     $IDF_TARGET"

if [ ! -f "$IDF_PATH/export.sh" ]; then
    echo "ESP-IDF export.sh not found: $IDF_PATH/export.sh" >&2
    echo "This script expects ESP-IDF to be already installed in the Docker image." >&2
    exit 1
fi

# shellcheck disable=SC1091
. "$IDF_PATH/export.sh"

echo "==> Tool versions"
python --version
idf.py --version
cmake --version | head -n 1
ninja --version || true
ccache --version | head -n 1 || true

if [ ! -d "$FW_SRC_DIR" ]; then
    echo "Firmware source directory not found: $FW_SRC_DIR" >&2
    exit 1
fi

echo "==> Copying firmware sources to staging"
cp -a "$FW_SRC_DIR" "$FW_DIR"

echo "==> Running firmware build"
idf.py -C "$FW_DIR" set-target "$IDF_TARGET"
idf.py -C "$FW_DIR" build

PROJECT_NAME="$(
    grep -E '^[[:space:]]*project\(' "$FW_DIR/CMakeLists.txt" \
        | sed -E 's/.*project\(([^)]+)\).*/\1/' \
        | tr -d '[:space:]'
)"

if [ -z "$PROJECT_NAME" ]; then
    echo "Could not detect project name from firmware/CMakeLists.txt" >&2
    exit 1
fi

BUILD_DIR="$FW_DIR/build"

BIN_SRC="$BUILD_DIR/${PROJECT_NAME}.bin"
ELF_SRC="$BUILD_DIR/${PROJECT_NAME}.elf"
MAP_SRC="$BUILD_DIR/${PROJECT_NAME}.map"
BOOTLOADER_SRC="$BUILD_DIR/bootloader/bootloader.bin"
PARTITIONS_SRC="$BUILD_DIR/partition_table/partition-table.bin"

if [ ! -f "$BIN_SRC" ]; then
    echo "Firmware binary not found: $BIN_SRC" >&2
    echo "Available build files:"
    find "$BUILD_DIR" -maxdepth 3 -type f | sort || true
    exit 1
fi

OUT_PREFIX="pc-water-cooling-controller-${VERSION}-firmware-${IDF_TARGET}"
ARCHIVE_DIR="$STAGING_DIR/$OUT_PREFIX"
ARCHIVE_FILE="$DIST_DIR/${OUT_PREFIX}.zip"
TMP_ARCHIVE_FILE="$STAGING_DIR/${OUT_PREFIX}.zip"

echo "==> Copying firmware artifacts"

cp "$BIN_SRC" "$DIST_DIR/${OUT_PREFIX}.bin"
cp "$ELF_SRC" "$DIST_DIR/${OUT_PREFIX}.elf"

if [ -f "$MAP_SRC" ]; then
    cp "$MAP_SRC" "$DIST_DIR/${OUT_PREFIX}.map"
fi

if [ -f "$BOOTLOADER_SRC" ]; then
    cp "$BOOTLOADER_SRC" "$DIST_DIR/${OUT_PREFIX}-bootloader.bin"
fi

if [ -f "$PARTITIONS_SRC" ]; then
    cp "$PARTITIONS_SRC" "$DIST_DIR/${OUT_PREFIX}-partition-table.bin"
fi

cat > "$DIST_DIR/${OUT_PREFIX}-flash.txt" <<EOF
PC Water Cooling Controller firmware ${VERSION}

Target:
  ${IDF_TARGET}

Flash command from repository root inside the build Docker image:

  esptool.py --chip ${IDF_TARGET} --baud 460800 write_flash \\
    0x0 ${OUT_PREFIX}-bootloader.bin \\
    0x8000 ${OUT_PREFIX}-partition-table.bin \\
    0x10000 ${OUT_PREFIX}.bin

Manual esptool example:

  esptool.py --chip ${IDF_TARGET} --baud 460800 write_flash \\
    0x0 ${OUT_PREFIX}-bootloader.bin \\
    0x8000 ${OUT_PREFIX}-partition-table.bin \\
    0x10000 ${OUT_PREFIX}.bin

EOF

echo "==> Creating firmware archive"
mkdir -p "$ARCHIVE_DIR"

cp "$DIST_DIR/${OUT_PREFIX}.bin" "$ARCHIVE_DIR/"
cp "$DIST_DIR/${OUT_PREFIX}.elf" "$ARCHIVE_DIR/"
cp "$DIST_DIR/${OUT_PREFIX}-flash.txt" "$ARCHIVE_DIR/"

if [ -f "$DIST_DIR/${OUT_PREFIX}.map" ]; then
    cp "$DIST_DIR/${OUT_PREFIX}.map" "$ARCHIVE_DIR/"
fi

if [ -f "$DIST_DIR/${OUT_PREFIX}-bootloader.bin" ]; then
    cp "$DIST_DIR/${OUT_PREFIX}-bootloader.bin" "$ARCHIVE_DIR/"
fi

if [ -f "$DIST_DIR/${OUT_PREFIX}-partition-table.bin" ]; then
    cp "$DIST_DIR/${OUT_PREFIX}-partition-table.bin" "$ARCHIVE_DIR/"
fi

(
    cd "$STAGING_DIR"
    zip -r "$TMP_ARCHIVE_FILE" "$OUT_PREFIX"
)
mv "$TMP_ARCHIVE_FILE" "$ARCHIVE_FILE"

echo "==> Firmware artifacts:"
ls -lh "$DIST_DIR"/"${OUT_PREFIX}"*
