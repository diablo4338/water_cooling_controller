# Starter runtime templates

These modules demonstrate one way to implement the contracts in `SKILL.md`. They are intentionally composable and should be adapted to the target application rather than installed as an independent UI framework.

## Resource layout

`TokenRepository.from_skill_root()` expects the skill layout:

```text
<root>/resources/fluent2-official-web-theme-tokens.json
<root>/resources/qt-token-map.json
<root>/resources/shell-token-map.json
<root>/templates/pyside6_fluent_ui/
```

When integrating into an application, either preserve an equivalent resource layout or construct `TokenRepository` with explicit resource paths.

## Typical bootstrap

```python
repository = TokenRepository.from_skill_root(skill_root)
manager = FluentThemeManager(app, repository)
manager.apply()
```

Set semantic properties through `set_fluent_property()` and avoid local style strings. Use the workbench-shell classes only where their information architecture suits the application.

For a VS Code-style single top row, construct `FluentWorkbenchWindow` with `TitleBarMode.FRAMELESS`; its title bar supplies functional File/View menus and accessible caption controls, while `FramelessWindowController` restores Qt-native edge/corner resizing. Pass the `FluentThemeManager` as `theme_manager` so `FluentWindowFrameController` updates the semantic active/inactive outer boundary on theme changes. On Windows 11, DWM draws that boundary and the retained `WindowsSnapLayoutWindowMixin`/`WindowsSnapLayoutAdapter` route only the custom maximize region to `HTMAXBUTTON`; generated QSS provides the boundary fallback elsewhere. The shell collapses low-priority title content below 640 px and supports a 500 epx minimum width. Keep `TitleBarMode.NATIVE_FALLBACK` available for platform fallback. Expanded client area retains native caption content and is not the one-row profile.
