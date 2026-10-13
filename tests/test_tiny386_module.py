from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Tiny386ModuleTest(unittest.TestCase):
    def test_real_bios_boot_lifecycle_and_disk_failures(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            shutil.copytree(ROOT / "include", stage / "include")
            shutil.copytree(ROOT / "modules/tiny386", stage / "modules/tiny386")
            source = stage / "modules/tiny386/main"
            bios = stage / "bios.c"
            subprocess.run(["python3", "-B", str(source.parent / "scripts/embed_bios.py"),
                            str(source.parent / "bios"), str(bios)], check=True)
            objects = []
            sources = [(p, ["-include", str(source / "port.h")])
                       for p in sorted((source / "vendor").glob("*.c"))]
            sources += [(source / "port.c", []), (source / "text.c", []),
                        (source / "tiny386.c", ["-Dmain=tiny386_main"]),
                        (bios, []), (ROOT / "tests/host/tiny386_module_test.c", [])]
            for index, (path, options) in enumerate(sources):
                obj = stage / f"{index}.o"
                subprocess.run(["cc", "-std=gnu99", "-O2", "-g", "-DBPP=16",
                                "-ffunction-sections", "-fdata-sections",
                                "-I", str(stage / "include"), "-I", str(source),
                                *options, "-c", str(path), "-o", str(obj)], check=True)
                objects.append(str(obj))
            binary = stage / "tiny386"
            subprocess.run(["cc", *objects, "-Wl,--gc-sections", "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)
