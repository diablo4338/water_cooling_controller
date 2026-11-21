#!/usr/bin/env python3
"""Regenerate SHA256SUMS for every distributable non-cache skill file."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


def package_files(root: Path) -> list[Path]:
    return sorted(
        (
            path
            for path in root.rglob("*")
            if path.is_file()
            and path.name != "SHA256SUMS"
            and path.suffix != ".pyc"
            and "__pycache__" not in path.relative_to(root).parts
        ),
        key=lambda path: path.relative_to(root).as_posix(),
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "root",
        nargs="?",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="Skill root (defaults to the parent of scripts/)",
    )
    root = parser.parse_args().root.resolve()
    lines = []
    for path in package_files(root):
        relative = path.relative_to(root).as_posix()
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        lines.append(f"{digest}  ./{relative}")
    output = root / "SHA256SUMS"
    output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Wrote {len(lines)} checksums to {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
