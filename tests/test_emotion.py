from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class EmotionTest(unittest.TestCase):
    def test_geometry_timing_instances_and_sound(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp, "emotion")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", str(ROOT / "include"),
                str(ROOT / "modules/emotion/main/emotion.c"),
                str(ROOT / "tests/host/emotion_test.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
