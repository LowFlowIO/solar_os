# Jet renderer

Pinned upstream: https://github.com/CubeCoders/Jet

Revision: `c56dfc0c012ba7a09a6e49b4123340ec24981441`.
The upstream MIT notice is retained in `LICENSE`. Sources are vendored;
firmware builds do not download the renderer. `Example.cpp` and `Sample.cpp`
are excluded. All translation units use `modules/jet/main/JetConfig.hpp`.

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
- The external library's standard allocator routes geometry and scene vectors
  through its private C++ runtime to SolarOS managed PSRAM. Host builds retain
  the standard host allocator. Exposed primitive and scene constructors clean
  up partial allocations if construction fails; provider calls report allocation
  failures. Sphere/capsule generation uses 64-bit coordinate intermediates.
- Trailing source whitespace is normalized.
- Optional cancellation is checked between rasterized triangles. Cancelled
  frames are rejected by the provider before the host exchanges their buffers.
- Sprite order uses a stable insertion sort without per-frame allocations.
- An optional post-geometry hook runs owned overlays before post-processing
  and sprite compositing. Picking queries can be inspected for composition.
- Particle pools use independent seedable random state. Lens flares allow
  direction, fade, element and sort-order updates and sprite deregistration.

The external SolarOS library enables full-width, non-interlaced RGB565, per-pixel depth,
lighting and perspective textures. Direct monochrome/indexed framebuffer
storage is not implemented. Geometry rendering must be serialized: upstream
still has shared baked-material and trigonometric state. Do not construct or
render scenes concurrently.

Upstream updates require reviewing these changes and rerunning native versus
wire-order image comparisons and firmware builds.
