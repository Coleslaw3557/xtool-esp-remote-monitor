// Drawing layer for the xTool buddy orb: palette-art expansion plus the ember
// overlay elements (progress arc, 7-segment time digits, label pills). Every
// dynamic element restores its pixels from the expanded art layer first, so
// repaints are cheap dirty-rects — no full-screen work after a state switch.
// Host-safe: no Arduino dependencies (tools/preview_ui.cpp compiles this).
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace ui {

constexpr int W = 360, H = 360;

// ember progress arc band at the rim (art is vignetted dark out here)
constexpr float ARC_R0 = 146.0f, ARC_R1 = 164.0f;

// fixed blit rects for the dynamic elements
constexpr int DIG_X0 = 62, DIG_X1 = 298, DIG_Y0 = 246, DIG_Y1 = 324;
constexpr int LBL_X0 = 88, LBL_X1 = 272, LBL_Y0 = 326, LBL_Y1 = 348;

// ---------- tiny math / pixel helpers ----------
static inline float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float sstep(float e0, float e1, float x) {
  if (e0 == e1) return x < e0 ? 0.0f : 1.0f;
  x = clampf((x - e0) / (e1 - e0), 0, 1);
  return x * x * (3 - 2 * x);
}
static inline uint16_t pack565(float r, float g, float b) {
  int ri = (int)clampf(r, 0, 255), gi = (int)clampf(g, 0, 255), bi = (int)clampf(b, 0, 255);
  return (uint16_t)(((ri & 0xF8) << 8) | ((gi & 0xFC) << 3) | (bi >> 3));
}
static inline void unpack565(uint16_t c, float &r, float &g, float &b) {
  r = ((c >> 11) & 31) * (255.0f / 31.0f);
  g = ((c >> 5) & 63) * (255.0f / 63.0f);
  b = (c & 31) * (255.0f / 31.0f);
}
static inline void restoreRect(uint16_t *fb, const uint16_t *layer, int x0, int y0, int x1, int y1) {
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > W) x1 = W;
  if (y1 > H) y1 = H;
  for (int y = y0; y < y1; y++)
    memcpy(fb + y * W + x0, layer + y * W + x0, (size_t)(x1 - x0) * 2);
}
// ember lift toward warm gold; amt 0 = untouched art, 1 = fully lit
static inline void emberPx(uint16_t *fb, const uint16_t *layer, int i, float amt) {
  if (amt <= 0.003f) return;
  float r, g, b;
  unpack565(layer[i], r, g, b);
  fb[i] = pack565(lerpf(r, 255, 0.78f * amt), lerpf(g, 189, 0.64f * amt),
                  lerpf(b, 84, 0.46f * amt));
}

// ---------- art layer prep (once per state switch) ----------
// scale<1 + dy shrinks the illustration upward (nearest neighbor) so the
// digit band below stays clear; the surround fills with the art's own dark
// corner color. scale=1, dy=0 is a straight palette expansion.
static void expandArt(uint16_t *dst, const uint16_t *pal, const uint8_t *px,
                      float scale = 1.0f, int dy = 0) {
  if (scale == 1.0f && dy == 0) {
    for (int i = 0; i < W * H; i++) dst[i] = pal[px[i]];
    return;
  }
  uint16_t fill = pal[px[2 * W + 2]];
  float inv = 1.0f / scale;
  for (int y = 0; y < H; y++) {
    int sy = (int)((y - 180 - dy) * inv + 180.0f + 0.5f);
    for (int x = 0; x < W; x++) {
      int sx = (int)((x - 180) * inv + 180.0f + 0.5f);
      dst[y * W + x] = (sx >= 0 && sx < W && sy >= 0 && sy < H) ? pal[px[sy * W + sx]] : fill;
    }
  }
}

// faint dark track under the arc so progress reads even at 0%
static void bakeArcTrack(uint16_t *dst) {
  int y0 = (int)(180 - ARC_R1) - 2, y1 = (int)(180 + ARC_R1) + 3;
  for (int y = y0; y < y1; y++)
    for (int x = y0; x < y1; x++) {
      float dx = x - 180.0f, dy = y - 180.0f;
      float r = sqrtf(dx * dx + dy * dy);
      float in = sstep(ARC_R0 - 2, ARC_R0 + 2, r) * (1.0f - sstep(ARC_R1 - 2, ARC_R1 + 2, r));
      if (in <= 0.01f) continue;
      float rr, gg, bb;
      unpack565(dst[y * W + x], rr, gg, bb);
      dst[y * W + x] = pack565(lerpf(rr, 10, 0.40f * in), lerpf(gg, 13, 0.40f * in),
                               lerpf(bb, 26, 0.35f * in));
    }
}

