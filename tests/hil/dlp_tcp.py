#!/usr/bin/env python3
"""Test DLP over a raw Wi-Fi TCP stream, with Telnet kept for control."""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import time

from model_bundle_stream import Console
from dlp_receiver import send_rgb565

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from dlp_image import DisplayLink, TcpStream, demo_frames


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=7777)
    parser.add_argument("--stream", default="dlptcp")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    deadline = time.monotonic() + 30
    while True:
        try:
            console = Console(args.host)
            break
        except OSError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(.5)
    logs = []
    created = running = False

    def command(text):
        output = console.command(text, 10)
        logs.append(text + "\n" + output)
        print(text + "\n" + output, flush=True)
        return output

    def wait_stopped():
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            status = command("dlp status")
            if "DLP: stopped" in status:
                return status
            time.sleep(.05)
        raise AssertionError("TCP EOF did not stop DLP")

    try:
        assert "DLP: stopped" in command("dlp status"), "receiver already running"
        assert "listening on port" in command(f"stream tcp listen {args.stream} {args.port}")
        created = True
        assert "Sharing: exclusive" in command(f"stream status {args.stream}")
        for compact in (True, False):
            assert "DLP session" in command(f"dlp serve display0 stream={args.stream}")
            running = True
            assert "app-dlp" in command("port list")
            assert "removed" not in command(f"stream tcp remove {args.stream}")
            with TcpStream((args.host, args.port)) as stream:
                link = DisplayLink(stream, compact=compact)
                width, height = link.connect()
                frames = demo_frames(width, height)
                first = next(frames)
                link.send_frame(first)
                sent = link.tx_bytes
                link.send_frame(first)
                assert not link.last_frame_stats.presented and link.tx_bytes - sent <= 256
                link.send_frame(next(frames))
                link.connect()
                link.send_frame(first)
                assert "frames=3" in command("dlp status")
                output = command("session create clock display0")
                match = re.search(r"session (\d+) created", output)
                assert match, output
                try:
                    assert "suspended" in command("dlp status")
                finally:
                    command("session close " + match[1])
                assert "Presentation timing: n=3" in command("dlp status")
                link.send_frame(next(frames))
                color = compact and send_rgb565(link, width, height)
                if color:
                    link.connect()
                    link.send_frame(first)
                count = 6 if color else 4
                assert f"frames={count}" in command("dlp status")
            status = wait_stopped()
            running = False
            assert f"Presentation timing: n={count}" in status
            assert "app-dlp" not in command("port list")
        assert "DLP session" in command(f"dlp serve display0 stream={args.stream}")
        running = True
        command("wifi status")
        # Measure a data-only connection; Telnet must not supply the Wi-Fi
        # latency lease on behalf of the TCP stream during this benchmark.
        console.socket.close()
        try:
            result = subprocess.run([sys.executable, "scripts/dlp_image.py", "--tcp",
                f"{args.host}:{args.port}", "--demo", "--fps", "30", "--frames", "100", "--stats"],
                capture_output=True, text=True, timeout=120)
        finally:
            console = Console(args.host)
        (args.output / "sender.log").write_text(result.stdout + result.stderr)
        assert result.returncode == 0, result.stderr
        assert "presented=100 unchanged=0" in result.stderr, result.stderr
        print(result.stderr.splitlines()[-1], flush=True)
        assert "Presentation timing: n=100" in wait_stopped()
        running = False
        command("session list")
        command("log show")
        print("DLP Wi-Fi TCP: compact/fixed, delta, resume, RGB, EOF, restart and 100 frames PASS", flush=True)
    finally:
        try:
            if running:
                command("dlp stop display0")
            if created:
                command(f"stream tcp remove {args.stream}")
        finally:
            (args.output / "console.log").write_text("\n".join(logs))
            console.socket.close()


if __name__ == "__main__":
    main()
