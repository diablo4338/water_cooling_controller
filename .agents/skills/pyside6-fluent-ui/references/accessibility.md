# Accessibility requirements for PySide6 Fluent UI

## Baseline

Accessibility is part of the implementation contract for every component and shell region.

Minimum visual targets:

- normal text contrast: 4.5:1;
- large text: 3:1;
- essential non-text controls, boundaries, and focus: 3:1;
- no critical information communicated only by color.

Verify actual resolved values and backgrounds; token naming alone does not prove contrast in a custom composition.

## Accessible identity

For icon-only and custom shell controls:

```python
button.setAccessibleName("Search")
button.setToolTip("Search")
button.setAccessibleDescription("Open the search workspace")
```

Keep names concise and action-oriented. Tooltip text and accessible name may match, but one does not replace the other.

Use stable accessibility/automation identifiers where supported by the project and PySide6 version. Preserve existing `objectName` values used by tests unless explicitly migrating them.

## Standard widgets first

Standard Qt controls already expose much of their role and state. Prefer them over painted `QWidget` replacements.

When composing a custom widget:

- make the actual interactive child focusable;
- expose name and description on that child;
- avoid multiple confusing focus stops inside one conceptual control;
- expose value/checked/expanded state through standard APIs;
- use `QAccessible` interfaces/events only when standard widgets cannot represent the component.

## Keyboard

Requirements:

- logical tab order follows visual reading order;
- all actions reachable without pointer;
- Space/Enter behavior follows control convention;
- arrow navigation works in radio groups, lists, tabs, and Activity Bar where appropriate;
- Escape dismisses transient surfaces where safe;
- shortcuts remain visible/discoverable in menus or tooltips where appropriate;
- focus does not become trapped in a Sidebar, Title Bar, or popup;
- focus returns to the trigger after a dialog or transient overlay closes.

Do not add every passive Status Bar label to the tab order. Only actionable entries need focus; provide a coherent order among them.

## Focus treatment

- visible in light and dark themes;
- visible over selected/invalid states;
- not dependent on hover;
- no layout movement when focus appears;
- consistent across buttons, inputs, navigation, item views, and shell controls.

If QSS focus borders cause size changes, use constant border geometry, a proxy-style focus indicator, or an overlay.

## High contrast

Where the Qt version supports it, inspect:

```text
QGuiApplication.styleHints().accessibility().contrastPreference()
```

Qt styles may adjust palettes and outlines using this preference. The application should:

- derive critical aliases from the effective system palette;
- minimize decorative colors;
- retain visible borders and focus;
- use system Highlight/HighlightedText for strong selection;
- test with real operating-system contrast themes;
- provide a manual override on platforms/versions that do not expose the preference.

Do not assume dark mode is high contrast.

## System light/dark

Follow:

```text
QGuiApplication.styleHints().colorScheme()
colorSchemeChanged
```

When the system scheme changes, Qt may update its default palette around the signal. Refresh application-specific tokens after the effective palette changes. Handle palette-change events where needed.

## Hover and input mode

Check `QStyleHints.useHoverEffects()` before making hover animation or hover-only affordances essential. Touch-capable workflows need adequate targets and cannot depend on a precise pointer.

For touch as a primary input, use approximately 44 px minimum targets or a project-approved accessible target based on platform guidance.

## Text scaling and clipping

- use layouts and size hints;
- check `QFontMetrics` before enforcing compact heights;
- permit labels to elide or wrap according to context;
- keep full text discoverable through tooltip/accessibility where elided;
- verify common display scales and any supported app font scaling;
- do not set fixed heights that clip a larger system font.

## Reduced motion

Qt does not expose one uniformly reliable cross-platform reduced-motion signal for every target. Provide an application setting such as:

```text
motion = system | full | reduced | none
```

In reduced mode:

- remove decorative transitions;
- shorten or remove large spatial movement;
- keep necessary progress feedback;
- avoid flashing or rapid repeated animation.

High-contrast/accessibility profiles may default to reduced motion.

## Status and validation announcements

Dynamic status should be exposed in a way assistive technology can discover. For custom status composites, update accessible descriptions and emit appropriate accessibility events if standard widget updates do not announce changes.

Field validation:

- label remains associated with field;
- message is visible near the field;
- error is included in accessible description or relation;
- focus is moved only when necessary and predictable;
- summary links to invalid fields for long forms where useful.

## Title Bar accessibility

- Title Bar actions have names and normal focus behavior;
- drag region excludes interactive controls;
- native caption buttons remain usable where retained;
- custom caption buttons, if ever used, expose standard minimize/maximize/close semantics;
- active/inactive styling does not reduce text below acceptable contrast;
- system menu and keyboard window management remain available on target platforms.

## Activity Bar accessibility

- one selected destination is exposed as checked/selected;
- labels are available through tooltip and accessible name;
- arrow or tab navigation is predictable;
- badge count is included in accessible description, not read as unlabeled decoration;
- reordering, if supported, has an accessible alternative.

## Status Bar accessibility

- passive text is concise and not needlessly focusable;
- actionable items have names and keyboard focus;
- severity includes text/icon meaning;
- rapidly updating values are throttled to avoid noisy announcements;
- Status Bar is not the sole place where critical errors are communicated.

## Verification

Test at least:

- keyboard-only workflow;
- high contrast/system contrast where supported;
- 100%, 125%, 150%, and 200% display scaling on relevant platforms;
- screen reader on the primary target OS;
- light/dark active and inactive window;
- minimum supported window size;
- icon-only actions and every custom composite.
