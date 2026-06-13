#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
. "${IDF_EXPORT:-$HOME/esp/esp-idf/export.sh}"
idf.py -C firmware build
