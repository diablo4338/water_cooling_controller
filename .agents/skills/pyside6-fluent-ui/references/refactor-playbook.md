# Existing-GUI refactor playbook

Use this workflow after the generic skill infrastructure has been validated independently. Do not inspect or refactor a project before the skill exists unless the user explicitly requests an exploratory prototype.

## 1. Audit without modifying files

Inventory:

- top-level windows and dialogs;
- navigation and shell ownership;
- Qt Designer `.ui` files;
- QSS sources and per-widget `setStyleSheet()` calls;
- palettes and proxy styles;
- hardcoded colors, fonts, dimensions, margins, radii, and icon paths;
- models, selection models, delegates, and custom painting;
- signals, slots, controllers, command routing, and persistence;
- focus order, accessibility properties, shortcuts, and tests;
- screenshots at representative states and scales.

Output a migration map before proposing edits.

## 2. Record preservation constraints

For each migration scope, explicitly record:

```text
behavior to preserve
public signals/slots
model and selection ownership
objectName / automation IDs
keyboard shortcuts
persistent layout/state
loading/error/empty/invalid behavior
performance constraints
supported platforms and Qt versions
```

A visual task does not authorize an architecture rewrite.

## 3. Establish the theme boundary

Choose one application-level entry point. It should:

- load the official resolved tokens;
- expose Qt semantic aliases;
- build `QPalette` groups;
- render one application QSS;
- emit a theme-change signal;
- follow system color scheme when configured;
- provide explicit light/dark/high-contrast overrides;
- own icon recoloring/caching policy.

Do not migrate components until this boundary is stable.

## 4. Capture baseline states

For each selected screen, capture:

- normal light and dark appearance where available;
- keyboard focus on key controls;
- selected/checked states;
- disabled state;
- invalid/error state;
- loading/busy state;
- empty state;
- minimum supported window size;
- one high-DPI scale.

Record functional tests and manual behavior checks that must continue to pass.

## 5. Migrate in layers

### Layer A — global foundations

- application font roles;
- window/canvas surfaces;
- selection and focus policy;
- theme switching;
- semantic property helper;
- approved icon source.

### Layer B — primitive controls

- push/tool buttons;
- inputs and text areas;
- comboboxes and spin boxes;
- checkboxes/radios/switches;
- progress and validation.

### Layer C — structure

- page headers and command surfaces;
- cards/sections/dividers;
- navigation;
- workbench shell regions if appropriate.

### Layer D — data views

- tables, trees, and lists;
- delegates;
- selection/current/focus distinctions;
- empty/loading/error overlays.

### Layer E — transient UI

- menus;
- tooltips;
- dialogs;
- drawers/popovers;
- notifications/message bars.

### Layer F — restrained motion

Add only after static states are correct. Respect the reduced-motion setting.

## 6. Use bounded workstreams

A good migration workstream changes one coherent scope, for example:

```text
Theme infrastructure + application bootstrap
Main window shell only
Settings dialog controls only
Results table and delegate only
All message bars and validation states
```

Avoid a single patch that changes every widget and business module. Small scopes make screenshot and behavior regressions diagnosable.

## 7. Preserve compatibility during migration

Use a temporary bridge only when needed:

- legacy class/object selectors may coexist with semantic properties;
- existing styles may remain on unmigrated screens;
- avoid cascading new QSS into unmigrated custom widgets without testing;
- mark compatibility selectors with an owner and removal condition;
- never keep two competing global theme managers.

## 8. Validate before deleting legacy code

For each scope:

1. run functional tests;
2. compare baseline states;
3. verify keyboard and focus;
4. test light/dark/high-contrast path;
5. test minimum size and display scaling;
6. inspect translated/long labels where possible;
7. confirm no model/controller behavior changed;
8. remove obsolete local styling only after passing.

## 9. Report the change

A completed refactor report should state:

- migrated screens/components;
- semantic aliases introduced/used;
- Qt mechanisms used (`QPalette`, QSS, delegate, composite, proxy style);
- behavior deliberately preserved;
- tests and visual states checked;
- platform limitations;
- remaining legacy styling and the next bounded scope.

## 10. Feed back only general lessons

After a real-project pilot:

- add reusable Fluent/Qt lessons to this core skill;
- keep application-specific colors, widget names, domain states, and architecture rules in a separate project profile;
- do not turn one app's workaround into a universal requirement without evidence.
