+++
id = "vision"
title = "QR vision"
section = "api"
summary = "Read QR payloads and image coordinates from stored images, cameras, or received JPEG frames"
keywords = "vision qr qrcode image camera rtsp crop resize"
packages_any = ["service_vision"]
agent_reference_sections = true
+++
# QR vision

[Python media API](python.media.md) · [Lua media API](lua.media.md)

## Quick reference

- Enable the `vision` group (`service.vision`) on a board with PSRAM. The
  `full` flavor enables it on supported boards. A camera or display is optional.
- `solaros.vision.qrcodes(image[, options])` reads a native `solaros.image`
  handle and returns QR payloads, four corners, and processing statistics.
- Options: `x`, `y`, `width`, `height`, `output_width`, `output_height`.
- Processing dimensions are limited to 640 by 480. At most eight decoded codes
  are returned. `truncated` indicates additional decoded codes were omitted.
- Python payloads are `bytes`; Lua payloads are binary strings. Inspect `eci`
  before interpreting a payload as text.
- Decode a camera/RTSP frame with `image.from_frame`, then release the frame
  before QR processing. Close the decoded image when finished.

## Images and regions

Use `solaros.image.open(path)` for a stored JPEG, PNG, GIF, or WebP image, or
`solaros.image.from_frame(frame)` for a leased camera/RTSP JPEG. QR processing
uses the decoded image without modifying its pixels. The image remains valid
after processing and can also be drawn or presented.

`x` and `y` select the crop origin, defaulting to zero. Zero or omitted `width`
and `height` select the remaining image from that origin. The crop must fit
within the image. Zero or omitted output dimensions select the crop dimensions.
Larger images require an explicit crop or resize to fit the processing limit.
Resize uses nearest-neighbour sampling; preserve aspect ratio for best results.

Corners are reported in original-image coordinates, including the crop offset
and inverse resize. Their order follows the QR orientation: top left, top right,
bottom right, bottom left. For mirrored codes, orientation reflects the decoder's
original corner order. Python uses tuples of `(x, y)` pairs; Lua uses arrays of
two-element arrays. Payloads do not execute any action automatically.

## Results and limits

The returned dictionary/table contains:

| Field | Meaning |
| --- | --- |
| `codes` | Decoded QR entries; empty when no valid code is found |
| `width`, `height` | Original image dimensions |
| `processed_width`, `processed_height` | Dimensions passed to recognition |
| `candidates` | Recognized QR candidates, including unsuccessful decodes |
| `decode_failures` | Candidates whose payload could not be decoded before the result limit |
| `truncated` | A further valid code was found after eight results |
| `preprocess_us` | Recognition-buffer allocation and grayscale conversion time |
| `detect_us` | Recognition time, including cooperative scheduling |
| `decode_us` | Payload decode and result-copy time |
| `elapsed_us` | Total native processing time, excluding source decode and task launch/cleanup |

Each QR entry contains `payload`, `corners`, `version`, `ecc_level`, `data_type`,
and `eci`. Payload bytes and metadata are copied into the interpreter and remain
valid after the image is closed. ECC values are M=0, L=1, H=2, Q=3; data types
include numeric=1, alphanumeric=2, byte=4, and Kanji=8.

Only one QR request runs at a time. A competing request returns a busy error.
Allocation failures return an error without falling back to internal RAM for
QR buffers. Processing uses a five-second cooperative deadline and checks app
cancellation. The call waits for the worker to stop before releasing its memory;
the deadline is not a hard preemption guarantee for an individual decode step.
The native worker does not call an interpreter.

## Python example

```python
import solaros

picture = solaros.image.open("/qr.jpg")
try:
    result = solaros.vision.qrcodes(picture)
    for code in result["codes"]:
        print(code["payload"], code["corners"])
finally:
    solaros.image.close(picture)
```

For a camera frame, retain the capture timestamp separately so that a result can
be associated with its source:

```python
frame = solaros.camera.snapshot()
try:
    timestamp = solaros.streams.frame_info(frame)["timestamp_us"]
    picture = solaros.image.from_frame(frame)
finally:
    solaros.streams.release_frame(frame)

try:
    result = solaros.vision.qrcodes(picture)
    print(timestamp, result["codes"])
finally:
    solaros.image.close(picture)
```

For repeated captures, open the source once, acquire/decode/release one frame,
process it, close its image, and call `solaros.time.sleep_ms` between iterations.
Do not collect an unbounded queue of frames or decoded images. A script cannot
open the exclusive local camera while `rtspd` or `cam-webd` owns it.

## Lua example

```lua
local picture = solaros.image.open("/qr.jpg")
local ok, result = pcall(solaros.vision.qrcodes, picture)
solaros.image.close(picture)
if not ok then error(result) end
for _, code in ipairs(result.codes) do
    print(code.payload, code.corners[1][1], code.corners[1][2])
end
```

The `vision` module follows its native package gate. Enabling Python or Lua
alone does not enable vision. Neural inference is not included in this API.
