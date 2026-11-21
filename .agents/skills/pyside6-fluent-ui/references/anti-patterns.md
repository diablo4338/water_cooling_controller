# Anti-patterns

## Architecture

### Scattered local style sheets

```python
widget.setStyleSheet("background: #fff; border-radius: 7px")
```

Why it fails: hardcoded theme values, incomplete states, inheritance surprises, and difficult runtime switching.

Use semantic properties and the application theme instead.

### Two global theme managers

Do not layer a new Fluent manager over an existing manager without a deliberate integration plan. Establish one source of truth.

### Visual refactor plus domain rewrite

Do not change models, controllers, persistence, or command semantics merely because the UI is being restyled.

### Universal `QProxyStyle`

A proxy style that redraws every control becomes a fragile custom toolkit. Use it only for narrow metrics/style hints.

## Tokens

### Raw colors in application widgets

Do not consume `#0f6cbd`, `grey[14]`, or `brand[80]` directly. Consume `brand_background` or another semantic alias.

### Pretending derived values are official

Control heights, workbench dimensions, and project density choices must be marked as Qt/project-derived. Do not label them Microsoft Fluent tokens.

### Color-only state

A red border without message/icon/accessibility context is not an adequate error state.

## Layout

### Fixed geometry for ordinary screens

Avoid `setGeometry()` and widespread fixed sizes for forms, panels, and content. Use layouts and size policies.

### Card inflation

Do not wrap every label/group in a rounded elevated card. Prefer whitespace, headings, and subtle dividers.

### Web breakpoint imitation

Do not copy CSS breakpoints directly. Respond to actual Qt window constraints, content metrics, and sidebar/panel priorities.

## Components

### Custom widget before standard control

A painted clickable `QWidget` usually loses keyboard, focus, accessibility, and native behavior. Start from `QPushButton`, `QToolButton`, item views, or a semantic composite.

### Missing state because QSS looked acceptable

Once QSS replaces native rendering, explicitly verify all relevant states and subcontrols.

### Emoji or text glyph icons

Do not use `⚙`, `×`, `☰`, or platform-dependent font glyphs as final UI icons. Use approved SVG assets.

### Disabled as low opacity only

Disabled controls need legibility, nonactivation, and distinguishable state—not merely 20% opacity.

## Activity Bar

- unrelated commands mixed with destinations;
- too many items to recognize;
- no tooltip or accessible name;
- selection indicated only by a low-contrast line;
- animated or unbounded badges;
- opening an unrelated modal/web surface rather than switching a view container.

## Status Bar

- paragraphs or verbose instructions;
- all items made focusable;
- routine information rendered as warning/danger;
- global and contextual state mixed randomly;
- click handlers attached to passive labels instead of semantic buttons;
- detailed logs/progress forced into the compact bar.

## Title Bar

### Frameless without an explicit profile decision

Do not remove native chrome merely because it is customizable. When the requested outcome is a VS Code-style single row, use the supplied frameless profile deliberately and keep the native-title fallback available. Do not substitute expanded client area and claim that it removed OS caption content.

### Manual drag by setting geometry

Avoid implementing movement primarily with mouse-delta geometry updates. Use `QWindow.startSystemMove()` and system resize.

### Interactive controls inside drag/safe regions

Buttons, menus, and fields must not accidentally initiate window move or overlap native controls.

### Fake caption controls without platform behavior

A visually accurate close button is not enough. Frameless implementations must preserve system menu, maximize/restore, snap/tiling, keyboard, active/inactive, and accessibility behavior.

## Accessibility

- removing focus outlines because they are visually inconvenient;
- hover-only commands;
- passive labels in the tab order;
- invalid state with no text;
- focus not restored after transient UI;
- assuming token names guarantee contrast;
- assuming system reduced-motion support is uniformly available across Qt platforms.

## Migration

- applying global QSS to the entire legacy application without a gallery or bounded pilot;
- deleting old styling before behavior/theme validation;
- renaming `objectName` values casually;
- replacing model/view with item widgets for styling convenience;
- accepting one screenshot as proof of completion;
- copying VS Code colors, branding, and arrangement instead of using the shell semantics.
