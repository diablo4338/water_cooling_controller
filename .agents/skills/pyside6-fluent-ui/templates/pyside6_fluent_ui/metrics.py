"""Non-theme logical metrics mirrored from the versioned semantic token maps.

Color values remain runtime theme aliases. These spacing and shell dimensions are
identical across light and dark themes, so small composite widgets can use named
constants without depending on a global theme singleton. Tests guard them against
the JSON source maps.
"""

SPACE_NONE = 0
SPACE_XXS = 2
SPACE_XS = 4
SPACE_S_NUDGE = 6
SPACE_S = 8
SPACE_M_NUDGE = 10
SPACE_M = 12
SPACE_L = 16
SPACE_XL = 20
SPACE_XXL = 24
SPACE_XXXL = 32

ICON_SIZE_SMALL = 16
ICON_SIZE_MEDIUM = 20
ICON_SIZE_LARGE = 24

SIDEBAR_MINIMUM_WIDTH = 180
SIDEBAR_PREFERRED_WIDTH = 280
SIDEBAR_MAXIMUM_WIDTH = 480

WORKBENCH_SNAP_MINIMUM_WIDTH = 500
WORKBENCH_MINIMUM_HEIGHT = 420
