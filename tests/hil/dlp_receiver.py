#!/usr/bin/env python3
"""Test an installed DLP receiver using a separate network console and serial stream.

Requires pyserial and Pillow. Leaves the receiver stopped and restores the
previous display session. Protocol completion is not a physical screen capture.
"""
import argparse
from pathlib import Path
import re
import secrets
import struct
import sys
import time

import serial
from model_bundle_stream import Console

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from dlp_image import DisplayLink, demo_frames


def send_rgb565(link, width, height):
    """Exercise the advertised color path independently of the MONO1 sender."""
    descriptor = link.command(1).payload
    if not struct.unpack_from("<I", descriptor, 8)[0] & 16:
        return False
    link.session = secrets.randbelow(0xffffffff) + 1
    link.command(2, struct.pack("<HH", 4, 0))
    link.command(0x10, struct.pack("<II", 1, 1))
    link.command(0x11, struct.pack("<IIHHHH", 1, 1, 0, 0, width, height))
    colors = (0xffff, 0xf800, 0x07e0, 0x001f, 0xffe0, 0x07ff, 0xf81f, 0)
    row = b"".join(struct.pack(">H", colors[min(7, x * 8 // width)]) for x in range(width))
    image = row * height
    for offset in range(0, len(image), 208):
        link.command(0x12, struct.pack("<III", 1, 1, offset) + image[offset:offset + 208])
    link.command(0x13, struct.pack("<I", 1))
    deadline = time.monotonic() + link.present_timeout
    while time.monotonic() < deadline:
        status = link.command(3).payload
        assert not struct.unpack_from("<H", status, 2)[0], status
        if struct.unpack_from("<I", status, 8)[0] == 1:
            return True
        time.sleep(.01)
    raise TimeoutError("RGB565 presentation did not complete")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--stream", default="cdc0")
    parser.add_argument("--display", default="display0")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    console = Console(args.host)
    logs = []
    running = False

    def command(text):
        output = console.command(text, 10)
        logs.append(text + "\n" + output)
        print(text + "\n" + output, flush=True)
        return output

    try:
        command("version")
        command("board")
        command("session list")
        command("mem policy")
        assert "DLP: stopped" in command("dlp status"), "A receiver is already running"
        assert "required" in command(f"dlp serve {args.display} stream=battery")
        assert "DLP: stopped" in command("dlp status")
        for compact in (True, False):
            output = command(f"dlp serve {args.display} stream={args.stream}")
            assert re.search(r"DLP session \d+:", output), output
            running = True
            # The separate console must remain usable and report stream ownership.
            assert "app-dlp" in command("port list")
            duplicate = command(f"dlp serve {args.display} stream={args.stream}")
            assert "owns" in duplicate, duplicate
            with serial.Serial(args.port, 115200, timeout=.05, write_timeout=2) as port:
                link = DisplayLink(port, compact=compact)
                width, height = link.connect()
                frames = demo_frames(width, height)
                image = next(frames)
                started = time.monotonic()
                link.send_frame(image)
                sent = link.tx_bytes
                # An unchanged image is confirmed without another PRESENT.
                assert link.send_frame(image) == 1
                assert link.tx_bytes - sent <= 256
                link.send_frame(next(frames))
                link.connect()  # New session must repaint even for the same image.
                assert link.send_frame(image) == 1
                logs.append(f"{'compact' if compact else 'fixed'} {width}x{height}: "
                    f"3 presentations in {time.monotonic() - started:.3f}s; "
                    f"TX {link.tx_bytes}, RX {link.rx_bytes}\n")
            # Closing serial alone leaves an endpoint listening for another sender.
            status = command("dlp status")
            assert "frames=3" in status, status
            # Pause behind another display session, then restore the receiver.
            output = command(f"session create clock {args.display}")
            match = re.search(r"session (\d+) created", output)
            assert match, output
            try:
                assert "suspended" in command("dlp status")
            finally:
                command("session close " + match[1])
            assert "serving" in command("dlp status")
            with serial.Serial(args.port, 115200, timeout=.05, write_timeout=2) as port:
                link = DisplayLink(port, compact=compact)
                link.connect()
                link.send_frame(image)
                color = compact and send_rgb565(link, width, height)
            assert f"frames={5 if color else 4}" in command("dlp status")
            command(f"dlp stop {args.display}")
            running = False
            assert "DLP: stopped" in command("dlp status")
            ports = command("port list")
            assert "app-dlp" not in ports, ports
        command("session list")
        command("mem policy")
        command("log show")
        print("DLP serial receiver lifecycle: PASS", flush=True)
    finally:
        if running:
            command(f"dlp stop {args.display}")
        (args.output / "console.log").write_text("\n".join(logs))
        console.socket.close()


if __name__ == "__main__":
    main()
