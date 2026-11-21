# Fluent principles translated for Qt Widgets

Use this reference when deciding **what the interface should feel like**, not merely which color to assign.

## Governing idea

A PySide6 Fluent application should look deliberate and coherent without fighting Qt or the host operating system. Microsoft’s Fluent 2 principles favor platform familiarity, focus, inclusion, and recognizable design signatures. In practice:

- reuse native Qt controls and behaviors for ordinary interactions;
- spend customization effort on the application’s signature workflows and shell;
- reduce chrome that competes with domain content;
- make hierarchy visible through spacing, typography, grouping, and state;
- preserve keyboard, accessibility, and window-manager expectations.

## Natural on the platform

Prefer familiar desktop patterns:

- menu, context menu, shortcuts, dialogs, focus, scrolling, selection, drag-and-drop, and window behavior should work as users expect;
- do not replace standard controls merely because a screenshot of Fluent React looks different;
- use platform/system fonts when the preferred Fluent font is unavailable;
- adapt density and layout to window size and input mode;
- keep the native title bar unless an integrated path preserves system behavior.

A Fluent result is not measured by how many native details were removed.

## Built for focus

The application’s content and primary workflow take precedence over decoration.

Do:

- use one clear primary action per local action group;
- distinguish primary, secondary, subtle, and destructive actions;
- use whitespace and alignment before borders;
- use a small number of stable surface levels;
- reveal secondary commands contextually where practical;
- keep animation short and purposeful.

Avoid:

- gradients, glows, and heavy shadows as default decoration;
- a border or card around every group;
- excessive brand color;
- persistent high-emphasis buttons for low-priority actions;
- using the Status Bar as an alert feed;
- large headers that consume working space in dense desktop tools.

## Inclusive by construction

Accessibility is part of the component contract, not a review at the end.

- Every icon-only action has an accessible name.
- Focus is visible and follows a logical order.
- State is not communicated by color alone.
- Text remains readable in disabled and inactive states.
- Light, dark, and high-contrast behavior is designed together.
- Motion can be reduced or disabled.
- Compact shell elements remain operable with keyboard and display scaling.

## Coherent, not identical

Use the same semantic roles across the application while allowing component-specific behavior.

Examples:

- `brand_background` always represents high-emphasis brand action or state;
- `text_secondary` always means reduced emphasis, but a table may use it differently from a Title Bar;
- `surface_secondary` can back a Sidebar or grouped settings section;
- `danger_foreground` means destructive/error semantics, never a decorative accent.

Do not force all controls to share identical padding or radius if their function, density, or platform behavior differs.

## Visual hierarchy

Establish hierarchy in this order:

1. placement and grouping;
2. spacing;
3. typography role and weight;
4. surface contrast;
5. borders and dividers;
6. color emphasis;
7. elevation and motion.

If a layout only works after adding many borders, revisit grouping and spacing first.

## Fluent color use

- Neutral colors carry most surfaces, text, and layout structure.
- Brand color identifies primary actions, selected destinations, links, and important progress.
- Status colors are semantic: success, warning, danger, information.
- Hover and pressed states should be visible without abrupt jumps.
- Focus uses a clear stroke; it is not merely another hover color.
- Strong brand or status surfaces use an appropriate on-color foreground.

## Typography

Use semantic roles rather than arbitrary size and weight changes:

```text
caption2
caption1
body
body_strong
body2
subtitle2
subtitle1
section_title
page_title
large_title
monospace
numeric
```

Guidance:

- sentence case by default;
- body text is usually regular weight;
- labels and concise headings may use semibold;
- bold is rare;
- numeric and tabular data may use the numeric family or tabular alignment;
- use `QFontMetrics` to prevent clipping in compact bars and controls.

## Spacing and density

Use the Fluent spacing ramp but assign values by relationship:

- 2–6 px: micro-alignment, icon/text adjustment, compact indicator gaps;
- 8–12 px: related controls and field internals;
- 16–20 px: local groups and card padding;
- 24–32 px: sections and page margins.

Desktop density profiles:

- **compact:** dense engineering/admin workflows; smaller shell and control metrics;
- **standard:** default desktop work;
- **comfortable:** touch-supporting or presentation-oriented layouts.

Compact does not mean inaccessible. Maintain focus, hit target, and text clarity.

## Motion

Use motion to explain change, not decorate it.

Good uses:

- panel expansion/collapse;
- view transition where spatial continuity matters;
- toast entrance and exit;
- progress transition;
- selection indicator movement.

Avoid animation for:

- routine text updates;
- every hover;
- high-frequency data refresh;
- operations where motion delays interaction.

Use official duration/easing tokens and provide a reduced-motion path.

## Workbench-shell judgment

The VS Code-inspired shell is appropriate when the application has several persistent workspaces or tools that users switch among frequently. It is not required for a simple form or single-purpose dialog.

Use an Activity Bar when destinations are primary and persistent. Use a Sidebar for the selected destination’s navigation or tools. Use the Status Bar for terse global/contextual state. Use the Title Bar for identity, title, command/search, and a restrained set of shell actions.

## Decision test

Before adding a visual element, ask:

1. What user need or hierarchy does it communicate?
2. Which semantic token describes that intent?
3. Is a native Qt pattern already sufficient?
4. Does it have complete keyboard and state behavior?
5. Does it remain clear in light, dark, high contrast, and inactive-window states?
6. Would removing it make the interface calmer without losing meaning?
