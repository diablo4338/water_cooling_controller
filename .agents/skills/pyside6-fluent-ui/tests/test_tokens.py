from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

ROOT = Path(__file__).resolve().parents[1]
TEMPLATES = ROOT / "templates"
if str(TEMPLATES) not in sys.path:
    sys.path.insert(0, str(TEMPLATES))

from pyside6_fluent_ui import metrics
from pyside6_fluent_ui.style import render_qss_file
from pyside6_fluent_ui.tokens import (
    TokenRepository,
    TokenValidationError,
    parse_cubic_bezier,
    parse_ms,
    parse_px,
)


class TokenRepositoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.repository = TokenRepository.from_skill_root(ROOT)

    def test_official_count(self) -> None:
        self.assertEqual(self.repository.metadata["tokenCountPerTheme"], 459)
        for mode in ("light", "dark"):
            self.assertEqual(len(self.repository.resolve(mode).official), 459)

    def test_semantic_aliases_match_official_tokens(self) -> None:
        light = self.repository.resolve("light")
        dark = self.repository.resolve("dark")
        self.assertEqual(light.value("brand_background"), light.value("colorBrandBackground"))
        self.assertEqual(dark.value("brand_background"), dark.value("colorBrandBackground"))
        self.assertNotEqual(light.value("window_background"), dark.value("window_background"))

    def test_metrics_parse(self) -> None:
        theme = self.repository.resolve("light")
        self.assertEqual(parse_px(theme.value("space_m")), 12.0)
        self.assertEqual(parse_px(theme.value("shell_activity_width")), 48.0)
        self.assertEqual(parse_ms(theme.value("duration_normal")), 200)


    def test_all_profiles_resolve_qss_placeholders_to_strings(self) -> None:
        qss_path = ROOT / "templates/pyside6_fluent_ui/fluent.qss.in"
        with TemporaryDirectory() as directory:
            for profile in ("fluent-workbench", "fluent-workbench-neutral-status"):
                for mode in ("light", "dark"):
                    theme = self.repository.resolve(mode, shell_profile=profile)
                    self.assertTrue(all(isinstance(value, str) for value in theme.aliases.values()))
                    rendered = render_qss_file(
                        qss_path,
                        theme,
                        asset_directory=Path(directory) / profile / mode,
                    )
                    self.assertFalse(
                        re.findall(r"@\{([A-Za-z_][A-Za-z0-9_]*)\}", rendered)
                    )

    def test_named_metrics_match_versioned_maps(self) -> None:
        qt_map = json.loads((ROOT / "resources/qt-token-map.json").read_text(encoding="utf-8"))
        shell_map = json.loads((ROOT / "resources/shell-token-map.json").read_text(encoding="utf-8"))
        self.assertEqual(metrics.SPACE_XS, 4)
        self.assertEqual(metrics.SPACE_S, 8)
        self.assertEqual(metrics.SPACE_M, 12)
        self.assertEqual(metrics.SPACE_L, 16)
        self.assertEqual(
            metrics.ICON_SIZE_LARGE,
            int(qt_map["aliases"]["metrics"]["icon_size_large"]["value"].removesuffix("px")),
        )
        self.assertEqual(
            metrics.SIDEBAR_PREFERRED_WIDTH,
            int(shell_map["metrics"]["shell_sidebar_preferred_width"]["value"].removesuffix("px")),
        )

    def test_parser_and_error_paths(self) -> None:
        self.assertEqual(parse_cubic_bezier("cubic-bezier(0.33,0,0.67,1)"), (0.33, 0.0, 0.67, 1.0))
        with self.assertRaises(TokenValidationError):
            parse_px("12pt")
        with self.assertRaises(TokenValidationError):
            self.repository.resolve("sepia")
        with self.assertRaises(TokenValidationError):
            self.repository.resolve("light", shell_profile="missing")

    def test_neutral_status_profile(self) -> None:
        theme = self.repository.resolve("dark", shell_profile="fluent-workbench-neutral-status")
        self.assertEqual(
            theme.value("shell_status_background"),
            theme.value("colorNeutralBackground2"),
        )

    def test_window_boundary_aliases_resolve_semantically(self) -> None:
        for mode in ("light", "dark"):
            theme = self.repository.resolve(mode)
            self.assertEqual(
                theme.value("shell_window_border_active"),
                theme.value("colorNeutralStroke1"),
            )
            self.assertEqual(
                theme.value("shell_window_border_inactive"),
                theme.value("colorNeutralStroke2"),
            )


if __name__ == "__main__":
    unittest.main()
