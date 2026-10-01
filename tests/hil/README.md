# RTSP hardware checks

These tests use live hardware. They do not flash or change persistent settings.
Run only with permission to occupy the publisher/viewer and receiver slots.

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
