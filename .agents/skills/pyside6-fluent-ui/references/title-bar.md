# Title Bar profiles

The Title Bar is the most platform-sensitive part of the workbench profile. Treat it as window-management infrastructure, not ordinary styling.

## Profile selection

Choose from the requested visual and platform outcome:

1. **Single VS Code-style row requested:** use Mode C, the frameless workbench profile.
2. **Native caption content is acceptable and Qt 6.9+ support is verified:** Mode A may blend the client background into the native caption area.
3. **Reliability or platform convention takes priority:** use Mode B, the native Title Bar plus command strip.

Mode A is not a substitute for Mode C. `ExpandedClientAreaHint` expands drawing into the caption area but does not remove the operating system's caption content; depending on safe-area margins it can still read as two visual tiers.

## Mode A: expanded native client area

On Qt 6.9+ where the platform plugin supports it, request:

```python
Qt.WindowType.ExpandedClientAreaHint
Qt.WindowType.NoTitleBarBackgroundHint
```

The intent is to draw client content into the title area while preserving native window controls where practical.

This mode deliberately retains native caption content. Do not describe it as a custom one-row title bar and do not add duplicate custom caption controls.

Requirements:

- capability/version check before using the flags;
- query `QWindow.safeAreaMargins()` after the native window exists;
- react to safe-area changes where the binding exposes the signal;
- keep buttons, fields, menus, and drag regions outside native-control/safe-area conflicts;
- maintain active/inactive styling;
- test maximized, restored, full-screen, multi-monitor, and scale-change states;
- provide a fallback without branching the whole application architecture.

Do not assume the safe area is constant or symmetric.

## Mode B: native Title Bar plus command strip

This is the default fallback and often the most reliable cross-platform choice.

Structure:

```text
OS-native title bar
FluentTitleBar / command strip inside the client area
main application shell
```

The strip may contain app identity, breadcrumbs/title, command search, and restrained shell actions. Do not draw duplicate minimize/maximize/close controls.

Benefits:

- native movement, resize, snapping/tiling, system menu, and caption controls remain intact;
- fewer platform-specific code paths;
- better compatibility with assistive technology and window managers;
- the same strip can be used in the expanded mode later.

## Mode C: single-row frameless workbench

Use this mode when the user explicitly wants the menu, command center, shell actions, and window controls in one client-owned row. `Qt.FramelessWindowHint` transfers significant native behavior to the application. A complete implementation must cover:

- system move using `QWindow.startSystemMove()`;
- system resize using `QWindow.startSystemResize(edge)`;
- resize hit testing for every supported edge/corner;
- double-click maximize/restore;
- correct drag behavior when maximized;
- system menu access where applicable;
- native or accurately adapted caption controls;
- inactive window styling;
- a visible active/inactive outer window boundary;
- keyboard access and screen-reader identity;
- snapping/tiling behavior;
- multiple monitors and per-monitor DPI;
- minimum size and maximized margins;
- platform-specific regression tests.

Do not implement primary dragging by repeatedly setting window geometry from mouse deltas. System move/resize delegates to the window manager and preserves behavior such as snapping and tiling more reliably.

Use `templates/pyside6_fluent_ui/title_bar.py` as the cross-platform baseline. `FluentTitleBar(mode=TitleBarMode.FRAMELESS)` provides application menus, caption controls, drag-region filtering, double-click maximize/restore, an Alt+Space window menu, and stable accessible identities. `FramelessWindowController` provides eight invisible edge/corner targets backed by `QWindow.startSystemResize()`.

Wrap the shell content with a `QFrame` using `fluentRole="windowFrame"` and attach `FluentWindowFrameController` from `templates/pyside6_fluent_ui/window_frame.py`. On Windows 11 it applies the active or inactive semantic border through `DWMWA_BORDER_COLOR` and retains DWM corner preference. When that attribute is unavailable or rejected, generated application QSS draws the client-side boundary. The controller updates activation and theme changes, suppresses the boundary while maximized/full-screen, and does not replace the resize or Snap Layout controllers.

On Windows 11, use `templates/pyside6_fluent_ui/windows_title_bar.py` as well. The top-level window must inherit `WindowsSnapLayoutWindowMixin` before `QMainWindow`, then retain the `WindowsSnapLayoutAdapter` instance created for its maximize button. The adapter:

- handles the top-level `QWidget.nativeEvent()` route, not an application-wide event filter;
- returns `HTMAXBUTTON` only when `WM_NCHITTEST` lands inside the visible, enabled maximize/restore button;
- converts signed, physical Win32 screen coordinates to Qt logical client coordinates for per-monitor DPI;
- defers QSS repolishing and maximize/restore activation until native message handling has returned;
- leaves every other native hit result to Qt and keeps the eight client-side resize handles intact;
- disables itself on non-Windows platforms and Windows versions before Windows 11.

