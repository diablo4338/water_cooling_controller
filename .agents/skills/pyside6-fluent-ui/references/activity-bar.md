# Activity Bar

The Activity Bar is a compact vertical navigator for **primary view containers**. It is not a general command toolbar.

## Semantics

An Activity Bar item should answer: "Which major workspace am I in?"

Good destinations:

- Explorer / project structure;
- Search;
- Results or analysis;
- source-control-like history;
- extensions/plugins when central to the product;
- application-wide settings or account destination at the bottom.

Poor destinations:

- Save, Export, Delete, Undo, or Run commands;
- an action that merely opens a transient dialog;
- every page in a long navigation tree;
- duplicate access to a command already visible in the current page.

Keep the primary set small enough to remain recognizable without labels. Use a Sidebar or secondary navigation for deeper hierarchy.

## Qt implementation contract

Preferred building blocks:

```text
FluentActivityBar : QFrame
├─ QVBoxLayout(margins=0, spacing=0)
├─ exclusive QButtonGroup
├─ primary checkable QToolButton items
├─ stretch
└─ optional global checkable/action QToolButton items
```

A `QListView` with a model/delegate is also appropriate when items are dynamic, reordered, or numerous. Choose one architecture and retain stable IDs independent of labels.

## Required item API

Each destination needs:

- stable `id`;
- localized text/accessible name;
- tooltip;
- SVG icon;
- checked/selected state;
- enabled state;
- optional integer badge or semantic alert marker;
- activation signal containing the stable ID;
- optional shortcut metadata.

Suggested public methods:

```python
add_destination(id, text, icon, *, position="primary", badge=None)
remove_destination(id)
set_current(id)
current_id()
set_badge(id, value_or_none)
set_destination_enabled(id, enabled)
```

Do not expose child buttons as the primary API unless the application specifically needs them.

## Interaction states

Required:

- rest;
- hover, when system hover effects are enabled;
- pressed;
- keyboard focus;
- selected/check state;
- disabled;
- optional badge and busy state.

Selection should be evident without relying only on color. Use a combination such as:

- narrow brand indicator on the leading edge;
- foreground/icon emphasis;
- subtle selected background;
- accessible checked state.

Do not use a large saturated tile for every selected item; the shell should remain calm.

## Keyboard behavior

- Tab enters/leaves the Activity Bar as a group according to the chosen widget architecture.
- Up/Down moves among enabled items.
- Home/End moves to first/last enabled item.
- Enter or Space activates the focused item.
- Focus and selection may differ temporarily; activation updates selection.
- Avoid looping if it conflicts with the rest of the shell; document the choice.

A `QButtonGroup` does not automatically implement all arrow behavior. Add an event filter or subclass if needed.

## Badges

Use badges for:

- a bounded count;
- an unread/attention indicator;
- a concise severity marker.

Rules:

- display `99+` or another documented cap rather than expanding the bar;
- provide the meaning in the accessible description and tooltip;
- do not communicate urgency only through red;
- hide zero unless zero itself is meaningful;
- avoid animation except a short, nonrepeating transition.

A badge can be a child `QLabel` overlay, a custom `QToolButton` paint layer, or a delegate element. If custom-painted, verify high DPI and accessibility separately.

## Reference metrics

VS Code currently uses these reference dimensions:

```text
standard: width 48, item 48, icon 24
compact:  width 36, item 32, icon 16
```

The skill treats them as density references, not requirements. Effective metrics must fit:

- configured icon size;
- 2 px focus treatment without clipping;
- badge geometry;
- target platform scaling;
- accessible pointer target expectations.

## Token roles

Use aliases from `resources/shell-token-map.json`:

```text
shell_activity_background
shell_activity_foreground
shell_activity_foreground_inactive
shell_activity_border
shell_activity_indicator
shell_activity_indicator_focus
shell_activity_hover
shell_activity_pressed
shell_activity_selected
shell_activity_badge_background
shell_activity_badge_foreground
```

Do not copy VS Code theme colors. The shell derives from the selected Fluent theme and application brand.

## Sidebar relationship

When an Activity item controls a Sidebar:

- selecting a new item shows its view container;
- activating the already-selected item may toggle Sidebar visibility only if documented and discoverable;
- hiding the Sidebar must not clear the selected destination;
- when the selected view is removed, choose a deterministic fallback;
- restore focus sensibly after showing/hiding.

## Accessibility

- call `setAccessibleName()` for every icon-only button;
- the tooltip may match the accessible name but does not replace it;
- expose checked state through a checkable standard control where possible;
- include badge meaning in accessible description;
- ensure indicator and focus contrast reach non-text UI targets;
- preserve logical reading order even when global items are visually at the bottom.

## Anti-patterns

- using Unicode characters or emoji as icons;
- making every item an unrelated command;
- labels rotated vertically;
- selection shown only by a 1 px low-contrast color line;
- hover-only discoverability;
- fixed 48 px metrics that clip at high DPI;
- a badge with no accessible meaning;
- changing application routes before the destination is ready, leaving blank content.
