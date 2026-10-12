from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "modules/tiny386/main"


class Tiny386CpuTest(unittest.TestCase):
    def test_fast_paths_match_cpu_state_memory_and_faults(self):
        source = (SOURCE / "vendor/i386.c").read_text()
        needle = "stepcount > 1 && try_dec_jcc8(cpu)"
        self.assertIn(needle, source)
        copy_needle = "if (!try_ram_movs(cpu, memls.addr1, memld.addr1, count, BIT / 8, dir))"
        self.assertIn(copy_needle, source)
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            reference = stage / "reference.c"
            reference.write_text(source.replace(needle, "stepcount > 1 && false")
                                .replace(copy_needle, "if (true)"))
            outputs = []
            for name, implementation in (("reference", reference), ("optimized", SOURCE / "vendor/i386.c")):
                binary = stage / name
                subprocess.run(["cc", "-std=gnu99", "-O2", "-g",
                                "-I", str(SOURCE), "-I", str(SOURCE / "vendor"),
                                f'-DCORE_SOURCE="{implementation}"',
                                str(ROOT / "tests/host/tiny386_cpu_test.c"),
                                "-o", str(binary)], check=True)
                outputs.append(subprocess.check_output([str(binary)], timeout=20))
            self.assertEqual(outputs[0], outputs[1])
