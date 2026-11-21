from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "check_contrast.py"
spec = importlib.util.spec_from_file_location("check_contrast", SCRIPT)
assert spec and spec.loader
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ContrastTests(unittest.TestCase):
    def test_representative_pairs_for_both_profiles(self) -> None:
        for profile in ("fluent-workbench", "fluent-workbench-neutral-status"):
            for mode in ("light", "dark"):
                values = module.load_values(ROOT, mode, profile)
                for foreground, background, target in module.TEXT_PAIRS + module.NON_TEXT_PAIRS:
                    ratio = module.contrast(values[foreground], values[background])
                    self.assertGreaterEqual(
                        ratio + 1e-9,
                        target,
                        f"{profile}/{mode}: {foreground} on {background} = {ratio:.2f}",
                    )


if __name__ == "__main__":
    unittest.main()
