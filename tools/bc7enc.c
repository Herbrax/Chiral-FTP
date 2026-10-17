/* Converts a 3840x2160 24-bit BMP into the BC7 DDS the PS5 dashboard shows
   behind a title (sce_sys/pic0.dds and pic1.dds): DX10 header,
   BC7_UNORM, one mip level, as tools/validate-assets.sh in ps5/ checks.

     cc -O2 -o bc7enc tools/bc7enc.c && ./bc7enc in.bmp out.dds

   Every 4x4 block uses BC7 mode 6 (one RGBA line, 7-bit endpoints plus a
   p-bit, 16 index levels): the end points are the block's extremes along its
   main colour axis. That suits flat artwork and smooth gradients, which is
   what the backgrounds are; the image is opaque. */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int W = 3840, H = 2160;
static const int WEIGHTS[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

static uint8_t *read_bmp(const char *path) {
  FILE *f = fopen(path, "rb");
  uint8_t head[54];
  if(!f || fread(head, 1, 54, f) != 54 || head[0] != 'B' || head[1] != 'M') {
    fprintf(stderr, "%s: not a BMP\n", path);
    exit(1);
  }
  uint32_t offset = head[10] | head[11] << 8 | head[12] << 16 | (uint32_t)head[13] << 24;
  int32_t w = head[18] | head[19] << 8 | head[20] << 16 | head[21] << 24;
  int32_t h = head[22] | head[23] << 8 | head[24] << 16 | head[25] << 24;
  int bpp = head[28] | head[29] << 8;
  int bottom_up = h > 0;
  if(h < 0) h = -h;
  if(w != W || h != H || (bpp != 24 && bpp != 32)) {
    fprintf(stderr, "%s: need a %dx%d 24- or 32-bit BMP, got %dx%d %d-bit\n", path, W, H, w, h, bpp);
    exit(1);
  }
  int bytes = bpp / 8, stride = (W * bytes + 3) & ~3;
  uint8_t *row = malloc((size_t)stride), *rgb = malloc((size_t)W * H * 3);
  fseek(f, (long)offset, SEEK_SET);
  for(int y = 0; y < H; y++) {
    if(fread(row, 1, (size_t)stride, f) != (size_t)stride) {
      fprintf(stderr, "%s: truncated\n", path);
      exit(1);
    }
    uint8_t *out = rgb + (size_t)(bottom_up ? H - 1 - y : y) * W * 3;
    for(int x = 0; x < W; x++) {  // BMP stores BGR
      out[x * 3 + 0] = row[x * bytes + 2];
      out[x * 3 + 1] = row[x * bytes + 1];
      out[x * 3 + 2] = row[x * bytes + 0];
    }
  }
  free(row);
  fclose(f);
  return rgb;
}

typedef struct {
  uint8_t bytes[16];
  int pos;
} bits_t;

static void put(bits_t *b, uint32_t value, int count) {
  for(int i = 0; i < count; i++, b->pos++) {
    if(value >> i & 1) b->bytes[b->pos >> 3] |= (uint8_t)(1 << (b->pos & 7));
  }
}

/* An endpoint channel as 7 bits plus the endpoint's p-bit. */
static int decoded(int q, int p) {
  return q << 1 | p;
}

static void quantize(const float e[4], int p, int q[4], float *err) {
  *err = 0;
  for(int c = 0; c < 4; c++) {
    int v = (int)lroundf((e[c] - (float)p) / 2.0f);
    q[c] = v < 0 ? 0 : v > 127 ? 127 : v;
    float d = (float)decoded(q[c], p) - e[c];
    *err += d * d;
  }
}

static void encode_block(const uint8_t px[16][4], uint8_t out[16]) {
  float mean[4] = {0}, axis[4] = {1, 1, 1, 0}, cov[4][4] = {{0}};
  for(int i = 0; i < 16; i++) for(int c = 0; c < 4; c++) mean[c] += px[i][c] / 16.0f;
  for(int i = 0; i < 16; i++) {
    float d[4];
    for(int c = 0; c < 4; c++) d[c] = px[i][c] - mean[c];
    for(int a = 0; a < 4; a++) for(int c = 0; c < 4; c++) cov[a][c] += d[a] * d[c];
  }
  for(int it = 0; it < 8; it++) {  // power iteration: the main colour axis
    float next[4] = {0}, len = 0;
    for(int a = 0; a < 4; a++) for(int c = 0; c < 4; c++) next[a] += cov[a][c] * axis[c];
    for(int c = 0; c < 4; c++) len += next[c] * next[c];
    if(len < 1e-6f) break;
    len = sqrtf(len);
    for(int c = 0; c < 4; c++) axis[c] = next[c] / len;
  }
  float lo = 1e9f, hi = -1e9f;
  for(int i = 0; i < 16; i++) {
    float t = 0;
    for(int c = 0; c < 4; c++) t += (px[i][c] - mean[c]) * axis[c];
    if(t < lo) lo = t;
    if(t > hi) hi = t;
  }
  float e0[4], e1[4];
  for(int c = 0; c < 4; c++) {
    e0[c] = fminf(255, fmaxf(0, mean[c] + axis[c] * lo));
    e1[c] = fminf(255, fmaxf(0, mean[c] + axis[c] * hi));
  }
  int q0[4], q1[4], p0, p1, t0[4], t1[4];
  float err0, err1, errt;
  quantize(e0, 0, q0, &err0);
  quantize(e0, 1, t0, &errt);
  p0 = errt < err0;
  if(p0) memcpy(q0, t0, sizeof(q0));
  quantize(e1, 0, q1, &err1);
  quantize(e1, 1, t1, &errt);
  p1 = errt < err1;
  if(p1) memcpy(q1, t1, sizeof(q1));

  int ends[2][4], palette[16][4], index[16];
  for(int c = 0; c < 4; c++) {
    ends[0][c] = decoded(q0[c], p0);
    ends[1][c] = decoded(q1[c], p1);
  }
  for(int k = 0; k < 16; k++)
    for(int c = 0; c < 4; c++) palette[k][c] = ((64 - WEIGHTS[k]) * ends[0][c] + WEIGHTS[k] * ends[1][c] + 32) >> 6;
  for(int i = 0; i < 16; i++) {
    int best = 0;
    long best_err = -1;
    for(int k = 0; k < 16; k++) {
      long e = 0;
      for(int c = 0; c < 4; c++) {
        long d = palette[k][c] - px[i][c];
        e += d * d;
      }
      if(best_err < 0 || e < best_err) best_err = e, best = k;
    }
    index[i] = best;
  }
  /* The first index is stored in 3 bits: its top bit must be 0. */
  if(index[0] & 8) {
    int tq[4], tp = p0;
    memcpy(tq, q0, sizeof(tq));
    memcpy(q0, q1, sizeof(q0));
    memcpy(q1, tq, sizeof(q1));
    p0 = p1;
    p1 = tp;
    for(int i = 0; i < 16; i++) index[i] = 15 - index[i];
  }

  bits_t b;
  memset(&b, 0, sizeof(b));
  put(&b, 1 << 6, 7);  // mode 6
  for(int c = 0; c < 4; c++) {
    put(&b, (uint32_t)q0[c], 7);
    put(&b, (uint32_t)q1[c], 7);
  }
  put(&b, (uint32_t)p0, 1);
  put(&b, (uint32_t)p1, 1);
  put(&b, (uint32_t)index[0], 3);
  for(int i = 1; i < 16; i++) put(&b, (uint32_t)index[i], 4);
  memcpy(out, b.bytes, 16);
}

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

int main(int argc, char **argv) {
  if(argc != 3) {
    fprintf(stderr, "usage: %s in.bmp out.dds\n", argv[0]);
    return 2;
  }
  uint8_t *rgb = read_bmp(argv[1]);
  size_t size = (size_t)W * H;  // BC7: one byte per pixel
  uint8_t header[148] = {0};
  memcpy(header, "DDS ", 4);
  put32(header + 4, 124);                    // header size
  put32(header + 8, 0x000A1007);             // caps, height, width, pixel format, mip count, linear size
  put32(header + 12, (uint32_t)H);
  put32(header + 16, (uint32_t)W);
  put32(header + 20, (uint32_t)size);        // linear size
  put32(header + 28, 1);                     // mip levels
  put32(header + 76, 32);                    // pixel format size
  put32(header + 80, 4);                     // FOURCC
  memcpy(header + 84, "DX10", 4);
  put32(header + 108, 0x1000);               // texture
  put32(header + 128, 98);                   // DXGI_FORMAT_BC7_UNORM
  put32(header + 132, 3);                    // 2D texture
  put32(header + 140, 1);                    // array size
  uint8_t *blocks = malloc(size);
  for(int by = 0; by < H / 4; by++) {
    for(int bx = 0; bx < W / 4; bx++) {
      uint8_t px[16][4];
      for(int i = 0; i < 16; i++) {
        const uint8_t *s = rgb + ((size_t)(by * 4 + i / 4) * W + (size_t)(bx * 4 + i % 4)) * 3;
        px[i][0] = s[0];
        px[i][1] = s[1];
        px[i][2] = s[2];
        px[i][3] = 255;
      }
      encode_block(px, blocks + ((size_t)by * (W / 4) + (size_t)bx) * 16);
    }
  }
  FILE *f = fopen(argv[2], "wb");
  if(!f || fwrite(header, 1, sizeof(header), f) != sizeof(header) || fwrite(blocks, 1, size, f) != size || fclose(f)) {
    fprintf(stderr, "%s: could not write\n", argv[2]);
    return 1;
  }
  free(blocks);
  free(rgb);
  return 0;
}
