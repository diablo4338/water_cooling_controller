# Phase 1 source review: PySide6 Fluent Workbench skill

**Status:** initial design-authority review completed
**Review date:** 2026-07-13
**Provisional skill name:** `pyside6-fluent-ui`
**Target:** PySide6 / Qt Widgets applications, especially existing GUIs being refactored incrementally

## 1. Scope established in Phase 1

The skill will guide an agent in refactoring an existing Qt Widgets interface so that it:

- follows Microsoft Fluent 2 principles and semantic design tokens;
- remains implemented with PySide6 and Qt-native mechanisms;
- preserves existing behavior, models, signals, controllers, object names, shortcuts, and tests;
- supports light, dark, and high-contrast-aware presentation;
- optionally uses a compact, VS Code-inspired workbench shell;
- includes a vertical Activity Bar, a bottom Status Bar, and an integrated Title Bar;
- avoids turning the application into a pixel-for-pixel VS Code clone.

The workbench shell is an **information-architecture profile**, not a replacement for Fluent. Fluent governs visual hierarchy, semantic colors, type, spacing, states, accessibility, and motion. VS Code supplies a proven desktop shell arrangement. Qt governs implementation and native platform behavior.

## 2. Authority hierarchy

The skill will resolve conflicts in this order:

1. **Official Qt and PySide documentation** for runtime behavior, widget semantics, window management, accessibility APIs, high-DPI behavior, and platform constraints.
2. **Official Microsoft Fluent 2 documentation and the official Fluent token package** for visual design, token semantics, typography, spacing, state colors, elevation, and accessibility targets.
3. **Official VS Code UX guidance and source** for workbench-shell semantics: Activity Bar, Primary Sidebar, Status Bar, Title Bar, and reference density.
4. **The reviewed community Fluent skill** as an organizational checklist and implementation-pattern inventory.
5. Project-specific preferences, provided they do not violate accessibility or native-window requirements.

This order matters. The community skill targets Fluent UI React v9 and therefore cannot be copied directly into a Qt skill.

## 3. Findings from the reviewed Fluent skill

Reviewed source:

- Repository: `fefogarcia/approved-skills`
- Commit: `2da55c784b3341f73c353cda0bc404a19c388fda`
- Main file: `skills/fluent2-design-system/SKILL.md`
- References:
  - `skills/fluent2-design-system/references/tokens.md`
  - `skills/fluent2-design-system/references/components.md`

### Reusable without major conceptual changes

- semantic global/alias token architecture;
- neutral, brand, and status-color roles;
- rest, hover, pressed, selected, focused, and disabled states;
- typography hierarchy;
- spacing, radius, stroke, shadow, and motion categories;
- appearance variants such as primary, secondary, subtle, transparent, and outline;
- component inventories and usage hierarchy;
- light, dark, high-contrast, and custom-brand concepts;
- prohibition on scattered hardcoded visual values;
- visible keyboard focus and semantic color pairing.

### Must be translated for Qt

| React/Fluent concept | PySide6/Qt translation |
|---|---|
| `FluentProvider` | application-level `FluentThemeManager` |
| `tokens.*` | immutable Python token objects plus generated QSS values |
| Griffel `makeStyles()` | generated application QSS, `QPalette`, and narrowly scoped style classes |
| React slots | named child widgets, object names, subcontrols, and composite-widget APIs |
| CSS pseudo-selectors | QSS pseudo-states and dynamic properties |
| CSS grid/flex | `QGridLayout`, box layouts, splitters, size policies, and resize logic |
| ARIA and Tabster | Qt accessibility properties, focus policies, tab order, and `QAccessible` roles/events |
| React component variants | dynamic properties such as `fluentAppearance`, `fluentSize`, and `fluentState` |
| browser media queries | Qt screen/window-size logic and system appearance signals |

### Must not be inherited literally

- React package installation;
- JSX examples;
- Griffel-only restrictions such as avoiding external CSS;
- browser-specific CSS behavior;
- React component APIs as if they were Qt APIs;
- assumptions that web focus, layout, or high-contrast behavior automatically exists in Qt.

## 4. Findings from official Fluent 2 guidance

### Core governing principles

The skill will encode these conclusions:

- Prefer native platform behavior and controls for ordinary interactions.
- Use Fluent for semantic consistency and signature experiences.
- Reduce decorative clutter and keep attention on the user’s content.
- Build accessibility into every component and state.
- Use semantic tokens instead of hardcoded colors or arbitrary dimensions.
- Preserve a clear type and spacing hierarchy.
- Treat the spacing ramp as a tool for relationships, not as a rule that every gap must be identical.

### Token strategy

Fluent uses raw global tokens and semantic alias tokens. The new skill will preserve that separation:

```text
official global values
        ↓
official Fluent aliases
        ↓
Qt-facing semantic aliases
        ↓
QPalette, QSS, delegates, metrics, and custom widgets
```

Existing extracted resources have been retained in this Phase 1 package:

- official resolved Web light theme: 459 tokens;
- official resolved Web dark theme: 459 tokens;
- source package: `@fluentui/react-theme`;
- package version: `9.2.1`;
- extraction date: `2026-07-12`.

The Web values will be used as a design-token source, not as evidence that Qt should behave like a browser.

### Typography decision

- Windows default: Segoe UI Variable where available, otherwise Segoe UI.
- Other platforms: native system font unless a documented project requirement overrides it.
- Default body role: 14 px / 20 px equivalent.
- Sentence case by default.
- Separate roles for caption, body, strong body, subtitle, section title, page title, numeric, and monospace text.
- Use `QFontMetrics` to prevent clipped compact-shell text.

### Spacing decision

The full Fluent ramp is retained, including:

```text
0, 2, 4, 6, 8, 10, 12, 16, 20, 24, 28, 32, 36, 40, 48, 52, 56
```

Component recipes will choose semantic roles such as `control_gap`, `section_gap`, and `page_margin`, rather than scattering raw integers.

## 5. Qt styling architecture selected for the skill

### `QPalette`: broad application roles

Use it for application-wide defaults and the Active, Inactive, and Disabled groups:

- window/base surfaces;
- standard text and placeholder text;
- selection;
- links;
- tooltip defaults;
- disabled foreground/background behavior.

### Generated application QSS: component visuals

Use it for:

- borders and radii;
- padding and control density;
- hover, pressed, checked, selected, and disabled visuals;
- dynamic semantic properties;
- item-view rows and subcontrols;
- menus, tabs, tooltips, scrollbars, and shell surfaces.

The skill will prohibit scattered per-widget `setStyleSheet()` calls except for documented diagnostic or generated cases.

### `QProxyStyle`: narrowly scoped behavior and metrics

Use only where QSS and normal widget APIs are insufficient:

- pixel metrics;
- selected style hints;
- focus rendering;
- indicator geometry;
- icon metrics;
- platform-aware control behavior.

### Custom painting: last resort

Custom painting will require an explicit justification and a complete state/accessibility contract.

## 6. VS Code-inspired workbench-shell profile

The skill will add an optional profile tentatively named:

```text
shellProfile = "fluent-workbench"
```

Proposed structure:

```text
┌──────────────── Integrated Title Bar ────────────────┐
│ menu/app identity | title/command center | actions   │
├───────┬──────────────────────────────────────────────┤
│       │ optional Primary Sidebar │ main content      │
│Activity│                         │                   │
│  Bar   │                         │                   │
│       │                         │                   │
├───────┴──────────────────────────────────────────────┤
│ global/workspace status        contextual status    │
└──────────────────── Status Bar ──────────────────────┘
```

The shell structure is optional. The Fluent token and component guidance remains useful in applications that do not use it.

## 7. Activity Bar decision

### Semantic role

The Activity Bar is for switching among a small set of primary view containers. It is not a vertical collection of unrelated commands.

Each item must:

- map to a persistent or meaningful application view;
- have a clear localized accessible name and tooltip;
- use a coherent icon family;
- expose selected, hover, pressed, keyboard-focus, disabled, and badge states;
- open or focus its associated sidebar/view rather than launch an unrelated modal action.

### Proposed Qt implementation

```text
FluentActivityBar : QFrame
  └─ QVBoxLayout
       ├─ exclusive QButtonGroup of checkable QToolButton items
       ├─ stretch
       └─ secondary/global items
```

Associated content will normally use `QStackedWidget`, a sidebar host, or an application-specific view router.

### Reference metrics, not mandates

Current VS Code source uses:

| Metric | Standard | Compact |
|---|---:|---:|
| Activity Bar width | 48 px | 36 px |
| Item height | 48 px | 32 px |
| Icon size | 24 px | 16 px |

These values will become optional reference tokens. The final Qt metric must still respect font metrics, display scaling, accessibility, and the application’s density profile.

### Activity Bar shell tokens

```text
shellActivityBackground
shellActivityForeground
shellActivityForegroundInactive
shellActivityBorder
shellActivityIndicator
shellActivityIndicatorFocus
shellActivityHoverBackground
shellActivityPressedBackground
shellActivityBadgeBackground
shellActivityBadgeForeground
```

These will resolve from Fluent aliases rather than importing VS Code theme colors verbatim.

## 8. Status Bar decision

### Information architecture

- Left side: global or workspace-wide state.
- Right side: current-view, current-document, or selection-specific state.
- Keep labels short.
- Use icons only where the metaphor is clear.
- Limit the number of persistent items.
- Use warning/error backgrounds only for exceptional, high-priority conditions.
- Background work should use a discreet progress item; elevated progress belongs in a proper notification or progress surface.

