"""Exercise the production receiver with the production DLP host under ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DlpReceiverTest(unittest.TestCase):
    def test_receiver(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "receiver"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I" + str(ROOT / "src/services"),
                "-I" + str(ROOT / "protocol/display_link/include"),
                str(ROOT / "src/services/solar_os_dlp_receiver.c"),
                str(ROOT / "protocol/display_link/src/sdlp.c"),
                str(ROOT / "tests/host/dlp_receiver_test.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
