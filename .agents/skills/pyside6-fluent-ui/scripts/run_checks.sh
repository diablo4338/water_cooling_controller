#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
python "$ROOT/scripts/validate_skill.py" "$ROOT"
python "$ROOT/scripts/check_contrast.py" "$ROOT"
python -m unittest discover -s "$ROOT/tests" -v
