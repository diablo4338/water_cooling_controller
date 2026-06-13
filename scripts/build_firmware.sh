#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

VERSION="${1:-dev}"
IDF_VERSION="${IDF_VERSION:-v5.4.1}"
IDF_ROOT="${IDF_ROOT:-$ROOT_DIR/.cache/esp-idf/$IDF_VERSION}"
IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-$ROOT_DIR/.cache/esp-idf-tools}"

export IDF_TOOLS_PATH

DIST_DIR="$ROOT_DIR/dist"
FW_DIR="$ROOT_DIR/firmware"

mkdir -p "$DIST_DIR"

echo "==> Building firmware"
echo "Version: $VERSION"
echo "ESP-IDF: $IDF_VERSION"
echo "Firmware dir: $FW_DIR"

if ! command -v git >/dev/null 2>&1; then
    echo "git is required" >&2
    exit 1
fi

if [ ! -d "$IDF_ROOT/.git" ]; then
    echo "==> Installing ESP-IDF into $IDF_ROOT"
    mkdir -p "$(dirname "$IDF_ROOT")"
    git clone --recursive --branch "$IDF_VERSION" --depth 1 \
        https://github.com/espressif/esp-idf.git "$IDF_ROOT"
fi

echo "==> Installing ESP-IDF tools if needed"
"$IDF_ROOT/install.sh" esp32c3

# shellcheck disable=SC1091
. "$IDF_ROOT/export.sh"

echo "==> Cleaning previous firmware build"
idf.py -C "$FW_DIR" fullclean || true

echo "==> Running firmware build"
idf.py -C "$FW_DIR" build

PROJECT_NAME="$(grep -E '^[[:space:]]*project\(' "$FW_DIR/CMakeLists.txt" | sed -E 's/.*project\(([^)]+)\).*/\1/' | tr -d '[:space:]')"

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
    find "$BUILD_DIR" -maxdepth 2 -type f | sort
    exit 1
fi

OUT_PREFIX="pc-water-cooling-controller-${VERSION}-firmware-esp32c3"

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
  ESP32-C3

Flash command from repository root:

  . "$IDF_ROOT/export.sh"
  idf.py -C firmware flash

Manual esptool example:

  esptool.py --chip esp32c3 --baud 460800 write_flash \\
    0x0 ${OUT_PREFIX}-bootloader.bin \\
    0x8000 ${OUT_PREFIX}-partition-table.bin \\
    0x10000 ${OUT_PREFIX}.bin

EOF

echo "==> Firmware artifacts:"
ls -lh "$DIST_DIR"/"${OUT_PREFIX}"*