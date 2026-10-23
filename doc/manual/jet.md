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
| `camera(x=0, y=0, z=-500, rx=0, ry=0, rz=0, fov=60)` | Camera position, integer-degree rotation and horizontal field of view, 10–150 degrees. Default near/far planes are 16/4096. |
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

## Geometry and scene controls

These operations require Jet library 1.2 or later. Python setters accept keyword
options; Lua setters take an options table. Setters change only supplied fields.
`capabilities()` reports `geometry`, `scene`, `max_objects`, and `max_lights`.

| Method | Contract |
| --- | --- |
| `primitive(...)` | Create a primitive and return its object handle. |
| `object_set(object, ...)`, `object_info(object)` | Update or inspect object properties. Info also includes vertex/triangle counts, six local-space bounds (min XYZ, max XYZ), and `lod_levels`. |
| `object_action(object, ...)` | Run a geometry or movement action; return the object handle, or the new handle for `clone`. |
| `mesh_update(object, vertices, triangles, material=0)` | Replace geometry using the `mesh_data()` format while retaining transforms, flags and LOD links. Failed validation or allocation leaves the previous geometry intact. |
| `camera_set(...)`, `camera_info()`, `camera_action(...)` | Partially update, inspect, or move the camera. |
| `point_light(...)`, `point_light_set(light, ...)`, `point_light_info(light)` | Create, update or inspect one of up to eight point lights; `remove(light)` releases it. |
| `gradient(colors)` | Copy a sequence of up to 1024 RGB888 color stops, interpolated over the display height. Empty sequence or Python `None` / Lua `nil` restores the solid background. |
| `lod(object, levels)` | Set up to eight alternative object handles, ordered from highest to lowest detail. Empty sequence clears the list. Cycles and removal of referenced objects are rejected. |
| `settings(...)`, `settings_info()` | Update or inspect scene-wide controls. |

Primitive options:

| Option | Default and meaning |
| --- | --- |
| `kind` | `"cube"`; also `"grid"`, `"plane"`, `"pyramid"`, `"sphere"`, `"capsule"`, `"cylinder"`, `"billboard"`, `"quad"`, `"debug_cube"`. |
| `width`, `height`, `depth`, `radius` | 150, 150, 150, 75; each 1–4096. Capsule height is total height and must be at least twice its radius. |
| `segments`, `rows`, `cols` | 12, 4, 4; segments 3–32, grid vertex rows/columns 2–32. Generated geometry must fit 2048 triangles and 8192 vertices. |
| `caps`, `u_scale`, `v_scale`, `per_cell_uv` | True, 1, 1, false. Cylinder caps, sphere UV repetition (1–32), and grid UV mode. |
| `material`, `material2`, `color` | Zero, zero, white. Zero selects the object's default material; grid material2 defaults to material. Debug cubes own six colored face materials. |
| `name` | Optional unique object name, fewer than 64 UTF-8 bytes. |

Object properties are `x/y/z`, `rx/ry/rz`, `scale_x/scale_y/scale_z` (1/64–16),
`enabled`, `culling` (`"back"`, `"front"`, `"none"`), `blend` (`"replace"`,
`"add"`, `"subtract"`, `"scale"`, `"average"`, `"xor"`), `billboard`,
`depth_test`, `depth_write`, `depth_bias` (-128–127), `precise_sort`,
`fade_near/fade_far`, `appear_near/appear_far` (0–16384), `lod_bias` (-8–8),
`lod_persist`, and `name`. Positions and rotations default to zero, scales
to one, culling to back, blend to replace, depth test/write and LOD persistence
to true. Fade bands default to disabled (both zero); otherwise far must exceed
near. Enabled defaults to true; billboard defaults to false except billboard
primitives.

`object_action()` takes `action` plus optional `x/y/z` (default zero) and
`numerator/denominator` (default one, 1–16384). Actions are `"translate"`,
`"rotate"`, `"look_at"`, `"bake_rotation"`, `"bake_scale"`,
`"cache_positions"`, `"invalidate_positions"`, `"clone"`, `"bake_lighting"`,
and `"unbake"`. Bake scale uses the supplied ratio and retains object scale;
bake rotation moves rotation into the vertices and resets it. Clones have
independent geometry and default materials, and share named materials and LOD
references. Position caching is optional and reports an allocation error if its
main buffer cannot be created. Baking lighting stores flat directional and
ambient lighting per triangle; unbake restores live lighting.

Camera properties are `x/y/z`, `rx/ry/rz`, `fov`, `near` (1–4095), and
`far` (2–4096), with near less than far. `camera_action()` accepts `action`
and `x/y/z`: `"translate"`, `"translate_local"`, `"translate_local_y"`,
`"rotate"`, `"rotate_local"`, `"look_at"`, `"look_at_x"`, or
`"look_at_y"`. Look-at values are target coordinates.

