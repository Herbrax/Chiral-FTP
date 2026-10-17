// Software renderer for Dear ImGui draw data. The PS5 build draws into a CPU
// framebuffer (like the boilerplate's VideoOut demo), and the computer build
// uses the same code, so both show identical pixels.
#pragma once

#include <cstdint>

#include "imgui.h"

namespace raster {

// 32-bit pixels laid out R, G, B, A in memory (IM_COL32 order).
struct Target {
  std::uint32_t *pixels;
  int width;
  int height;
};

// Acknowledges ImGui 1.92 texture requests. Textures stay in ImGui's own CPU
// memory and are sampled from there.
void update_textures(ImDrawData *data);
void render(ImDrawData *data, const Target &target, std::uint32_t clear);

}  // namespace raster
