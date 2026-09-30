from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
PACKAGES = (ROOT / "packages/solar_os_packages.toml").read_text(encoding="utf-8")
class MediaServicePolicyTest(unittest.TestCase):
    def test_media_package_contains_protocol_core(self):
        package = PACKAGES.split("[packages.service_media]", 1)[1].split(
            "[packages.", 1
        )[0]
        self.assertIn('label = "service.media"', package)
        self.assertIn('"services/solar_os_media.c"', package)
        self.assertIn('"services/solar_os_rtp.c"', package)
        self.assertIn('"services/solar_os_rtp_jpeg.c"', package)
        self.assertIn('"services/solar_os_rtsp.c"', package)

if __name__ == "__main__":
    unittest.main()
