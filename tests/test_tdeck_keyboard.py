from pathlib import Path
import subprocess
import tempfile
import tomllib
import unittest

from tests.test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class TdeckKeyboardTest(unittest.TestCase):
    def test_native_backlight_shortcut_is_consumed(self):
        source = (ROOT / "src/services/solar_os_tdeck_keyboard.c").read_text()
        defines = tomllib.loads((ROOT / "boards/manifests/t_deck_plus.toml").read_text())["defines"]
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "tdeck_matrix.inc").write_text(production_functions(source, ("process_matrix",)))
            Path(tmp, "solar_os_board.h").write_text('#pragma once\n#include "solar_os_keys.h"\n' + ''.join(
                f'#define {name} {defines[name]}\n' for name in (
                    "SOLAR_OS_BOARD_TDECK_KEYMAP", "SOLAR_OS_BOARD_INPUT_CHORDS")))
            binary = Path(tmp, "keyboard")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-include", str(ROOT / "tests/host/compat.h"),
                "-I", tmp, "-I", str(ROOT / "tests/host"),
                "-I", str(ROOT / "src"), "-I", str(ROOT / "src/services"),
                "-idirafter", str(ROOT / "include"),
                str(ROOT / "tests/host/tdeck_keyboard_test.c"),
                str(ROOT / "src/services/solar_os_input.c"),
                str(ROOT / "src/services/solar_os_input_composition.c"),
                str(ROOT / "src/services/solar_os_keys.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
