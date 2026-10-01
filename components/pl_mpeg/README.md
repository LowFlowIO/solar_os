PL_MPEG by Dominic Szablewski, MIT license (included in pl_mpeg.h).
Upstream: https://github.com/phoboslab/pl_mpeg
Pinned revision: c871f2be022ece7ef4f64230b4fb8e1fb9eb6023.

SolarOS changes: checked low-level allocations, sticky buffer errors, bounded
compressed buffers and dimensions, and flushing the final reordered reference
picture, plus bounds checks on motion-compensation reads. Only the low-level
demux/video/audio interfaces are used. Seeking exposes picture temporal
references, rebases timestamps to the frame/sample grid, and bounds demux
packet scanning with guards against zero timestamp differences.

Malformed picture, slice, macroblock, and DC-coefficient fields propagate
through the sticky video-buffer error.