### Proposed Qt implementation

A dedicated `FluentStatusBar` composite is preferred over treating `QStatusBar` as an unstructured message area:

```text
FluentStatusBar : QFrame
  └─ QHBoxLayout
       ├─ left/global item host
       ├─ stretch
       └─ right/contextual item host
```

The public API should support stable IDs, alignment, priority, update, removal, visibility, keyboard focus, tooltip, accessible name, and semantic severity.

### Reference metric

Current VS Code source uses a 22 px Status Bar height. The Qt skill will treat 22 px as a compact reference and calculate an effective minimum from the configured font, padding, focus border, and device scale.

### Status Bar shell tokens

```text
shellStatusBackground
shellStatusForeground
shellStatusBorder
shellStatusItemHoverBackground
shellStatusItemPressedBackground
shellStatusItemFocusBorder
shellStatusWarningBackground
shellStatusWarningForeground
shellStatusDangerBackground
shellStatusDangerForeground
shellStatusProgressForeground
```

## 9. Integrated Title Bar decision

The Title Bar is the highest-risk shell component because it interacts with the operating system. The skill will make it optional and platform/version gated.

### Content model

It may contain:

- app identity or compact menu access;
- window/document/workspace title;
- optional command/search center;
- a restrained set of contextual actions;
- native window controls or a platform adapter.

It must support active and inactive-window states.

### Reference metrics

Current VS Code source uses a default 35 px custom Title Bar when its command center is present, and a 30 px variant in a simpler configuration. These are reference values only.

### Preferred implementation path: Qt 6.9+

Where supported:

- request `Qt.ExpandedClientAreaHint`;
- combine it with `Qt.NoTitleBarBackgroundHint`;
- keep native window controls where practical;
- observe `QWindow.safeAreaMargins()` and `safeAreaMarginsChanged`;
- keep interactive controls outside system-reserved regions;
- update active/inactive styling from the window activation state.

### Fallback path

For unsupported Qt versions or platforms:

- retain the native title bar;
- place a Fluent command/title strip directly below it;
- preserve all native movement, resizing, menus, snapping, and window controls.

This fallback is considered successful, not a degraded failure.

### Fully frameless path

A frameless custom window will be opt-in, not the default. It must:

- call `QWindow.startSystemMove()` for dragging;
- call `QWindow.startSystemResize()` for resizing;
- avoid manual geometry dragging as the primary implementation;
- implement double-click maximize/restore;
- expose the system menu where applicable;
- preserve keyboard access;
- test snapping/tiling, multi-monitor movement, scaling changes, maximized margins, and inactive state;
- document platform-specific limitations around native caption buttons and snap-layout behavior.

### Title Bar shell tokens

```text
shellTitleBackgroundActive
shellTitleForegroundActive
shellTitleBackgroundInactive
shellTitleForegroundInactive
shellTitleBorder
shellTitleCommandBackground
shellTitleCommandHoverBackground
shellTitleCommandFocusBorder
```

## 10. Accessibility and system-preference decisions

The skill will require:

- `accessibleName` on every icon-only action;
- concise `accessibleDescription` when the action needs additional context;
- stable `accessibleIdentifier` values where UI automation needs them;
- logical tab order and visible keyboard focus;
- correct selected/checked/expanded state exposure;
- no status conveyed solely by color;
- contrast verification for both light and dark themes;
- active/inactive-window verification for the Title Bar;
- an explicit high-contrast token set.

System integration policy:

- follow `QStyleHints.colorScheme` and its change signal for system light/dark behavior;
- process palette-change events after the system palette updates;
- on Qt 6.10+, inspect `QAccessibilityHints.contrastPreference`;
- provide a manual high-contrast override on older Qt versions;
- respect `QStyleHints.useHoverEffects` where hover is not appropriate;
- do not assume a cross-platform reduced-motion preference is exposed consistently by Qt; provide an application setting and disable nonessential motion in high-contrast/accessibility modes.

## 11. Adopt / translate / reject matrix

