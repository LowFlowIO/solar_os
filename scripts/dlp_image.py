#!/usr/bin/env python3
"""Send test images to an already running SolarOS DLP display receiver.

Start `dlp serve display0 stream=cdc0` from a separate device console first.
Install pyserial and Pillow on the desktop. This sender uses MONO1 and can
exercise changed rectangles, RLE, exact retries and fresh-session repaint.

The client is shared in origin with pico_display_link/scripts/display_link.py.
"""
import argparse
from dataclasses import dataclass
import itertools
from pathlib import Path
import secrets
import struct
import sys
import time
import zlib

CELL_SIZE = 256
OK, BUSY, BAD_SESSION, STALE_BOOT = 0, 4, 5, 6
GET_INFO, CONFIGURE, GET_STATUS = 1, 2, 3
GET_EXTENSIONS, GET_SYNC, RECT_RLE = 4, 5, 0x15
PRESERVE_CONTENT, RLE, SYNC = 1, 32, 64
BEGIN_FRAME, BEGIN_RECT, RECT_DATA, PRESENT, ABORT_FRAME = range(0x10, 0x15)
HEADER = struct.Struct("<4sBBBBIIHHQ4x")


class ProtocolError(RuntimeError):
    def __init__(self, opcode, result):
        super().__init__(f"DLP command 0x{opcode:02x} returned result {result}")
        self.result = result


@dataclass
class Cell:
    kind: int
    opcode: int
    session: int
    sequence: int
    result: int
    boot: int
    payload: bytes


def encode_cell(opcode, session, sequence, boot, payload=b"", compact=False):
    if len(payload) > 220 or not 0 < sequence <= 0xffffffff:
        raise ValueError("Invalid SDLP request")
    packet = bytearray(CELL_SIZE)
    HEADER.pack_into(packet, 0, b"SDLP", 1, 0, 1, opcode, session, sequence,
                     0, len(payload), boot)
    packet[32:32 + len(payload)] = payload
    if compact:
        packet = packet[:32 + len(payload)]
        packet[:4] = b"DLP1"
        return bytes(packet) + struct.pack("<I", zlib.crc32(packet))
    struct.pack_into("<I", packet, 252, zlib.crc32(packet[:252]))
    return bytes(packet)


def decode_cell(packet):
    if len(packet) < 36 or len(packet) > CELL_SIZE:
        raise ValueError("Wrong cell length")
    magic, version, reserved, kind, op, session, seq, result, n, boot = HEADER.unpack_from(packet)
    compact = magic == b"DLP1"
    expected = 36 + n if compact else CELL_SIZE
    if len(packet) != expected:
        raise ValueError("Wrong cell length")
    crc, = struct.unpack_from("<I", packet, len(packet) - 4)
    if (magic not in (b"SDLP", b"DLP1") or version != 1 or reserved or kind > 2 or n > 220
            or any(packet[28:32]) or (not compact and any(packet[32 + n:252]))
            or crc != zlib.crc32(packet[:-4]) or (kind and not seq)
            or (kind == 0 and (op or seq or result or n))
            or (kind == 1 and result)):
        raise ValueError("Invalid SDLP cell")
    return Cell(kind, op, session, seq, result, boot, packet[32:32 + n])


def rle_encode(data, capacity=206):
    output = bytearray()
    at = 0
    while at < len(data) and len(output) + 2 <= capacity:
        run = 1
        while at + run < len(data) and run < 130 and data[at + run] == data[at]:
            run += 1
        if run >= 3:
            output.extend((128 + run - 3, data[at]))
            at += run
        else:
            start = at
            while True:
                at += 1
                if (at == len(data) or at - start == 128
                        or at - start == capacity - len(output) - 1):
                    break
                if at + 2 < len(data) and data[at] == data[at + 1] == data[at + 2]:
                    break
            output.append(at - start - 1)
            output.extend(data[start:at])
    return bytes(output), at


def changed_rects(before, after, width, height):
    stride = (width + 7) // 8
    rectangles = []
    for y in range(0, height, 8):
        end = min(y + 8, height)
        columns = [x for x in range(stride) if any(
            before[row * stride + x] != after[row * stride + x] for row in range(y, end))]
        if columns:
            left, right = min(columns) * 8, min(width, (max(columns) + 1) * 8)
            rectangles.append((left, y, right - left, end - y))
    return rectangles


