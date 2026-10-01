"""Exercise the shared adapter with real TJpgDec and stb, including fallbacks."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TJPGD = ROOT / "managed_components/espressif__esp_jpeg/tjpgd"


@unittest.skipUnless(shutil.which("cc") and shutil.which("convert") and TJPGD.exists(),
                     "requires C compiler, ImageMagick and resolved esp_jpeg component")
class JpegFastTest(unittest.TestCase):
    def test_simd_adapter_strips_alignment_scaling_and_cleanup(self):
        vendor = ROOT / "managed_components/espressif__esp_new_jpeg/include"
        if not vendor.exists():
            self.skipTest("requires resolved esp_new_jpeg headers")
        with tempfile.TemporaryDirectory(prefix="solaros-jpeg-simd-test-") as directory:
            binary = Path(directory) / "jpeg_simd_test"
            host = ROOT / "tests/host"
            subprocess.run([
                "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-DSOLAR_OS_JPEG_SIMD_HOST", f"-I{host / 'jpeg_simd_stubs'}",
                f"-I{host}", f"-I{ROOT / 'components/stb_image'}", f"-I{vendor}",
                f"-I{ROOT / 'src'}",
                str(host / "jpeg_simd_test.c"),
                str(ROOT / "components/stb_image/jpeg_simd.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_decode_formats_scaling_fallback_and_malformed_input(self):
        with tempfile.TemporaryDirectory(prefix="solaros-jpeg-test-") as directory:
            tmp = Path(directory)
            images = [tmp / name for name in
                      ("baseline.jpg", "odd.jpg", "progressive.jpg", "cmyk.jpg")]
            subprocess.run(["convert", "-size", "64x48", "gradient:red-blue",
                            "-sampling-factor", "2x2", "-quality", "85", str(images[0])], check=True)
            subprocess.run(["convert", "-size", "67x51", "gradient:green-yellow",
                            "-sampling-factor", "2x1", "-quality", "85", str(images[1])], check=True)
            subprocess.run(["convert", str(images[0]), "-interlace", "Plane",
                            str(images[2])], check=True)
            subprocess.run(["convert", str(images[0]), "-colorspace", "CMYK",
                            str(images[3])], check=True)
            binary = tmp / "jpeg_fast_test"
            host = ROOT / "tests/host"
            subprocess.run([
                "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-DSOLAR_OS_JPEG_FAST_HOST", f"-I{host / 'jpeg_stubs'}", f"-I{host}",
                f"-I{ROOT / 'components/stb_image/include'}",
                f"-I{ROOT / 'components/stb_image'}", f"-I{TJPGD}",
                str(host / "jpeg_fast_test.c"),
                str(ROOT / "components/stb_image/stb_image_port.c"),
                str(ROOT / "components/stb_image/jpeg_fast.c"),
                str(ROOT / "components/stb_image/jpeg_simd.c"), str(TJPGD / "tjpgd.c"),
                "-o", str(binary)], check=True)
            subprocess.run([str(binary), *map(str, images)], check=True)


if __name__ == "__main__":
    unittest.main()