| Source pattern | Decision | Reason |
|---|---|---|
| Fluent global and alias tokens | Adopt | Core semantic foundation |
| Official resolved light/dark values | Adopt as source data | Concrete, versioned values |
| Fluent typography and spacing ramps | Adopt with Qt metric handling | Cross-platform design guidance |
| Fluent component state hierarchy | Adopt | Framework-independent interaction semantics |
| React `FluentProvider` | Translate | Replace with a Qt theme manager |
| Griffel `makeStyles` | Translate | Qt uses palette, QSS, style APIs, and delegates |
| React component APIs | Translate conceptually | No runtime React dependency |
| VS Code Activity Bar semantics | Adopt | Strong workbench navigation model |
| VS Code Status Bar grouping | Adopt | Clear global/contextual distinction |
| VS Code exact colors | Reject | Fluent aliases and app branding must govern |
| VS Code reference dimensions | Adopt as optional density references | Useful but not universally accessible |
| Electron/DOM title-bar code | Reject | Wrong runtime and platform layer |
| Qt 6.9 expanded client area | Adopt with capability checks | Best native integration path |
| Manual frameless dragging by setting geometry | Reject as default | Breaks native window-manager behavior |
| Native title-bar fallback | Adopt | Preserves platform behavior and reliability |
| Per-widget inline QSS | Reject by default | Creates inheritance and consistency problems |
| App-wide palette plus generated QSS | Adopt | Matches Qt capabilities |
| `QProxyStyle` for every visual detail | Reject | Too invasive and platform-fragile |
| Narrowly scoped `QProxyStyle` | Adopt | Appropriate for metrics/style hints |

## 12. Revised skill structure resulting from Phase 1

```text
pyside6-fluent-ui/
├── SKILL.md
├── references/
│   ├── fluent-principles.md
│   ├── token-architecture.md
│   ├── qt-styling-architecture.md
│   ├── component-mappings.md
│   ├── interaction-states.md
│   ├── accessibility.md
│   ├── workbench-shell.md
│   ├── activity-bar.md
│   ├── status-bar.md
│   ├── title-bar.md
│   ├── refactor-playbook.md
│   └── validation-checklist.md
├── resources/
│   ├── fluent2-web-light.json
│   ├── fluent2-web-dark.json
│   ├── qt-token-map.json
│   ├── shell-token-map.json
│   └── source-manifest.json
├── templates/
│   ├── fluent_tokens.py
│   ├── fluent_theme.py
│   ├── fluent_palette.py
│   ├── fluent.qss.in
│   ├── fluent_activity_bar.py
│   ├── fluent_status_bar.py
│   └── fluent_title_bar.py
└── scripts/
    ├── extract_official_tokens.py
    ├── generate_qss.py
    ├── audit_qt_styles.py
    ├── check_contrast.py
    └── build_widget_gallery.py
```

## 13. Phase 1 boundaries

This phase intentionally did **not**:

- inspect the user’s application repository;
- assume the application’s current navigation architecture;
- author the final `SKILL.md`;
- refactor any GUI code;
- choose an application-specific brand color;
- copy VS Code icons, product branding, or exact theme colors.

## 14. Remaining work before Phase 2 authoring

The source foundation is sufficient to begin the first generic skill draft. The next phase should:

1. define the skill’s exact invocation triggers and non-negotiable rules;
2. produce the Qt-facing token vocabulary and shell-token map;
3. write the first complete `SKILL.md`;
4. author the Activity Bar, Status Bar, and Title Bar reference contracts;
5. build isolated sample components and a workbench widget gallery;
6. validate the title-bar implementation on supported Qt/platform combinations before recommending it as a default.

## 15. Source index

### Microsoft Fluent 2

- https://fluent2.microsoft.design/design-principles
- https://fluent2.microsoft.design/design-tokens
- https://fluent2.microsoft.design/typography
- https://fluent2.microsoft.design/layout
- https://fluent2.microsoft.design/accessibility
- https://github.com/microsoft/fluentui

### Reviewed community skill

- https://github.com/fefogarcia/approved-skills
- pinned commit: `2da55c784b3341f73c353cda0bc404a19c388fda`

### VS Code workbench guidance and source

- https://code.visualstudio.com/api/ux-guidelines/overview
- https://code.visualstudio.com/api/ux-guidelines/activity-bar
- https://code.visualstudio.com/api/ux-guidelines/status-bar
- https://code.visualstudio.com/api/references/theme-color
- https://github.com/microsoft/vscode
- reviewed source commit: `f390b25a75f0c64c17c8145f286e232838bc3370`

### Qt / PySide

- https://doc.qt.io/qtforpython-6/overviews/qtwidgets-stylesheet.html
- https://doc.qt.io/qtforpython-6/PySide6/QtGui/QPalette.html
- https://doc.qt.io/qtforpython-6/PySide6/QtWidgets/QProxyStyle.html
- https://doc.qt.io/qtforpython-6/PySide6/QtGui/QStyleHints.html
- https://doc.qt.io/qtforpython-6/PySide6/QtGui/QAccessibilityHints.html
- https://doc.qt.io/qtforpython-6/PySide6/QtWidgets/QWidget.html
- https://doc.qt.io/qtforpython-6/PySide6/QtGui/QWindow.html
- https://doc.qt.io/qt-6/qt.html#WindowType-enum
