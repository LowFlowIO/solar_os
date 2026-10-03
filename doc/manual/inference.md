+++
id = "inference"
title = "Model inference"
section = "api"
summary = "Load resident ESP-DL models from storage and execute named tensor inputs and outputs"
keywords = "inference espdl model tensor neural network quantization python lua"
packages_any = ["service_inference"]
agent_reference_sections = true
+++
# Model inference

[Python API](python.md) · [Lua API](lua.md)

## Quick reference

- Enable `inference` (`service.inference`) on ESP32-S3 with PSRAM.
- `solaros.inference.load(path)` loads a compatible `.espdl` file and returns
  a resident integer handle; `close(handle)` releases it.
- `inputs(handle)` and `outputs(handle)` describe named tensors.
- `run(handle, inputs)` accepts every named input and returns owned output
  buffers, tensor descriptors, and native timing statistics.
- Use `reset(handle)` for stateful operators and `close_all()` for session cleanup.

## Availability and ownership

`solaros.inference` requires `service.inference`, provided by the `inference`
group. The `full` flavor enables this group on ESP32-S3 boards with PSRAM.
Camera, display, and image services are optional. No models are built into the
firmware; put compatible `.espdl` files on a mounted SolarOS storage volume.

`load` returns an integer handle. The model file and workspace stay resident
in native memory until `close(handle)`, `close_all()`, or interpreter teardown.
A handle belongs to the script session that loaded it. Closed handles are
invalid and are never reused during that boot. Removing a file after loading
does not release its resident model.

The backend is ESP-DL 3.3.13, with ESP32-S3 kernels. Models must be exported
for this target and compatible operators. The loader accepts unencrypted,
single-model EDL1 and EDL2 containers. Packed and encrypted containers are
unsupported. File structure and bounded static interfaces are checked before
ESP-DL construction; this is not a sandbox for arbitrary model graphs. Use
models from a trusted exporter and validate their numerical behavior.

## API

Python and Lua expose the same module functions:

| Function | Result or effect |
| --- | --- |
| `load(path[, timeout_ms])` | Load from a shell-style storage path; return an integer handle |
| `info(handle)` | Backend, target, memory estimates, and named input/output descriptors |
| `inputs(handle)` | Map of input names to tensor descriptors |
| `outputs(handle)` | Map of output names to tensor descriptors |
| `run(handle, inputs[, timeout_ms])` | Owned output tensors and native timing statistics |
| `reset(handle)` | Reset stateful operators while keeping the model resident |
| `set_mode(handle, mode)` | Select `"single"`, `"auto"`, or `"dual"` execution for subsequent runs |
| `close(handle)` | Release one model |
| `close_all()` | Release all models owned by the current interpreter |

`info` contains `backend="espdl"`, `target="esp32s3"`, `model_bytes`,
`internal_bytes`, `external_bytes`, `inputs`, and `outputs`. Memory figures
are the observed free-heap difference across loading, not peak requirements;
other activity and shared operator registration can affect them.

`mode` reports the selected execution mode, initially `"single"`. `"auto"`
lets ESP-DL choose parallel execution for suitable operators; `"dual"` requests
splitting supported operators between both cores. Operators that cannot split
still execute on one core. SIMD acceleration is available in all modes.
For example, call `solaros.inference.set_mode(model, "auto")` before `run`.
Both worker results are joined before the next operator and before cancellation
returns. Mode changes do not reload or reset a model.

Dual-core worker storage is allocated on first parallel dispatch and reused
across runs and resident models. It is released when the last resident model
closes, including interpreter teardown. Load-time memory estimates exclude
this later allocation. ESP32-S3 SIMD kernels execute from cached firmware
flash; model tensors and workspace use PSRAM.

Every tensor descriptor contains:

| Field | Meaning |
| --- | --- |
| `dtype` | Element type, such as `int8`, `int16`, or `float32` |
| `shape` | Dimensions in the model's native axis order |
| `bytes` | Required dense buffer length |
| `exponents` | ESP-DL power-of-two quantization exponents; empty for floating-point and Boolean ports |

