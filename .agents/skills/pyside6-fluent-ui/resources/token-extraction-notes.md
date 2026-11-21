# Fluent 2 token extraction

- Official package: `@fluentui/react-theme`
- Package version: `9.2.1`
- Extracted: `2026-07-12`
- Resolved tokens per theme: `459`

## Files

- `fluent2_official_web_theme_tokens.json`: all resolved Microsoft `webLightTheme` and `webDarkTheme` values.
- `fluent2_qt_tokens.py`: the same complete dictionaries plus a compact Qt-oriented semantic mapping and helpers for pixel and duration tokens.

## Important implementation note

These are Fluent UI **Web** theme values. They are appropriate as a design-token source for a Qt/PySide application, but Qt control geometry, native metrics, focus behavior, and platform integration still need Qt-specific implementation.
