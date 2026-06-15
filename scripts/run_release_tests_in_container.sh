#!/usr/bin/env bash

set -euo pipefail

PYTHON_VERSION="${1:?Python version argument is required}"
PYTHON_MM="$(cut -d. -f1,2 <<< "${PYTHON_VERSION}")"
PYTHON_BIN="/opt/python-${PYTHON_VERSION}/bin/python${PYTHON_MM}"

if [[ ! -x "${PYTHON_BIN}" ]]; then
    echo "Python binary not found: ${PYTHON_BIN}" >&2
    exit 1
fi

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/pcwcc-release-tests.XXXXXX")"
STAGE_DIR="${WORK_DIR}/src"
VENV_DIR="${WORK_DIR}/venv"

cleanup() {
    if [[ -d "${VENV_DIR}" ]]; then
        echo "==> Removing test venv"
        rm -rf "${VENV_DIR}"
    fi

    if [[ -d "${WORK_DIR}" ]]; then
        echo "==> Removing staged sources"
        rm -rf "${WORK_DIR}"
    fi
}
trap cleanup EXIT

echo "==> Staging sources into ${STAGE_DIR}"
mkdir -p "${STAGE_DIR}"
cp -a /work/pcwcc "${STAGE_DIR}/"

echo "==> Creating test venv"
"${PYTHON_BIN}" -m venv "${VENV_DIR}"

echo "==> Installing dependencies"
"${VENV_DIR}/bin/python" -m pip install --upgrade pip wheel setuptools
bash /work/scripts/run_pcwcc_tests.sh "${STAGE_DIR}/pcwcc" "${VENV_DIR}/bin/python"
