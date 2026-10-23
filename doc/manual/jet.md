+++
title = "Jet 3D renderer"
id = "jet"
section = "api"
summary = "Script-defined 3D scenes using the resident Jet library"
keywords = "jet 3d render scene mesh cube camera lighting python lua library"
packages_any = ["service_jet"]
agent_reference_sections = true
+++

# Jet 3D renderer

`solaros.jet` renders script-defined scenes using the installed `jet` resident
library. The firmware includes the `service.jet` frontend, not the renderer.
The Python and Lua package groups include this frontend on supported boards;
custom flavors can select the `jet` group separately. It requires ESP32-S3,
PSRAM and a display with RGB565 or MONO1 presentation.

Install a compatible library from your configured signed package catalog:

```text
pkg install jet
```

Installation and rendering are separate operations. `jet.open()` uses the
installed file; it does not download code. A missing or incompatible library
produces an error. Only one scene may be open across all scripts and native
consumers. Close it before `pkg unload jet` or `pkg remove jet`.

## Python

```python
import solaros

with solaros.jet.open() as scene:
    scene.background(0xffffff)
    scene.camera(z=-500, fov=60)
    cube = scene.cube(150, 150, 150, color=0x2192ff)
    for angle in range(360):
        if solaros.should_exit():
            break
        scene.transform(cube, z=100, rx=angle, ry=angle * 2)
        scene.render()
        solaros.time.sleep_ms(1)
```

`open(display=None, fps=0)` returns a scene. The default target is the calling
foreground display. From a port shell, provide a ready target name from
`display list`, for example `solaros.jet.open("display0")`. `fps` is 0 through
30; zero uses the target's preferred stream rate on monochrome targets and 30
on color targets. Presentation may coalesce queued frames.

| Scene method | Arguments and result |
| --- | --- |
| `background(color)` | Set RGB888 clear color; default white. |
| `camera(x=0, y=0, z=-500, rx=0, ry=0, rz=0, fov=60)` | Camera position, integer-degree rotation and horizontal field of view, 10–150 degrees. Near/far planes are 16/4096. |
| `light(ambient=0x5a5a5a, color=0xfff5e6, intensity=220, azimuth=35, elevation=40)` | Ambient RGB888 and directional light; intensity is 0–255, direction is in integer degrees. |
| `cube(width, height, depth, color=0xffffff)` | Create a shaded cube; each dimension is 1–4096. Return an integer object handle. |
| `mesh(vertices, triangles, color=0xffffff)` | Copy vertices and indexed triangles; return an object handle. Vertices are `(x,y,z)` sequences. Python indices are zero-based. Normals are generated separately for each triangle. |
| `mesh_data(vertices, triangles, material=0)` | Copy XYZ/UV/normal vertices and optional per-triangle material handles; see below. |
| `material(...)`, `material_set(handle, ...)`, `material_info(handle)` | Create, partially update, or inspect a reusable material. |
| `texture(width, height, pixels, ...)`, `texture_set(handle, ...)`, `texture_info(handle)` | Copy, update, or inspect a texture. |
| `assign(object, material)` | Replace every triangle's material; zero restores the object's default material. |
| `capabilities()` | Query available provider features, geometry counts and resource limits. |
| `transform(object, x=0, y=0, z=0, rx=0, ry=0, rz=0)` | Replace an object's position and rotation. Omitted fields reset to zero. |
| `remove(object)` | Remove an object. Its handle becomes invalid. |
| `render()` | Render synchronously, then transfer the completed color buffer to the shared presenter without copying. |
| `status()` | Return `frames`, `presented`, `dropped`, `render_us`, `present_us`, `width`, `height`, and `error`. Times are cumulative microseconds. `presented` counts successful presentation calls, not optical refreshes. |
| `close()` | Release objects, buffers, display ownership and the library lease; repeated close is harmless. |

Colors are `0x000000` through `0xffffff`. Positions and vertex coordinates are
integer design units in -4096 through 4096; rotations are integer degrees.
Objects use back-face culling and Gouraud shading, with 16-bit per-pixel depth.
A scene supports 32 objects. Each mesh accepts up to 1024 input vertices and
2048 triangles. Vertex winding determines the visible side.

Use a `with` block or `try/finally` for prompt cleanup. Interpreter teardown also
closes retained scenes after their provider calls finish. Closed scene objects
cannot access a later scene. Foreground session switching joins presentation
and releases its display claim before another session draws; rendering resumes
on return. End a graphics/TUI session or close Emotion faces before opening Jet.
The claimed target defers terminal and status-bar painting until the scene closes
or suspends, including when a port-shell script targets the display shell's screen.

## Lua

Lua exposes the same operations as colon methods. `solaros.jet.open(display,
fps)` defaults to `nil, 0`; method arguments follow the Python table's order.
Lua mesh tables and triangle indices are **one-based**. Object handles are opaque
integers, not array indices.
New resource constructors and setters use an options table in Lua, for example
`scene:material{color=0xffffff, shading="phong"}` and
`scene:material_set(handle, {alpha=160})`. Texture creation is
`scene:texture(width, height, pixels, options)`; mesh data uses
`scene:mesh_data(vertices, triangles, material)`.

```lua
local solaros = require("solaros")
local scene <close> = solaros.jet.open(nil)
local cube = scene:cube(150, 150, 150, 0x2192ff)
for angle = 0, 359 do
    if solaros.should_exit() then break end
    scene:transform(cube, 0, 0, 100, angle, angle * 2, 0)
    scene:render()
    solaros.time.sleep_ms(1)
end
```