// soft dark rounded pill so digits/labels stay readable over any art
static void bakePill(uint16_t *dst, int x0, int y0, int x1, int y1, float darken) {
  float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
  float hx = (x1 - x0) * 0.5f, hy = (y1 - y0) * 0.5f;
  float rad = fminf(hx, hy) * 0.85f;
  for (int y = y0 - 4; y < y1 + 4; y++) {
    if (y < 0 || y >= H) continue;
    for (int x = x0 - 4; x < x1 + 4; x++) {
      if (x < 0 || x >= W) continue;
      float qx = fabsf(x - cx) - (hx - rad), qy = fabsf(y - cy) - (hy - rad);
      float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
      float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0.0f) - rad;
      float a = (1.0f - sstep(-5.0f, 3.0f, d)) * darken;
      if (a <= 0.01f) continue;
      float rr, gg, bb;
      unpack565(dst[y * W + x], rr, gg, bb);
      dst[y * W + x] = pack565(lerpf(rr, 8, a), lerpf(gg, 10, a), lerpf(bb, 22, a));
    }
  }
}

// ---------- progress arc ----------
// fractions run 0..1 from 12 o'clock, clockwise; spark flickers the tip
static void arcPaint(uint16_t *fb, const uint16_t *layer, float f0, float f1, float amt,
                     float spark, int &bx0, int &by0, int &bx1, int &by1) {
  f0 = clampf(f0, 0, 1);
  f1 = clampf(f1, 0, 1);
  if (f0 <= 0.0005f) { // full-band repaint
    bx0 = by0 = (int)(180 - ARC_R1) - 4;
    bx1 = by1 = (int)(180 + ARC_R1) + 5;
  } else { // sector bbox by sampling
    float fx0 = 1e9f, fy0 = 1e9f, fx1 = -1e9f, fy1 = -1e9f;
    for (int i = 0; i <= 14; i++) {
      float a = -1.5707963f + (f0 + (f1 - f0) * i / 14.0f) * 6.2831853f;
      for (int e = 0; e < 2; e++) {
        float rr = e ? ARC_R1 + 2 : ARC_R0 - 2;
        float px = 180 + cosf(a) * rr, py = 180 + sinf(a) * rr;
        fx0 = fminf(fx0, px);
        fy0 = fminf(fy0, py);
        fx1 = fmaxf(fx1, px);
        fy1 = fmaxf(fy1, py);
      }
    }
    bx0 = (int)fx0 - 3;
    by0 = (int)fy0 - 3;
    bx1 = (int)fx1 + 4;
    by1 = (int)fy1 + 4;
  }
  if (bx0 < 0) bx0 = 0;
  if (by0 < 0) by0 = 0;
  if (bx1 > W) bx1 = W;
  if (by1 > H) by1 = H;
  restoreRect(fb, layer, bx0, by0, bx1, by1);
  if (f1 <= f0) return;
  for (int y = by0; y < by1; y++)
    for (int x = bx0; x < bx1; x++) {
      float dx = x - 180.0f, dy = y - 180.0f;
      float rr = sqrtf(dx * dx + dy * dy);
      if (rr < ARC_R0 - 1.5f || rr > ARC_R1 + 1.5f) continue;
      float a = atan2f(dy, dx) + 1.5707963f;
      if (a < 0) a += 6.2831853f;
      float fr = a / 6.2831853f;
      if (fr < f0 || fr >= f1) continue;
      float edge = sstep(ARC_R0 - 1.5f, ARC_R0 + 1.5f, rr) *
                   (1.0f - sstep(ARC_R1 - 1.5f, ARC_R1 + 1.5f, rr));
      float tip = sstep(f1 - 0.05f, f1, fr);
      float hot = 0.78f + 0.22f * tip + 0.35f * spark * tip;
      emberPx(fb, layer, y * W + x, clampf(amt * edge * hot, 0, 1));
    }
}

// ---------- ember 7-segment time ----------
static const uint8_t SEG7[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

static void segRect(uint16_t *fb, const uint16_t *layer, int x0, int y0, int x1, int y1, float amt) {
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > W) x1 = W;
  if (y1 > H) y1 = H;
  for (int y = y0; y < y1; y++)
    for (int x = x0; x < x1; x++) {
      float a = amt;
      if (x == x0 || x == x1 - 1 || y == y0 || y == y1 - 1) a *= 0.45f;
      emberPx(fb, layer, y * W + x, a);
    }
}

static void drawDigit(uint16_t *fb, const uint16_t *layer, int x, int y, int wd, int ht, int t,
                      int d, float amt) {
  uint8_t m = SEG7[d];
  int hh = (ht - 3 * t) / 2;
  if (m & 0x01) segRect(fb, layer, x + t, y, x + wd - t, y + t, amt);
  if (m & 0x02) segRect(fb, layer, x + wd - t, y + t, x + wd, y + t + hh, amt);
  if (m & 0x04) segRect(fb, layer, x + wd - t, y + 2 * t + hh, x + wd, y + 2 * t + 2 * hh, amt);
  if (m & 0x08) segRect(fb, layer, x + t, y + 2 * t + 2 * hh, x + wd - t, y + 3 * t + 2 * hh, amt);
  if (m & 0x10) segRect(fb, layer, x, y + 2 * t + hh, x + t, y + 2 * t + 2 * hh, amt);
  if (m & 0x20) segRect(fb, layer, x, y + t, x + t, y + t + hh, amt);
  if (m & 0x40) segRect(fb, layer, x + t, y + t + hh, x + wd - t, y + 2 * t + hh, amt);
}

