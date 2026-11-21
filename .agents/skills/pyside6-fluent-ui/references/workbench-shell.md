# Fluent workbench shell

Use this reference when an application benefits from a persistent desktop workbench: several primary destinations, a contextual sidebar, a large central canvas, and compact global status. Do not impose this shell on a simple form, wizard, or single-purpose utility.

## Structure

```text
┌──────────────────── Title/command surface ────────────────────┐
│ app/menu | title or command center | contextual actions       │
├──────────┬─────────────────────────────────────────────────────┤
│ Activity │ Primary Sidebar │ main content / editor / viewport │
│ Bar      │                 │                                   │
│          │                 ├───────────────────────────────────┤
│          │                 │ optional secondary/bottom panel   │
├──────────┴─────────────────┴───────────────────────────────────┤
│ global/workspace status                 contextual status      │
└──────────────────────── Status Bar ─────────────────────────────┘
```

The shell can match VS Code's structural arrangement, including a single client-owned top row, without copying VS Code product marks, exact color themes, commands, or proprietary branding. Use Fluent semantic tokens and the application's own identity.

## When the profile fits

Use it when most of these are true:

- the application has three or more persistent primary work areas;
- users repeatedly switch among tools while maintaining one working context;
- a contextual sidebar is useful but should not consume permanent central space;
- the central area is a document, engineering viewport, table, canvas, or editor;
- concise global/context status benefits expert users;
- users are likely to keep the application open for long work sessions.

Do not use it merely to make an application "look technical." A simple settings window, short wizard, or one-screen utility normally needs a conventional window layout.

## Region contracts

### Title/command surface

Owns window/application identity and a restrained set of shell-level commands. In the single-row profile it contains a real `QMenuBar`, optional command center, shell actions, and custom caption controls in one horizontal layout. In the native fallback the same menu/command surface sits below OS chrome without custom caption controls. It is not a second toolbar for every page action. See `title-bar.md`.

### Activity Bar

Switches among a small number of primary view containers. Selection persists and normally controls the Primary Sidebar or top-level view. See `activity-bar.md`.

### Primary Sidebar

Shows navigation or tools belonging to the selected Activity Bar destination. It should be resizable and may be hideable. Use a `QSplitter`, a sidebar host, or a `QDockWidget` according to the existing architecture.

### Main content

Owns the user's primary task. Do not give shell chrome higher visual emphasis than this region. Page-specific command bars belong inside the main content or its immediate header.

### Optional panel

Use for secondary output, logs, properties, validation, terminal-like content, or diagnostics that benefit from sharing the working context. It must be resizable, hideable, and focusable. Do not use it as a dumping ground for every secondary feature.

### Status Bar

Displays compact global/workspace state on the left and contextual state on the right. See `status-bar.md`.

## Recommended Qt composition

```python
QMainWindow
└─ central QWidget / FluentWindowFrame
   └─ QVBoxLayout(spacing=0, margins=0)
      ├─ FluentTitleBar (frameless single row or native-fallback command strip)
      ├─ QSplitter(Qt.Horizontal)
      │  ├─ FluentActivityBar
      │  └─ QSplitter(Qt.Horizontal)
      │     ├─ PrimarySidebarHost
      │     └─ QSplitter(Qt.Vertical)
      │        ├─ MainContentHost
      │        └─ OptionalPanelHost
      └─ FluentStatusBar
```

For the frameless profile, make `FluentWindowFrame` a semantic `QFrame` wrapper and attach `FluentWindowFrameController`. Keep the inner workbench layout at zero margins; the controller selects a native DWM boundary on supported Windows 11 systems or the generated-QSS client fallback elsewhere. Native and expanded title profiles retain their platform frame and do not add a second boundary.

Do not blindly nest splitters if the existing application already owns docks or viewport geometry. Preserve the existing ownership model and map the regions onto it.

## Focus order

A reasonable default shell order is:

1. title/command controls;
2. Activity Bar;
3. Primary Sidebar;
4. main content;
5. optional panel;
6. actionable Status Bar entries.

This is not a mandate to put every region in the tab chain. Passive labels and decorative containers must not become focus stops. Provide direct shortcuts for expert users when the application already has a command system.

## Density profiles

The shell may expose `standard` and `compact` density profiles. Density changes spacing and target metrics, not font legibility or focus visibility.

| Region | Standard reference | Compact reference |
|---|---:|---:|
| Activity Bar width | 48 px | 36 px |
| Activity item height | 48 px | 32 px |
| Activity icon | 24 px | 16 px |
| Title surface | 35 px | 30 px |
| Status Bar | calculated, ~24–28 px | calculated, 22 px reference |

Treat these values as starting points. Compute effective minimums from `QFontMetrics`, icon size, focus border, padding, and device scaling.

## State restoration

When the existing application persists layout state, preserve:

- selected Activity Bar destination;
- Sidebar visibility and width;
- Panel visibility and height;
- splitter sizes;
- last focused meaningful child where safe;
- window geometry and maximized/full-screen state.

Use stable IDs, not translated labels, as persistence keys.

## Responsive behavior

Desktop responsiveness means controlled reallocation, not a web breakpoint imitation.

At narrower widths:

1. reduce optional page padding;
2. collapse or hide the Primary Sidebar with a clear restore action;
3. move low-priority title actions to overflow;
4. hide nonessential Status Bar text before essential state;
5. keep Activity Bar and focus targets usable;
6. never overlap native window controls or safe-area regions.

For a Windows 11 Snap Layout-compatible frameless shell, support a minimum width of no more than 500 epx. Hide the center command surface and low-priority trailing actions before menus or caption controls; 330 epx or below is preferred only when the remaining application can stay usable.

Do not silently remove primary actions. Use overflow menus or explicit collapsible regions.

## Visual hierarchy

- shell backgrounds should be quiet neutral surfaces;
- selected Activity Bar state may use a narrow brand indicator plus foreground change;
- dividers should be subtle and used only where spatial separation is insufficient;
- central content should usually have the strongest contrast and largest working area;
- status severity colors are exceptional, not decorative;
- shadows are for genuine floating layers, not every docked region.

## Validation

Verify:

- each shell region can be reached and left with keyboard;
- splitter collapse/restore does not strand focus;
- selected Activity Bar state and visible Sidebar agree;
- Status Bar content remains readable at minimum supported width;
- active/inactive title state updates correctly;
- active/inactive frameless outer-boundary state updates correctly and disappears when maximized/full-screen;
- light, dark, high-contrast, and system-following behavior;
- 100%, 125%, 150%, and 200% display scaling where supported;
- persisted layout remains compatible with the pre-refactor application.
- Windows 11 frameless mode passes the live `HTMAXBUTTON` probe and manual Snap Layout hover/Win+Z checks.