Point-light options are `x/y/z`, `color` (white), `range_min` (0),
`range_max` (255), `enabled` (true), and a unique `name`. Range values are
0–16384 with minimum less than maximum. Scene settings are `clear` (true),
`lod_scale` (0 disables distance LOD; otherwise 1–16384 design units),
`lod_bias` (-8–8, default zero), and `water_time` (numeric animation time,
0–1000000). Objects used only as LOD alternatives should be disabled as
standalone objects with `object_set(alternative, enabled=False)`.

Complete examples are `examples/python/jet_scene.py` and
`examples/lua/jet_scene.lua`.

## Sprites and picking

Jet library 1.3 or later adds `sprite(...)`, `sprite_set(handle, ...)`,
`sprite_info(handle)`, `pick(points)`, and `picks()`. Python uses keyword
options and Lua uses options tables. `capabilities()` includes `sprites`,
`picking`, `max_sprites` (32), and `max_picks` (32).

Sprites draw after 3D geometry, in ascending `z_order`, without depth testing.
`sprite()` returns a handle; `remove(sprite)` unregisters it. An attached
material cannot be removed until its sprites and geometry release it.

| Sprite option | Default and meaning |
| --- | --- |
| `x`, `y` | Zero; top-left screen pixel, -4096–4096. |
| `width`, `height` | 32; solid rectangle dimensions, 0–4096. Textured sprites use texture dimensions. |
| `material`, `color` | Zero and white. Zero uses the sprite's own solid-color material; `color` changes this fallback. |
| `alpha`, `enabled` | 255 and true. Alpha multiplies material alpha. |
| `z_order` | Zero, -32768–32767; equal orders retain their current relative order. |
| `blend` | `"replace"` uses normal alpha compositing; `"add"` adds source channels with saturation. For additive sprites, nonzero alpha enables the full source contribution, so put falloff into the texture. |
| `scale` | One; integer nearest-neighbor upscale, 1–16. |
| `flip_x`, `flip_y` | False; reverse texture sampling. |
| `mirror_x`, `mirror_y` | False; append reflected halves to double the texture extent along that axis. Flips apply to the expanded image. |
| `name` | Optional unique sprite name, fewer than 64 UTF-8 bytes. |

`pick(points)` copies up to 32 `(x,y)` pairs in screen pixels. Coordinates
must be inside the display, or -1 to disable a query. Empty sequence or
Python `None` / Lua `nil` disables picking. New queries clear the previous
results; call `render()` before `picks()`.

`picks()` returns a list/table of result dictionaries/tables, one per query:
`hit`, `object` (handle, zero on a miss), `triangle`, `depth`, `x`, and `y`.
Python triangle indices start at zero and use -1 on a miss; Lua indices start
at one and use zero on a miss. Results describe the last completed render and
retain those handle values even if an object is subsequently removed.
The index addresses the selected mesh, including any LOD alternative.

Picking tests filled triangle coverage. Sprites, wireframe outlines, texture
color-key holes and per-pixel transparency do not participate. Overlapping
triangles are compared using mean camera-space triangle depth rather than an
exact ray intersection. Picking is disabled by default; leaving the query list
empty skips all query iterations.

```python
cursor = scene.sprite(x=width // 2 - 2, y=height // 2 - 2,
                      width=5, height=5, color=0xff0000)
scene.pick(((width // 2, height // 2),))
scene.render()
selection = scene.picks()[0]
if selection["hit"]:
    selected_object = selection["object"]
```

Complete examples are `examples/python/jet_picking.py` and
`examples/lua/jet_picking.lua`.

## Asset loading

Jet library 1.4 or later reports `assets=True` in `capabilities()` and adds
three loaders. They accept file contents, copy retained data into owned storage,
and leave the scene unchanged if validation or allocation fails. Read files
with the language's normal file APIs; the provider does not open paths or fetch
dependencies.

| Method | Input and result |
| --- | --- |
| `load_obj(data, scale=1, material=0, color=0xffffff, name="")` | OBJ text or bytes, up to 1 MiB; return an object handle. Python accepts keyword options; Lua takes an options table after the data string. |
| `load_mtl(data)` | MTL text or bytes, up to 256 KiB; return material handles in declaration order. The complete material set is published together. |
| `load_bmp(data, ...)` | BMP bytes, up to 8 MiB; return a texture handle. Accepts the texture sampling and name options. Format is always `"rgb565"`; `pixels` and `palette` options are not accepted. |

OBJ loading supports `v`, `vt`, `vn`, `usemtl`, positive and negative face
indices, and `v`, `v/vt`, `v//vn`, or `v/vt/vn` face elements. Convex
polygons of up to 16 vertices are triangulated as a fan. Missing normals are
generated per triangle; supplied normals are retained. An optional fourth
position component is the homogeneous divisor. OBJ units are multiplied by
`128 * scale`, matching Jet's loader convention; scale is 1/1024–32 and final
coordinates must fit -4096–4096. A file may define up to 1024 positions, 8192
combined UV/normal records, and 2048 generated triangles. Lines may contain at
most 4096 bytes.

