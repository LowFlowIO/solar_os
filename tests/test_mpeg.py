from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

@unittest.skipUnless(shutil.which("cc") and shutil.which("ffmpeg"), "requires cc and ffmpeg")
class MpegTest(unittest.TestCase):
    def test_real_decoder_and_allocation_failures(self):
        with tempfile.TemporaryDirectory(prefix="solaros-mpeg-test-") as directory:
            tmp = Path(directory)
            files = []
            for codec, audio, name in [
                ("mpeg1video", True, "av.mpg"),
                ("mpeg1video", False, "video.mpg"),
                ("mpeg2video", True, "unsupported.mpg"),
                ("mpeg1video", True, "stereo.mpg"),
            ]:
                path = tmp / name
                cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
                       "-i", "testsrc2=size=160x120:rate=25"]
                if audio:
                    cmd += ["-f", "lavfi", "-i",
                            "sine=frequency=440:sample_rate=48000" if name == "stereo.mpg" else
                            "sine=frequency=440:sample_rate=44100"]
                cmd += ["-t", "4", "-c:v", codec, "-bf", "2", "-g", "12", "-q:v", "5"]
                if audio:
                    cmd += ["-c:a", "mp2", "-b:a", "96k", "-ac", "2" if name == "stereo.mpg" else "1"]
                cmd += ["-f", "mpeg", "-packetsize", "2048" if audio else "32768", str(path)]
                subprocess.run(cmd, check=True)
                files.append(path)
            source = files[0].read_bytes()
            seq = source.index(b"\x00\x00\x01\xb3") + 4
            oversized = bytearray(source)
            oversized[seq:seq+3] = bytes([1920 >> 4, ((1920 & 15) << 4) | (1080 >> 8), 1080 & 255])
            invalid_rate = bytearray(source)
            invalid_rate[seq+3] &= 0xf0
            for name, data in [("oversized.mpg", oversized), ("bad-rate.mpg", invalid_rate),
                               ("truncated.mpg", source[:20]), ("unknown.mpg", b"not an MPEG file")]:
                path = tmp / name
                path.write_bytes(data)
                files.append(path)
            binary = tmp / "mpeg_test"
            subprocess.run([
                "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                f"-I{ROOT / 'tests/host'}", f"-I{ROOT / 'src'}",
                f"-I{ROOT / 'src/services'}", f"-I{ROOT / 'components/pl_mpeg'}",
                str(ROOT / "tests/host/mpeg_test.c"),
                str(ROOT / "src/services/solar_os_mpeg.c"),
                str(ROOT / "src/services/solar_os_mpeg_raster.c"), "-lm", "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary), *map(str, files)], check=True, timeout=30)

if __name__ == "__main__":
    unittest.main()
