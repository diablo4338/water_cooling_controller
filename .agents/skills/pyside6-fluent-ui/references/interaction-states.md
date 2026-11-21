# Interaction-state contract

Every interactive component has a state contract. Styling a rest state is not a completed component.

The machine-readable matrix is:

```text
resources/component-state-matrix.json
```

## State definitions

### Rest

Default enabled state. Text, icon, background, border, and affordance are legible without interaction.

### Hover

Pointer is over the interactive target and the platform permits hover effects. Hover:

- must not be the only way to discover a core action;
- should increase clarity without moving layout;
- should use semantic hover aliases;
- may be suppressed when `QStyleHints.useHoverEffects()` is false.

### Pressed

Pointer/key activation is in progress. Pressed should be distinct from hover and should not make content unreadable.

### Focus

Keyboard or accessibility focus. Requirements:

- visible without hover;
- contrast against surrounding surface;
- follows logical tab order;
- not removed to achieve a clean screenshot;
- separate from selected/check state.

Qt QSS may not reproduce Fluent’s dual focus stroke exactly. Preserve a strong visible stroke or use a narrow proxy-style/focus overlay.

### Selected or checked

Persistent state after activation. Examples: active navigation destination, checked toggle, selected data row.

- do not rely only on subtle color when multiple items are adjacent;
- retain readable text/icons;
- expose Qt checked/selected state;
- selected item may use a brand indicator plus subtle background.

### Disabled

Not currently actionable.

- cannot activate through mouse, keyboard, shortcut, or programmatic UI path;
- remains legible;
- tooltip/accessibility may explain why when useful;
- do not use opacity so low that text disappears;
- do not confuse disabled with read-only.

### Invalid

Current value fails validation.

- use semantic danger foreground/border;
- provide a nearby message;
- update accessible description or error relation;
- preserve the entered value unless domain rules require correction;
- focus should remain visible over the invalid state.

### Read-only

Value can be inspected/copied but not edited.

- preserve text contrast;
- preserve selection/copy where appropriate;
- distinguish from disabled;
- avoid presenting an ordinary editable affordance if editing is impossible.

### Busy

An action or region is processing.

- prevent duplicate invocation;
- expose progress or status text;
- preserve cancellation if supported;
- do not freeze the GUI thread;
- choose local versus global progress according to scope.

## Additional shell states

### Active/inactive window

Custom Title Bar and shell surfaces need distinct active/inactive treatment. Inactive does not mean disabled; reduce emphasis while retaining legibility.

### Badge state

Activity badges communicate count or urgent state. Keep them short, accessible, and updated atomically with underlying state.

### Severity

Status and Message Bar severity:

```text
info
success
warning
danger
```

Severity colors are semantic, not arbitrary customization.

## Component matrix summary

| Component | Required states |
|---|---|
| Push/tool button | rest, hover, pressed, focus, disabled; checked/busy when applicable |
| Line edit | rest, hover, focus, disabled, invalid, read-only |
| Combo/spin | rest, hover, pressed, focus, disabled, invalid, read-only |
| Checkbox/radio/switch | rest, hover, pressed, focus, checked, disabled |
| Navigation/Activity item | rest, hover, pressed, focus, selected, disabled |
| Status item | rest, hover, pressed, focus, disabled; busy/severity when applicable |
| Table/list row | rest, hover, focus, selected, disabled; invalid/busy when applicable |
| Interactive card | rest, hover, pressed, focus, selected, disabled |
| Title Bar | active window, inactive window, focus within |

## State precedence

When states overlap, use this conceptual precedence:

```text
disabled
  > invalid/busy semantic constraints
  > pressed
  > focus + selected/checked (both may be visible)
  > selected/checked
  > hover
  > rest
```

Focus should often remain additive rather than replacing selected/invalid visuals.

## Dynamic property refresh

When changing semantic properties:

```python
set_fluent_property(widget, "fluentInvalid", True)
set_fluent_property(widget, "fluentBusy", True)
```

Repolish only that widget. For model/view row state, use model roles and emit `dataChanged`; do not assign dynamic properties to recycled viewport paint operations.

## Testing states

For each component:

1. tab to it;
2. activate with keyboard;
3. activate with pointer;
4. toggle/select if applicable;
5. disable and verify all activation paths;
6. trigger invalid/read-only/busy states;
7. switch light/dark/high contrast;
8. inspect at target display scales;
9. verify screen-reader name/state where applicable.

The widget gallery should expose controls for forcing each state without relying on hidden application conditions.
