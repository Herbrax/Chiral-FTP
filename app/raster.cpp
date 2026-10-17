#include "raster.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace raster {
namespace {

struct Clip {
  int x0, y0, x1, y1;  // inclusive-exclusive pixel bounds
};

inline std::uint32_t texel(const ImTextureData *tex, float u, float v) {
  if(!tex || !tex->Pixels) return 0xFFFFFFFFu;
  int x = (int)(u * tex->Width);
  int y = (int)(v * tex->Height);
  x = x < 0 ? 0 : x >= tex->Width ? tex->Width - 1 : x;
  y = y < 0 ? 0 : y >= tex->Height ? tex->Height - 1 : y;
  if(tex->Format == ImTextureFormat_Alpha8) {
    return 0x00FFFFFFu | ((std::uint32_t)tex->Pixels[y * tex->Width + x] << 24);
  }
  std::uint32_t c;
  std::memcpy(&c, tex->Pixels + ((size_t)y * tex->Width + x) * 4, 4);
  return c;
}

// (a * b) / 255 for 8-bit channels.
inline std::uint32_t mul8(std::uint32_t a, std::uint32_t b) {
  std::uint32_t t = a * b + 128;
  return (t + (t >> 8)) >> 8;
}

inline std::uint32_t modulate(std::uint32_t c, std::uint32_t t) {
  return mul8(c & 0xFF, t & 0xFF) | mul8(c >> 8 & 0xFF, t >> 8 & 0xFF) << 8 |
         mul8(c >> 16 & 0xFF, t >> 16 & 0xFF) << 16 | mul8(c >> 24, t >> 24) << 24;
}

inline void blend(std::uint32_t *dst, std::uint32_t src) {
  std::uint32_t a = src >> 24;
  if(a == 0) return;
  if(a == 255) {
    *dst = src | 0xFF000000u;
    return;
  }
  std::uint32_t d = *dst, ia = 255 - a;
  std::uint32_t r = mul8(src & 0xFF, a) + mul8(d & 0xFF, ia);
  std::uint32_t g = mul8(src >> 8 & 0xFF, a) + mul8(d >> 8 & 0xFF, ia);
  std::uint32_t b = mul8(src >> 16 & 0xFF, a) + mul8(d >> 16 & 0xFF, ia);
  *dst = r | g << 8 | b << 16 | 0xFF000000u;
}

// An axis-aligned quad (ImGui rectangles and glyphs): two triangles sharing the
// diagonal, one colour, UVs varying linearly along x and y.
bool try_quad(const ImDrawVert *v, const ImDrawIdx *idx, const Target &t, const Clip &clip,
              const ImTextureData *tex) {
  const ImDrawVert &a = v[idx[0]], &b = v[idx[1]], &c = v[idx[2]];
  const ImDrawVert &d = v[idx[3]], &e = v[idx[4]], &f = v[idx[5]];
  // ImGui emits quads as (0,1,2) (0,2,3).
  if(&a != &d || &c != &e) return false;
  const ImDrawVert &p3 = f;
  if(a.col != b.col || a.col != c.col || a.col != p3.col) return false;
  // Corners: a=TL, b=TR, c=BR, p3=BL.
  if(a.pos.y != b.pos.y || c.pos.y != p3.pos.y || a.pos.x != p3.pos.x || b.pos.x != c.pos.x) return false;
  if(a.uv.y != b.uv.y || c.uv.y != p3.uv.y || a.uv.x != p3.uv.x || b.uv.x != c.uv.x) return false;
  float x0 = a.pos.x, x1 = b.pos.x, y0 = a.pos.y, y1 = c.pos.y;
  if(x1 <= x0 || y1 <= y0) return false;

  int ix0 = std::max(clip.x0, (int)std::ceil(x0 - 0.5f));
  int ix1 = std::min(clip.x1, (int)std::ceil(x1 - 0.5f));
  int iy0 = std::max(clip.y0, (int)std::ceil(y0 - 0.5f));
  int iy1 = std::min(clip.y1, (int)std::ceil(y1 - 0.5f));
  if(ix0 >= ix1 || iy0 >= iy1) return true;

  float du = (b.uv.x - a.uv.x) / (x1 - x0);
  float dv = (c.uv.y - a.uv.y) / (y1 - y0);
  bool solid = du == 0 && dv == 0;
  std::uint32_t col = a.col;
  if(solid) {
    std::uint32_t s = modulate(col, texel(tex, a.uv.x, a.uv.y));
    if((s >> 24) == 0) return true;
    for(int y = iy0; y < iy1; y++) {
      std::uint32_t *row = t.pixels + (size_t)y * t.width;
      if((s >> 24) == 255) {
        std::fill(row + ix0, row + ix1, s);
      } else {
        for(int x = ix0; x < ix1; x++) blend(row + x, s);
      }
    }
    return true;
  }
  for(int y = iy0; y < iy1; y++) {
    std::uint32_t *row = t.pixels + (size_t)y * t.width;
    float vv = a.uv.y + (y + 0.5f - y0) * dv;
    float uu = a.uv.x + (ix0 + 0.5f - x0) * du;
    for(int x = ix0; x < ix1; x++, uu += du) {
      blend(row + x, modulate(col, texel(tex, uu, vv)));
    }
  }
  return true;
}

inline float edge(float ax, float ay, float bx, float by, float px, float py) {
  return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

void triangle(const ImDrawVert &v0, const ImDrawVert &v1, const ImDrawVert &v2, const Target &t,
              const Clip &clip, const ImTextureData *tex) {
  float area = edge(v0.pos.x, v0.pos.y, v1.pos.x, v1.pos.y, v2.pos.x, v2.pos.y);
  if(std::fabs(area) < 1e-6f) return;
  float minx = std::min({v0.pos.x, v1.pos.x, v2.pos.x});
  float maxx = std::max({v0.pos.x, v1.pos.x, v2.pos.x});
  float miny = std::min({v0.pos.y, v1.pos.y, v2.pos.y});
  float maxy = std::max({v0.pos.y, v1.pos.y, v2.pos.y});
  int ix0 = std::max(clip.x0, (int)std::floor(minx));
  int ix1 = std::min(clip.x1, (int)std::ceil(maxx) + 1);
  int iy0 = std::max(clip.y0, (int)std::floor(miny));
  int iy1 = std::min(clip.y1, (int)std::ceil(maxy) + 1);
  if(ix0 >= ix1 || iy0 >= iy1) return;

  float inv = 1.0f / area;
  bool same_col = v0.col == v1.col && v0.col == v2.col;
  bool same_uv = v0.uv.x == v1.uv.x && v0.uv.x == v2.uv.x && v0.uv.y == v1.uv.y && v0.uv.y == v2.uv.y;
  std::uint32_t flat = same_uv ? texel(tex, v0.uv.x, v0.uv.y) : 0;
  float c0[4], c1[4], c2[4];
  for(int k = 0; k < 4; k++) {
    c0[k] = (float)(v0.col >> (8 * k) & 0xFF);
    c1[k] = (float)(v1.col >> (8 * k) & 0xFF);
    c2[k] = (float)(v2.col >> (8 * k) & 0xFF);
  }
  for(int y = iy0; y < iy1; y++) {
    float py = y + 0.5f;
    std::uint32_t *row = t.pixels + (size_t)y * t.width;
    for(int x = ix0; x < ix1; x++) {
      float px = x + 0.5f;
      float w0 = edge(v1.pos.x, v1.pos.y, v2.pos.x, v2.pos.y, px, py) * inv;
      float w1 = edge(v2.pos.x, v2.pos.y, v0.pos.x, v0.pos.y, px, py) * inv;
      float w2 = 1.0f - w0 - w1;
      if(w0 < 0 || w1 < 0 || w2 < 0) continue;
      std::uint32_t col;
      if(same_col) {
        col = v0.col;
      } else {
        col = 0;
        for(int k = 0; k < 4; k++) {
          float ch = c0[k] * w0 + c1[k] * w1 + c2[k] * w2;
          col |= (std::uint32_t)(ch < 0 ? 0 : ch > 255 ? 255 : ch + 0.5f) << (8 * k);
        }
      }
      std::uint32_t tx = same_uv ? flat
                                 : texel(tex, v0.uv.x * w0 + v1.uv.x * w1 + v2.uv.x * w2,
                                         v0.uv.y * w0 + v1.uv.y * w1 + v2.uv.y * w2);
      blend(row + x, modulate(col, tx));
    }
  }
}

}  // namespace

void update_textures(ImDrawData *data) {
  if(!data || !data->Textures) return;
  for(ImTextureData *tex : *data->Textures) {
    switch(tex->Status) {
    case ImTextureStatus_WantCreate:
      tex->SetTexID((ImTextureID)(intptr_t)tex);
      tex->SetStatus(ImTextureStatus_OK);
      break;
    case ImTextureStatus_WantUpdates:
      tex->SetStatus(ImTextureStatus_OK);  // pixels are read in place
      break;
    case ImTextureStatus_WantDestroy:
      tex->SetTexID(ImTextureID_Invalid);
      tex->SetStatus(ImTextureStatus_Destroyed);
      break;
    default:
      break;
    }
  }
}

void render(ImDrawData *data, const Target &t, std::uint32_t clear) {
  std::fill(t.pixels, t.pixels + (size_t)t.width * t.height, clear);
  if(!data) return;
  for(const ImDrawList *list : data->CmdLists) {
    const ImDrawVert *verts = list->VtxBuffer.Data;
    const ImDrawIdx *indices = list->IdxBuffer.Data;
    for(const ImDrawCmd &cmd : list->CmdBuffer) {
      if(cmd.UserCallback) continue;
      Clip clip{std::max(0, (int)cmd.ClipRect.x), std::max(0, (int)cmd.ClipRect.y),
                std::min(t.width, (int)std::ceil(cmd.ClipRect.z)),
                std::min(t.height, (int)std::ceil(cmd.ClipRect.w))};
      if(clip.x0 >= clip.x1 || clip.y0 >= clip.y1) continue;
      const ImTextureData *tex = (const ImTextureData *)(intptr_t)cmd.GetTexID();
      const ImDrawVert *v = verts + cmd.VtxOffset;
      const ImDrawIdx *idx = indices + cmd.IdxOffset;
      unsigned n = cmd.ElemCount;
      for(unsigned i = 0; i < n;) {
        if(i + 6 <= n && try_quad(v, idx + i, t, clip, tex)) {
          i += 6;
          continue;
        }
        triangle(v[idx[i]], v[idx[i + 1]], v[idx[i + 2]], t, clip, tex);
        i += 3;
      }
    }
  }
}

}  // namespace raster