The `<close>` declaration releases the scene on normal completion and errors.
Explicit `scene:close()` is also available. Use **Ctrl+]** to stop either runtime.
Complete two-cube examples are supplied as `examples/python/jet.py` and
`examples/lua/jet.lua` in the firmware source tree.

## Materials, textures and mesh attributes

These operations require Jet library 1.1 or later. The original library still
supports the basic methods; `capabilities()` reports unavailable extensions as
false. Resource handles belong to their scene and become invalid when removed
or when the scene closes. `remove()` refuses a material referenced by geometry
or a texture referenced by a material. Detach or remove its consumers first.

```python
texture = scene.texture(2, 2, b"\x00\x01\x01\x00", format="index8",
                        palette=(0xffffff, 0x2192ff))
material = scene.material(texture=texture, shading="phong", specular=80, gloss=32)
scene.assign(cube, material)
scene.material_set(material, alpha=180)
```

`material()` accepts these keyword options, or positional values in this order.
`material_set(handle, ...)` changes only supplied options.

| Option | Default and range |
| --- | --- |
| `color` | White RGB888 tint. |
| `shading` | `"gouraud"`; also `"flat"`, `"phong"`, `"wireframe"`, `"unlit"`, `"water"`, `"additive"`. |
| `texture` | Texture handle, or zero for a solid color. |
| `alpha`, `diffuse`, `specular`, `gloss` | 255, 255, 0, 0 respectively; each 0–255. Gloss sets the Phong specular exponent. |
| `emissive`, `perspective` | False, true; bypass lighting and enable perspective-correct UV interpolation respectively. |
| `water_bias`, `reflection_max_y` | 0 and -1; water reflection row offset 0–255 and maximum reflection row -1–1023. Water shading needs a scene background gradient. |
| `name` | Optional unique material name, fewer than 64 UTF-8 bytes. |

`texture(width, height, pixels, ...)` requires dimensions and pixel bytes as
positional arguments. Width and height are 1–1024. All pixel and palette data
is copied into provider-owned PSRAM; the script may release its input buffers.

| Texture option | Default and meaning |
| --- | --- |
| `format` | `"rgb565"`: two bytes per pixel, most significant byte first. `"rgb888"` uses R,G,B byte triples; `"index8"` uses one-byte palette indices. |
| `address` | `"wrap"`; also `"clamp"` and `"zero"` for out-of-range UVs. |
| `alpha_key` | -1 disables color-key transparency; otherwise an RGB888 color. |
| `screen_space`, `reflection` | False; sample using screen coordinates or reflected view direction. |
| `bilinear` | False; true enables bilinear sampling for RGB textures. Indexed textures use nearest sampling. |
| `offset` | 0–255; palette offset, wrapped by palette length. |
| `palette` | Required for `index8`: 1–256 RGB888 colors. Every input index must fit this palette. |
| `pixels` | Replacement byte buffer for `texture_set()`; supply creation pixels positionally. |
| `name` | Optional unique texture name, fewer than 64 UTF-8 bytes. |

`texture_set(handle, ...)` retains dimensions and format. It can replace pixels
or palette and change sampling options. Allocation or validation failure leaves
the previous texture usable. `texture_info()` includes width, height, format,
palette size and sampling settings. `material_info()` returns material settings.
A scene allows 16 textures and 32 reusable materials, plus each object's
default material.

`mesh_data()` accepts vertices consistently shaped as `(x,y,z)`, `(x,y,z,u,v)`,
or `(x,y,z,u,v,nx,ny,nz)`. UVs are normalized numeric values in -32–32; normals
are numeric components in -1–1. Without explicit normals, per-face normals are
generated. Supply unit-length normals for smooth lighting. Each triangle is
`(a,b,c)` or `(a,b,c,material)`, with the usual language-specific index base.
The optional fourth element overrides the default material argument for that
triangle. Zero selects the object's default material.

Complete textured examples are `examples/python/jet_materials.py` and
`examples/lua/jet_materials.lua`.

## Presentation and memory

Rendering uses the display's full dimensions. Width must be a multiple of eight;
both dimensions must be at most 1024 pixels. The host owns three RGB565 color
buffers and one depth buffer, each `width × height × 2` bytes of PSRAM, in
addition to loaded code, geometry and temporary render queues.

RGB565 targets receive wire-order pixels directly. MONO1 targets use luminance
and 4×4 ordered dithering, with one additional `ceil(width / 8) × height` byte
buffer. Terminal palette changes do not invert the scene. Transport may still
rotate, scale or DMA-stage frames. E-paper refresh policy and settling determine
visible motion independently of successful presentation calls.

The module has no OBJ file loader or indexed-only display output.
Native consumers can use the C provider table in `solar_os_jet_abi.h`
through the [resident library service](native-library.md).

## Quick reference

| Operation | Python / Lua |
| --- | --- |
| Install provider | `pkg install jet` |
| Open scene | `solaros.jet.open(display, fps)`; defaults `None`/`nil`, 0 |
| Geometry | `cube(width,height,depth,color)`; `mesh(vertices,triangles,color)` |
| Update view | `camera(x,y,z,rx,ry,rz,fov)`; `light(ambient,color,intensity,azimuth,elevation)` |
| Update object | `transform(object,x,y,z,rx,ry,rz)`; `remove(object)` |
| Draw and inspect | `render()`; `status()` |
| Release | `close()`, Python `with`, Lua `<close>` |
| Limits | One scene, 32 objects, 1024 input vertices and 2048 triangles per mesh; RGB888 colors; design coordinates -4096..4096 |
