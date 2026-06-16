#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

IMAGE_NAME="${IMAGE_NAME:-pcwcc-release-build:ci}"
PYTHON_VERSION="${PYTHON_VERSION:?PYTHON_VERSION is required}"

docker run --rm \
    --user "$(id -u):$(id -g)" \
    -v "${ROOT_DIR}:/work" \
    -w /work \
    "${IMAGE_NAME}" \
    bash ./scripts/run_release_tests_in_container.sh "${PYTHON_VERSION}"
