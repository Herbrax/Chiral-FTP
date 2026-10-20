// Software renderer for Dear ImGui draw data. The Switch build draws into a
// CPU framebuffer (libnx's linear framebuffer mode), and the computer build
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
  // Pixels from the start of one row to the next. Equal to width for a
  // plain buffer; larger when drawing straight into a framebuffer whose
  // rows are padded, which is how the Switch build avoids copying a whole
  // frame on its way to the screen.
  int stride;
};

// Acknowledges ImGui 1.92 texture requests. Textures stay in ImGui's own CPU
// memory and are sampled from there.
void update_textures(ImDrawData *data);
void render(ImDrawData *data, const Target &target, std::uint32_t clear);

}  // namespace raster
