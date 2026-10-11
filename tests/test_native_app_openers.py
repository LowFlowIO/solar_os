from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class NativeAppOpenersTest(unittest.TestCase):
    def test_installed_file_associations_and_missing_modules(self):
        source = (ROOT / "src/apps/solar_os_app_registry.c").read_text()
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "app_opener.inc").write_text(production_functions(
                source, ("solar_os_app_registry_find_opener",)))
            binary = Path(tmp, "openers")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", tmp, "-I", str(ROOT / "src/apps"),
                str(ROOT / "tests/host/native_app_openers_test.c"),
                str(ROOT / "src/apps/solar_os_app_file_types.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)
