# Status Bar

The Status Bar presents compact, continuously useful state. It should not become a second toolbar or notification history.

## Information architecture

### Left/global side

Use for state that applies broadly to the application or workspace:

- current project/workspace;
- connection or synchronization state;
- background task summary;
- source or environment mode;
- global validation count;
- persistent application mode.

### Right/contextual side

Use for the current page, document, selection, or tool:

- selection count;
- cursor/location or measurement mode;
- zoom/scale;
- current units;
- active renderer/backend;
- page-specific toggle when compact and highly relevant.

When an item changes scope, move it to the correct side rather than preserving historical placement.

## Proposed Qt API

A dedicated `FluentStatusBar` composite is preferred when stable items and priorities matter:

```python
add_entry(
    id,
    text,
    *,
    alignment="left",       # left/global or right/contextual
    priority=0,
    icon=None,
    tooltip=None,
    accessible_name=None,
    callback=None,
    severity="normal",      # normal, info, success, warning, danger
)
update_entry(id, **changes)
remove_entry(id)
set_entry_visible(id, visible)
set_profile("brand" | "neutral")
```

Use stable IDs for updates and persistence. Do not search for entries by visible text.

A standard `QStatusBar` may be used when the existing application already relies on it, provided the same semantic API and state rules are maintained.

## Layout and priority

- Within each side, sort by priority, then stable insertion order.
- Keep a small spacing between related items and a divider only between clear groups.
- Low-priority items may collapse to icon-only or overflow at narrow widths.
- Essential warnings and progress must not disappear silently.
- Avoid horizontally expanding labels that push critical state offscreen.

Use `QSizePolicy` and elision rather than fixed widths for ordinary text.

## Interaction

Passive entries:

- are not focusable;
- may have a tooltip and accessible description;
- do not change appearance on hover.

Actionable entries:

- are real buttons or expose button semantics;
- have hover, pressed, focus, and disabled states;
- provide a clear tooltip and accessible name;
- use Enter/Space activation;
- remain concise enough to fit the bar.

Do not make a passive `QLabel` clickable through an opaque mouse handler; use a `QToolButton` or a semantic wrapper.

## Progress

For background progress:

- show a discreet spinner/progress icon plus concise text;
- allow cancellation only when the task supports it safely;
- update at a reasonable cadence to avoid excessive layout and paint work;
- announce meaningful transitions, not every percentage tick;
- move detailed progress and errors to a proper panel, dialog, or notification surface.

## Severity

- `normal`: regular shell background;
- `info`: normally a small icon/text treatment, not a full blue bar;
- `success`: transient or contextual, not a permanent green shell;
- `warning`: use when action or attention is required;
- `danger`: reserve for exceptional blocking/error conditions.

A whole-bar warning/danger override should be rare. Pair color with an icon and text. Restore the prior profile deterministically when the condition clears.

## Reference metrics

VS Code's Status Bar reference height is 22 px. In Qt, calculate:

```text
max(reference_height,
    font_height + vertical_padding*2,
    icon_size + vertical_padding*2,
    focus_border_requirement)
```

A typical standard profile may therefore be 24–28 px. Do not force 22 px if it clips text or focus at the target scale.

## Tokens

Use aliases from `resources/shell-token-map.json`:

```text
shell_status_background
shell_status_foreground
shell_status_border
shell_status_hover
shell_status_pressed
shell_status_focus
shell_status_warning_background
shell_status_warning_foreground
shell_status_danger_background
shell_status_danger_foreground
shell_status_progress
```

The map may include brand and neutral profiles. Pick one profile consistently; do not mix random per-entry backgrounds.

## Accessibility

- actionable entries get accessible names;
- passive status changes that matter should use an appropriate Qt accessibility event or a dedicated live-status mechanism in the application;
- avoid announcing rapid progress updates;
- status is never color-only;
- keyboard focus order follows visual order within each side;
- focus can leave the Status Bar without cycling indefinitely.

## Anti-patterns

- paragraphs of text;
- every page action duplicated in the Status Bar;
- ambiguous icon-only status with no tooltip;
- warning/danger colors for routine information;
- animated progress that never stops;
- fixed item widths that truncate translated labels;
- putting current-selection state on the global side;
- making every status item focusable.