This adapter addresses the custom-maximize hit-test requirement documented by Microsoft; it does not make the OS flyout application-owned. Manually verify the actual hover flyout, Win+Z, zone selection, maximized/restored behavior, and multiple DPI/monitor configurations on each supported Windows build. Keep Mode B available if native integration regresses.

## Content model

The integrated surface has up to four zones:

```text
leading: app identity and QMenuBar menus
center:  title, breadcrumb, or command center
trailing: restrained contextual actions
overlay:  native window controls / reserved safe area
```

Rules:

- ordinary page commands stay in the page or local toolbar;
- use a real non-native `QMenuBar` in the row so Alt mnemonics and keyboard menu navigation remain Qt-native;
- the title remains understandable when the center control is hidden;
- command search has an accessible name, shortcut, placeholder, and focus treatment;
- low-priority actions move to overflow at narrow widths;
- keep the minimum width at or below 500 effective pixels for common Windows Snap Layouts; 330 or below is preferable when the application remains usable;
- drag regions must not cover interactive controls;
- avoid large logos or saturated branding that competes with content.

## Drag regions

In expanded or frameless mode:

- only noninteractive regions initiate system move;
- clicking a child button, menu, field, or link never starts a move;
- use `childAt()`/event-target checks or a dedicated drag-region widget;
- exclude the native safe area;
- preserve text selection and context menus in interactive title content;
- test touch/pen behavior if supported.

A reusable `FluentTitleDragRegion` may call `windowHandle().startSystemMove()` on left-button press.

## Caption controls

Prefer native controls. When custom controls are unavoidable:

- use standard minimize, maximize/restore, and close semantics;
- update maximize/restore icon and accessible name with state;
- make Close visually distinct only on hover/pressed according to platform convention;
- provide sufficient target size and visible focus;
- preserve right-to-left and platform placement conventions where relevant;
- never use text glyphs such as `X`, `—`, or `□` as final production icons.

## Active and inactive states

The title surface must respond to window activation changes.

Suggested dynamic property:

```python
set_fluent_property(title_bar, "windowActive", window.isActiveWindow())
```

Update from `QEvent.WindowActivate`, `QEvent.WindowDeactivate`, and relevant state changes. Inactive state should reduce foreground emphasis while retaining readability; do not make it look disabled.

## Reference metrics

Current VS Code source provides two useful density references:

```text
35 px — custom title bar with command center
30 px — simpler title configuration
```

These are not normative. Effective Qt height must fit:

- font and line height;
- icon/control targets;
- focus border;
- menu and command-center geometry;
- safe-area/native-control requirements;
- target display scale.

## Tokens

Use `resources/shell-token-map.json`:

```text
shell_title_background_active
shell_title_foreground_active
shell_title_background_inactive
shell_title_foreground_inactive
shell_title_border
shell_title_command_background
shell_title_command_hover
shell_title_command_pressed
shell_title_command_focus
shell_window_border_active
shell_window_border_inactive
```

Do not import VS Code's exact workbench theme colors. Resolve from the Fluent theme and application brand.

## Accessibility

- title text should remain available as the actual window title via `setWindowTitle()`;
- icon-only controls get accessible names and tooltips;
- command search is a real input/button, not a painted decoration;
- focus remains visible over active and inactive title surfaces;
- no drag region becomes an unexplained keyboard focus stop;
- focus can move from title controls into the body predictably;
- native system actions remain available in the chosen mode.

## Validation matrix

At minimum test:

| State | Checks |
|---|---|
| active/restored | title, drag, controls, focus |
| inactive | semantic foreground/background and outer-boundary change |
| maximized | safe margins, drag/restore, no clipped controls, no restored-state outer border |
| full-screen | boundary hidden, correct surface visibility and restoration |
| 100–200% scale | no overlap or clipped targets |
| multi-monitor | movement and scale transition |
| narrow window | overflow and title elision |
| Windows 11 frameless | `HTMAXBUTTON`, hover flyout, native press/release, Win+Z, zone selection |
| keyboard only | command focus, window actions, leave region |
| light/dark/high contrast | contrast and native-control integration |

## Starter-template contract

The template implements all three profiles behind `TitleBarMode`:

- native-title fallback with the Fluent menu/command strip below OS chrome;
- capability-gated expanded-area flags and safe-area hooks without duplicate caption controls;
- a single-row frameless baseline with menus, accessible caption actions, system move, edge/corner system resize, double-click maximize/restore, and an Alt+Space window menu;
- a semantic frameless-window boundary with Windows 11 DWM rendering and a generated-QSS client fallback;
- a capability-gated Windows 11 native-event mixin/adapter for `HTMAXBUTTON`, deferred maximize/restore activation, and Snap Layout-compatible responsive sizing.

Applications must still run the validation matrix on every platform they claim. Keep a startup setting or command-line option that selects the native fallback when a platform integration regresses.
