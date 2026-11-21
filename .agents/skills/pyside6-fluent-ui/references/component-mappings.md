# Fluent component mappings for Qt Widgets

Use this reference before introducing a custom widget. The goal is semantic and behavioral parity, not API-name parity.

## Actions

### Button → `QPushButton`

Appearances:

| Fluent intent | Property | Use |
|---|---|---|
| Primary | `fluentAppearance="primary"` | one main action per local group |
| Secondary | `secondary` | supporting action |
| Outline | `outline` | medium-emphasis alternative |
| Subtle | `subtle` | low-emphasis inline action |
| Transparent | `transparent` | shell/navigation or minimal chrome |
| Danger | `danger` | destructive action after clear labeling |

Rules:

- default action uses `setDefault(True)` only when Enter should activate it;
- destructive actions are not made default accidentally;
- icon-only buttons use `QToolButton`, not an empty-text push button;
- busy buttons disable duplicate activation and expose progress/status;
- minimum widths are contextual; do not force every button to 96 px.

### Icon command → `QToolButton`

Use for toolbars, Activity Bar, Title Bar, and compact actions.

- set accessible name and tooltip;
- use regular icon at rest and filled/selected variant where available;
- use checkable state for persistent toggles or selected destinations;
- preserve keyboard focus;
- use 32 px medium command target by default, larger when touch is primary.

### Split/compound actions

Qt has no direct universal Fluent SplitButton. Options:

- primary `QPushButton` plus adjacent `QToolButton` menu trigger;
- `QToolButton` with `MenuButtonPopup` when the primary action remains clear;
- small composite widget with one focus model and accessible names.

Avoid ambiguous dropdown arrows on actions that do not have a stable default behavior.

## Inputs

### Input → `QLineEdit`

- label it externally or through a form-row label;
- use placeholder as a hint, not a label replacement;
- validation message appears near the field and in accessible description;
- support read-only separately from disabled;
- use clear/search icons only when the action is understood;
- preserve standard text-edit shortcuts and context menu.

### Textarea → `QPlainTextEdit` or `QTextEdit`

Use `QPlainTextEdit` for plain text and logs; `QTextEdit` for rich text. Do not choose `QTextEdit` solely for multiline behavior.

### Select/Dropdown → `QComboBox`

- use non-editable mode for fixed options;
- use editable mode for freeform/search-like selection;
- avoid styling away the native arrow without replacing its affordance;
- define popup item selection, disabled items, and keyboard behavior;
- use a model for rich or grouped options.

### SpinButton → `QSpinBox` / `QDoubleSpinBox`

- keep native keyboard and wheel semantics unless project rules require otherwise;
- specify units with prefix/suffix where clear;
- avoid hiding step controls if users rely on them;
- validate range and special values accessibly.
- use one 24 px trailing column with stacked up/down buttons; use 16 px rows at standard density and 12 px rows at compact density;
- position button subcontrols from the padding box so hover/pressed fills stay inside the outer frame and focus border;
- use 16 px Fluent System Icon chevrons tinted from semantic foreground aliases, including disabled and high-contrast output;
- define rest, hover, pressed, disabled/off, invalid, read-only, focus, and compact selectors together rather than mixing a styled frame with native steppers.
- target read-only subcontrols with `[readOnly="true"]::up-button`/`::down-button`; parent-state forms such as `:read-only::down-arrow` can leak the QSS `image` onto the whole spin box on Qt's Windows style.

### Search box → `QLineEdit` composite

A search field may include:

- search icon;
- clear action;
- optional result count/status;
- debounce in controller logic, not paint/style code.

Do not overload the Title Bar command center with every filtering task; page-local search remains in the page or Sidebar.

## Selection controls

### Checkbox → `QCheckBox`

- preserve checked, unchecked, and partial state;
- use partial state only for genuine mixed selection;
- label is clickable;
- invalid state includes a message;
- do not replace the indicator with a decorative icon lacking state exposure.

### Radio group → `QRadioButton` + `QButtonGroup`

- use for one-of-many choices visible together;
- provide a group label;
- maintain arrow-key navigation and exclusive selection;
- do not use radio buttons as navigation tabs.

### Switch → styled `QCheckBox` or narrow custom composite

Use for an immediate binary setting. If a custom switch is necessary:

- base it on an accessible checkable control where possible;
- expose checked state;
- preserve Space activation;
- show on/off text where ambiguity exists;
- implement focus, disabled, hover, and high contrast.

## Layout and surfaces

### Card → semantic `QFrame`

Set `fluentRole="card"`.

Use cards for:

- a self-contained object or choice;
- a clear preview/action unit;
- content that benefits from a distinct surface.

Do not use cards as a default substitute for layout groups. Prefer margins, headings, and dividers.

### Divider → `QFrame`

