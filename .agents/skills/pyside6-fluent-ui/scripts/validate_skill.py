#!/usr/bin/env python3
"""Static validation for the reusable skill package; PySide6 is optional."""

from __future__ import annotations

import argparse
import ast
import hashlib
import json
from pathlib import Path
import re
import sys

REQUIRED = [
    "LICENSE",
    "NOTICE.md",
    "SHA256SUMS",
    "SKILL.md",
    "resources/fluent2-official-web-theme-tokens.json",
    "resources/qt-token-map.json",
    "resources/shell-token-map.json",
    "resources/component-state-matrix.json",
    "resources/source-manifest.json",
    "templates/pyside6_fluent_ui/__init__.py",
    "templates/pyside6_fluent_ui/tokens.py",
    "templates/pyside6_fluent_ui/metrics.py",
    "templates/pyside6_fluent_ui/theme.py",
    "templates/pyside6_fluent_ui/style.py",
    "templates/pyside6_fluent_ui/activity_bar.py",
    "templates/pyside6_fluent_ui/status_bar.py",
    "templates/pyside6_fluent_ui/title_bar.py",
    "templates/pyside6_fluent_ui/windows_title_bar.py",
    "templates/pyside6_fluent_ui/window_frame.py",
    "templates/pyside6_fluent_ui/icons/minimize.svg",
    "templates/pyside6_fluent_ui/icons/maximize.svg",
    "templates/pyside6_fluent_ui/icons/restore.svg",
    "templates/pyside6_fluent_ui/icons/close.svg",
    "templates/pyside6_fluent_ui/icons/chevron-up.svg",
    "templates/pyside6_fluent_ui/icons/chevron-down.svg",
    "templates/pyside6_fluent_ui/workbench.py",
    "templates/pyside6_fluent_ui/widgets.py",
    "templates/pyside6_fluent_ui/fluent.qss.in",
    "examples/widget_gallery.py",
    "scripts/update_checksums.py",
    "scripts/verify_windows_snap_layout.py",
    "tests/test_tokens.py",
    "tests/test_structure.py",
    "tests/test_contrast.py",
    "tests/test_title_bar.py",
    "tests/test_windows_snap_layout.py",
    "tests/test_window_frame.py",
    "tests/test_spin_box.py",
]

SUPERSEDED = [
    "templates/fluent.qss.in",
    "templates/pyside6_fluent_ui/fluent_tokens.py",
    "templates/pyside6_fluent_ui/fluent_theme.py",
    "templates/pyside6_fluent_ui/fluent_palette.py",
    "templates/pyside6_fluent_ui/fluent_activity_bar.py",
    "templates/pyside6_fluent_ui/fluent_status_bar.py",
    "templates/pyside6_fluent_ui/fluent_title_bar.py",
    "examples/workbench_gallery.py",
]

REFERENCE_RE = re.compile(r"`((?:references|resources|templates|scripts|examples)/[^`]+)`")
PLACEHOLDER_RE = re.compile(r"@\{([A-Za-z_][A-Za-z0-9_]*)\}")
QSS_ASSET_PLACEHOLDERS = {
    "spin_up_icon",
    "spin_down_icon",
    "spin_up_icon_interactive",
    "spin_down_icon_interactive",
    "spin_up_icon_disabled",
    "spin_down_icon_disabled",
}
TEXT_GLYPH_ICON_RE = re.compile(r"\.setText\(\s*[\"'][×⚙☰□—][\"']\s*\)")
CHECKSUM_RE = re.compile(r"^([0-9a-fA-F]{64})\s+\*?(?:\./)?(.+)$")


def _package_files(root: Path) -> dict[str, Path]:
    files: dict[str, Path] = {}
    for path in root.rglob("*"):
        if not path.is_file() or path.name == "SHA256SUMS":
            continue
        relative = path.relative_to(root)
        if "__pycache__" in relative.parts or path.suffix == ".pyc":
            continue
        files[relative.as_posix()] = path
    return files


def _validate_checksums(root: Path, errors: list[str], notes: list[str]) -> None:
    manifest = root / "SHA256SUMS"
    if not manifest.is_file():
        return

    listed: dict[str, str] = {}
    for line_number, raw_line in enumerate(
        manifest.read_text(encoding="utf-8-sig").splitlines(), 1
    ):
        line = raw_line.strip()
        if not line:
            continue
        match = CHECKSUM_RE.fullmatch(line)
        if match is None:
            errors.append(f"Malformed SHA256SUMS entry on line {line_number}")
            continue
        digest, relative = match.groups()
        normalized = Path(relative).as_posix()
        if normalized.startswith("../") or Path(normalized).is_absolute():
            errors.append(f"Unsafe SHA256SUMS path on line {line_number}: {relative}")
            continue
        if normalized in listed:
            errors.append(f"Duplicate SHA256SUMS path: {normalized}")
            continue
        listed[normalized] = digest.lower()

    package_files = _package_files(root)
    for relative in sorted(set(package_files) - set(listed)):
        errors.append(f"SHA256SUMS missing package file: {relative}")
    for relative in sorted(set(listed) - set(package_files)):
        errors.append(f"SHA256SUMS references missing package file: {relative}")
    for relative in sorted(set(package_files) & set(listed)):
        actual = hashlib.sha256(package_files[relative].read_bytes()).hexdigest()
        if actual != listed[relative]:
            errors.append(f"SHA256 mismatch: {relative}")

    if not any(error.startswith(("SHA256SUMS", "SHA256 mismatch", "Malformed SHA256SUMS", "Unsafe SHA256SUMS", "Duplicate SHA256SUMS")) for error in errors):
        notes.append(f"Validated {len(listed)} SHA-256 package checksums")