static void drawColon(uint16_t *fb, const uint16_t *layer, int x, int y, int wd, int ht, int t,
                      float amt) {
  int cx = x + (wd - t) / 2;
  segRect(fb, layer, cx, y + ht / 3 - t / 2, cx + t, y + ht / 3 + t - t / 2, amt);
  segRect(fb, layer, cx, y + 2 * ht / 3 - t / 2, cx + t, y + 2 * ht / 3 + t - t / 2, amt);
}

// restores the digit band from the layer and draws MM:SS (H:MM:SS >= 100 min)
static void drawTime(uint16_t *fb, const uint16_t *layer, uint32_t secs, float amt) {
  restoreRect(fb, layer, DIG_X0, DIG_Y0, DIG_X1, DIG_Y1);
  char txt[9];
  int wd, ht, t, gap, cw;
  if (secs >= 6000) {
    uint32_t hrs = secs / 3600;
    if (hrs > 9) hrs = 9;
    snprintf(txt, sizeof txt, "%lu:%02lu:%02lu", (unsigned long)hrs,
             (unsigned long)((secs / 60) % 60), (unsigned long)(secs % 60));
    wd = 28;
    ht = 52;
    t = 7;
    gap = 7;
    cw = 11;
  } else {
    snprintf(txt, sizeof txt, "%02lu:%02lu", (unsigned long)(secs / 60),
             (unsigned long)(secs % 60));
    wd = 38;
    ht = 64;
    t = 9;
    gap = 10;
    cw = 14;
  }
  int total = 0;
  for (const char *p = txt; *p; p++) total += (*p == ':' ? cw : wd) + gap;
  total -= gap;
  int x = 180 - total / 2;
  int y = (DIG_Y0 + DIG_Y1) / 2 - ht / 2;
  for (const char *p = txt; *p; p++) {
    if (*p == ':') {
      drawColon(fb, layer, x, y, cw, ht, t, amt);
      x += cw + gap;
    } else {
      drawDigit(fb, layer, x, y, wd, ht, t, *p - '0', amt);
      x += wd + gap;
    }
  }
}

// ---------- gold label (5x7 caps doubled, on its own little pill) ----------
static const uint8_t FONT57[27][5] = {
    {0x7C, 0x12, 0x11, 0x12, 0x7C}, {0x7F, 0x49, 0x49, 0x49, 0x36},
    {0x3E, 0x41, 0x41, 0x41, 0x22}, {0x7F, 0x41, 0x41, 0x22, 0x1C},
    {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F},
    {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01},
    {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F},
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x09, 0x09, 0x09, 0x06},
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01},
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F},
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43},
    {0x00, 0x00, 0x00, 0x00, 0x00}, // A..Z then space
};

static void drawLabel(uint16_t *fb, const uint16_t *layer, const char *s, float amt) {
  restoreRect(fb, layer, LBL_X0, LBL_Y0, LBL_X1, LBL_Y1);
  int n = (int)strlen(s);
  int adv = 14; // 5x2 px glyph + 4 px gap
  int tw = n * adv - 4;
  int x0 = 180 - tw / 2;
  int cy = (LBL_Y0 + LBL_Y1) / 2;
  // pill behind the text, sized to it
  for (int y = LBL_Y0; y < LBL_Y1; y++)
    for (int x = x0 - 12; x < x0 + tw + 12; x++) {
      if (x < 0 || x >= W) continue;
      float qx = fabsf(x - 180.0f) - (tw * 0.5f + 12 - 9);
      float qy = fabsf(y - cy) - (10 - 9);
      float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
      float d = sqrtf(ox * ox + oy * oy) - 9;
      float a = (1.0f - sstep(-4.0f, 2.0f, d)) * 0.60f;
      if (a <= 0.01f) continue;
      float rr, gg, bb;
      unpack565(fb[y * W + x], rr, gg, bb);
      fb[y * W + x] = pack565(lerpf(rr, 8, a), lerpf(gg, 10, a), lerpf(bb, 22, a));
    }
  int y0 = cy - 7;
  for (int c = 0; c < n; c++) {
    int gi = (s[c] >= 'A' && s[c] <= 'Z') ? s[c] - 'A' : 26;
    const uint8_t *g = FONT57[gi];
    for (int col = 0; col < 5; col++)
      for (int row = 0; row < 7; row++) {
        if (!(g[col] >> row & 1)) continue;
        for (int sy = 0; sy < 2; sy++)
          for (int sx = 0; sx < 2; sx++) {
            int px = x0 + c * adv + col * 2 + sx, py = y0 + row * 2 + sy;
            if (px >= 0 && px < W && py >= 0 && py < H)
              emberPx(fb, layer, py * W + px, amt);
          }
      }
  }
}

} // namespace ui