class DisplayLink:
    """Stop-and-wait DLP client; port supplies ordered read/write operations.

    Prefer compact framing and fall back to fixed v1 cells on discovery.
    Serial ports, USB CDC, socket/file adapters and in-process pipes all use
    the same client. Serialize calls; retained image state is per session.
    """
    def __init__(self, port, timeout=2.0, compact=True):
        self.port = port
        self.timeout = timeout
        self.buffer = bytearray()
        self.sequence = self.boot = self.session = self.frame = 0
        self.width = self.height = 0
        self.present_timeout = 30.0
        self.compact = compact
        self.capabilities = 0
        self.previous = None
        self.tx_bytes = self.rx_bytes = 0
        self.max_rect_bytes = 0

    def _receive(self, deadline):
        while time.monotonic() < deadline:
            starts = [i for magic in (b"SDLP", b"DLP1")
                      if (i := self.buffer.find(magic)) >= 0]
            index = min(starts) if starts else -1
            if index < 0:
                self.buffer[:] = self.buffer[-3:]
            elif index:
                del self.buffer[:index]
            length = CELL_SIZE
            if self.buffer.startswith(b"DLP1") and len(self.buffer) >= 32:
                length = 36 + struct.unpack_from("<H", self.buffer, 18)[0]
                if length > CELL_SIZE:
                    del self.buffer[0]
                    continue
            if len(self.buffer) >= length:
                try:
                    cell = decode_cell(self.buffer[:length])
                except ValueError:
                    del self.buffer[0]
                    continue
                del self.buffer[:length]
                return cell
            waiting = getattr(self.port, "in_waiting", 0)
            data = self.port.read(max(1, min(CELL_SIZE, waiting)))
            self.rx_bytes += len(data)
            self.buffer.extend(data)
        raise TimeoutError("Timed out waiting for a display response")

    def command(self, opcode, payload=b"", attempts=3):
        self.sequence += 1
        discovery = opcode == GET_INFO
        session = 0 if discovery else self.session
        packet = encode_cell(opcode, session, self.sequence,
                             0 if discovery else self.boot, payload, self.compact)
        # Retries retain the same sequence and bytes, so device replay prevents
        # duplicate writes or a second PRESENT after a lost response.
        for attempt in range(attempts):
            self.buffer.clear()  # Discard incomplete replies before exact retry.
            deadline = time.monotonic() + self.timeout
            offset = 0
            while offset < len(packet):
                n = self.port.write(packet[offset:])
                if n is None or n < 0 or n > len(packet) - offset:
                    raise OSError("Invalid display stream write result")
                if time.monotonic() >= deadline:
                    raise TimeoutError("Display stream write stalled")
                if not n:
                    time.sleep(0.001)
                    continue
                offset += n
                self.tx_bytes += n
            try:
                while True:
                    answer = self._receive(deadline)
                    if (answer.kind == 2 and answer.opcode == opcode
                            and answer.sequence == self.sequence
                            and answer.session == session):
                        if answer.result != OK:
                            raise ProtocolError(opcode, answer.result)
                        if not discovery and answer.boot != self.boot:
                            raise RuntimeError("Peripheral restarted; reconnect the display client")
                        return answer
            except TimeoutError:
                if attempt == attempts - 1:
                    raise

    def connect(self):
        self.previous = None
        self.frame = 0
        original_timeout = self.timeout
        try:
            if self.compact:
                self.timeout = min(original_timeout, 0.4)
            answer = self.command(GET_INFO, attempts=1 if self.compact else 3)
        except TimeoutError:
            if not self.compact:
                raise
            self.compact = False
            self.buffer.clear()
            self.timeout = original_timeout
            answer = self.command(GET_INFO)
        finally:
            self.timeout = original_timeout
        p = answer.payload
        if len(p) != 128 or struct.unpack_from("<H", p)[0] != 128:
            raise ValueError("Invalid display descriptor")
        self.width, self.height, formats = struct.unpack_from("<HHI", p, 4)
        if not self.width or not self.height or not formats & 1:
            raise ValueError("This client requires a display supporting MONO1")
        self.present_timeout = struct.unpack_from("<I", p, 36)[0] / 1000 + 2
        self.capabilities = struct.unpack_from("<I", p, 16)[0]
        self.max_rect_bytes = struct.unpack_from("<I", p, 28)[0]
        if self.max_rect_bytes < (self.width + 7) // 8:
            raise ValueError("Invalid rectangle limit")
        self.boot = answer.boot
        self.session = secrets.randbelow(0xffffffff) + 1
        until = time.monotonic() + self.present_timeout
        while True:
            try:
                self.command(CONFIGURE, struct.pack("<HH", 0, 0))
                break
            except ProtocolError as error:
                if error.result != BUSY or time.monotonic() >= until:
                    raise
                time.sleep(0.01)
        try:
            extensions = self.command(GET_EXTENSIONS).payload
            if len(extensions) != 4 or struct.unpack("<I", extensions)[0] & ~(RLE | SYNC):
                raise ValueError("Invalid DLP extensions")
            self.capabilities |= struct.unpack("<I", extensions)[0]
        except ProtocolError as error:
            if error.result != 1:
                raise
        return self.width, self.height

    def send_frame(self, pixels, wait_timeout=10.0):
        stride = (self.width + 7) // 8
        if not self.width or len(pixels) != stride * self.height:
            raise ValueError("Frame size does not match the discovered geometry")
        if self.width % 8:
            mask = 0xff << (self.width % 8)
            if any(pixels[y * stride + stride - 1] & mask for y in range(self.height)):
                raise ValueError("Nonzero row-padding bits")
        try:
            return self._upload_frame(pixels, stride, wait_timeout)
        except BaseException:
            self.previous = None
            raise

    def _upload_frame(self, pixels, stride, wait_timeout):
        whole = [(0, 0, self.width, self.height)]
        full = self.previous is None or not self.capabilities & PRESERVE_CONTENT
        rectangles = whole if full else changed_rects(
            self.previous, pixels, self.width, self.height)
        if not rectangles:
            if self.capabilities & SYNC:
                sync = self.command(GET_SYNC).payload
                if len(sync) != 4 or struct.unpack("<I", sync)[0] > 1:
                    raise ValueError("Invalid retained-content status")
                if not struct.unpack("<I", sync)[0]:
                    return self.frame
            full, rectangles = True, whole

        def bands(rectangles):
            for x, y, width, height in rectangles:
                row = (width + 7) // 8
                count = self.max_rect_bytes // row
                for offset in range(0, height, count):
                    yield x, y + offset, width, min(count, height - offset)

        def cost(rectangles):
            return sum(1 + (((w + 7) // 8 * h + 207) // 208)
                       for x, y, w, h in bands(rectangles))

        if not full and cost(rectangles) >= cost(whole):
            full, rectangles = True, whole
        self.frame += 1
        f = self.frame
        until = time.monotonic() + wait_timeout
        while True:
            try:
                self.command(BEGIN_FRAME, struct.pack("<II", f, int(full)))
                break
            except ProtocolError as error:
                # A second writer or reset can invalidate retained content.
                if not full and error.result in (2, 3):
                    full, rectangles = True, whole
                    continue
                if error.result != BUSY or time.monotonic() >= until:
                    raise
                time.sleep(0.01)
        try:
            for rect, (x, y, width, height) in enumerate(bands(rectangles), 1):
                self.command(BEGIN_RECT, struct.pack("<IIHHHH", f, rect, x, y, width, height))
                row = (width + 7) // 8
                data = b"".join(pixels[(y + k) * stride + x // 8:
                                       (y + k) * stride + x // 8 + row]
                                for k in range(height))
                offset = 0
                while offset < len(data):
                    raw = min(208, len(data) - offset)
                    used = raw
                    op, payload = RECT_DATA, data[offset:offset + raw]
                    if self.capabilities & RLE:
                        encoded, consumed = rle_encode(data[offset:offset + 1024])
                        if consumed > raw or (consumed == raw and len(encoded) + 2 < raw):
                            op, used = RECT_RLE, consumed
                            payload = struct.pack("<H", consumed) + encoded
                    self.command(op, struct.pack("<III", f, rect, offset) + payload)
                    offset += used
            self.command(PRESENT, struct.pack("<I", f))
            until = time.monotonic() + self.present_timeout
            while time.monotonic() < until:
                p = self.command(GET_STATUS).payload
                if len(p) != 24:
                    raise ValueError("Invalid status response")
                error = struct.unpack_from("<H", p, 2)[0]
                completed, failed = struct.unpack_from("<II", p, 8)
                if failed == f or p[0] == 4:
                    raise RuntimeError(f"Frame {f} failed, device error {error}")
                if completed == f:
                    self.previous = bytes(pixels)
                    return f
                time.sleep(0.005)
            raise TimeoutError(f"Frame {f} did not finish presenting")
        except BaseException:
            try:
                self.command(ABORT_FRAME, struct.pack("<I", f))
            except (OSError, RuntimeError, TimeoutError):
                pass
            raise


USBDisplay = DisplayLink  # Existing producers remain source compatible.


def pack_image(image, width, height):
    """Fit onto a white canvas and pack horizontal MONO1, black=1, LSB first."""
    from PIL import Image, ImageOps
    fitted = ImageOps.pad(image.convert("L"), (width, height),
                         method=Image.Resampling.LANCZOS, color=255).convert("1")
    stride = (width + 7) // 8
    output = bytearray(stride * height)
    pixels = fitted.load()
    for y in range(height):
        for x in range(width):
            if not pixels[x, y]:
                output[y * stride + x // 8] |= 1 << (x % 8)
    return bytes(output)


def image_frames(paths, width, height, loop):
    from PIL import Image, ImageSequence
    while True:
        for path in paths:
            with Image.open(path) as image:
                for frame in ImageSequence.Iterator(image):
                    yield pack_image(frame, width, height)
        if not loop:
            return


def demo_frames(width, height):
    from PIL import Image, ImageDraw
    for number in itertools.count():
        image = Image.new("1", (width, height), 1)
        draw = ImageDraw.Draw(image)
        draw.rectangle((0, 0, width - 1, height - 1), outline=0)
        draw.text((6, 5), "DISPLAY LINK", fill=0)
        draw.text((6, 20), f"{width} x {height}  FRAME {number + 1}", fill=0)
        x = 2 + (number * 7) % max(1, width - 20)
        draw.rectangle((x, max(34, height - 22), x + 12, height - 4), fill=0)
        yield pack_image(image, width, height)


def raw_frames(stream, size):
    while True:
        frame = bytearray()
        while len(frame) < size:
            data = stream.read(size - len(frame))
            if not data:
                if frame:
                    raise ValueError(f"Truncated raw frame: {len(frame)} of {size} bytes")
                return
            frame.extend(data)
        yield bytes(frame)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate")
    parser.add_argument("--legacy-cells", action="store_true", help="Use fixed 256-byte framing")
    parser.add_argument("--stats", action="store_true", help="Print actual transmitted/received bytes per frame")
    parser.add_argument("--fps", type=float, default=2.0, help="Maximum frame rate (default: 2)")
    parser.add_argument("--frames", type=int, default=0, help="Stop after N frames; 0 means unlimited")
    parser.add_argument("--loop", action="store_true", help="Repeat input images")
    parser.add_argument("--reconnect-every", type=int, default=0,
                        help="Start a fresh DLP session every N frames to test full repaint")
    parser.add_argument("--demo", action="store_true", help="Generate an animated test screen")
    parser.add_argument("--raw", metavar="PATH", help="Packed MONO1 frames; '-' reads stdin")
    parser.add_argument("images", nargs="*", type=Path, help="Images/animated GIFs in playback order")
    args = parser.parse_args()
    if (args.fps <= 0 or args.frames < 0 or args.baud <= 0 or args.reconnect_every < 0
            or sum((bool(args.images), args.demo, args.raw is not None)) != 1
            or (args.loop and not args.images)):
        parser.error("Choose images, --demo, or --raw; --loop requires images and --fps must be positive")
    try:
        import serial
        from PIL import Image  # noqa: F401 -- fail before opening the device
    except ImportError as error:
        parser.exit(1, f"{error}. Install pyserial and Pillow first.\n")
    raw = None
    try:
        print(f"Opening Display Link on {args.port}...", file=sys.stderr, flush=True)
        with serial.Serial(args.port, args.baud, timeout=0.05, write_timeout=2) as port:
            print("Connected to serial port; discovering display...", file=sys.stderr, flush=True)
            link = DisplayLink(port, compact=not args.legacy_cells)
            width, height = link.connect()
            print(f"DLP display: {width}x{height} MONO1, "
                  f"{'compact' if link.compact else 'fixed cells'}", file=sys.stderr)
            if args.demo:
                frames = demo_frames(width, height)
            elif args.raw:
                raw = sys.stdin.buffer if args.raw == "-" else open(args.raw, "rb")
                frames = raw_frames(raw, ((width + 7) // 8) * height)
            else:
                frames = image_frames(args.images, width, height, args.loop)
            for count, frame in enumerate(frames, 1):
                if args.reconnect_every and count > 1 and (count - 1) % args.reconnect_every == 0:
                    link.connect()
                started = time.monotonic()
                sent, received, previous = link.tx_bytes, link.rx_bytes, link.frame
                frame_id = link.send_frame(frame)
                if args.stats:
                    print(f"Frame {count}: TX {link.tx_bytes - sent}, RX {link.rx_bytes - received} bytes"
                          f"{' (unchanged)' if previous == frame_id else ''}", file=sys.stderr, flush=True)
                if count == 1 or count % 10 == 0 or count == args.frames:
                    print(f"Presented frame {frame_id}", file=sys.stderr, flush=True)
                if args.frames and count >= args.frames:
                    break
                time.sleep(max(0, 1 / args.fps - (time.monotonic() - started)))
    except KeyboardInterrupt:
        return 130
    except (OSError, ValueError, RuntimeError) as error:
        parser.exit(1, f"{error}\n")
    finally:
        if raw is not None and raw is not sys.stdin.buffer:
            raw.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