def validate(root: Path) -> tuple[list[str], list[str]]:
    errors: list[str] = []
    notes: list[str] = []

    for relative in REQUIRED:
        if not (root / relative).is_file():
            errors.append(f"Missing required file: {relative}")
    for relative in SUPERSEDED:
        if (root / relative).exists():
            errors.append(f"Superseded parallel draft still present: {relative}")

    _validate_checksums(root, errors, notes)

    skill = root / "SKILL.md"
    if skill.is_file():
        text = skill.read_text(encoding="utf-8")
        if not text.startswith("---\n") or "\n---\n" not in text[4:]:
            errors.append("SKILL.md does not contain YAML-style front matter")
        for phrase in (
            "name: pyside6-fluent-ui",
            "Activity Bar",
            "Status Bar",
            "Title Bar",
            "Do not introduce React, WinUI, XAML, or browser dependencies",
        ):
            if phrase not in text:
                errors.append(f"SKILL.md missing expected phrase: {phrase}")
        for relative in sorted(set(REFERENCE_RE.findall(text))):
            if not (root / relative).exists():
                errors.append(f"SKILL.md links to missing file: {relative}")

    for path in root.rglob("*.json"):
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
            if not isinstance(data, dict):
                errors.append(f"JSON root must be an object: {path.relative_to(root)}")
        except Exception as exc:  # noqa: BLE001 - aggregate validation findings
            errors.append(f"Invalid JSON {path.relative_to(root)}: {exc}")

    for path in root.rglob("*.py"):
        try:
            ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
        except SyntaxError as exc:
            errors.append(f"Syntax error {path.relative_to(root)}:{exc.lineno}: {exc.msg}")

    for path in (root / "templates").rglob("*.py"):
        if TEXT_GLYPH_ICON_RE.search(path.read_text(encoding="utf-8")):
            errors.append(f"Template uses a text glyph as a production icon: {path.relative_to(root)}")

    sys.path.insert(0, str(root / "templates"))
    try:
        from pyside6_fluent_ui.tokens import TokenRepository
    except Exception as exc:  # noqa: BLE001
        errors.append(f"Could not import token module: {exc}")
    else:
        try:
            repository = TokenRepository.from_skill_root(root)
            qss_template = (
                root / "templates/pyside6_fluent_ui/fluent.qss.in"
            ).read_text(encoding="utf-8")
            placeholders = set(PLACEHOLDER_RE.findall(qss_template))
            for profile in ("fluent-workbench", "fluent-workbench-neutral-status"):
                for theme_name in ("light", "dark"):
                    theme = repository.resolve(theme_name, shell_profile=profile)
                    non_strings = [name for name, value in theme.aliases.items() if not isinstance(value, str)]
                    if non_strings:
                        errors.append(
                            f"Non-string aliases for {theme_name}/{profile}: {sorted(non_strings)}"
                        )
                    missing = sorted(
                        placeholders - set(theme.aliases) - QSS_ASSET_PLACEHOLDERS
                    )
                    if missing:
                        errors.append(f"Missing QSS aliases for {theme_name}/{profile}: {missing}")
                    rendered = qss_template
                    for alias, value in theme.aliases.items():
                        rendered = rendered.replace(f"@{{{alias}}}", str(value))
                    for asset in QSS_ASSET_PLACEHOLDERS:
                        rendered = rendered.replace(f"@{{{asset}}}", "symbolic.svg")
                    leftovers = sorted(set(PLACEHOLDER_RE.findall(rendered)))
                    if leftovers:
                        errors.append(
                            f"Unresolved QSS aliases for {theme_name}/{profile}: {leftovers}"
                        )
            notes.append(
                f"Validated {repository.metadata.get('tokenCountPerTheme', '?')} official tokens per theme"
            )
        except Exception as exc:  # noqa: BLE001
            errors.append(f"Token/QSS validation failed: {exc}")

    try:
        import PySide6  # type: ignore
    except ModuleNotFoundError:
        notes.append("PySide6 runtime unavailable: runtime/gallery execution skipped")
    else:
        notes.append(f"PySide6 runtime available: {PySide6.__version__}")

    return errors, notes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "root",
        nargs="?",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="Skill root (defaults to the parent of scripts/)",
    )
    args = parser.parse_args()
    root = args.root.resolve()
    errors, notes = validate(root)

    print(f"Skill root: {root}")
    for note in notes:
        print(f"NOTE: {note}")
    if errors:
        for error in errors:
            print(f"ERROR: {error}")
        print(f"FAILED with {len(errors)} error(s)")
        return 1
    print("PASS: reusable skill static validation completed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
