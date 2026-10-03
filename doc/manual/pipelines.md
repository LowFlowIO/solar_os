+++
id = "pipelines"
title = "Native image pipelines"
section = "api"
summary = "OS-owned QR and model processing jobs with bounded latest results and frame timings"
keywords = "pipeline vision qr inference model background job image camera rtsp python lua"
packages_any = ["service_pipeline"]
agent_reference_sections = true
+++
# Native image pipelines

## Quick reference

- `pipeline start qr <source>` creates an OS-owned QR processing job.
- `pipeline start model <source> <handle>` uses a resident image bundle.
- `pipeline result <id>` reads the latest result; `stop` releases workers and
  sources; `destroy` also removes the configuration and latest result.

The `pipeline` service connects an image source to a native processor in an
OS-owned background job. Python and Lua configure and inspect the job; their
sessions can exit while processing continues. QR and model processors share the
same source, cancellation, ownership, timing, and latest-result interface.

Competing lifecycle controls can report busy. If another client takes control
of the public job during startup, a retained configuration remains discoverable
through `pipeline list` and can be explicitly destroyed.

Enable the `pipelines` group and the desired processor package (`vision` for QR,
`inference` for ESP-DL). The `full` flavor includes these groups where supported.
RTSP sources additionally require `service.rtsp-client` and a JPEG video track.

## Start and control

```text
model bundle /dl/models/pedestrian/bundle.json
pipeline start model /dl/pedestrian.png 1
pipeline status 1
pipeline result 1
pipeline destroy 1
model unload 1
```

Use the actual handles returned by `model` and `pipeline`. Sources are absolute
image paths, native video stream IDs listed by `stream list`, or `rtsp://` URLs.
The shell also resolves relative image paths. Files run once when `limit` is
zero; streams run until explicitly stopped. A positive limit selects the number
of successful results. `interval_ms` adds a cancellable delay between frames
(0 to 60000, default 0). Use `job status pipeline-<id>` to inspect ownership and
`job start pipeline-<id>` to restart a retained configuration.

Up to four pipeline records exist until explicitly destroyed. Model processors
require a resident native bundle with exactly one image input. General
multi-input and non-image models remain available through `inference.run` and
`inference.run_bundle`. Stopping or finishing releases the job's reference;
the model stays resident until explicitly unloaded. Unloading a referenced
model reports busy. Jobs waiting for worker memory have not yet retained their
model; unloading it before admission makes the eventual start fail.

Local camera streams follow the provider's ownership rules. An exclusive camera
source cannot be opened while another job owns it. Source failures terminate the
pipeline and appear in `last_error`; native inference or QR admission contention
discards that frame and increments `busy_frames`.

## Python and Lua

Both expose `solaros.pipeline` with the same positional arguments:

```python
from solaros import pipeline

p = pipeline.start("rtsp://192.168.1.10:8554/camera", "model", model_handle)
print(p)  # Processing continues after this script exits.
```

```python
from solaros import pipeline

print(pipeline.list())
print(pipeline.status(p))
result = pipeline.result(p, last_sequence)  # None when no newer result exists.
pipeline.stop(p)       # Keep configuration and latest result.
pipeline.destroy(p)    # Remove both, with idempotent worker cleanup.
```

Lua uses the same calls and returns tables (`nil` when no newer result exists).
`start(source, processor="qr", model=0, limit=0, interval_ms=0)` returns an integer
handle. Arguments are positional. Absolute paths and `./` paths resolve through
the caller's storage context; other strings are interpreted as native stream IDs
or RTSP URLs. Interpreter teardown does not stop pipelines. Stale handles fail.

## Results and statistics

`result` copies the latest record without consuming it. There is no result queue:
slower readers see the newest completed result and can detect skipped sequences.
Each record contains `pipeline`, `sequence`, `source_timestamp_us`, `decode_us`,
`process_us`, `frame_us`, and a processor result. Sequence increases across job
restarts. Source timestamps are capture time for local frames and arrival time
for RTSP frames; files use zero. `frame_us` includes source acquisition, decoding,
and processing including processor serialization, but excludes the configured
interval and timing-envelope serialization.
These timings describe one request and do not assert sustained camera FPS.

The RTSP receiver keeps the latest JPEG, dropping incoming frames while a frame
is leased. The pipeline releases the compressed lease immediately after decoding,
before processing. Each pipeline owns at most one decoded frame and one latest
serialized result. Results are bounded to 64 KiB plus the timing envelope.

QR results include original-image corners, `payload_hex`, payload `length`, ECI,
QR version, ECC level, data type, candidate/failure counts, processed dimensions,
and recognition timings. Hex preserves binary payloads, embedded NUL, and
non-UTF-8 bytes. QR processing scales larger images to fit within 640 by 480,
preserving aspect ratio and returning corners in original-image coordinates.
QR requests use the native five-second timeout.

Model results include the native classification, detections, or raw result,
image transforms, preprocessing, inference, copying, and postprocessing timings.
Requests use a ten-second timeout. Raw outputs include dtype, shape, exponents,
byte count, and `data_hex`; total raw output bytes must not exceed 24000 and the
serialized record must fit the result bound. Larger tensor outputs can be read
through the general inference API.

Stopping cooperatively cancels active work, joins workers, and releases retained
images, frame leases, sources, and model references before returning. Processing
workers use internal stacks allocated only while active; configurations and
result buffers use PSRAM.
