#include "Scene.hpp"
#include "Primitives.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <memory>
#include <vector>

using namespace Renderer;

// Compare raw output from native/wire-order builds outside the renderer. The
// fixture exercises actual scene transforms, clears, depth, shaders, textures,
// translucent framebuffer reads, reflection reads, sprites and scene teardown.
int main(int argc, char **argv) {
    assert(argc == 2);
    FILE *output = std::fopen(argv[1], "wb");
    assert(output);
    constexpr int width = 64, height = 48, count = width * height, guard = 16;
    constexpr uint16_t sentinel = 0xa15c;
    for (int lifetime = 0; lifetime < 3; ++lifetime) {
        std::vector<uint16_t> color(count + guard * 2, sentinel);
        std::vector<uint16_t> depth(count + guard * 2, sentinel);
        std::vector<uint16_t> reflection(count);
        std::array<uint16_t, height> gradient;
        for (int y = 0; y < height; ++y) gradient[y] = uint16_t(0x1234 + y * 35);
        uint16_t texels[] = {0xf800, 0x07e0, 0x001f, 0xffe0};
        Texture texture(2, 2, texels);
        Material material(0xe94b), overlay(0x3b62), textured(0xffff, &texture);
        material.specular = 20;
        Camera camera;
        camera.setPosition(0, 0, -400);
        camera.nearPlane = 1; camera.farPlane = 4096; camera.setFOV(65, width);
        AmbientLight ambient(Color{50, 65, 80});
        DirectionalLight light(Vector3{40, 45, 0}, Color{255, 245, 230}, 230);
        std::unique_ptr<Object> cube(Primitives::createCube(170, 150, 130, &material));
        Scene scene(color.data() + guard, depth.data() + guard, width, height);
        scene.setCamera(&camera); scene.setAmbientLight(&ambient); scene.setDirectionalLight(&light);
        scene.setBackcolor(0x1234); scene.setClearBuffer(true); scene.addObject(cube.get());
        Sprite2D solid, sprite;
        solid.material = &overlay; solid.x = 3; solid.y = 2;
        solid.width = 15; solid.height = 8; solid.alpha = 135;
        sprite.material = &textured; sprite.x = 38; sprite.y = 30;
        sprite.scale = 3; sprite.alpha = 187;
        sprite.textureFlags = Sprite2D::MIRROR_X | Sprite2D::FLIP_Y;
        scene.addSprite(&solid); scene.addSprite(&sprite);
        for (int frame = 0; frame < 48; ++frame) {
            material.shadingMode = static_cast<ShadingMode>(frame % 7);
            material.alpha = frame % 3 ? 255 : 117;
            material.specularExponent = frame % 2 ? 0 : 32;
            material.diffuseMap = frame % 4 ? nullptr : &texture;
            cube->setRotation(frame * 7, frame * 13, frame * 3);
            cube->setPosition(frame % 2 ? -10 : 5, 0, 0);
            scene.backgroundGradientColors = frame % 2 ? gradient.data() : nullptr;
            scene.getRenderer()->wireframeMode = frame % 12 == 11;
            scene.getRenderer()->reflectBuffer = frame ? reflection.data() : nullptr;
            sprite.blendMode = frame % 2 ? BlendMode::BLEND_ADD : BlendMode::BLEND_REPLACE;
            scene.render();
            assert(scene.lastFrameRasterizedTriangles > 0);
            assert(std::all_of(color.begin(), color.begin() + guard, [](auto p) { return p == sentinel; }));
            assert(std::all_of(color.end() - guard, color.end(), [](auto p) { return p == sentinel; }));
            assert(std::all_of(depth.begin(), depth.begin() + guard, [](auto p) { return p == sentinel; }));
            assert(std::all_of(depth.end() - guard, depth.end(), [](auto p) { return p == sentinel; }));
            std::copy_n(color.data() + guard, count, reflection.data());
            assert(std::fwrite(color.data() + guard, sizeof(uint16_t), count, output) == count);
        }
    }
    // Independent constant-color check verifies channel order and depth writes.
    std::vector<uint16_t> color(count), depth(count, 0xffff);
    Camera camera; camera.nearPlane = 1; camera.farPlane = 4096;
    Rasterizer raster(color.data(), width, height, depth.data(), &camera);
    Material red(uint16_t(0xf800)); red.shadingMode = ShadingMode::UNLIT;
    RenderVertex a, b, c;
    a.position = {-10, -10, 100}; b.position = {100, -10, 100}; c.position = {-10, 100, 100};
    assert(raster.drawTriangle(a, b, c, &red, nullptr, nullptr, false, false, false, 0));
    assert(depth[10 * width + 10] == 100);
    assert(std::fwrite(color.data(), sizeof(uint16_t), count, output) == count);
    std::fclose(output);
}
