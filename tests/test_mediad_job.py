from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
JOB = (ROOT / "src/jobs/solar_os_mediad_job.c").read_text(encoding="utf-8")
REGISTRY = (ROOT / "src/jobs/solar_os_job_registry.c").read_text(encoding="utf-8")
PACKAGES = (ROOT / "packages/solar_os_packages.toml").read_text(encoding="utf-8")
JOBS_MANUAL = (ROOT / "doc/manual/jobs.reference.md").read_text(encoding="utf-8")


class MediadJobPolicyTest(unittest.TestCase):
    def test_package_and_registry_wiring(self):
        package = PACKAGES.split("[packages.job_mediad]", 1)[1].split(
            "[packages.", 1
        )[0]
        self.assertIn('label = "job.mediad"', package)
        self.assertIn('"service_camera"', package)
        self.assertIn('"service_media"', package)
        self.assertIn('"jobs/solar_os_mediad_job.c"', package)
        self.assertIn("SOLAR_OS_PACKAGE_JOB_MEDIAD", REGISTRY)
        self.assertIn('{"mediad", "RTSP/RTP media publisher"', REGISTRY)

    def test_job_owns_camera_and_uses_standard_media_core(self):
        self.assertIn('#define MEDIAD_OWNER "job:mediad"', JOB)
        self.assertIn("solar_os_camera_acquire(MEDIAD_OWNER", JOB)
        self.assertIn("solar_os_rtp_jpeg_parse", JOB)
        self.assertIn("solar_os_rtp_jpeg_packetize", JOB)
        self.assertIn("solar_os_rtcp_sender_report", JOB)
        self.assertIn("SOLAR_OS_MEDIA_RTP_PACKET_MAX", JOB)

    def test_single_client_and_bounded_stop_are_explicit(self):
        self.assertIn("mediad_reject_pending_client", JOB)
        self.assertIn("solar_os_task_wait_done", JOB)
        self.assertIn("solar_os_camera_release_frame", JOB)
        self.assertIn("RTSP/1.0 453 Not Enough Bandwidth", JOB)

    def test_manual_documents_rtsp_url_and_security(self):
        section = JOBS_MANUAL.split("## mediad", 1)[1].split("\n## ", 1)[0]
        self.assertIn("rtsp://device/media", section)
        self.assertIn("one RTSP client", section)
        self.assertIn("unauthenticated and unencrypted", section)


if __name__ == "__main__":
    unittest.main()
