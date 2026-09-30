from pathlib import Path
import tomllib
import unittest

ROOT = Path(__file__).resolve().parents[1]
APP = (ROOT / "src/apps/solar_os_rtsp_app.c").read_text()
CLIENT = (ROOT / "src/services/solar_os_rtsp_client.c").read_text()
PACKAGES = tomllib.loads((ROOT / "packages/solar_os_packages.toml").read_text())


class RtspAppTest(unittest.TestCase):
    def test_registry_packages_and_user_manual(self):
        self.assertIn("app_rtsp", PACKAGES["groups"]["rtsp"]["members"])
        self.assertIn("service_rtsp_client", PACKAGES["packages"]["app_rtsp"]["depends"])
        self.assertIn("service_media", PACKAGES["packages"]["service_rtsp_client"]["depends"])
        self.assertIn("service_audio", PACKAGES["packages"]["service_rtsp_client"]["depends"])
        registry = (ROOT / "src/apps/solar_os_app_registry.c").read_text()
        self.assertIn('APP_ENTRY("rtsp"', registry)
        self.assertIn("SOLAR_OS_APP_CAP_PORT", registry.split('APP_ENTRY("rtsp"', 1)[1].split("\n", 1)[0])
        manual = (ROOT / "doc/manual/apps.md").read_text().split("## rtsp", 1)[1].split("## ", 1)[0]
        self.assertIn("--audio-only", manual)
        self.assertIn("oscilloscope", manual)
        self.assertIn("TCP interleaving", manual)

    def test_audio_only_uses_common_scope_and_video_uses_common_blitter(self):
        self.assertIn("solar_os_oscilloscope_widget_draw", APP)
        self.assertIn("solar_os_oscilloscope_widget_submit_s16", APP)
        self.assertIn("solar_os_gfx_blit_raster", APP)
        self.assertIn("solar_os_media_transport_button_draw", APP)
        self.assertIn(".video = rtsp.graphical && !audio_only", APP)
        self.assertIn(".samples = audio_samples", CLIENT)

    def test_cold_state_and_worker_ownership(self):
        self.assertIn(".state_slot = &rtsp_state", APP)
        self.assertIn(".state_release_ready = release_ready", APP)
        self.assertIn("solar_os_task_wait_done", APP)
        self.assertNotIn("solar_os_camera_", APP + CLIENT)
        self.assertNotIn("solar_os_stb_", CLIENT)
        self.assertIn('SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "rtsp.jpeg"', CLIENT)
        self.assertIn("solar_os_audio_s16_convert", CLIENT)
        self.assertIn("c->leased || c->pending", CLIENT)
        self.assertIn("CLIENT_TIMEOUT_US", CLIENT)


if __name__ == "__main__":
    unittest.main()
