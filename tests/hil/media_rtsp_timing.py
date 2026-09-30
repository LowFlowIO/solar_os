#!/usr/bin/env python3
"""Check live RTP/JPEG pacing and reconnect timing (requires running mediad).

Example: python3 tests/hil/media_rtsp_timing.py rtsp://192.168.1.238/media
The test occupies the single receiver slot, sends TEARDOWN, waits, and reconnects.
"""

import argparse
import re
import select
import socket
import struct
import time
from urllib.parse import urlsplit


def udp_pair():
    for _ in range(128):
        rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        rtcp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            rtp.bind(("0.0.0.0", 0))
            port = rtp.getsockname()[1]
            if port % 2 or port == 65535:
                continue
            rtcp.bind(("0.0.0.0", port + 1))
            rtp.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024)
            return rtp, rtcp, port
        except OSError:
            pass
        finally:
            if rtcp.getsockname()[1] == 0:
                rtp.close()
                rtcp.close()
    raise RuntimeError("Could not bind an adjacent UDP port pair")


class Session:
    def __init__(self, url):
        parsed = urlsplit(url)
        if parsed.scheme != "rtsp" or not parsed.hostname:
            raise ValueError("Expected an rtsp:// URL")
        self.url = url.rstrip("/") + "/"
        self.session = None
        self.cseq = 0
        self.pending = b""
        self.control = socket.create_connection(
            (parsed.hostname, parsed.port or 554), 3
        )
        self.control.settimeout(3)

    def request(self, method, url=None, extra=""):
        self.cseq += 1
        text = (
            f"{method} {url or self.url} RTSP/1.0\r\n"
            f"CSeq: {self.cseq}\r\n{extra}"
        )
        if self.session:
            text += f"Session: {self.session}\r\n"
        self.control.sendall((text + "\r\n").encode())
        while b"\r\n\r\n" not in self.pending:
            self.read_more()
        end = self.pending.index(b"\r\n\r\n") + 4
        header = self.pending[:end].decode("ascii")
        length_match = re.search(r"Content-Length: (\d+)", header, re.I)
        length = int(length_match[1]) if length_match else 0
        if length > 8192:
            raise RuntimeError("Oversized RTSP body")
        while len(self.pending) < end + length:
            self.read_more()
        body = self.pending[end:end + length].decode("ascii")
        self.pending = self.pending[end + length:]
        if not header.startswith("RTSP/1.0 200 "):
            raise RuntimeError(f"{method} failed: {header}")
        cseq = re.search(r"CSeq: (\d+)", header, re.I)
        if not cseq or int(cseq[1]) != self.cseq:
            raise RuntimeError("RTSP CSeq mismatch")
        match = re.search(r"Session: ([^;\r\n]+)", header, re.I)
        if match:
            self.session = match[1]
        return header, body

    def read_more(self):
        block = self.control.recv(4096)
        if not block:
            raise RuntimeError("RTSP peer disconnected")
        self.pending += block
        if len(self.pending) > 16384:
            raise RuntimeError("Oversized RTSP response")

    def close(self):
        try:
            if self.session:
                self.request("TEARDOWN")
        finally:
            self.control.close()


def check_session(url, seconds):
    session = Session(url)
    rtp = rtcp = None
    try:
        header, sdp = session.request("DESCRIBE", extra="Accept: application/sdp\r\n")
        rate = re.search(r"a=framerate:([\d.]+)", sdp)
        codec = re.search(r"a=rtpmap:(\d+) JPEG/90000", sdp)
        control = re.search(r"a=control:(trackID=\d+)", sdp)
        if not rate or not codec or not control:
            raise RuntimeError("Expected a JPEG/90000 video track and frame rate")
        fps = float(rate[1])
        if not 0 < fps <= 30:
            raise RuntimeError("Unexpected publisher frame rate")
        base = re.search(r"Content-Base: ([^\r\n]+)", header, re.I)
        track_url = (base[1] if base else session.url) + control[1]
        rtp, rtcp, port = udp_pair()
        session.request(
            "SETUP", track_url,
            f"Transport: RTP/AVP/UDP;unicast;client_port={port}-{port + 1}\r\n",
        )
        play, _ = session.request("PLAY")
        info = re.search(r"rtptime=(\d+)", play)
        if not info:
            raise RuntimeError("Missing PLAY RTP clock origin")
        origin = int(info[1])
        start = time.monotonic()
        frames = []
        last_seq = None
        gaps = 0
        complete = False
        timestamp = None
        first_timestamp = None
        packets = 0
        reports = 0
        while time.monotonic() - start < seconds:
            ready, _, _ = select.select([rtp, rtcp], [], [], .1)
            for source in ready:
                data, _ = source.recvfrom(65536)
                if source is rtcp:
                    reports += int(len(data) >= 28 and data[1] == 200)
                    continue
                if len(data) < 20 or (data[1] & 127) != int(codec[1]):
                    continue
                packets += 1
                sequence, stamp = struct.unpack("!HI", data[2:8])
                if first_timestamp is None:
                    first_timestamp = stamp
                gap = last_seq is not None and sequence != (last_seq + 1) % 65536
                gaps += int(gap)
                last_seq = sequence
                if stamp != timestamp:
                    timestamp = stamp
                    complete = int.from_bytes(data[13:16], "big") == 0
                elif gap:
                    complete = False
                if data[1] & 128 and complete:
                    frames.append((stamp, time.monotonic() - start))
        if len(frames) < max(3, int(seconds * fps * .6)):
            raise AssertionError(f"Only {len(frames)} complete frames in {seconds}s")
        initial_ms = ((first_timestamp - origin) % 2**32) / 90
        if initial_ms > 1000:
            raise AssertionError(f"First RTP frame is {initial_ms:.1f}ms after PLAY")
        deltas = [
            ((current[0] - previous[0]) % 2**32) / 90000
            for previous, current in zip(frames, frames[1:])
        ]
        max_gap = max(deltas)
        if not all(0 < delta <= max(1, 4 / fps) for delta in deltas):
            raise AssertionError(f"RTP frame clock jumps: max gap {max_gap:.3f}s")
        media_elapsed = ((frames[-1][0] - frames[0][0]) % 2**32) / 90000
        wall_elapsed = frames[-1][1] - frames[0][1]
        if abs(media_elapsed - wall_elapsed) > .5:
            raise AssertionError(
                f"Media clock elapsed {media_elapsed:.3f}s, wall {wall_elapsed:.3f}s"
            )
        if seconds >= 6 and not reports:
            raise AssertionError("No RTCP sender report received")
        print(
            f"PASS: {len(frames)} frames/{seconds:g}s, {fps:g} fps configured, "
            f"first RTP offset {initial_ms:.1f}ms, max gap {max_gap:.3f}s, "
            f"clock drift {media_elapsed - wall_elapsed:.3f}s, "
            f"{packets} packets, {gaps} sequence gaps, {reports} RTCP reports",
            flush=True,
        )
    finally:
        try:
            session.close()
        finally:
            if rtp:
                rtp.close()
            if rtcp:
                rtcp.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("url")
    parser.add_argument("--seconds", type=float, default=6)
    parser.add_argument("--idle-seconds", type=float, default=2)
    args = parser.parse_args()
    if args.seconds < 3 or not 0 <= args.idle_seconds <= 30:
        parser.error("Use at least 3 seconds per session and an idle wait of 0..30s")
    check_session(args.url, args.seconds)
    time.sleep(args.idle_seconds)
    check_session(args.url, args.seconds)


if __name__ == "__main__":
    main()