Use HLine/VLine or a semantic frame. Keep dividers subtle. A labeled divider is usually a layout containing lines and a label, not custom painting.

### Accordion → checkable header + content widget

Use a `QToolButton` or header composite controlling content visibility. Expose expanded/collapsed state and preserve focus.

### Drawer → choose by behavior

- persistent resizable side content: `QSplitter` pane;
- dockable tool: `QDockWidget`;
- temporary modal side surface: overlay composite;
- small contextual content: popup or dialog instead.

Do not animate a permanent Sidebar as if it were a mobile drawer unless the window width demands it.

## Navigation

### Tab list → `QTabBar` + `QStackedWidget`

Use for peer views within one context. Avoid too many tabs. Preserve close/reorder behavior if the application already has it.

### Primary navigation → `QListView`, `QTreeView`, or button group

Use a model/view control for dynamic, long, hierarchical, or data-driven navigation. Use a checkable button group for a small fixed set.

### Activity Bar → `FluentActivityBar`

Use for a small set of primary view containers. Read `activity-bar.md`.

### Breadcrumb → small action/label composite

Use for hierarchy and navigation, not as decorative page title. Ensure truncated segments remain discoverable.

## Data display

### Table/DataGrid → `QTableView`

Preserve model/view architecture.

Implement:

- header hierarchy and sort indicator;
- hover and selection;
- keyboard row/cell focus;
- alternate rows only when useful;
- empty/loading/error states outside the paint loop;
- delegates for badges, progress, icons, and rich cells;
- column sizing that remains usable at minimum window width.

Do not use `QTableWidget` for production data merely because the gallery uses it for a compact example.

### Tree → `QTreeView`

- preserve expansion state;
- make disclosure indicators visible in all themes;
- use selection distinct from keyboard focus;
- avoid deeply nested padding that consumes content width;
- ensure context menus and inline rename remain accessible.

### List → `QListView`

Use delegates for rich list items. Avoid nesting interactive child widgets in thousands of rows unless virtualization and focus behavior are intentionally handled.

### Badge/Tag → `QLabel` or small composite

Set semantic role/severity. Keep text short. Tags representing removable filters need a dedicated remove action and accessible label.

### Progress → `QProgressBar` or busy indicator

- use determinate progress when the amount is known;
- use indeterminate state when unknown;
- include text or accessible status;
- do not use a spinner for long-running work without context or cancellation where appropriate.

## Feedback

### Message Bar → reusable `QFrame` composite

Structure:

```text
icon | title/message | optional actions | dismiss
```

Set `fluentRole="messageBar"` and `fluentSeverity`. It should participate in layout rather than float arbitrarily over content.

### Toast → managed transient overlay

- use for brief confirmation or nonblocking status;
- provide timeout appropriate to content;
- keep actionable notifications visible until acted upon/dismissed;
- announce accessibly;
- do not use toast for validation that belongs next to a field.

### Tooltip → `QToolTip` or approved rich tooltip

- icon-only commands require a tooltip;
- tooltip does not replace accessible name;
- keep content concise;
- do not hide essential instructions only in hover UI.

## Overlays

### Dialog → `QDialog`

- clear title and purpose;
- default and cancel behavior explicit;
- destructive action visually and semantically clear;
- focus starts at the appropriate control;
- focus returns to trigger on close;
- support Escape where safe;
- avoid replacing a simple confirmation with a large custom modal.

### Popover → popup widget or menu according to behavior

Use `QMenu` for commands and selections that fit menu semantics. Use a popup widget for richer transient content. Manage focus and dismissal explicitly.

### Menu → `QMenu`

Preserve native keyboard navigation, mnemonics, shortcuts, separators, and submenus. Style conservatively.

## Shell components

| Component | Qt implementation |
|---|---|
| Window frame | `QFrame[fluentRole="windowFrame"]` plus `FluentWindowFrameController`; Windows 11 uses DWM border color and other environments use the generated-QSS client fallback |
| Title Bar | `FluentTitleBar` plus frameless controller; on Windows 11 add `WindowsSnapLayoutWindowMixin`/`WindowsSnapLayoutAdapter`; otherwise use expanded integration or native fallback |
| Activity Bar | `QFrame` + exclusive checkable `QToolButton` group |
| Primary Sidebar | `QStackedWidget` or model/view container in a splitter pane |
| Main content | existing central widget/page stack |
| Panel | splitter pane or `QDockWidget` depending behavior |
| Status Bar | dedicated `FluentStatusBar` composite or carefully structured `QStatusBar` |

## Custom widget acceptance test

Before creating a custom control, document:

1. why no standard widget/composite works;
2. input and keyboard behavior;
3. accessible role, name, value, and state;
4. all visual states;
5. high-DPI and theme behavior;
6. focus and tab order;
7. test strategy;
8. performance implications.
