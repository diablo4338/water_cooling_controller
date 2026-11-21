# Validation checklist

Use this checklist for isolated gallery validation and for every migrated application scope.

## Provenance and tokens

- [ ] Official source package/version/extraction date are recorded.
- [ ] Every referenced upstream token exists in both light and dark snapshots.
- [ ] Application code consumes semantic aliases, not resolved hex values.
- [ ] Derived Qt metrics are labeled as derived rather than official Fluent values.
- [ ] Brand customization preserves the semantic alias layer.
- [ ] No secret, local-machine, or user-specific paths are embedded.

## Static styling architecture

- [ ] One application theme manager owns palette and QSS application.
- [ ] No new scattered per-widget `setStyleSheet()` calls.
- [ ] QSS placeholders resolve completely.
- [ ] Dynamic property vocabulary is stable and documented.
- [ ] Repolish is performed only when properties requiring it change.
- [ ] `QProxyStyle` overrides are narrow and tested.
- [ ] Custom painting has an explicit state and accessibility contract.

## Component states

For every applicable control:

- [ ] rest;
- [ ] hover or documented no-hover behavior;
- [ ] pressed;
- [ ] keyboard focus;
- [ ] selected/checked/current;
- [ ] disabled;
- [ ] invalid;
- [ ] read-only;
- [ ] busy/loading.

- [ ] Focus and selection are distinguishable where both can occur.
- [ ] Focus treatment does not shift layout.
- [ ] Disabled state remains legible and cannot activate.
- [ ] Busy actions prevent duplicate execution.

## Typography and layout

- [ ] Text uses semantic type roles.
- [ ] Default font follows platform policy.
- [ ] Long and translated labels do not clip.
- [ ] Layouts and size policies replace ordinary fixed geometry.
- [ ] Spacing comes from semantic/official ramps.
- [ ] Minimum window size remains usable.
- [ ] No decorative card inflation.
- [ ] Icons are SVG/high-DPI safe and from one approved family.

## Theme and contrast

- [ ] light theme;
- [ ] dark theme;
- [ ] system-following theme changes;
- [ ] high-contrast path or documented fallback;
- [ ] active and inactive window states;
- [ ] normal text contrast target 4.5:1;
- [ ] large text target 3:1;
- [ ] essential non-text/focus boundaries target 3:1;
- [ ] status is not color-only.

## Keyboard and accessibility

- [ ] All actions reachable by keyboard.
- [ ] Tab order follows visual reading order.
- [ ] Arrow navigation works where conventional.
- [ ] Escape closes transient UI safely.
- [ ] Focus returns to trigger after dialog/popover closure.
- [ ] Icon-only controls have accessible names and tooltips.
- [ ] Custom composites expose meaningful standard widget roles/states.
- [ ] Passive Status Bar entries are not needless focus stops.
- [ ] Badge/status meaning appears in accessible description.

## Item views

- [ ] Model and selection-model ownership preserved.
- [ ] Current, selected, hover, focus, and disabled rows are coherent.
- [ ] Delegate painting respects palette/theme and high DPI.
- [ ] Keyboard selection/navigation still works.
- [ ] Empty/loading/error states are represented.
- [ ] Large-data performance remains acceptable.

## Workbench shell

### Activity Bar

- [ ] Items are primary destinations, not arbitrary commands.
- [ ] Selection and visible Sidebar agree.
- [ ] Up/Down/Home/End and activation behavior work.
- [ ] Badges are bounded and accessible.
- [ ] Standard/compact metrics do not clip at target scales.

### Status Bar

- [ ] Global items are left; contextual items are right.
- [ ] Stable IDs and priorities drive updates/order.
- [ ] Passive/actionable semantics differ correctly.
- [ ] Warning/danger colors are exceptional and labeled.
- [ ] Narrow-window overflow preserves essential state.

### Title Bar

- [ ] Chosen mode is documented: expanded, native fallback, or frameless.
- [ ] A one-row requirement uses frameless mode; expanded mode is not misrepresented as removing OS caption content.
- [ ] Menus use a real `QMenuBar` with Alt mnemonics and keyboard navigation.
- [ ] Native move/resize/snap/tiling behavior is preserved.
- [ ] Safe-area/native-control overlap is prevented.
- [ ] Active/inactive style updates.
- [ ] Frameless restored windows have a visible semantic active/inactive outer boundary.
- [ ] Native DWM boundary and generated-QSS client fallback do not render simultaneously.
- [ ] The restored-state outer boundary is suppressed when maximized/full-screen and returns after restore.
- [ ] Maximize/restore/full-screen behavior works.
- [ ] Multi-monitor and per-monitor scale transitions tested.
- [ ] Frameless limitations are documented when applicable.
- [ ] Frameless caption buttons have stable object names, tooltips, accessible names, and maximize/restore state updates.
- [ ] Frameless edge and corner resize targets delegate to `startSystemResize()`.
- [ ] Windows 11 frameless mode returns `HTMAXBUTTON` only over the maximize/restore region.
- [ ] Windows 11 maximize/restore still works by pointer after native hit-test routing.
- [ ] Windows 11 Snap Layout hover, Win+Z, and zone selection are manually verified.
- [ ] Minimum width is at most 500 epx for common Snap Layouts, with low-priority title content collapsed first.
- [ ] Rounded corners and all eight client resize targets remain intact after native adapter integration.
- [ ] Native fallback can be selected without changing application architecture.

## Scaling and platform

Test where supported:

- [ ] 100%;
- [ ] 125%;
- [ ] 150%;
- [ ] 200%;
- [ ] at least one multi-monitor transition;
- [ ] Windows target;
- [ ] Linux/macOS targets claimed by the application;
- [ ] supported minimum and preferred Qt/PySide versions.

## Regression and cleanup

- [ ] Existing functional tests pass.
- [ ] Signals/slots/controllers/shortcuts unchanged unless explicitly requested.
- [ ] `objectName` and automation hooks preserved or intentionally migrated.
- [ ] Persistent window/splitter state remains compatible.
- [ ] Obsolete style constants and selectors removed within migrated scope.
- [ ] Remaining legacy styling is listed.
- [ ] Before/after screenshots or gallery evidence retained.