Buffers contain contiguous, row-major, little-endian elements. Shapes have no
implicit image meaning. Supported descriptor types are `int8`, `uint8`,
`int16`, `uint16`, `int32`, `uint32`, `int64`, `uint64`, `float16`, `float32`,
`float64`, and `bool`; individual operators support a subset of these types.

For quantized tensors, `real_value = stored_value * 2**exponent`, with zero
point zero. A single exponent applies to the whole tensor. Per-channel
exponents are exposed as exported; use the model's contract for their axis.
The runtime performs no normalization, quantization, layout conversion,
classification, or detection postprocessing.

## Named inputs and owned outputs

`run` requires every input exactly once, keyed by its exported name. Supply
raw Python buffer objects or Lua binary strings to interpret bytes using the
model's port descriptor. Alternatively, supply typed records:

```python
{"data": buffer, "dtype": "int8", "shape": [2, 4], "exponents": [0]}
```

Typed records require `data`, `dtype`, and `shape`. Optional `bytes` must equal
the buffer length. Optional `exponents` must match the port; omission inherits
the port's quantization. Unknown fields, wrong names, missing inputs, mismatched
types/shapes/quantization, and incorrect lengths raise errors.

The result contains `outputs`, a map of names to descriptors plus `data`.
Python output data is `bytes`; Lua output data is a binary string. Output
data and metadata remain valid across subsequent runs, resets, and model
closure. Each call copies its outputs into the interpreter, so large outputs
must also fit its heap. Inputs are borrowed only until the synchronous call
returns.

`input_us`, `inference_us`, `output_us`, and `elapsed_us` report native copying
and execution time, including cooperative yields. They exclude interpreter
argument conversion, returned-object construction, and task startup.

## Python example

This example assumes a model with two int8 inputs `a`, `b`, each shaped `[2,4]`,
and an output named `sum`. Inspect the descriptors for other models.

```python
from solaros import inference
import struct

model = inference.load("/sdcard/models/arithmetic_int8.espdl")
try:
    print(inference.inputs(model))
    a = struct.pack("<8b", 1, -2, 3, 4, -5, 6, 7, -8)
    b = struct.pack("<8b", 2, 3, -4, 1, 6, -2, 0, 4)
    result = inference.run(model, {
        "a": {"data": a, "dtype": "int8", "shape": [2, 4]},
        "b": b,
    })
    values = struct.unpack("<8b", result["outputs"]["sum"]["data"])
    print(values, result["inference_us"])
finally:
    inference.close(model)
```

## Lua example

```lua
local inference = solaros.inference
local model = inference.load("/sdcard/models/arithmetic_int8.espdl")
local ok, result = pcall(function()
    local a = string.pack("bbbbbbbb", 1, -2, 3, 4, -5, 6, 7, -8)
    local b = string.pack("bbbbbbbb", 2, 3, -4, 1, 6, -2, 0, 4)
    return inference.run(model, {
        a = {data = a, dtype = "int8", shape = {2, 4}},
        b = b,
    })
end)
inference.close(model)
assert(ok, result)
print(string.unpack("bbbbbbbb", result.outputs.sum.data))
```

## Limits, cancellation, and errors

Each interpreter can hold up to four models, subject to available memory.
Each model can have up to 16 inputs and 16 outputs, rank up to eight, tensor
buffers up to 4 MiB, and names up to 127 bytes. Model files are limited to
16 MiB and graphs to 4096 operators. Interfaces use fixed positive dimensions;
dynamic input shapes are unsupported. File, workspace, interpreter heap, and
other services must all fit simultaneously.

One native inference operation runs at a time across Python and Lua. Competing
load/run/reset/close calls report busy rather than queueing. Timeouts default
to 10000 ms and accept 1–60000 ms. Cancellation is checked during file reads
and between operators. Model construction and an individual operator are not
preempted; a call can finish later than its requested timeout. Cleanup waits
for the worker before releasing model or argument memory.

Missing files, unsupported containers/operators, invalid interfaces, allocation
failure, busy execution, and timeout raise interpreter errors. A failed load
does not publish a handle. Failed execution discards partial outputs and resets
the model's operator state; a subsequent valid call can reuse the handle.
