from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / "modules/apple2"


class Apple2ModuleTest(unittest.TestCase):
    def test_embedded_basic_disk_paths_and_foreground_lifecycle(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            roms = stage / "roms_data.c"
            subprocess.run([
                "python3", "-B", str(MODULE / "scripts/embed_roms.py"),
                str(MODULE / "rom"), str(roms),
            ], check=True)
            for fixture in ("apple2_app_test", "apple2_app_entry_test"):
                sources = [
                    ROOT / "tests/host" / (fixture + ".c"),
                    MODULE / "main/machine.c",
                    MODULE / "main/video.c",
                    MODULE / "main/vendor/puce6502.c",
                ]
                options = []
                if fixture == "apple2_app_entry_test":
                    sources += [MODULE / "main/apple2.c", roms]
                    options = ["-Dmain=apple2_app_entry"]
                binary = stage / fixture
                subprocess.run([
                    "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "include"), "-I", str(MODULE / "main"),
                    *options, *map(str, sources), "-o", str(binary),
                ], check=True)
                subprocess.run([str(binary)], check=True, timeout=15)
