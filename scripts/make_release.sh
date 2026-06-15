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

deactivate_esp_env() {
    local idf_python_bin="${IDF_PYTHON_ENV_PATH:-}"
    local idf_tools_bin="${IDF_TOOLS_PATH:-}"
    local python_mm=""
    local python_bin=""

    if declare -F deactivate >/dev/null 2>&1; then
        deactivate || true
    fi

    if [ -n "$idf_python_bin" ]; then
        PATH="$(remove_path_entry "$idf_python_bin/bin" "$PATH")"
    fi

    if [ -n "$idf_tools_bin" ]; then
        PATH="$(remove_path_entry "$idf_tools_bin" "$PATH")"
    fi

    if [ -n "${PYTHON_VERSION:-}" ] && [ -d "/opt/python-${PYTHON_VERSION}/bin" ]; then
        python_mm="$(cut -d. -f1,2 <<< "$PYTHON_VERSION")"
        python_bin="/opt/python-${PYTHON_VERSION}/bin/python${python_mm}"
        PATH="/opt/python-${PYTHON_VERSION}/bin:${PATH}"
        export PYTHON="$python_bin"
        export PYTHON_BIN="$python_bin"
    fi

    export PATH
    unset IDF_PATH || true
    unset IDF_TOOLS_PATH || true
    unset IDF_TARGET || true
    unset IDF_CCACHE_ENABLE || true
    unset IDF_PYTHON_ENV_PATH || true
    unset ESP_IDF_VERSION || true
}

echo "==> Making release artifacts"
echo "Version: $VERSION"

rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"

./scripts/build_firmware.sh "$VERSION"
deactivate_esp_env
./scripts/build_appimage.sh "$VERSION"
./scripts/build_hardware.sh "$VERSION"

echo "==> Creating checksums"
(
    cd "$DIST_DIR"
    sha256sum * > "pc-water-cooling-controller-${VERSION}-checksums.sha256"
)

echo "==> Release artifacts are ready:"
ls -lh "$DIST_DIR"
