#!/usr/bin/env python3
"""Exercise a display session with an isolated JPEG/L16 publisher.

Requires no-auth Telnet, FFmpeg and MediaMTX. Never flashes, changes Wi-Fi or
stops an existing server. Uses session 0 and quits the test app on exit.
Run only with that display session idle. Output is a memory/stack evidence log.
"""

import argparse
import os
import re
import select
import signal
import socket
import subprocess
import tempfile
import time
from pathlib import Path


class Console:
    def __init__(self, host):
        self.socket = socket.create_connection((host, 23), 3)
        self.read(1)

    def read(self, seconds, prompt=False):
        result = b""
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            ready, _, _ = select.select([self.socket], [], [], max(0, min(.1, end - time.monotonic())))
            if ready:
                block = self.socket.recv(65536)
                if not block:
                    raise RuntimeError("Telnet connection ended")
                result += block
                if prompt and re.search(rb"\w+@[^\r\n]+:/[^\r\n]* ", result):
                    break
        return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", result.decode(errors="replace"))

    def command(self, line):
        self.read(.05)
        self.socket.sendall((line + "\r\n").encode())
        result = self.read(5, prompt=True) + self.read(.25)
        print(result, flush=True)
        return result

    def snapshot(self, name):
        print("\nSNAPSHOT", name, flush=True)
        result = {}
        for command in ("mem", "top", "stream list", "job status rtspd"):
            result[command] = self.command(command)
        return result


