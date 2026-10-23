from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import tomllib
import unittest

ROOT = Path(__file__).resolve().parents[1]


class JetFramebufferTest(unittest.TestCase):
    def test_app_lifecycle_and_allocation_failures(self):
        compiler = shutil.which("g++")
        if not compiler:
            self.skipTest("g++ unavailable")
        catalog = tomllib.loads((ROOT / "packages/solar_os_packages.toml").read_text())
        sources = [str(ROOT / "src" / path) for path in catalog["packages"]["engine_jet"]["sources"]]
        with tempfile.TemporaryDirectory(prefix="solaros-jet-app-") as temporary:
            binary = Path(temporary) / "jet-app"
            command = [compiler, "-std=c++17", "-O1", "-g", "-pthread", "-fsanitize=address",
                       "-fno-omit-frame-pointer", "-DJET_SOLAROS_ALLOCATOR=1",
                       f"-I{ROOT / 'tests/host/jet_stubs'}", f"-I{ROOT / 'src'}",
                       f"-I{ROOT / 'src/apps'}", f"-I{ROOT / 'src/services'}",
                       f"-I{ROOT / 'src/vendor/jet'}",
                       str(ROOT / "tests/host/jet_app_test.cpp"),
                       str(ROOT / "src/apps/solar_os_jet_app.cpp"), *sources, "-o", str(binary)]
            subprocess.run(command, check=True, capture_output=True, text=True)
            subprocess.run([str(binary)], check=True, capture_output=True, text=True,
                           env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1"})

    def test_native_and_wire_order_render_the_same_images(self):
        compiler = shutil.which("g++")
        if not compiler:
            self.skipTest("g++ unavailable")
        catalog = tomllib.loads((ROOT / "packages/solar_os_packages.toml").read_text())
        sources = [str(ROOT / "src" / path) for path in catalog["packages"]["engine_jet"]["sources"]]
        with tempfile.TemporaryDirectory(prefix="solaros-jet-") as temporary:
            images = []
            for swapped, optimization in ((0, "-O1"), (1, "-O1"), (1, "-O3")):
                binary = Path(temporary) / f"jet-{swapped}-{optimization}"
                output = Path(temporary) / f"jet-{swapped}-{optimization}.rgb565"
                command = [compiler, "-std=c++17", optimization, "-g", "-fsanitize=address",
                           "-fno-omit-frame-pointer", f"-DJET_FRAMEBUFFER_SWAP_BYTES={swapped}",
                           f"-DJET_CONSTANT_COLOR_LIGHTING={swapped}",
                           f"-DJET_UNIFORM_BYTE_CLEAR={swapped}",
                           f"-I{ROOT / 'src/services'}", f"-I{ROOT / 'src/vendor/jet'}",
                           str(ROOT / "tests/host/jet_framebuffer_test.cpp"), *sources, "-o", str(binary)]
                subprocess.run(command, check=True, capture_output=True, text=True)
                subprocess.run([str(binary), str(output)], check=True, capture_output=True,
                               text=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1"})
                images.append(output.read_bytes())
            self.assertEqual(len(images[0]), (144 + 1) * 64 * 48 * 2)
            native, wire, optimized_wire = images
            self.assertEqual(wire, optimized_wire)
            self.assertEqual(native[0::2], wire[1::2])
            self.assertEqual(native[1::2], wire[0::2])
            red = ((144 * 64 * 48) + 10 * 64 + 10) * 2
            self.assertEqual(wire[red:red + 2], b"\xf8\x00")
            # A non-symmetric background catches accidental no-op byte order.
            self.assertEqual(wire[:2], b"\x12\x34")


if __name__ == "__main__":
    unittest.main()
