from pathlib import Path
import subprocess
import tempfile
import unittest
from test_native_vm_lifecycle import production_functions

ROOT = Path(__file__).resolve().parents[1]

class NativeForegroundTest(unittest.TestCase):
    def test_isolated_session_lifecycle(self):
        source = (ROOT / "src/services/solar_os_native_foreground.c").read_text()
        names = ("foreground_current", "foreground_context_state", "foreground_caller",
                 "foreground_result", "foreground_enable_suspend", "foreground_suspend_requested",
                 "foreground_guest_suspend", "foreground_get_service", "foreground_worker_cleanup",
                 "solar_os_native_foreground_entry", "solar_os_native_foreground_owner",
                 "solar_os_native_foreground_claim", "solar_os_native_foreground_release",
                 "foreground_admit", "foreground_request_stop", "foreground_stop", "foreground_suspend", "foreground_resume")
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "foreground.inc").write_text(production_functions(source, names))
            binary = Path(tmp, "lifecycle")
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", tmp, "-I", str(ROOT / "include"),
                            "-I", str(ROOT / "tests/host"),
                            str(ROOT / "tests/host/native_foreground_test.c"),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
