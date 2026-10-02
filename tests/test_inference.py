import ast
import re
import sys
import tempfile
import tomllib
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import generate_flavor_config as flavors
import patch_espdl as overlay


class InferenceIntegrationTest(unittest.TestCase):
    def test_target_and_psram_gates_without_camera(self):
        catalog = flavors.load_catalog(ROOT / "packages/solar_os_packages.toml")
        groups = {group: group == "inference" for group in catalog.groups}
        selected = flavors.enable_required_packages(catalog,
            {name: False for name in catalog.packages}, {"service_inference"})
        for target in ("esp32", "esp32s3", "esp32c3", "esp32p4"):
            for capabilities in (set(), {"psram"}):
                _, packages = flavors.apply_board_capability_pruning(catalog, groups,
                    flavors.apply_target_pruning(catalog, selected, target), capabilities)
                self.assertEqual(packages["service_inference"], target == "esp32s3" and bool(capabilities))
                for dependency in ("service_camera", "service_image", "service_vision"):
                    self.assertFalse(packages[dependency])
        for name in ("core", "rover", "netrunner", "writerdeck", "full"):
            flavor = tomllib.loads((ROOT / f"flavors/{name}.toml").read_text())
            self.assertEqual(flavor["groups"].get("inference", False), name == "full")

    def test_documented_python_examples_parse(self):
        for example in re.findall(r"```python\n(.*?)```", (ROOT / "doc/manual/inference.md").read_text(), re.S):
            ast.parse(example)

    def test_overlay_rejects_version_and_source_drift(self):
        with tempfile.TemporaryDirectory() as directory:
            component = Path(directory)
            manifest = component / "idf_component.yml"
            manifest.write_text('version: "3.3.12"\n')
            with self.assertRaisesRegex(ValueError, "pinned version"):
                overlay.patch(component)
            manifest.write_text('version: 3.3.13\n')
            header = component / "dl/model/include/dl_model_base.hpp"
            header.parent.mkdir(parents=True)
            header.write_text("    Model() { unexpected(); }\n")
            with self.assertRaisesRegex(ValueError, "review overlay"):
                overlay.patch(component)
            header.write_text(overlay.OLD)
            overlay.replace(header, overlay.OLD, overlay.NEW)
            overlay.replace(header, overlay.OLD, overlay.NEW)
            self.assertEqual(header.read_text(), overlay.NEW)


if __name__ == "__main__":
    unittest.main()