def stop(process):
    if process:
        process.terminate()
        try:
            process.wait(4)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--telnet", required=True)
    parser.add_argument("--host-address", required=True, help="LAN address reachable from the device")
    parser.add_argument("--mediamtx", required=True)
    parser.add_argument("--cycles", type=int, default=3)
    parser.add_argument("--seconds", type=float, default=30)
    parser.add_argument("--interrupt", action="store_true", help="Stop and restart our publisher during playback")
    parser.add_argument("--capture", action="store_true", help="Also budget rtspd audio0.capture after each playback cycle")
    args = parser.parse_args()
    if args.cycles < 1 or args.seconds < 10:
        parser.error("cycles must be positive and seconds at least 10")
    console = server = source = receiver = None
    capture_started = playback_started = False
    with tempfile.TemporaryDirectory(prefix="solaros-rtsp-soak-") as directory:
        root = Path(directory)
        config = root / "server.yml"
        config.write_text("readTimeout: 30s\nrtsp: true\nrtspAddress: :18554\nrtspTransports: [udp]\n"
                          "rtpAddress: :18000\nrtcpAddress: :18001\nrtmp: false\nhls: false\n"
                          "webrtc: false\nsrt: false\nmoq: false\npaths:\n  all_others:\n    source: publisher\n")
        with (root / "server.log").open("w+") as server_log, (root / "source.log").open("w+") as source_log:
            def publish():
                return subprocess.Popen([
                    "ffmpeg", "-hide_banner", "-loglevel", "warning", "-re", "-f", "lavfi",
                    "-i", "testsrc2=size=160x120:rate=10", "-re", "-f", "lavfi",
                    "-i", "sine=frequency=440:sample_rate=16000", "-map", "0:v", "-map", "1:a",
                    "-c:v", "mjpeg", "-threads", "1", "-pix_fmt", "yuvj420p", "-q:v", "6",
                    "-huffman", "default", "-force_duplicated_matrix", "1", "-c:a", "pcm_s16be",
                    "-ar", "16000", "-ac", "1", "-f", "rtsp", "-rtsp_transport", "udp",
                    "rtsp://127.0.0.1:18554/hil"], stdout=source_log, stderr=subprocess.STDOUT)
            try:
                server = subprocess.Popen([args.mediamtx, str(config)], stdout=server_log, stderr=subprocess.STDOUT)
                time.sleep(1)
                if server.poll() is not None:
                    raise RuntimeError("Isolated MediaMTX failed to start; check port conflicts")
                source = publish()
                time.sleep(2)
                if source.poll() is not None:
                    raise RuntimeError("FFmpeg publisher failed")
                console = Console(args.telnet)
                console.command("version")
                console.command("board")
                sessions = console.command("session list")
                if not re.search(r"^0\s+shell\s+shell\s+active\s+display", sessions, re.M):
                    raise RuntimeError("Display session 0 is not an idle shell; leave the existing app untouched")
                baseline = console.snapshot("baseline")
                if re.search(r"rtspd\s+(running|starting|waiting)", baseline["job status rtspd"]):
                    raise RuntimeError("rtspd already active; leave the existing job untouched")
                for cycle in range(args.cycles):
                    mode = "--audio-only " if cycle % 2 else ""
                    playback_started = True
                    console.command(f"session send 0 rtsp --stats {mode}rtsp://{args.host_address}:18554/hil")
                    time.sleep(args.seconds / 2)
                    active = console.snapshot(f"active {cycle + 1}")
                    assert "rtsp-net" in active["top"] and "rtsp-sink" in active["top"], "Playback workers missing"
                    if not mode:
                        assert "rtsp-jpeg" in active["top"], "JPEG decoder missing"
                    if args.interrupt:
                        if cycle % 2:
                            os.kill(source.pid, signal.SIGSTOP)
                        else:
                            stop(source)
                            source = None
                        time.sleep(7)
                        console.snapshot(f"outage {cycle + 1}")
                        if source:
                            os.kill(source.pid, signal.SIGCONT)
                        else:
                            source = publish()
                        time.sleep(12)
                        recovered = console.snapshot(f"recovered {cycle + 1}")
                        assert "rtsp-net" in recovered["top"] and "rtsp-sink" in recovered["top"], "Automatic reconnect failed"
                    time.sleep(args.seconds / 2)
                    logs = console.command("log show 200")
                    blocks = re.findall(r"blocks/s=(\d+)", logs)
                    assert blocks and int(blocks[-1]) > 0, "No recent audio output"
                    if args.interrupt:
                        epochs = re.findall(r"epoch=(\d+)", logs)
                        assert epochs and int(epochs[-1]) > 1, "Missing reconnect epoch evidence"
                    console.command("input emit q")
                    time.sleep(2)
                    playback_started = False
                    released = console.snapshot(f"released {cycle + 1}")
                    assert not re.search(r"rtsp-(net|sink|jpeg|audio)", released["top"]), "Media worker leaked after exit"
                    for endpoint in ("audio0.playback", "audio0.capture"):
                        row = re.search(rf"^{re.escape(endpoint)}\s+[^\r\n]+", released["stream list"], re.M)
                        assert row and row[0].rstrip().endswith("-"), "Audio ownership leaked after exit"
                    if args.capture:
                        capture_started = True
                        console.command("job start rtspd video=none audio=audio0.capture port=18555")
                        receiver = subprocess.Popen([
                            "ffmpeg", "-hide_banner", "-loglevel", "warning", "-rtsp_transport", "udp",
                            "-i", f"rtsp://{args.telnet}:18555/media", "-t", "10", "-f", "null", "-"],
                            stdout=source_log, stderr=subprocess.STDOUT)
                        time.sleep(3)
                        capture = console.snapshot(f"capture {cycle + 1}")
                        assert re.search(r"^rtspd\s", capture["top"], re.M) and "rtsp-audio" in capture["top"], "Capture workers missing/admission denied"
                        assert receiver.wait(timeout=20) == 0, "Native microphone RTP/L16 reception failed"
                        receiver = None
                        console.command("job stop rtspd")
                        capture_started = False
                        time.sleep(2)
                        closed = console.snapshot(f"capture released {cycle + 1}")
                        assert not re.search(r"^rtspd\s|rtsp-audio", closed["top"], re.M), "Capture worker leaked"
                print("HIL PLAYBACK SOAK PASS", flush=True)
            finally:
                if console:
                    try:
                        if capture_started:
                            console.command("job stop rtspd")
                        if playback_started:
                            console.command("input emit q")
                    except (OSError, RuntimeError) as error:
                        print("Device cleanup warning:", error, flush=True)
                    finally:
                        console.socket.close()
                stop(source)
                stop(receiver)
                stop(server)
                for name, handle in (("SERVER", server_log), ("SOURCE", source_log)):
                    handle.seek(0)
                    print(name, handle.read(), flush=True)


if __name__ == "__main__":
    main()