`usemtl` names must match materials already in the scene. Load MTL data first;
`mtllib`, object/group names and smoothing declarations do not load files or
split the object. Jet's `mt HEX565 [alpha]` extension creates an object-owned
face material; up to 32 are allowed. `md` or `mr` restores the default material.

MTL loading supports `newmtl`, `Kd`, `Ks`, `Ns`, `d`, `Tr`, `illum`,
and `map_Kd`. Diffuse color and opacity values are 0–1. Specular color is
averaged into Jet's scalar specular coefficient; gloss is clamped to 255.
`illum 0` selects unlit shading, 1 selects Gouraud, and 2–10 select Phong.
Other MTL properties are ignored. Texture-map names, including spaces, must
exactly match textures loaded or created beforehand; map options are not
supported. Material names must be unique and fit the normal 63-byte limit.

BMP loading accepts Windows DIB headers of at least 40 bytes, dimensions
1–1024, top-down or bottom-up rows, and uncompressed 1-, 4-, 8-, 16-, 24-, or
32-bit pixels. Paletted files are expanded to RGB565. Uncompressed 16-bit files
use RGB555; 16-bit bit-field files support RGB555 and RGB565 masks. Standard
BGR masks are also accepted for 32-bit bit-field files. RLE and other
compression are rejected. The 32-bit alpha byte is ignored; use the texture's
`alpha_key` option when color-key transparency is needed.

```python
with open("/sdcard/assets/checker.bmp", "rb") as source:
    texture = scene.load_bmp(source.read(), name="checker.bmp")
with open("/sdcard/assets/model.mtl") as source:
    materials = scene.load_mtl(source.read())
with open("/sdcard/assets/model.obj") as source:
    model = scene.load_obj(source.read(), scale=0.7)
```

Self-contained examples that build a tiny BMP and import a textured OBJ cube
are `examples/python/jet_assets.py` and `examples/lua/jet_assets.lua`.

## Shared mesh instancing

Jet library 1.5 or later reports `instancing=True`. A prototype is an immutable
copy of an object's local geometry. Its default and object-owned face materials
are copied; named materials stay shared and remain editable. The source object
can be changed or removed afterward. Its position, rotation, scale and LOD
links are not part of the prototype; bake any wanted geometry changes first.
Objects that already contain instances cannot become prototypes.

| Method | Contract |
| --- | --- |
| `prototype(object, name="")` | Freeze local geometry and return a prototype handle. Python accepts the name keyword; Lua takes an options table. |
| `prototype_info(prototype)` | Return id, name, vertex/triangle counts and six local-space bounds. |
| `instance(prototype, ...)` | Create an object containing one instance; return its object handle. |
| `instance_add(object, prototype, ...)` | Append an instance to an existing object; return its index. The object may also have its own geometry. |
| `instance_set(object, index, ...)` | Partially update an instance's local pose and material overrides. |
| `instance_info(object, index)` | Return local pose/options, prototype handle, per-face material handles, row-major 3×3 `basis`, and `baked`. |
| `instance_remove(object, index)`, `instance_clear(object)` | Remove one or all instances, preserving the object's own geometry. Later indices shift down after removal. |
| `remove(prototype)` | Release a prototype after all of its instances and clones are removed. |

Instance indices are zero-based in Python and one-based in Lua. Options are
`x/y/z`, `rx/ry/rz`, `material` (all default zero), and `materials`
(optional per-triangle material handles). Python uses keywords; Lua uses an
options table. Zero material handles retain the prototype's original face
materials. A nonzero uniform `material` takes precedence over the per-face
table. A supplied table must match the prototype's triangle count; an empty
sequence clears it. Material references reject removal while in use.

Instances share prototype geometry and inherit their containing object's
transform, culling, fade, LOD and depth flags. Their local bounds must fit
-4096–4096. A scene supports 16 prototypes and up to 32 instances per object;
the object's own geometry plus all instances must fit 2048 triangles.
`capabilities()` reports `max_prototypes` and `max_instances`.
`object_info()` includes the instance count and total geometry counts.

`clone` copies the batch placements while retaining shared prototypes.
`bake_rotation` moves the object's rotation into its geometry and instance
placements. `bake_scale` creates shared scaled mesh copies within that batch;
`instance_info().baked` then becomes true while `prototype` identifies the
original prototype whose materials remain retained. Later partial instance
updates preserve that baked geometry. Pose angles are rounded integer Euler
angles; `basis` exposes the exact retained matrix.
`assign(object, material)` updates both the object's own faces and all
instance overrides, clearing per-face override tables.

Picking results additionally include `mesh`, `batch`, and `instance`.
`object` identifies the owning scene object, `batch` identifies the object
whose geometry was selected by LOD, and `mesh` is the actual object/prototype
handle. `instance` uses the language's index base, or Python -1 / Lua 0 for
ordinary geometry. For older providers these extra handles are zero and the
instance index indicates ordinary geometry. Results retain the last completed
frame's values even after resources are removed.

Complete examples are `examples/python/jet_instances.py` and
`examples/lua/jet_instances.lua`.

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
