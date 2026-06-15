#!/usr/bin/env bash

set -euo pipefail

APP_DIR="${1:?App directory argument is required}"
PYTHON_BIN="${2:?Python executable argument is required}"

if [[ ! -d "${APP_DIR}" ]]; then
    echo "App directory not found: ${APP_DIR}" >&2
    exit 1
fi

if [[ ! -x "${PYTHON_BIN}" ]]; then
    echo "Python executable not found: ${PYTHON_BIN}" >&2
    exit 1
fi

echo "==> Installing app dependencies"
"${PYTHON_BIN}" -m pip install "${APP_DIR}[dev]"

if [[ -d "${APP_DIR}/tests" ]]; then
    echo "==> Running tests"
    PYTHONPATH="${APP_DIR}/src" "${PYTHON_BIN}" -m pytest \
        -c "${APP_DIR}/pytest.ini" \
        "${APP_DIR}/tests" \
        -q \
        -m "not integration"
fi
