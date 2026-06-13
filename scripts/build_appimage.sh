#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

VERSION="${1:-dev}"
DIST_DIR="$ROOT_DIR/dist"
APP_DIR="$ROOT_DIR/pcwcc"
BUILD_DIR="$ROOT_DIR/build/appimage"
APPDIR="$BUILD_DIR/AppDir"

APP_NAME="PC Water Cooling Controller"
APP_ID="pc-water-cooling-controller"
PYTHON_BIN="${PYTHON_BIN:-python3}"

mkdir -p "$DIST_DIR"
rm -rf "$BUILD_DIR"
mkdir -p "$APPDIR"

echo "==> Building AppImage"
echo "Version: $VERSION"
echo "App dir: $APP_DIR"

if ! command -v "$PYTHON_BIN" >/dev/null 2>&1; then
    echo "Python not found: $PYTHON_BIN" >&2
    exit 1
fi

echo "==> Creating venv"
"$PYTHON_BIN" -m venv "$BUILD_DIR/venv"

# shellcheck disable=SC1091
. "$BUILD_DIR/venv/bin/activate"

python -m pip install --upgrade pip wheel setuptools
python -m pip install -r "$APP_DIR/requirements.txt"
python -m pip install pyinstaller

echo "==> Running PyInstaller"
cd "$APP_DIR"

pyinstaller \
    --name pcwcc \
    --noconfirm \
    --clean \
    --windowed \
    --paths "$APP_DIR/src" \
    "$APP_DIR/src/pcwcc/main.py"

cd "$ROOT_DIR"

if [ ! -x "$APP_DIR/dist/pcwcc/pcwcc" ]; then
    echo "PyInstaller output not found: $APP_DIR/dist/pcwcc/pcwcc" >&2
    find "$APP_DIR/dist" -maxdepth 3 -type f | sort || true
    exit 1
fi

echo "==> Creating AppDir"

mkdir -p "$APPDIR/usr/bin"
mkdir -p "$APPDIR/usr/share/applications"
mkdir -p "$APPDIR/usr/share/icons/hicolor/256x256/apps"

cp -a "$APP_DIR/dist/pcwcc" "$APPDIR/usr/bin/pcwcc"

cat > "$APPDIR/AppRun" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

HERE="$(dirname "$(readlink -f "$0")")"

export QT_AUTO_SCREEN_SCALE_FACTOR=1
export QT_ENABLE_HIGHDPI_SCALING=1

exec "$HERE/usr/bin/pcwcc/pcwcc" "$@"
EOF

chmod +x "$APPDIR/AppRun"

cat > "$APPDIR/${APP_ID}.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=${APP_NAME}
Exec=pcwcc
Icon=${APP_ID}
Categories=Utility;System;
Terminal=false
EOF

cp "$APPDIR/${APP_ID}.desktop" "$APPDIR/usr/share/applications/${APP_ID}.desktop"

python - <<'PY'
from pathlib import Path
import base64

# Minimal 1x1 PNG icon placeholder.
# Replace later with a real 256x256 icon.
png_b64 = (
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8"
    "/x8AAwMCAO+/p9sAAAAASUVORK5CYII="
)

root = Path("build/appimage/AppDir")
icon_name = "pc-water-cooling-controller.png"

for path in [
    root / icon_name,
    root / "usr/share/icons/hicolor/256x256/apps" / icon_name,
]:
    path.write_bytes(base64.b64decode(png_b64))
PY

echo "==> Downloading appimagetool"
APPIMAGETOOL="$BUILD_DIR/appimagetool-x86_64.AppImage"

if [ ! -f "$APPIMAGETOOL" ]; then
    wget -q -O "$APPIMAGETOOL" \
        "https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage"
    chmod +x "$APPIMAGETOOL"
fi

echo "==> Building final AppImage"

OUT_FILE="$DIST_DIR/pc-water-cooling-controller-${VERSION}-desktop-linux-x86_64.AppImage"

ARCH=x86_64 "$APPIMAGETOOL" "$APPDIR" "$OUT_FILE"

chmod +x "$OUT_FILE"

echo "==> AppImage artifact:"
ls -lh "$OUT_FILE"