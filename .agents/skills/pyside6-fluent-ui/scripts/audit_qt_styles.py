#!/usr/bin/env python3
"""Static audit for common Qt styling migration risks.

This is a triage tool, not a proof of correctness. Review findings in context.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import json
from pathlib import Path
import re


@dataclass(slots=True)
class Finding:
    severity: str
    rule: str
    path: str
    line: int
    excerpt: str


RULES: list[tuple[str, str, re.Pattern[str]]] = [
    ("warning", "inline-stylesheet", re.compile(r"\.setStyleSheet\s*\(")),
    ("warning", "hardcoded-hex-color", re.compile(r"(?<![A-Za-z0-9_])#[0-9a-fA-F]{3,8}(?![A-Za-z0-9_])")),
    ("warning", "hardcoded-rgb-color", re.compile(r"\brgba?\s*\(")),
    ("warning", "fixed-geometry", re.compile(r"\.(?:setGeometry|move)\s*\(")),
    ("info", "fixed-size", re.compile(r"\.(?:setFixedSize|setFixedWidth|setFixedHeight)\s*\(")),
    ("info", "hardcoded-layout-spacing", re.compile(r"\.(?:setSpacing|setContentsMargins)\s*\([^\n]*\d")),
    ("info", "font-pixel-size", re.compile(r"\.(?:setPixelSize|setPointSize)\s*\(\s*\d")),
    ("warning", "frameless-window", re.compile(r"FramelessWindowHint")),
    (
        "warning",
        "manual-window-drag",
        re.compile(r"(?:setGeometry|move)\s*\([^\n]*(?:globalPosition|globalPos|mouse)"),
    ),
]

TEXT_SUFFIXES = {".py", ".qss", ".ui", ".qrc", ".cpp", ".h", ".hpp"}
DEFAULT_EXCLUDES = {".git", ".venv", "venv", "build", "dist", "__pycache__", "node_modules"}


def scan(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for path in root.rglob("*"):
        if not path.is_file() or path.suffix.lower() not in TEXT_SUFFIXES:
            continue
        if any(part in DEFAULT_EXCLUDES for part in path.parts):
            continue
        # Generated theme/token files are expected to contain resolved values.
        if any(part in {"generated", "resources", "tokens"} for part in path.parts):
            continue
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except UnicodeDecodeError:
            continue
        for line_number, line in enumerate(lines, 1):
            stripped = line.strip()
            if not stripped or stripped.startswith(("#", "//", "/*", "*")):
                continue
            if "fluent-audit: allow" in line:
                continue
            for severity, rule, pattern in RULES:
                if rule == "hardcoded-layout-spacing" and pattern.search(line):
                    numbers = [float(value) for value in re.findall(r"(?<![A-Za-z_])-?\d+(?:\.\d+)?", line)]
                    if numbers and all(value == 0 for value in numbers):
                        continue
                if pattern.search(line):
                    findings.append(
                        Finding(
                            severity=severity,
                            rule=rule,
                            path=str(path.relative_to(root)),
                            line=line_number,
                            excerpt=stripped[:220],
                        )
                    )
    return findings


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--json", action="store_true", dest="as_json")
    parser.add_argument("--fail-on-warning", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    findings = scan(root)
    if args.as_json:
        print(json.dumps([asdict(item) for item in findings], indent=2))
    else:
        for item in findings:
            print(f"{item.severity.upper():7} {item.rule:26} {item.path}:{item.line}  {item.excerpt}")
        print(f"\n{len(findings)} finding(s)")
    if args.fail_on_warning and any(item.severity == "warning" for item in findings):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
