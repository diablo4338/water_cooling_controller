# Token architecture and Qt-facing aliases

## Source of truth

The skill ships a versioned resolved snapshot of Microsoft’s official Fluent Web themes:

```text
resources/fluent2-official-web-theme-tokens.json
```

Snapshot metadata:

- package: `@fluentui/react-theme`;
- package version: `9.2.1`;
- 459 resolved values per theme;
- light and dark Web themes;
- extraction date and source paths embedded in the file.

Use the Web theme as a **design-token source**. Qt still governs widget metrics, font behavior, native controls, and window integration.

## Token layers

```text
Official Fluent globals
        ↓
Official Fluent aliases
        ↓
Qt-facing semantic aliases
        ↓
Project-specific semantic additions
        ↓
QPalette / generated QSS / delegates / metrics
```

The Qt-facing map is:

```text
resources/qt-token-map.json
```

The optional workbench layer is:

```text
resources/shell-token-map.json
```

Application code should consume the Qt-facing names. Project-specific aliases may reference them, but should not duplicate their resolved light/dark values.

## Why an extra Qt alias layer exists

Official names such as `colorNeutralBackground1` are useful and preserved, but a Qt application benefits from domain-neutral roles such as:

```text
window_background
surface_background
surface_secondary
text_primary
border_primary
focus_border
brand_background
selection_subtle_background
```

This layer lets the underlying Fluent version change without forcing widget code to understand the complete upstream token graph.

## Core surface aliases

| Qt alias | Official token | Intended use |
|---|---|---|
| `window_background` | `colorNeutralBackground2` | broad window/workspace background |
| `canvas_background` | `colorNeutralBackground1` | main content canvas |
| `surface_background` | `colorNeutralBackground1` | default controls and surfaces |
| `surface_secondary` | `colorNeutralBackground2` | sidebars and grouped secondary surfaces |
| `surface_tertiary` | `colorNeutralBackground3` | inset/tertiary surfaces |
| `surface_elevated` | `colorNeutralBackground4` | floating surface when distinction is needed |
| `card_background` | `colorNeutralCardBackground` | cards |

Use a small number of levels on one screen. Do not expose every official background level merely because it exists.

## Core foreground and stroke aliases

| Qt alias | Official token |
|---|---|
| `text_primary` | `colorNeutralForeground1` |
| `text_secondary` | `colorNeutralForeground2` |
| `text_tertiary` | `colorNeutralForeground3` |
| `text_disabled` | `colorNeutralForegroundDisabled` |
| `border_primary` | `colorNeutralStroke1` |
| `border_subtle` | `colorNeutralStroke2` |
| `border_accessible` | `colorNeutralStrokeAccessible` |
| `focus_border` | `colorStrokeFocus2` |
| `focus_inner` | `colorStrokeFocus1` |

When Qt/QSS cannot reproduce Fluent’s dual focus ring exactly, preserve strong visible contrast and avoid layout shift. A narrowly scoped `QProxyStyle` or focus overlay may be more appropriate than increasing the normal border width.

## Brand and selection aliases

```text
brand_background
brand_background_hover
brand_background_pressed
brand_background_selected
brand_background_subtle
brand_foreground
brand_border
selection_strong_background
selection_strong_foreground
selection_subtle_background
selection_subtle_foreground
```

Use strong selection for high-emphasis active controls or the application palette highlight. Use subtle selection for data rows, navigation, and surfaces where a solid brand fill would dominate content.

## Status aliases

Each status has:

```text
<status>_background
<status>_background_strong
<status>_foreground
<status>_border
```

Supported semantic statuses:

```text
info
success
warning
danger
```

Status components include text or accessible descriptions. A warning or error is never represented solely by a red/orange border.

## Typography aliases

The official base ramp is retained. Use these semantic presets:

| Role | Size | Weight | Line height |
|---|---:|---:|---:|
| caption2 | 10 px | 400 | 14 px |
| caption1 | 12 px | 400 | 16 px |
| caption1 strong | 12 px | 600 | 16 px |
| body | 14 px | 400 | 20 px |
| body strong | 14 px | 600 | 20 px |
| body2 | 16 px | 400 | 22 px |
| subtitle2 | 16 px | 600 | 22 px |
| subtitle1 | 20 px | 600 | 28 px |
| section/title3 | 24 px | 600 | 32 px |
| title2 | 28 px | 600 | 36 px |
| title1 | 32 px | 600 | 40 px |

Prefer the system UI font when the Fluent-preferred family is unavailable. Do not bundle or redistribute fonts as part of this skill.

## Spacing, radii, and strokes

Primary spacing ramp:

```text
0, 2, 4, 6, 8, 10, 12, 16, 20, 24, 32 px
```

Radii:

```text
0, 2, 4, 6, 8, 12, 16 px, circular
```

Strokes:

```text
1, 2, 3, 4 px
```

Use `radius_medium` for ordinary controls, `radius_large` for cards/sections, and larger radii sparingly for overlays or signature surfaces. A desktop workbench should not turn every rectangle into a pill.

## Control density

Official Fluent v9 Button styles imply these total control heights from line height, padding, and stroke:

```text
small   24 px
medium  32 px
large   40 px
```

These are provided as Qt adaptation metrics. They are not permission to force every control to an exact height. Verify font metrics, style subcontrols, focus treatment, and display scaling.

Reference icon sizes:

```text
small   16 px
medium  20 px
large   24 px
```

## Motion aliases

Durations:

```text
50, 100, 150, 200, 250, 300, 400, 500 ms
```

Use:

- `duration_faster` for hover/pressed color transitions where animation is enabled;
- `duration_fast` or `duration_normal` for small panel/indicator changes;
- `duration_gentle` or `duration_slow` for larger spatial transitions;
- `curve_decelerate` for entrance;
- `curve_accelerate` for exit;
- `curve_standard` for ordinary transitions.

Qt can represent cubic Bézier easing with `QEasingCurve.BezierSpline`. The template runtime includes a parser/helper.

## Shadows and elevation

Fluent shadows contain two layers; `QGraphicsDropShadowEffect` exposes one. Therefore:

- prefer surface contrast and borders for ordinary cards;
- use a restrained single-shadow approximation for menus/popovers/dialogs;
- avoid applying `QGraphicsDropShadowEffect` to large scrolling trees;
- do not claim a single Qt shadow is an exact Fluent shadow;
- test performance and clipping.

## High contrast

Do not ship fixed high-contrast hex values as the primary strategy.

1. Determine whether the system requests high contrast where the Qt version exposes it.
2. Start from the effective system `QPalette`.
3. Map critical aliases to system roles such as Window, Base, Text, Button, Highlight, and HighlightedText.
4. Minimize decorative backgrounds and preserve clear outlines.
5. Test with the actual target operating-system contrast themes.

The strategy is encoded in `qt-token-map.json`.

## Project-specific aliases

Projects may add semantic roles such as:

```text
viewport_background
measurement_valid
measurement_invalid
simulation_running
simulation_paused
```

Rules:

- define the role, not a color name;
- map it to a Qt-facing or official semantic token;
- provide light, dark, and high-contrast behavior;
- document where it is allowed;
- do not overload a generic status color with unrelated domain meaning.

## Updating the token snapshot

When updating Fluent:

1. record package version, source commit, and extraction date;
2. regenerate resolved light/dark themes;
3. validate that every Qt and shell alias still resolves;
4. review changed values visually in the widget gallery;
5. rerun contrast checks;
6. do not silently update production styling during an unrelated feature change.
