# Hardware checks

These tests use live hardware. They do not flash or change persistent settings.
Run with permission to use the relevant hardware resources. RTSP tests occupy
the publisher/viewer and receiver slots.

## Python RGB export

Upload `image_rgb.py` and a small picture such as
`tests/fixtures/vision/blank.png` to mounted device storage, then run:

```text
python /image_rgb.py /blank.png
```

Requires the image and Python packages. The full RGB picture must fit the
512 KiB Python heap. Checks the actual MicroPython buffer constructor, RGB
length, nearest-neighbor resize coordinates, invalid arguments, allocation
failure and retry, and copy lifetime after closing the native image. A passing
run prints `RGB_PYTHON_OK`. The script releases its image in `finally`.

## Inference execution modes

Upload `inference_modes.py`, a validated integer `.espdl` model with one input,
and its prepared binary input tensor. Run:

```text
python /inference_modes.py /model.espdl /input.bin 3 5
```

Each cycle loads once, compares single/auto/dual execution with one warm-up and
five measured calls per mode, checks identical output hashes, tests deadline
recovery and stale handles, then releases the model. Timings exclude input
preparation, checksums and explicit garbage collection. Observe native `mem`
and `top` before and after interpreter exit to check worker and model release.
Successful completion prints `INFERENCE_MODES_OK`.

## Native image-to-tensor preparation

Upload `image_tensor.py`, `image_tensor.lua`, the reference MobileNetV2 model,
and the classifier's 300×300 `cat.png` sample. Requires image and inference plus
the relevant interpreter. Run:

```text
python /dl/image_tensor.py /dl/models/imagenet/imagenet_cls_mobilenetv2_s8_v1.espdl /dl/cat.png
lua /dl/image_tensor.lua
```

The Lua test uses those model/picture paths directly. Both tests compare every
prepared byte against the interpreter reference, check invalid options and
buffer ownership after image closure, and release the model. The Python test
also checks crop/letterbox geometry and reports a tensor hash and preprocessing
time. Success prints `IMAGE_TENSOR_PYTHON_OK` or `IMAGE_TENSOR_LUA_OK`.

## Resident model-bundle RTSP loop

Install `examples/python/model_bundle.py`, `examples/python/infer.py`, and a
checked image-model bundle on the device. Requires no-auth Telnet, FFmpeg,
MediaMTX, and free host ports 18654/18100/18101. Give the host's LAN address.

```sh
python3 tests/hil/model_bundle_stream.py \
  --telnet 192.168.1.113 --host-address 192.168.1.192 \
  --mediamtx /path/to/mediamtx \
  --bundle /dl/models/pedestrian/bundle.json \
  --output /tmp/model-bundle-stream --count 12 --interrupt
```

The output directory must not exist. The harness starts and stops its own
server and synthetic 160×120 JPEG publisher, verifies the finite result count,
source dimensions and increasing frame timestamps, then optionally cancels
active continuous inference. It records JSON results, device memory/task
snapshots, and process logs; success prints `MODEL_BUNDLE_STREAM_OK`. It checks
worker removal after release. This proves received-stream inference and cleanup;
the synthetic feed does not validate detector accuracy or local camera capture.

## Native viewer and audio-publisher soak

Requires no-auth Telnet, an idle display shell in session 0, FFmpeg and MediaMTX.
Give the host's LAN address, not loopback. The harness owns its child processes
and temporary files, and quits its viewer/stops its capture job on exit. It does
not stop existing host servers or an already-running `rtspd` job. Host ports
18554/18000/18001 and device port 18555 must be free.

```sh
python3 tests/hil/rtsp_playback_soak.py \
  --telnet 192.168.1.124 --host-address 192.168.1.192 \
  --mediamtx /path/to/mediamtx --cycles 3 --seconds 60 --interrupt --capture
```

Cycles alternate JPEG+L16 and audio-only playback. `--interrupt` alternates
stopping/restarting the publisher and pausing/resuming it to exercise control
EOF, missing paths and the five-second media timeout. Checks include renewed
audio output, connection epochs, live workers and their removal on exit.
`--capture` additionally receives microphone L16 through FFmpeg after playback,
covering publisher negotiation, DMA, reader/control stacks and cleanup.
It does not assert simultaneous capture/playback support.

Use larger `--seconds`/`--cycles` for overnight runs. Save stdout to an evidence
log. Snapshots include internal/PSRAM/DMA free bytes, lifetime low-water marks,
largest free blocks, task minimum-free stack bytes and stream owners. Compare
each released snapshot with the same run's baseline. Heap views overlap; task
request totals in `mem policy` are cumulative, not the active stack budget.
These are observations, not a guaranteed maximum under every firmware workload.

## Native camera publisher pacing

Requires an already-running `rtspd` video publisher and its sole receiver slot
to be available:

```sh
python3 tests/hil/media_rtsp_timing.py rtsp://192.168.1.238/media \
  --seconds 60 --cycles 10 --idle-seconds 2
```

Checks complete-frame throughput, RTP sequence gaps, frame timestamps, initial
backlog, clock drift and RTCP reports across explicit teardown/reconnect cycles.
It does not measure decoded presentation latency or implement a video viewer.
