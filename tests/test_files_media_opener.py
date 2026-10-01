from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class FilesMediaOpenerTest(unittest.TestCase):
    def test_mpeg_binding_is_package_gated(self):
        registry = (ROOT / "src/apps/solar_os_app_registry.c").read_text()
        self.assertRegex(registry, re.compile(
            r'#if SOLAR_OS_PACKAGE_APP_VPLAY\s+APP_FILE_ENTRY\("vplay",'
            r'[^\n]+"\.mpg \.mpeg"\),\s+#endif'))

    def test_files_uses_registry_and_returns_after_playback(self):
        source = (ROOT / "src/apps/solar_os_files.c").read_text()
        opener = source.split("static const char *files_default_viewer(")[1].split(
            "static bool files_launch_app(")[0]
        self.assertIn("solar_os_app_registry_find_opener(path)", opener)
        self.assertIn("return entry->name;", opener)
        launch = source.split("static bool files_launch_app(")[1].split(
            "static void files_open_selected(")[0]
        self.assertIn("files_default_viewer(path)", launch)
        self.assertIn("SOLAR_OS_LAUNCH_CHILD_RETURN", launch)


if __name__ == "__main__":
    unittest.main()
