# Jet renderer

Pinned upstream: https://github.com/CubeCoders/Jet

Revision: `c56dfc0c012ba7a09a6e49b4123340ec24981441`.
The upstream MIT notice is retained in `LICENSE`. Sources are vendored;
firmware builds do not download the renderer. `Example.cpp` and `Sample.cpp`
are excluded. All translation units use `services/JetConfig.hpp`.

Local changes:

- `Framebuffer.hpp` defines compile-time native or byte-swapped RGB565 storage.
  Materials and textures retain native RGB565 values. Rasterizer reads decode
  framebuffer pixels before blending/reflection, and stores encode them.
- Scene background clears, sprites and tiled texture output use that policy.
  Complete full-width wire-order frames need no post-render conversion buffer.
- The native-order simple-span optimization is disabled for swapped output.
  Half-width/field layouts and buffered post-processing/checkerboard
  reconstruction are rejected at compile time with swapped output.
- Transform scratch belongs to the scene and is released on scene destruction.
- Untextured faces with constant Gouraud brightness or constant Phong normals
  compute their final lit color once per triangle. Coverage and depth remain
  per pixel; reference builds can set `JET_CONSTANT_COLOR_LIGHTING=0`.
- Uniform equal-byte RGB565 backgrounds use a bulk fill within the active
  render band; `JET_UNIFORM_BYTE_CLEAR=0` selects the original row-fill path.
- ESP-IDF attribute declarations are included explicitly for blend SIMD kernels.
- Geometry and scene vectors use SolarOS managed PSRAM on ESP-IDF. Host builds
  retain the standard allocator. Cube and scene construction clean up partial
  allocations if construction fails; the demo reports allocation failures.
- Trailing source whitespace is normalized.

The SolarOS package enables full-width, non-interlaced RGB565, per-pixel depth,
lighting and perspective textures. Direct monochrome/indexed framebuffer
storage is not implemented. Geometry rendering must be serialized: upstream
still has shared baked-material and trigonometric state. Do not construct or
render scenes concurrently.

Upstream updates require reviewing these changes and rerunning native versus
wire-order image comparisons and firmware builds.
