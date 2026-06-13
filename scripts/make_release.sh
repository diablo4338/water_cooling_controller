#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
./scripts/build_firmware.sh
echo "Release assembly is not implemented yet."
